// The JSON codecs used by the admin API and the console (lr_json.cpp) plus the
// usage-block parser from lr_proxy.cpp. Everything produced here is parsed back
// with nlohmann, which is the same library core serialises with.
#include "lr_test_check.h"

import nlohmann.json;
import literouter.core;

namespace {

using nlohmann::json;

void testSnapshotJson() {
    LR_GROUP("toJsonString(Snapshot)");
    literouter::Snapshot snapshot;
    snapshot.running = true;
    snapshot.host = "127.0.0.1";
    snapshot.port = 43210;
    snapshot.base_url = "http://127.0.0.1:43210";
    snapshot.started_unix = 1700000000.5;
    snapshot.uptime_sec = 12.25;
    literouter::TrafficBucket hour;
    hour.hour_unix = 1'700'000'000.0;
    hour.requests = 7;
    hour.successes = 6;
    hour.failures = 1;
    hour.bytes_out = 4096;
    hour.tokens_prompt = 100;
    hour.tokens_completion = 40;
    hour.cost_usd = 0.5;
    snapshot.hourly = {hour};
    snapshot.config_path = "/tmp/literouter/config.json";
    snapshot.version = "0.2.0";
    snapshot.total_requests = 17;
    snapshot.total_success = 15;
    snapshot.total_failure = 2;
    snapshot.active_requests = 1;
    snapshot.bytes_out = 4096;
    snapshot.tokens_prompt = 321;
    snapshot.tokens_completion = 111;
    snapshot.latency_ms_avg = 123.5;
    snapshot.log_seq = 42;
    snapshot.breakers_open = 1;

    literouter::ProviderStat stat;
    stat.provider = "alpha";
    stat.requests = 9;
    stat.successes = 8;
    stat.failures = 1;
    stat.aborted = 4;
    stat.retries_in = 2;
    stat.bytes_out = 2048;
    stat.bytes_in = 512;
    stat.tokens_prompt = 100;
    stat.tokens_completion = 50;
    stat.latency_ms_last = 12.5;
    stat.latency_ms_avg = 20.0;
    stat.latency_ms_p95 = 40.0;
    stat.last_used_unix = 1700000010.0;
    snapshot.providers.push_back(stat);

    literouter::ProviderHealth health;
    health.provider = "alpha";
    health.state = literouter::ProviderHealth::State::Degraded;
    health.consecutive_failures = 1;
    health.total_failures = 3;
    health.last_error = "connection failed";
    health.cooldown_remaining = 25.5;
    snapshot.health.push_back(health);

    const std::string text = literouter::toJsonString(snapshot);
    const json parsed = json::parse(text, nullptr, false);
    LR_CHECK_MSG(!parsed.is_discarded(), "snapshot JSON did not parse");
    if (parsed.is_discarded()) {
        return;
    }
    LR_CHECK(parsed.is_object());

    LR_CHECK_EQ(parsed.at("running").get<bool>(), true);
    LR_CHECK_EQ(parsed.at("host").get<std::string>(), "127.0.0.1");
    LR_CHECK_EQ(parsed.at("port").get<int>(), 43210);
    LR_CHECK_EQ(parsed.at("base_url").get<std::string>(), "http://127.0.0.1:43210");
    LR_CHECK_EQ(parsed.at("version").get<std::string>(), "0.2.0");
    LR_CHECK_EQ(parsed.at("config_path").get<std::string>(), "/tmp/literouter/config.json");
    LR_CHECK(lr_test::closeTo(parsed.at("started_unix").get<double>(), 1700000000.5));
    LR_CHECK(lr_test::closeTo(parsed.at("uptime_sec").get<double>(), 12.25));
    // Guarded before every access: `operator[]` on a const json asserts on a
    // missing key, so a serialiser that forgot the array would crash the suite
    // instead of failing it.
    if (parsed.contains("hourly") && parsed.at("hourly").is_array()) {
        const json &hours = parsed.at("hourly");
        LR_CHECK_EQ(static_cast<long long>(hours.size()), 1);
        if (!hours.empty()) {
            const json &bucket = hours.at(0);
            LR_CHECK_EQ(bucket.at("requests").get<std::uint64_t>(), static_cast<std::uint64_t>(7));
            LR_CHECK_EQ(bucket.at("successes").get<std::uint64_t>(),
                        static_cast<std::uint64_t>(6));
            LR_CHECK_EQ(bucket.at("failures").get<std::uint64_t>(),
                        static_cast<std::uint64_t>(1));
            LR_CHECK_EQ(bucket.at("bytes_out").get<std::uint64_t>(),
                        static_cast<std::uint64_t>(4096));
            LR_CHECK_EQ(bucket.at("tokens_prompt").get<std::uint64_t>(),
                        static_cast<std::uint64_t>(100));
            LR_CHECK_EQ(bucket.at("tokens_completion").get<std::uint64_t>(),
                        static_cast<std::uint64_t>(40));
            LR_CHECK(lr_test::closeTo(bucket.at("cost_usd").get<double>(), 0.5));
            LR_CHECK(lr_test::closeTo(bucket.at("hour_unix").get<double>(), 1'700'000'000.0));
        }
    } else {
        LR_CHECK_MSG(false, "the snapshot JSON has no hourly array");
    }

    LR_CHECK_EQ(parsed.at("total_requests").get<std::uint64_t>(),
                static_cast<std::uint64_t>(17));
    LR_CHECK_EQ(parsed.at("total_success").get<std::uint64_t>(),
                static_cast<std::uint64_t>(15));
    LR_CHECK_EQ(parsed.at("total_failure").get<std::uint64_t>(), static_cast<std::uint64_t>(2));
    LR_CHECK_EQ(parsed.at("active_requests").get<std::uint64_t>(),
                static_cast<std::uint64_t>(1));
    LR_CHECK_EQ(parsed.at("bytes_out").get<std::uint64_t>(), static_cast<std::uint64_t>(4096));
    LR_CHECK_EQ(parsed.at("tokens_prompt").get<std::uint64_t>(),
                static_cast<std::uint64_t>(321));
    LR_CHECK_EQ(parsed.at("tokens_completion").get<std::uint64_t>(),
                static_cast<std::uint64_t>(111));
    LR_CHECK(lr_test::closeTo(parsed.at("latency_ms_avg").get<double>(), 123.5));
    LR_CHECK_EQ(parsed.at("log_seq").get<std::uint64_t>(), static_cast<std::uint64_t>(42));
    LR_CHECK_EQ(parsed.at("breakers_open").get<int>(), 1);

    LR_CHECK(parsed.at("providers").is_array());
    LR_CHECK_EQ(static_cast<long long>(parsed.at("providers").size()), 1);
    const json &provider = parsed.at("providers").at(0);
    LR_CHECK_EQ(provider.at("provider").get<std::string>(), "alpha");
    LR_CHECK_EQ(provider.at("requests").get<std::uint64_t>(), static_cast<std::uint64_t>(9));
    LR_CHECK_EQ(provider.at("successes").get<std::uint64_t>(), static_cast<std::uint64_t>(8));
    LR_CHECK_EQ(provider.at("failures").get<std::uint64_t>(), static_cast<std::uint64_t>(1));
    LR_CHECK_EQ(provider.at("aborted").get<std::uint64_t>(), static_cast<std::uint64_t>(4));
    LR_CHECK_EQ(provider.at("retries_in").get<std::uint64_t>(), static_cast<std::uint64_t>(2));
    LR_CHECK_EQ(provider.at("bytes_in").get<std::uint64_t>(), static_cast<std::uint64_t>(512));
    LR_CHECK_EQ(provider.at("tokens_prompt").get<std::uint64_t>(),
                static_cast<std::uint64_t>(100));
    LR_CHECK_EQ(provider.at("tokens_completion").get<std::uint64_t>(),
                static_cast<std::uint64_t>(50));
    LR_CHECK(lr_test::closeTo(provider.at("latency_ms_last").get<double>(), 12.5));
    LR_CHECK(lr_test::closeTo(provider.at("latency_ms_p95").get<double>(), 40.0));
    LR_CHECK(lr_test::closeTo(provider.at("last_used_unix").get<double>(), 1700000010.0));

    LR_CHECK(parsed.at("health").is_array());
    LR_CHECK_EQ(static_cast<long long>(parsed.at("health").size()), 1);
    const json &row = parsed.at("health").at(0);
    LR_CHECK_EQ(row.at("provider").get<std::string>(), "alpha");
    LR_CHECK_EQ(row.at("state").get<std::string>(), "degraded");
    LR_CHECK_EQ(row.at("consecutive_failures").get<int>(), 1);
    LR_CHECK_EQ(row.at("total_failures").get<std::uint64_t>(), static_cast<std::uint64_t>(3));
    LR_CHECK_EQ(row.at("last_error").get<std::string>(), "connection failed");
    LR_CHECK(lr_test::closeTo(row.at("cooldown_remaining").get<double>(), 25.5));

    // The two arrays keep their parallel order, which is what the console
    // zips them by.
    literouter::Snapshot empty;
    const json emptyParsed = json::parse(literouter::toJsonString(empty), nullptr, false);
    LR_CHECK(!emptyParsed.is_discarded());
    LR_CHECK_EQ(static_cast<long long>(emptyParsed.at("providers").size()), 0);
    LR_CHECK_EQ(static_cast<long long>(emptyParsed.at("health").size()), 0);
    LR_CHECK_EQ(emptyParsed.at("running").get<bool>(), false);
}

void testLogEntryJson() {
    LR_GROUP("toJsonString(LogEntry)");
    literouter::LogEntry entry;
    entry.seq = 7;
    entry.time_unix = 1700000000.25;
    entry.level = "warn";
    entry.request_id = "abc123";
    entry.kind = "chat";
    entry.model = "fast";
    entry.provider = "alpha";
    entry.upstream_model = "gpt-4o-2024-08-06";
    entry.status = 429;
    entry.stream = true;
    entry.failover = true;
    entry.attempt = 2;
    entry.attempts_total = 3;
    entry.latency_ms = 250.5;
    entry.bytes = 1024;
    entry.message = "HTTP 429 — failing over";

    const json parsed = json::parse(literouter::toJsonString(entry), nullptr, false);
    LR_CHECK_MSG(!parsed.is_discarded(), "log entry JSON did not parse");
    if (parsed.is_discarded()) {
        return;
    }
    LR_CHECK_EQ(parsed.at("seq").get<std::uint64_t>(), static_cast<std::uint64_t>(7));
    LR_CHECK(lr_test::closeTo(parsed.at("time_unix").get<double>(), 1700000000.25));
    LR_CHECK_EQ(parsed.at("level").get<std::string>(), "warn");
    LR_CHECK_EQ(parsed.at("request_id").get<std::string>(), "abc123");
    LR_CHECK_EQ(parsed.at("kind").get<std::string>(), "chat");
    LR_CHECK_EQ(parsed.at("model").get<std::string>(), "fast");
    LR_CHECK_EQ(parsed.at("provider").get<std::string>(), "alpha");
    LR_CHECK_EQ(parsed.at("upstream_model").get<std::string>(), "gpt-4o-2024-08-06");
    LR_CHECK_EQ(parsed.at("status").get<int>(), 429);
    LR_CHECK_EQ(parsed.at("stream").get<bool>(), true);
    LR_CHECK_EQ(parsed.at("failover").get<bool>(), true);
    LR_CHECK_EQ(parsed.at("attempt").get<int>(), 2);
    LR_CHECK_EQ(parsed.at("attempts_total").get<int>(), 3);
    LR_CHECK(lr_test::closeTo(parsed.at("latency_ms").get<double>(), 250.5));
    LR_CHECK_EQ(parsed.at("bytes").get<std::uint64_t>(), static_cast<std::uint64_t>(1024));
    LR_CHECK_EQ(parsed.at("message").get<std::string>(), "HTTP 429 — failing over");

    // Bodies are omitted unless the server was told to keep them, so a log line
    // cannot leak a prompt by accident.
    LR_CHECK(!parsed.contains("request_body"));
    LR_CHECK(!parsed.contains("response_body"));
    // `time` is the display form the console prints: HH:MM:SS.mmm.
    const std::string time = parsed.at("time").get<std::string>();
    LR_CHECK_EQ(time.size(), std::size_t{12});
    LR_CHECK(time[2] == ':' && time[5] == ':' && time[8] == '.');

    const std::string date = parsed.at("date").get<std::string>();
    LR_CHECK_EQ(date.size(), std::size_t{10});
    LR_CHECK(date[4] == '-' && date[7] == '-');

    const std::string datetime = parsed.at("datetime").get<std::string>();
    LR_CHECK_EQ(datetime.size(), std::size_t{23});
    LR_CHECK(datetime[4] == '-' && datetime[7] == '-' && datetime[10] == ' ' &&
             datetime[13] == ':' && datetime[16] == ':' && datetime[19] == '.');

    LR_CHECK_EQ(entry.shortDateTimeText().size(), std::size_t{14});
    LR_CHECK(entry.shortDateTimeText()[2] == '-' && entry.shortDateTimeText()[5] == ' ' &&
             entry.shortDateTimeText()[8] == ':' && entry.shortDateTimeText()[11] == ':');

    entry.request_body = "{\"model\":\"fast\"}";
    entry.response_body = "{\"ok\":true}";
    const json withBodies = json::parse(literouter::toJsonString(entry), nullptr, false);
    LR_CHECK(!withBodies.is_discarded());
    LR_CHECK_EQ(withBodies.at("request_body").get<std::string>(), "{\"model\":\"fast\"}");
    LR_CHECK_EQ(withBodies.at("response_body").get<std::string>(), "{\"ok\":true}");

    // timeText() is what a person reads, so a zero timestamp must still render
    // in the fixed shape.
    literouter::LogEntry zero;
    LR_CHECK_EQ(zero.timeText().size(), std::size_t{12});
    LR_CHECK_EQ(zero.dateText().size(), std::size_t{10});
    LR_CHECK_EQ(zero.dateTimeText().size(), std::size_t{23});
    LR_CHECK_EQ(zero.shortDateTimeText().size(), std::size_t{14});
}

void testProviderProbeJson() {
    LR_GROUP("toJsonString(ProviderProbe)");
    literouter::ProviderProbe probe;
    probe.reachable = true;
    probe.status = 200;
    probe.latency_ms = 88.25;
    probe.detail = "3 models advertised";
    probe.models = {"gpt-4o", "gpt-4o-mini", "z-model"};

    const json parsed = json::parse(literouter::toJsonString(probe), nullptr, false);
    LR_CHECK_MSG(!parsed.is_discarded(), "probe JSON did not parse");
    if (parsed.is_discarded()) {
        return;
    }
    LR_CHECK_EQ(parsed.at("reachable").get<bool>(), true);
    LR_CHECK_EQ(parsed.at("status").get<int>(), 200);
    LR_CHECK(lr_test::closeTo(parsed.at("latency_ms").get<double>(), 88.25));
    LR_CHECK_EQ(parsed.at("detail").get<std::string>(), "3 models advertised");
    LR_CHECK(parsed.at("models").is_array());
    LR_CHECK_EQ(static_cast<long long>(parsed.at("models").size()), 3);
    LR_CHECK_EQ(parsed.at("models").at(2).get<std::string>(), "z-model");

    // A failed probe still serialises: the console renders the detail line.
    literouter::ProviderProbe failed;
    failed.detail = "connection refused";
    const json failedParsed = json::parse(literouter::toJsonString(failed), nullptr, false);
    LR_CHECK(!failedParsed.is_discarded());
    LR_CHECK_EQ(failedParsed.at("reachable").get<bool>(), false);
    LR_CHECK_EQ(failedParsed.at("status").get<int>(), 0);
    LR_CHECK_EQ(static_cast<long long>(failedParsed.at("models").size()), 0);
}

// The regression the UTF-8 pass exists for. A `config_path` that is not valid
// UTF-8 — what a Windows `path::string()` yields under a DBCS code page — used
// to reach nlohmann's strict `dump()`, which throws `type_error.316`. On the
// serve path that throw came from the telemetry thread and from `writePidFile`
// with no handler above either, so the process aborted. These checks are the
// same shape as the crash: build the struct with the bad bytes, serialize it,
// and require that it comes back rather than throws.
void testSerializationOfInvalidUtf8() {
    LR_GROUP("toJsonString survives a value that is not valid UTF-8");

    // "你好" as cp936/GBK bytes — what a Chinese Windows user name becomes.
    const std::string gbk_path = std::string{"/tmp/"} + "\xC4\xE3\xBA\xC3" + "/config.json";
    LR_CHECK(!literouter::isValidUtf8(gbk_path));

    literouter::Snapshot snapshot;
    snapshot.running = true;
    snapshot.config_path = gbk_path;

    // The property under test: this call does not throw.
    const std::string text = literouter::toJsonString(snapshot);
    const json parsed = json::parse(text, nullptr, false);
    LR_CHECK_MSG(!parsed.is_discarded(), "the snapshot JSON did not parse");
    if (!parsed.is_discarded()) {
        // The field is present and holds the repaired text, so the caller still
        // learns which file the instance is running from.
        const std::string path = parsed.at("config_path").get<std::string>();
        LR_CHECK(literouter::isValidUtf8(path));
        LR_CHECK(path.find("config.json") != std::string::npos);
        LR_CHECK(path.find('\xEF') != std::string::npos);
    }

    // The second crash site: a logged upstream body with a stray byte. The log
    // entry is serialized by the same chokepoint, so it gets the same
    // guarantee — this is the shape that killed the flusher thread.
    literouter::LogEntry entry;
    entry.seq = 1;
    entry.level = "info";
    entry.message = std::string{"upstream said caf"} + "\xE9";
    entry.response_body = std::string{"{\"content\":\"caf"} + "\xE9" + "\"}";
    const std::string entryText = literouter::toJsonString(entry);
    const json entryParsed = json::parse(entryText, nullptr, false);
    LR_CHECK_MSG(!entryParsed.is_discarded(), "the log entry JSON did not parse");
    if (!entryParsed.is_discarded()) {
        LR_CHECK(literouter::isValidUtf8(entryParsed.at("message").get<std::string>()));
        LR_CHECK(literouter::isValidUtf8(entryParsed.at("response_body").get<std::string>()));
    }

    // A ProviderProbe carries an upstream error string, which is relay-
    // controlled text — the third of the chokepoints.
    literouter::ProviderProbe probe;
    probe.detail = std::string{"HTTP 502: caf"} + "\xE9";
    probe.models = {std::string{"z-"} + "\xE9"};
    const json probeParsed = json::parse(literouter::toJsonString(probe), nullptr, false);
    LR_CHECK_MSG(!probeParsed.is_discarded(), "the probe JSON did not parse");
    if (!probeParsed.is_discarded()) {
        LR_CHECK(literouter::isValidUtf8(probeParsed.at("detail").get<std::string>()));
        LR_CHECK(literouter::isValidUtf8(probeParsed.at("models").at(0).get<std::string>()));
    }

    // And the config document, whose path field is the fourth.
    literouter::AppConfig config;
    config.server.host = std::string{"127.0.0.1\xE9"};
    const json configParsed = json::parse(literouter::toJsonString(config), nullptr, false);
    LR_CHECK_MSG(!configParsed.is_discarded(), "the config JSON did not parse");
}

void testAccumulateUsage() {
    LR_GROUP("accumulateUsage");
    const auto promptOf = [](const literouter::ProviderStat &stat) {
        return static_cast<long long>(stat.tokens_prompt);
    };
    const auto completionOf = [](const literouter::ProviderStat &stat) {
        return static_cast<long long>(stat.tokens_completion);
    };

    {
        literouter::ProviderStat stat;
        literouter::ProxyServer::accumulateUsage(
            R"({"id":"cmpl-1","object":"chat.completion","usage":{"prompt_tokens":11,"completion_tokens":7,"total_tokens":18}})",
            stat);
        LR_CHECK_EQ(promptOf(stat), 11);
        LR_CHECK_EQ(completionOf(stat), 7);
    }
    {
        // The Messages-shaped relays spell the same counters differently.
        literouter::ProviderStat stat;
        literouter::ProxyServer::accumulateUsage(
            R"({"usage":{"input_tokens":3,"output_tokens":9}})", stat);
        LR_CHECK_EQ(promptOf(stat), 3);
        LR_CHECK_EQ(completionOf(stat), 9);
    }
    {
        // A relay that reports both spellings in one block contributes both.
        literouter::ProviderStat stat;
        literouter::ProxyServer::accumulateUsage(
            R"({"usage":{"prompt_tokens":5,"input_tokens":2,"completion_tokens":4,"output_tokens":1}})",
            stat);
        LR_CHECK_EQ(promptOf(stat), 7);
        LR_CHECK_EQ(completionOf(stat), 5);
    }
    {
        // Calls accumulate: one stat covers a relay's whole lifetime.
        literouter::ProviderStat stat;
        literouter::ProxyServer::accumulateUsage(R"({"usage":{"prompt_tokens":1}})", stat);
        literouter::ProxyServer::accumulateUsage(R"({"usage":{"prompt_tokens":2}})", stat);
        LR_CHECK_EQ(promptOf(stat), 3);
    }
    {
        // No usage block, a non-object usage, non-numeric counters, plain
        // garbage and an empty body all contribute nothing rather than
        // poisoning the counter.
        literouter::ProviderStat stat;
        literouter::ProxyServer::accumulateUsage(R"({"id":"x","choices":[]})", stat);
        literouter::ProviderStat nonObject;
        literouter::ProxyServer::accumulateUsage(R"({"usage":5})", nonObject);
        literouter::ProviderStat wrongType;
        literouter::ProxyServer::accumulateUsage(R"({"usage":{"prompt_tokens":"11"}})", wrongType);
        literouter::ProviderStat garbage;
        literouter::ProxyServer::accumulateUsage("not json at all", garbage);
        literouter::ProviderStat empty;
        literouter::ProxyServer::accumulateUsage("", empty);
        LR_CHECK_EQ(promptOf(stat) + promptOf(nonObject) + promptOf(wrongType) + promptOf(garbage) +
                        promptOf(empty),
                    0);
        LR_CHECK_EQ(completionOf(garbage) + completionOf(empty), 0);
    }
    {
        // A chunked answer handed over as an array of chunk objects: the usage
        // block lives on the last chunk and is still found.
        literouter::ProviderStat stat;
        literouter::ProxyServer::accumulateUsage(
            R"([{"choices":[{"delta":{"content":"Hel"}}]},{"choices":[{"delta":{}}],"usage":{"prompt_tokens":6,"completion_tokens":2}}])",
            stat);
        LR_CHECK_EQ(promptOf(stat), 6);
        LR_CHECK_EQ(completionOf(stat), 2);
    }
    {
        // Best-effort contract: a raw `data:` transcript is not one JSON
        // document, so it contributes nothing. The streaming path never calls
        // this function — it relays bytes and records zero tokens — which is
        // what keeps this from being a hole in the counters.
        literouter::ProviderStat stat;
        literouter::ProxyServer::accumulateUsage(
            "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
            "data: {\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":4}}\n\n"
            "data: [DONE]\n\n",
            stat);
        LR_CHECK_EQ(promptOf(stat), 0);
        LR_CHECK_EQ(completionOf(stat), 0);
    }
}

void testSeedConfigJsonKeepsSecretReferences() {
    LR_GROUP("ConfigStore::toJson never expands a secret");
    const lr_test::EnvGuard guard{"OPENAI_API_KEY"};
    guard.assign("sk-expanded-should-not-appear");

    const literouter::ConfigStore store;
    const std::string text = store.toJson();
    LR_CHECK(text.find("${OPENAI_API_KEY}") != std::string::npos);
    LR_CHECK(text.find("${RELAY_API_KEY}") != std::string::npos);
    LR_CHECK_MSG(text.find("sk-expanded-should-not-appear") == std::string::npos,
                 "toJson expanded an environment reference into the file body");

    // Still parseable, and still a reference when read back.
    auto parsed = literouter::appConfigFromJson(text);
    LR_CHECK(parsed.has_value());
    if (parsed) {
        LR_CHECK_EQ(static_cast<long long>(parsed->providers.size()), 2);
        LR_CHECK_EQ(parsed->providers[0].api_key, "${OPENAI_API_KEY}");
        LR_CHECK(literouter::isSecretReference(parsed->providers[0].api_key));
    }
}

void testEnsureLocalTimezone() {
    LR_GROUP("ensureLocalTimezone");
    literouter::ensureLocalTimezone();
#ifndef _WIN32
    if (std::filesystem::exists("/etc/localtime")) {
        const char *tz = std::getenv("TZ");
        LR_CHECK(tz != nullptr);
    }
#endif
    literouter::LogEntry entry;
    entry.time_unix = 1700000000.0;
    LR_CHECK(!entry.dateText().empty());
    LR_CHECK(!entry.timeText().empty());
}

} // namespace

int main() {
    testSnapshotJson();
    testLogEntryJson();
    testProviderProbeJson();
    testSerializationOfInvalidUtf8();
    testAccumulateUsage();
    testSeedConfigJsonKeepsSecretReferences();
    testEnsureLocalTimezone();
    return LR_SUMMARY("test_json_api");
}
