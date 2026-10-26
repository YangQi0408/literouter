#include <httplib.h>
#include "lr_test_check.h"

import literouter.core;
import nlohmann.json;

int main() {
    using json = nlohmann::json;
    lr_test::ProxyEnvironment environment;
    const lr_test::EnvGuard stateEnv{"LITEROUTER_STATE_DIR"};
    const auto directory = std::filesystem::temp_directory_path() /
                           ("literouter-management-" + literouter::hexId(6));
    std::filesystem::create_directories(directory);
    stateEnv.assign(literouter::pathToUtf8(directory));
    std::atomic<int> calls{0};
    std::string probePath, probeHeader;
    std::mutex probeMutex;
    httplib::Server relay;
    relay.Post("/chat/completions", [&](const auto &, auto &reply) {
        ++calls;
        reply.set_content(R"({"choices":[],"usage":{"prompt_tokens":5,"completion_tokens":3}})",
                          "application/json");
    });
    relay.Get("/v1beta/models", [&](const auto &request, auto &reply) {
        std::scoped_lock lock{probeMutex};
        probePath = request.path;
        probeHeader = request.get_header_value("x-goog-api-key");
        reply.set_content(R"({"models":[{"name":"models/gemini-fixture"}]})", "application/json");
    });
    const int upstreamPort = relay.bind_to_any_port("127.0.0.1");
    LR_CHECK_MSG(upstreamPort > 0, "relay did not bind");
    std::thread worker([&] { relay.listen_after_bind(); });
    relay.wait_until_ready();
    {
        literouter::AppConfig config;
        config.server.port = 0;
        config.server.persist_telemetry = false;
        config.server.response_cache_ttl_sec = 60;
        config.server.traffic_bucket_sec = 60;
        config.server.traffic_bucket_count = 10;
        config.providers = {{.id = "priced", .base_url = std::format("http://127.0.0.1:{}", upstreamPort),
                             .price_in_per_million = 2, .price_out_per_million = 3}};
        config.routes = {{.model = "m", .targets = {{.provider = "priced"}}}};
        literouter::ProxyServer proxy;
        const auto started = proxy.start(config);
        LR_CHECK_MSG(started.has_value(), started ? "" : started.error());
        if (started) {
            httplib::Client client{"127.0.0.1", proxy.boundPort()};
            client.set_read_timeout(5);
            LR_GROUP("the admin reader preserves every cache, usage, price and trend field");
            for (int i = 0; i < 2; ++i) {
                const auto response = client.Post("/v1/chat/completions", R"({"model":"m","messages":[]})",
                                                   "application/json");
                LR_CHECK_MSG(response && response->status == 200, response ? response->body : "no answer");
            }
            const auto direct = proxy.snapshot();
            const auto remote = literouter::fetchStatus(direct.base_url);
            LR_CHECK_MSG(remote.reachable, remote.error);
            if (remote.reachable) {
                LR_CHECK_MSG(remote.snapshot.cache_enabled && remote.snapshot.cache_hits == 1 &&
                             remote.snapshot.cache_misses == 1 && remote.snapshot.cache_entries == 1,
                             "cache state was replaced by reader defaults");
                // Uptime changes during the HTTP round trip; everything else
                // must round-trip the exact serializer fields, not a subset.
                auto actual = json::parse(literouter::toJsonString(remote.snapshot));
                auto expected = json::parse(literouter::toJsonString(direct));
                actual.erase("uptime_sec");
                expected.erase("uptime_sec");
                LR_CHECK_MSG(actual == expected, "admin snapshot did not round-trip: " + actual.dump());
            }
            LR_CHECK_MSG(calls.load() == 1, "cached request was unnecessarily forwarded");
            LR_GROUP("inline probes honor the requested protocol and authentication");
            const json probe{{"protocol", "gemini"}, {"api_key", "fake-gemini-key"},
                             {"base_url", config.providers.front().base_url}};
            const auto reply = client.Post("/__literouter/probe", probe.dump(), "application/json");
            LR_CHECK_MSG(reply && reply->status == 200, reply ? reply->body : "probe failed");
            {
                std::scoped_lock lock{probeMutex};
                LR_CHECK_MSG(probePath == "/v1beta/models" && probeHeader == "fake-gemini-key",
                             "inline probe used OpenAI's path or credential");
            }
            const auto invalid = client.Post("/__literouter/probe",
                R"({"base_url":"http://127.0.0.1:0","protocol":"gemini"})", "application/json");
            LR_CHECK_MSG(invalid && invalid->status == 422, "invalid inline probe was not rejected");
            proxy.stop();
        }
    }
    relay.stop();
    worker.join();
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    return LR_SUMMARY("test_management");
}
