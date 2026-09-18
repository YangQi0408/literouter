#include "lr_test_check.h"
import nlohmann.json;
import literouter.core;

namespace {
using namespace literouter;
using json = nlohmann::json;
static_assert(!std::is_copy_constructible_v<ClientLedger>);
static_assert(!std::is_move_constructible_v<ClientLedger>);

struct Scratch {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("lr-clients-" + hexId(8));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

ClientConfig account() {
    ClientConfig c;
    c.id = "alice";
    c.name = "Alice";
    c.keys = {{"laptop", "client-laptop", true}, {"desktop", "${LR_TEST_CLIENT_KEY}", true}};
    return c;
}

void authentication() {
    LR_GROUP("client identity, admin separation, references and ambiguous credentials");
    auto generated = generateClientKey();
    auto another = generateClientKey();
    LR_CHECK(generated && another);
    LR_CHECK(generated->starts_with("lr_") && generated->size() == 67);
    LR_CHECK(*generated != *another);
    LR_CHECK_EQ(redactSecrets(*generated), "lr_[redacted]");
    lr_test::EnvGuard env("LR_TEST_CLIENT_KEY"); env.assign("client-desktop");
    lr_test::EnvGuard admin("LR_TEST_ADMIN_KEY"); admin.assign("administrator");
    AppConfig cfg;
    LR_CHECK(authenticateClient(cfg, "")->administrator);
    cfg.server.api_key = "${LR_TEST_ADMIN_KEY}";
    cfg.clients.push_back(account());
    LR_CHECK(!authenticateClient(cfg, ""));
    LR_CHECK(!authenticateClient(cfg, "invalid"));
    LR_CHECK(authenticateClient(cfg, "administrator", true)->administrator);
    LR_CHECK_EQ(authenticateClient(cfg, "client-desktop")->client_id, "alice");
    LR_CHECK_EQ(authenticateClient(cfg, "client-desktop")->key_id, "desktop");
    LR_CHECK(!authenticateClient(cfg, "client-desktop", true));
    cfg.clients[0].keys[1].enabled = false;
    LR_CHECK(!authenticateClient(cfg, "client-desktop"));
    cfg.clients[0].enabled = false;
    LR_CHECK(!authenticateClient(cfg, "client-laptop"));
    cfg.clients[0].enabled = true;
    cfg.server.api_key.clear();
    LR_CHECK(!authenticateClient(cfg, "", true));
    cfg.server.api_key = "client-laptop";
    LR_CHECK(!authenticateClient(cfg, "client-laptop", true));
    cfg.server.api_key = "admin";
    cfg.clients.push_back(account()); cfg.clients.back().id = "bob";
    LR_CHECK(!authenticateClient(cfg, "client-laptop"));
}

void configAndPolicies() {
    LR_GROUP("strict access config parsing and raw secret round trips");
    AppConfig cfg;
    cfg.server.api_key = "admin";
    auto c = account();
    c.models = {"public"}; c.provider_groups = {"paid"};
    cfg.clients = {c};
    ProviderConfig p;
    p.id = "upstream"; p.base_url = "http://localhost:1"; p.groups = {"paid", "fast"};
    cfg.providers = {p};
    auto parsed = appConfigFromJson(toJsonString(cfg));
    LR_CHECK(parsed.has_value());
    LR_CHECK_EQ(parsed->clients[0].keys[1].api_key, "${LR_TEST_CLIENT_KEY}");
    LR_CHECK_EQ(parsed->providers[0].groups[0], "paid");
    LR_CHECK(clientAllowsModel(c, "public"));
    LR_CHECK(!clientAllowsModel(c, "private"));
    LR_CHECK(clientAllowsProvider(c, p));
    p.groups = {"other"}; LR_CHECK(!clientAllowsProvider(c, p));
    c.models.clear(); c.provider_groups.clear();
    LR_CHECK(clientAllowsModel(c, "any")); LR_CHECK(clientAllowsProvider(c, p));
    LR_CHECK(validate(cfg).ok());
    for (const auto field : {&ClientConfig::requests_per_day, &ClientConfig::tokens_per_day,
                             &ClientConfig::token_reservation}) {
        auto unsafe = cfg;
        unsafe.clients[0].*field = 9007199254740992ULL;
        LR_CHECK(!validate(unsafe).ok());
    }
    cfg.server.api_key.clear(); LR_CHECK(!validate(cfg).ok());
    cfg.server.api_key = "admin";
    cfg.clients[0].keys.push_back({"extra", "admin", true});
    LR_CHECK(!validate(cfg).ok());
    for (const auto *bad : {
        R"({"clients":{}})", R"({"clients":[{"models":"public"}]})",
        R"({"clients":[{"provider_groups":[123]}]})", R"({"clients":[{"keys":{}}]})",
        R"({"clients":[{"requests_per_day":-1}]})", R"({"clients":[{"tokens_per_day":1.5}]})",
        R"({"clients":[{"max_concurrent":4294967296}]})", R"({"clients":[{"keys":[null]}]})"})
        LR_CHECK_MSG(!appConfigFromJson(bad), bad);
    LR_CHECK(appConfigFromJson("{}")->clients.empty());
}

void limitsAndPersistence() {
    LR_GROUP("shared concurrent permits, UTC daily quotas and restart persistence");
    Scratch dir;
    ClientLedger ledger;
    LR_CHECK(ledger.open(dir.path / "quota.json").has_value());
    auto c = account(); c.max_concurrent = 1; c.requests_per_day = 2;
    auto first = ledger.admit(c);
    LR_CHECK(first.has_value());
    auto rejected = ledger.admit(c);
    LR_CHECK(!rejected); LR_CHECK_EQ(rejected.error().status, 429);
    LR_CHECK_EQ(ledger.snapshot()[0].requests, 1);
    (*first)->finish(true, 10, 20, 0.25, true);
    (*first)->finish(true, 999, 999, 100, true);
    LR_CHECK_EQ(ledger.snapshot()[0].tokens_prompt, 10);
    LR_CHECK_EQ(ledger.snapshot()[0].active_requests, 0);
    auto second = ledger.admit(c);
    LR_CHECK(second.has_value());
    second->reset();
    LR_CHECK_EQ(ledger.snapshot()[0].failures, 1);
    LR_CHECK(!ledger.admit(c));
    ledger.close();
    ClientLedger reopened;
    LR_CHECK(reopened.open(dir.path / "quota.json").has_value());
    LR_CHECK(!reopened.admit(c));
    LR_CHECK_EQ(reopened.snapshot()[0].tokens_today, 30);
    LR_CHECK(lr_test::closeTo(reopened.snapshot()[0].cost_usd, 0.25));
    auto bob = c; bob.id = "bob";
    LR_CHECK(reopened.admit(bob).has_value());
    reopened.close();
    std::ifstream in(dir.path / "quota.json");
    auto j = json::parse(in); in.close();
    j["clients"][0]["day_unix"] = std::floor(nowUnix() / 86400) * 86400 - 86400;
    std::ofstream(dir.path / "quota.json") << j.dump();
    ClientLedger tomorrow;
    LR_CHECK(tomorrow.open(dir.path / "quota.json").has_value());
    LR_CHECK_EQ(tomorrow.snapshot()[0].requests_today, 0);
    LR_CHECK(tomorrow.admit(c).has_value());
}

void tokensAndFailure() {
    LR_GROUP("token reservations, missing usage, crash recovery and invalid storage");
    Scratch dir;
    ClientLedger ledger;
    LR_CHECK(ledger.open(dir.path / "quota.json").has_value());
    auto c = account(); c.tokens_per_day = 100; c.token_reservation = 60;
    auto first = ledger.admit(c);
    LR_CHECK(first.has_value());
    LR_CHECK_EQ(ledger.snapshot()[0].reserved_tokens, 60);
    LR_CHECK(!ledger.admit(c));
    (*first)->finish(true, 10, 20, 0, true);
    LR_CHECK_EQ(ledger.snapshot()[0].tokens_today, 30);
    auto second = ledger.admit(c);
    LR_CHECK(second.has_value());
    (*second)->finish(true, 0, 0, 0, false);
    LR_CHECK_EQ(ledger.snapshot()[0].tokens_today, 90);
    LR_CHECK_EQ(ledger.snapshot()[0].reserved_tokens, 0);
    LR_CHECK(!ledger.admit(c));
    auto partial = c; partial.id = "partial";
    auto interrupted = ledger.admit(partial);
    LR_CHECK(interrupted.has_value());
    (*interrupted)->finish(false, 80, 0, 0, false);
    LR_CHECK_EQ(ledger.snapshot()[1].tokens_today, 80);
    LR_CHECK_EQ(ledger.snapshot()[1].reserved_tokens, 0);
    LR_CHECK(!ledger.admit(partial));
    auto crash = c; crash.id = "crash";
    auto live = ledger.admit(crash);
    // A separate snapshot models a process dying with an outstanding request;
    // two live ledgers must never write the same path.
    std::filesystem::copy_file(dir.path / "quota.json", dir.path / "crash.json");
    ClientLedger restart;
    LR_CHECK(restart.open(dir.path / "crash.json").has_value());
    LR_CHECK_EQ(restart.snapshot()[1].active_requests, 0);
    LR_CHECK_EQ(restart.snapshot()[1].tokens_today, 60);
    LR_CHECK(!restart.admit(crash));
    std::ofstream(dir.path / "bad.json") << "broken";
    ClientLedger bad;
    LR_CHECK(!bad.open(dir.path / "bad.json"));
    LR_CHECK_EQ(bad.admit(c).error().status, 503);
    std::ofstream(dir.path / "file") << "not a directory";
    ClientLedger blocked;
    LR_CHECK(!blocked.open(dir.path / "file" / "quota.json"));
    LR_CHECK_EQ(blocked.admit(c).error().status, 503);
}

void corruptedStateAndUsage() {
    LR_GROUP("structurally corrupt quota state fails closed and retry intervals stay representable");
    Scratch dir;
    ClientLedger ledger;
    const auto path = dir.path / "quota.json";
    LR_CHECK(ledger.open(path).has_value());
    auto c = account(); c.requests_per_minute = 1; c.tokens_per_day = 10; c.token_reservation = 10;
    auto ticket = ledger.admit(c);
    LR_CHECK(ticket.has_value());
    (*ticket)->finish(true, 0, 0, 0, true);
    ledger.close();
    std::ifstream input(path);
    const auto valid = json::parse(input); input.close();
    const std::vector<std::function<void(json&)>> corruptions = {
        [](json &j) { j["clients"][0]["recent"] = nullptr; },
        [](json &j) { j["clients"][0]["recent"] = json::object(); },
        [](json &j) { j["clients"][0].erase("reserved_tokens"); },
        [](json &j) { j["clients"][0]["reserved_tokens"] = 100; },
        [](json &j) { j["clients"][0]["day_unix"] = 1e100; },
        [](json &j) { j["clients"][0]["day_unix"] = 1; },
        [](json &j) { j["clients"][0]["recent"] = json::array({1e100}); },
        [](json &j) { j["clients"][0]["successes"] = 2; },
        [](json &j) { j["clients"][0]["requests_today"] = 2; },
    };
    for (const auto &corrupt : corruptions) {
        auto doc = valid; corrupt(doc);
        std::ofstream(path) << doc.dump();
        ClientLedger bad;
        LR_CHECK(!bad.open(path));
        LR_CHECK_EQ(bad.admit(c).error().status, 503);
        LR_CHECK(bad.snapshot().empty());
    }
    auto future = valid;
    future["clients"][0]["recent"] = json::array({253402300799.0});
    std::ofstream(path) << future.dump();
    ClientLedger clock_rollback;
    LR_CHECK(clock_rollback.open(path).has_value());
    LR_CHECK_EQ(clock_rollback.admit(c).error().retry_after_sec, std::numeric_limits<int>::max());
    std::filesystem::remove(path);
    LR_CHECK(!clock_rollback.admit(c)); // deleting storage cannot reset a live ledger
    c.requests_per_minute = 0;
    auto after_clock_step = clock_rollback.admit(c);
    LR_CHECK(after_clock_step.has_value());
    (*after_clock_step)->finish(true, 0, 0, 0, true);
    clock_rollback.close();
    ClientLedger clock_restart;
    LR_CHECK(clock_restart.open(path).has_value());

    LR_GROUP("zero usage is reported while malformed numeric usage stays unknown");
    LR_CHECK(tokenUsageReported(R"({"usage":{"input_tokens":0,"output_tokens":0}})"));
    LR_CHECK(!tokenUsageReported(R"({"usage":{"input_tokens":-1,"output_tokens":1e100}})"));
    LR_CHECK(!tokenUsageReported("{}"));
    LR_CHECK(!tokenUsageReported(R"({"input_tokens":10})"));
    LR_CHECK(!tokenUsageReported(R"({"response":{"usage":{"input_tokens":10}}})"));
    LR_CHECK(!tokenUsageReported(R"({"message":{"usage":{"output_tokens":10}}})"));
    LR_CHECK(!tokenUsageReported(R"([[{"usage":{"input_tokens":10}}]])"));
    LR_CHECK(!tokenUsageReported(R"({"usage":{"total_tokens":10}})"));
    LR_CHECK(!tokenUsageReported(R"({"usage":{"input_tokens":-0}})"));
    LR_CHECK(!tokenUsageReported(R"({"usage":{"input_tokens":1.5,"output_tokens":"2"}})"));
    LR_CHECK(!tokenUsageReported(R"({"usageMetadata":{"promptTokenCount":1.0}})"));
    LR_CHECK(!tokenUsageReported(R"({"usage":{"input_tokens":18446744073709551616}})"));
    LR_CHECK(!tokenUsageReported("not json"));
    const std::vector<std::pair<std::string, std::pair<std::uint64_t, std::uint64_t>>> buffered_cases{
        {R"({"usage":{"prompt_tokens":7,"completion_tokens":3}})", {7, 3}},
        {R"({"usage":{"input_tokens":2.0,"output_tokens":0.0}})", {2, 0}},
        {R"({"usageMetadata":{"promptTokenCount":0,"candidatesTokenCount":0}})", {0, 0}},
        {R"([null,{"usage":{"input_tokens":2}},{"usageMetadata":{"candidatesTokenCount":3}}])", {2, 3}},
    };
    for (const auto &item : buffered_cases) {
        LR_CHECK(tokenUsageReported(item.first));
        ProviderStat counted;
        ProxyServer::accumulateUsage(item.first, counted);
        LR_CHECK_EQ(counted.tokens_prompt, item.second.first);
        LR_CHECK_EQ(counted.tokens_completion, item.second.second);
    }
    StreamUsageObserver observer;
    observer.feed("data: {\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0}}\n\n");
    LR_CHECK(observer.usage().reported);
    ProviderStat usage;
    ProxyServer::accumulateUsage(R"({"usage":{"input_tokens":-1,"output_tokens":1e100}})", usage);
    LR_CHECK_EQ(usage.tokens_prompt, 0);
    LR_CHECK_EQ(usage.tokens_completion, 0);
    ProxyServer::accumulateUsage(R"({"usage":{"input_tokens":18446744073709551615,"prompt_tokens":1}})", usage);
    LR_CHECK(usage.tokens_prompt == std::numeric_limits<std::uint64_t>::max());
}

void rateAndThreads() {
    LR_GROUP("rolling rate survives restart and simultaneous requests share a limit");
    Scratch dir;
    ClientLedger ledger;
    LR_CHECK(ledger.open(dir.path / "rate.json").has_value());
    auto c = account(); c.requests_per_minute = 1;
    auto first = ledger.admit(c);
    LR_CHECK(first.has_value()); (*first)->finish(true, 0, 0, 0, true);
    ledger.close();
    ClientLedger restart;
    LR_CHECK(restart.open(dir.path / "rate.json").has_value());
    auto rejected = restart.admit(c);
    LR_CHECK(!rejected); LR_CHECK(rejected.error().retry_after_sec > 0);
    auto parallel = c; parallel.id = "parallel"; parallel.requests_per_minute = 0;
    parallel.max_concurrent = 2;
    std::atomic<int> admitted{0};
    std::vector<std::shared_ptr<ClientRequest>> tickets;
    std::mutex mutex;
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) threads.emplace_back([&] {
        auto ticket = restart.admit(parallel);
        if (ticket) { ++admitted; std::scoped_lock lock{mutex}; tickets.push_back(*ticket); }
    });
    for (auto &thread : threads) thread.join();
    LR_CHECK_EQ(admitted.load(), 2);
    tickets.clear();
    LR_CHECK(restart.admit(parallel).has_value());
}

void ownershipAndIdentity() {
    LR_GROUP("quota identity survives port changes and aliases of the config path");
    Scratch dir;
    lr_test::EnvGuard state("LITEROUTER_STATE_DIR"); state.assign(dir.path.string());
    const auto path = defaultClientQuotaPath(dir.path / "config.json", 1234);
    LR_CHECK(path == defaultClientQuotaPath(dir.path / "config.json", 5678));
    LR_CHECK(path == defaultClientQuotaPath(dir.path / "unused" / ".." / "config.json", 0));
    LR_CHECK(path != defaultClientQuotaPath(dir.path / "other.json", 1234));
    LR_CHECK_EQ(path.filename().string().size(), std::string("clients-config-.json").size() + 64);
    LR_CHECK(defaultClientQuotaPath({}, 1234) == dir.path / "clients-1234.json");

    LR_GROUP("exclusive ownership is lazy and spans outstanding request completion");
    auto c = account(); c.requests_per_day = 1;
    ClientLedger first, second, third;
    LR_CHECK(first.open(path).has_value());
    LR_CHECK(second.open(path).has_value());
    LR_CHECK(!std::filesystem::exists(path));
    LR_CHECK(!std::filesystem::exists(path.string() + ".lock"));
    auto request = first.admit(c);
    LR_CHECK(request.has_value());
    LR_CHECK_EQ(second.admit(c).error().status, 503);
    LR_CHECK(!third.open(path, true));
    first.close();
    LR_CHECK_EQ(first.admit(c).error().status, 503);
    LR_CHECK_EQ(second.admit(c).error().status, 503);
    LR_CHECK(!first.open(path));
    (*request)->finish(true, 2, 3, 0, true);
    LR_CHECK_EQ(first.snapshot()[0].tokens_prompt, 2);
    // The lazy second owner must load the first owner's new usage under its
    // lock; its old empty in-memory view cannot grant another request.
    LR_CHECK_EQ(second.admit(c).error().status, 429);
    LR_CHECK(!third.open(path, true));
    second.close();
    LR_CHECK(third.open(path, true).has_value());
    LR_CHECK_EQ(third.snapshot()[0].requests, 1);
    third.close();

    LR_GROUP("destruction keeps the lock until the request releases its permit");
    const auto deferred = dir.path / "deferred.json";
    std::shared_ptr<ClientRequest> held;
    {
        ClientLedger owner;
        LR_CHECK(owner.open(deferred, true).has_value());
        LR_CHECK(!std::filesystem::exists(deferred));
        LR_CHECK(!third.open(deferred, true));
        held = *owner.admit(c);
    }
    LR_CHECK(!third.open(deferred, true));
    held.reset();
    LR_CHECK(third.open(deferred, true).has_value());
    LR_CHECK_EQ(third.snapshot()[0].failures, 1);
}
} // namespace

int main() {
    authentication(); configAndPolicies(); limitsAndPersistence(); tokensAndFailure(); rateAndThreads(); corruptedStateAndUsage(); ownershipAndIdentity();
    return LR_SUMMARY("test_clients");
}
