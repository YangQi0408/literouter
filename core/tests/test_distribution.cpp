#include <httplib.h>
#include "lr_test_check.h"
import nlohmann.json;
import literouter.core;

namespace {
using namespace literouter;
using json = nlohmann::json;
using namespace std::chrono_literals;

httplib::Headers key(std::string value) { return {{"Authorization", "Bearer " + value}}; }
const std::string request = R"({"model":"public","messages":[{"role":"user","content":"hello"}]})";

bool until(const std::function<bool()> &condition) {
    for (int i = 0; i < 300; ++i) {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void integration() {
    LR_GROUP("real proxy enforces account permissions and shares quota across keys");
    const auto dir = std::filesystem::temp_directory_path() / ("lr-distribution-" + hexId(8));
    std::filesystem::create_directories(dir);
    lr_test::EnvGuard state("LITEROUTER_STATE_DIR"); state.assign(dir.string());
    lr_test::EnvGuard secret("LR_DIST_KEY"); secret.assign("client-two");
    lr_test::EnvGuard admin_secret("LR_DIST_ADMIN_KEY"); admin_secret.assign("admin-private-key");
    lr_test::EnvGuard fallback("LR_DIST_FALLBACK"); fallback.assign("");
    httplib::Server upstream;
    std::atomic<int> paid{0}, private_calls{0};
    std::atomic<bool> block{false}, entered{false}, release{false};
    upstream.Post("/paid/chat/completions", [&](const auto &, auto &res) {
        ++paid;
        if (block) {
            entered = true;
            until([&] { return release.load(); });
        }
        res.set_content(R"({"choices":[{"message":{"role":"assistant","content":"ok"}}],"usage":{"prompt_tokens":10,"completion_tokens":20}})", "application/json");
    });
    upstream.Post("/private/chat/completions", [&](const auto &, auto &res) {
        ++private_calls; res.set_content("{}", "application/json");
    });
    const int upstream_port = upstream.bind_to_any_port("127.0.0.1");
    LR_CHECK(upstream_port > 0);
    std::thread upstream_thread([&] { upstream.listen_after_bind(); });
    upstream.wait_until_ready();

    AppConfig cfg;
    cfg.server.port = 0;
    cfg.server.api_key = "${LR_DIST_ADMIN_KEY:-admin-fallback-private}";
    cfg.server.persist_telemetry = false;
    ProviderConfig p;
    p.id = "private"; p.base_url = std::format("http://127.0.0.1:{}/private", upstream_port);
    p.groups = {"private"}; p.models = {"public", "hidden"}; p.priority = 1;
    cfg.providers.push_back(p);
    p.id = "paid"; p.base_url = std::format("http://127.0.0.1:{}/paid", upstream_port);
    p.groups = {"paid"}; p.models = {"public"}; p.priority = 10;
    p.api_key = "${LR_DIST_FALLBACK:-upstream-private-key}"; p.price_in_per_million = 1; p.price_out_per_million = 2;
    cfg.providers.push_back(p);
    cfg.routes.push_back({"public", {{"private", "private-real"}, {"paid", "paid-real"}}, true});
    cfg.routes.push_back({"hidden", {{"private", "hidden-real"}}, true});
    ClientConfig alice;
    alice.id = "alice"; alice.name = "Alice"; alice.models = {"public"};
    alice.provider_groups = {"paid"}; alice.max_concurrent = 1; alice.requests_per_day = 2;
    alice.keys = {{"one", "client-one", true}, {"two", "${LR_DIST_KEY}", true},
                  {"fallback", "${LR_DIST_FALLBACK:-client-fallback-private}", true}};
    cfg.clients = {alice};
    auto bob = alice; bob.id = "bob"; bob.keys = {{"one", "bob-key", true}};
    bob.requests_per_day = 0;
    cfg.clients.push_back(bob);
    ProxyServer proxy;
    proxy.setConfigPath((dir / "config.json").string());
    auto started = proxy.start(cfg);
    LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
    if (!started) { upstream.stop(); upstream_thread.join(); return; }
    const int port = proxy.boundPort();
    ProxyServer competing;
    competing.setConfigPath((dir / "config.json").string());
    const auto competing_start = competing.start(cfg);
    LR_CHECK(!competing_start);
    LR_CHECK(competing_start.error().find("quota state") != std::string::npos);
    competing.stop();
    httplib::Client http("127.0.0.1", port);
    http.set_read_timeout(5, 0);
    auto response = http.Get("/__literouter/config", key("client-one"));
    LR_CHECK(response && response->status == 401);
    response = http.Get("/__literouter/metrics", key("client-one"));
    LR_CHECK(response && response->status == 401);
    response = http.Post("/__literouter/shutdown", key("client-one"), "", "application/json");
    LR_CHECK(response && response->status == 401);
    response = http.Get("/health"); LR_CHECK(response && response->status == 200);
    response = http.Get("/v1/models", key("client-one"));
    LR_CHECK(response && response->status == 200);
    LR_CHECK(response->body.find("hidden") == std::string::npos);
    LR_CHECK(response->body.find("private") == std::string::npos);
    LR_CHECK(response->body.find("paid-real") == std::string::npos);
    response = http.Get("/v1beta/models", key("client-one"));
    LR_CHECK(response && response->body.find("hidden") == std::string::npos);
    response = http.Post("/v1/chat/completions", key("client-one"), R"({"model":"hidden"})", "application/json");
    LR_CHECK(response && response->status == 403);
    LR_CHECK_EQ(paid.load(), 0); LR_CHECK_EQ(private_calls.load(), 0);
    response = http.Post("/v1/chat/completions", key("client-one"), request, "application/json");
    LR_CHECK(response && response->status == 200);
    response = http.Post("/v1/chat/completions", {{"x-api-key", "client-two"}}, request, "application/json");
    LR_CHECK(response && response->status == 200);
    response = http.Post("/v1/chat/completions", key("client-one"), request, "application/json");
    LR_CHECK(response && response->status == 429);
    LR_CHECK(response->has_header("Retry-After"));
    LR_CHECK_EQ(paid.load(), 2); LR_CHECK_EQ(private_calls.load(), 0);
    LR_CHECK_EQ(proxy.snapshot().clients[0].requests, 2);
    LR_CHECK_EQ(proxy.snapshot().clients[0].tokens_prompt, 20);
    LR_CHECK_EQ(proxy.snapshot().clients[0].tokens_completion, 40);
    LR_CHECK(lr_test::closeTo(proxy.snapshot().clients[0].cost_usd, 0.0001));
    auto logs = proxy.logsSince(0, 100);
    LR_CHECK(std::ranges::any_of(logs, [](const auto &entry) {
        return entry.client_id == "alice" && entry.client_key_id == "two" && entry.status == 200;
    }));
    auto status = fetchStatus(std::format("http://127.0.0.1:{}", port), "${LR_DIST_ADMIN_KEY}");
    LR_CHECK(status.reachable); LR_CHECK_EQ(status.snapshot.clients[0].requests_today, 2);
    auto reset = adminPost(std::format("http://127.0.0.1:{}", port), "/reset-stats", "", "${LR_DIST_ADMIN_KEY}");
    LR_CHECK(reset.reachable && reset.status == 200);
    LR_CHECK_EQ(proxy.snapshot().clients[0].requests_today, 2);

    LR_GROUP("redacted administration round trip keeps exact secret references");
    response = http.Get("/__literouter/config", key("admin-private-key"));
    LR_CHECK(response && response->status == 200);
    LR_CHECK(response->body.find("admin-private-key") == std::string::npos);
    LR_CHECK(response->body.find("upstream-private-key") == std::string::npos);
    LR_CHECK(response->body.find("client-one") == std::string::npos);
    LR_CHECK(response->body.find("client-two") == std::string::npos);
    LR_CHECK(response->body.find("admin-fallback-private") == std::string::npos);
    LR_CHECK(response->body.find("client-fallback-private") == std::string::npos);
    auto config_body = json::parse(response->body).at("config");
    LR_CHECK_EQ(config_body["clients"][0]["keys"][1]["api_key"].get<std::string>(), "${LR_DIST_KEY}");
    config_body["clients"][0]["name"] = "Alice renamed";
    response = http.Put("/__literouter/config", key("admin-private-key"), config_body.dump(), "application/json");
    LR_CHECK(response && response->status == 200);
    const auto stored = ConfigStore::load(dir / "config.json");
    LR_CHECK(stored.has_value());
    LR_CHECK_EQ(stored->config().clients[0].keys[0].api_key, "client-one");
    LR_CHECK_EQ(stored->config().clients[0].keys[1].api_key, "${LR_DIST_KEY}");
    LR_CHECK_EQ(stored->config().server.api_key, "${LR_DIST_ADMIN_KEY:-admin-fallback-private}");
    LR_CHECK_EQ(stored->config().providers[1].api_key, "${LR_DIST_FALLBACK:-upstream-private-key}");
    LR_CHECK_EQ(stored->config().clients[0].keys[2].api_key, "${LR_DIST_FALLBACK:-client-fallback-private}");
    config_body["server"]["api_key_clear"] = true;
    response = http.Put("/__literouter/config", key("admin-private-key"), config_body.dump(), "application/json");
    LR_CHECK(response && response->status == 422);

    LR_GROUP("concurrency permit spans the real upstream request");
    block = true;
    std::atomic<int> slow_status{0};
    std::thread slow([&] {
        httplib::Client connection("127.0.0.1", port);
        connection.set_read_timeout(5, 0);
        auto result = connection.Post("/v1/chat/completions", key("bob-key"), request, "application/json");
        slow_status = result ? result->status : -1;
    });
    LR_CHECK(until([&] { return entered.load(); }));
    response = http.Post("/v1/chat/completions", key("bob-key"), request, "application/json");
    LR_CHECK(response && response->status == 429);
    release = true; slow.join(); LR_CHECK_EQ(slow_status.load(), 200);
    block = false;
    LR_CHECK(until([&] { return proxy.snapshot().clients[1].active_requests == 0; }));
    cfg.clients[1].keys[0].enabled = false;
    proxy.updateConfig(cfg);
    response = http.Post("/v1/chat/completions", key("bob-key"), request, "application/json");
    LR_CHECK(response && response->status == 401);

    LR_GROUP("resetting optional telemetry or restarting cannot reset account quotas");
    proxy.resetStats();
    LR_CHECK_EQ(proxy.snapshot().clients[0].requests_today, 2);

    LR_GROUP("personal instances defer storage and independent configs own separate quotas");
    const auto personal_path = dir / "personal.json";
    const auto personal_quota = defaultClientQuotaPath(personal_path, 0);
    auto personal_cfg = cfg;
    personal_cfg.clients.clear();
    ProxyServer personal;
    personal.setConfigPath(personal_path.string());
    LR_CHECK(personal.start(personal_cfg).has_value());
    LR_CHECK(!std::filesystem::exists(personal_quota));
    LR_CHECK(!std::filesystem::exists(personal_quota.string() + ".lock"));
    personal.updateConfig(cfg);
    httplib::Client personal_http("127.0.0.1", personal.boundPort());
    response = personal_http.Post("/v1/chat/completions", key("client-one"), request, "application/json");
    LR_CHECK(response && response->status == 200);
    LR_CHECK_EQ(personal.snapshot().clients[0].requests_today, 1);
    LR_CHECK_EQ(proxy.snapshot().clients[0].requests_today, 2);
    LR_CHECK(std::filesystem::exists(personal_quota));
    personal.stop();

    proxy.stop();
    // Occupy the old port so the restart must bind a different ephemeral port.
    httplib::Server previous_port;
    previous_port.set_socket_options([](auto socket) {
        int yes = 1;
#ifdef _WIN32
        setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&yes), sizeof(yes));
#endif
    });
    LR_CHECK(previous_port.bind_to_port("127.0.0.1", port));
    cfg.server.port = 0;
    ProxyServer restarted;
    restarted.setConfigPath((dir / "config.json").string());
    LR_CHECK(restarted.start(cfg).has_value());
    LR_CHECK(restarted.boundPort() != port);
    httplib::Client again("127.0.0.1", restarted.boundPort());
    response = again.Post("/v1/chat/completions", key("client-one"), request, "application/json");
    LR_CHECK(response && response->status == 429);
    restarted.stop();
    previous_port.stop();
    upstream.stop(); upstream_thread.join();
    LR_CHECK(std::filesystem::exists(defaultClientQuotaPath(dir / "config.json", port)));
    LR_CHECK(!std::filesystem::exists(dir / std::format("telemetry-{}.json", port)));
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
} // namespace

int main() { integration(); return LR_SUMMARY("test_distribution"); }
