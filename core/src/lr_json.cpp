// JSON codecs. One unit owns the shapes on the wire and on disk so the config
// file, the admin API and the console cannot drift apart — a field added here
// appears in all three at once.
//
// The reader is deliberately tolerant: unknown keys are ignored (a file written
// by a newer build still loads) and missing keys keep the struct's default (a
// hand-written minimal file still works). Only structurally wrong input —
// invalid JSON, a scalar where an object is expected — is refused.
module;

#ifndef _WIN32
#include <unistd.h>
#include <stdlib.h>
#endif
#include <time.h>

module literouter.core;

import std;
import nlohmann.json;

namespace literouter {

namespace {

using json = nlohmann::json;

const json &nullJson() {
    static const json value = json::object();
    return value;
}

// `get` with a fallback, for the three scalar types the config uses.
std::string readString(const json &node, const char *key, std::string fallback = {}) {
    if (!node.is_object()) {
        return fallback;
    }
    const auto it = node.find(key);
    if (it == node.end() || !it->is_string()) {
        return fallback;
    }
    return it->get<std::string>();
}

bool readBool(const json &node, const char *key, bool fallback) {
    if (!node.is_object()) {
        return fallback;
    }
    const auto it = node.find(key);
    if (it == node.end() || !it->is_boolean()) {
        return fallback;
    }
    return it->get<bool>();
}

int readInt(const json &node, const char *key, int fallback) {
    if (!node.is_object()) {
        return fallback;
    }
    const auto it = node.find(key);
    if (it == node.end() || !it->is_number()) {
        return fallback;
    }
    return it->get<int>();
}

double readDouble(const json &node, const char *key, double fallback) {
    if (!node.is_object()) {
        return fallback;
    }
    const auto it = node.find(key);
    if (it == node.end() || !it->is_number()) {
        return fallback;
    }
    return it->get<double>();
}

std::vector<std::string> readStringArray(const json &node, const char *key) {
    std::vector<std::string> out;
    if (!node.is_object()) {
        return out;
    }
    const auto it = node.find(key);
    if (it == node.end() || !it->is_array()) {
        return out;
    }
    for (const auto &item : *it) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

std::map<std::string, std::string> readStringMap(const json &node, const char *key) {
    std::map<std::string, std::string> out;
    if (!node.is_object()) {
        return out;
    }
    const auto it = node.find(key);
    if (it == node.end() || !it->is_object()) {
        return out;
    }
    // Iterated with explicit iterators rather than `items()` + a structured
    // binding: nlohmann's `iteration_proxy_value` gets its `tuple_size` /
    // `tuple_element` specialisations declared inside the `nlohmann.json`
    // module, and those are not visible here — so the binding would not compile
    // in this translation unit even though the same line works in a header-only
    // build of the library.
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        if (entry.value().is_string()) {
            out.emplace(entry.key(), entry.value().get<std::string>());
        }
    }
    return out;
}

ProviderConfig providerFromJson(const json &node) {
    ProviderConfig out;
    out.id = readString(node, "id");
    out.name = readString(node, "name");
    out.base_url = readString(node, "base_url");
    out.api_key = readString(node, "api_key");
    out.enabled = readBool(node, "enabled", true);
    out.priority = readInt(node, "priority", 100);
    out.weight = readInt(node, "weight", 1);
    out.timeout_sec = readInt(node, "timeout_sec", 120);
    out.connect_timeout_sec = readInt(node, "connect_timeout_sec", 15);
    out.supports_stream = readBool(node, "supports_stream", true);
    out.models = readStringArray(node, "models");
    out.groups = readStringArray(node, "groups");
    out.headers = readStringMap(node, "headers");
    out.chat_path = readString(node, "chat_path", "/chat/completions");
    out.embeddings_path = readString(node, "embeddings_path", "/embeddings");
    out.protocol = readString(node, "protocol", "openai");
    out.price_in_per_million = readDouble(node, "price_in_per_million", 0.0);
    out.price_out_per_million = readDouble(node, "price_out_per_million", 0.0);
    out.note = readString(node, "note");
    if (out.name.empty()) {
        out.name = out.id;
    }
    return out;
}

json providerToJson(const ProviderConfig &value) {
    json node = json::object();
    node["id"] = value.id;
    node["name"] = value.name;
    node["base_url"] = value.base_url;
    // Written back exactly as it was read: a "${TOKEN}" reference stays a
    // reference, which is the whole point of supporting the form.
    node["api_key"] = value.api_key;
    node["enabled"] = value.enabled;
    node["priority"] = value.priority;
    node["weight"] = value.weight;
    node["timeout_sec"] = value.timeout_sec;
    node["connect_timeout_sec"] = value.connect_timeout_sec;
    node["supports_stream"] = value.supports_stream;
    node["models"] = value.models;
    node["groups"] = value.groups;
    if (!value.headers.empty()) {
        node["headers"] = value.headers;
    }
    node["chat_path"] = value.chat_path;
    node["embeddings_path"] = value.embeddings_path;
    if (!value.protocol.empty() && value.protocol != "openai") {
        node["protocol"] = value.protocol;
    }
    // Only when written down: an absent price is "unknown", and saying 0.0 in
    // every relay's JSON would make that indistinguishable from a free one.
    if (value.price_in_per_million > 0.0) {
        node["price_in_per_million"] = value.price_in_per_million;
    }
    if (value.price_out_per_million > 0.0) {
        node["price_out_per_million"] = value.price_out_per_million;
    }
    if (!value.note.empty()) {
        node["note"] = value.note;
    }
    return node;
}

RouteConfig routeFromJson(const json &node) {
    RouteConfig out;
    out.model = readString(node, "model");
    out.enabled = readBool(node, "enabled", true);
    if (const auto it = node.find("targets"); it != node.end() && it->is_array()) {
        for (const auto &item : *it) {
            RouteTarget target;
            if (item.is_string()) {
                // Shorthand: "provider" means "same model name upstream".
                target.provider = item.get<std::string>();
            } else if (item.is_object()) {
                target.provider = readString(item, "provider");
                target.model = readString(item, "model");
            }
            if (!target.provider.empty()) {
                out.targets.push_back(std::move(target));
            }
        }
    }
    return out;
}

json routeToJson(const RouteConfig &value) {
    json targets = json::array();
    for (const auto &target : value.targets) {
        json node = json::object();
        node["provider"] = target.provider;
        if (!target.model.empty()) {
            node["model"] = target.model;
        }
        targets.push_back(std::move(node));
    }
    json node = json::object();
    node["model"] = value.model;
    node["enabled"] = value.enabled;
    node["targets"] = std::move(targets);
    return node;
}

ServerConfig serverFromJson(const json &node) {
    ServerConfig out;
    out.host = readString(node, "host", out.host);
    out.port = readInt(node, "port", out.port);
    out.tls_cert_file = readString(node, "tls_cert_file");
    out.tls_key_file = readString(node, "tls_key_file");
    out.api_key = readString(node, "api_key");
    out.pass_through_unknown = readBool(node, "pass_through_unknown", out.pass_through_unknown);
    out.max_attempts = readInt(node, "max_attempts", out.max_attempts);
    out.routing_policy = readString(node, "routing_policy", out.routing_policy);
    out.request_deadline_sec = readInt(node, "request_deadline_sec", out.request_deadline_sec);
    out.session_affinity_sec = readInt(node, "session_affinity_sec", out.session_affinity_sec);
    out.reload_on_change = readBool(node, "reload_on_change", out.reload_on_change);
    out.circuit_failure_threshold =
        readInt(node, "circuit_failure_threshold", out.circuit_failure_threshold);
    out.circuit_cooldown_sec = readInt(node, "circuit_cooldown_sec", out.circuit_cooldown_sec);
    out.skip_open_circuits = readBool(node, "skip_open_circuits", out.skip_open_circuits);
    out.log_capacity = readInt(node, "log_capacity", out.log_capacity);
    out.log_bodies = readBool(node, "log_bodies", out.log_bodies);
    out.log_body_limit = readInt(node, "log_body_limit", out.log_body_limit);
    out.persist_telemetry = readBool(node, "persist_telemetry", out.persist_telemetry);
    out.web_ui = readBool(node, "web_ui", out.web_ui);
    out.language = readString(node, "language", out.language);
    out.ui_scale = readDouble(node, "ui_scale", out.ui_scale);
    return out;
}

json serverToJson(const ServerConfig &value) {
    json node = json::object();
    node["host"] = value.host;
    node["port"] = value.port;
    node["tls_cert_file"] = value.tls_cert_file;
    node["tls_key_file"] = value.tls_key_file;
    node["api_key"] = value.api_key;
    node["pass_through_unknown"] = value.pass_through_unknown;
    node["max_attempts"] = value.max_attempts;
    node["routing_policy"] = value.routing_policy;
    node["request_deadline_sec"] = value.request_deadline_sec;
    node["session_affinity_sec"] = value.session_affinity_sec;
    node["reload_on_change"] = value.reload_on_change;
    node["circuit_failure_threshold"] = value.circuit_failure_threshold;
    node["circuit_cooldown_sec"] = value.circuit_cooldown_sec;
    node["skip_open_circuits"] = value.skip_open_circuits;
    node["log_capacity"] = value.log_capacity;
    node["log_bodies"] = value.log_bodies;
    node["log_body_limit"] = value.log_body_limit;
    node["persist_telemetry"] = value.persist_telemetry;
    node["web_ui"] = value.web_ui;
    node["language"] = value.language;
    node["ui_scale"] = value.ui_scale;
    return node;
}

json healthToJson(const ProviderHealth &value) {
    json node = json::object();
    node["provider"] = value.provider;
    node["state"] = value.stateName();
    node["consecutive_failures"] = value.consecutive_failures;
    node["total_failures"] = value.total_failures;
    node["last_error"] = value.last_error;
    node["cooldown_remaining"] = value.cooldown_remaining;
    return node;
}

json statToJson(const ProviderStat &value) {
    json node = json::object();
    node["provider"] = value.provider;
    node["requests"] = value.requests;
    node["successes"] = value.successes;
    node["failures"] = value.failures;
    node["aborted"] = value.aborted;
    node["retries_in"] = value.retries_in;
    node["bytes_out"] = value.bytes_out;
    node["bytes_in"] = value.bytes_in;
    node["tokens_prompt"] = value.tokens_prompt;
    node["tokens_completion"] = value.tokens_completion;
    node["latency_ms_last"] = value.latency_ms_last;
    node["latency_ms_avg"] = value.latency_ms_avg;
    node["latency_ms_p95"] = value.latency_ms_p95;
    node["last_used_unix"] = value.last_used_unix;
    node["cost_usd"] = value.cost_usd;
    return node;
}

} // namespace

std::string ValidationIssue::levelName() const {
    switch (level) {
    case Level::Info: return "info";
    case Level::Warning: return "warning";
    case Level::Error: return "error";
    }
    return "error";
}

std::string ProviderHealth::stateName() const {
    switch (state) {
    case State::Unknown: return "unknown";
    case State::Healthy: return "healthy";
    case State::Degraded: return "degraded";
    case State::Open: return "open";
    }
    return "unknown";
}

void ensureLocalTimezone() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        const char *tz = std::getenv("TZ");
        if (tz == nullptr || *tz == '\0') {
#ifndef _WIN32
            // If TZ is unset, glibc tries to open /etc/localtime.
            // On hermetic toolchains with custom sysroots or rpaths, glibc's hardcoded
            // default prefix may not exist, which causes localtime_r to fall back to UTC.
            // Explicitly pointing TZ to ":/etc/localtime" tells glibc to resolve the file directly.
            if (access("/etc/localtime", R_OK) == 0) {
                setenv("TZ", ":/etc/localtime", 0);
                tzset();
            }
#else
            _tzset();
#endif
        }
    });
}

namespace {
std::tm toLocalTm(double time_unix, int &millisOut) {
    ensureLocalTimezone();
    const auto seconds = static_cast<std::time_t>(time_unix);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    double frac = time_unix - static_cast<double>(seconds);
    if (frac < 0.0) {
        frac = 0.0;
    }
    millisOut = static_cast<int>(frac * 1000.0);
    if (millisOut > 999) {
        millisOut = 999;
    }
    return local;
}
} // namespace

std::string LogEntry::timeText() const {
    int millis = 0;
    const std::tm local = toLocalTm(time_unix, millis);
    return std::format("{:02d}:{:02d}:{:02d}.{:03d}", local.tm_hour, local.tm_min, local.tm_sec,
                       millis);
}

std::string LogEntry::dateText() const {
    int millis = 0;
    const std::tm local = toLocalTm(time_unix, millis);
    return std::format("{:04d}-{:02d}-{:02d}", local.tm_year + 1900, local.tm_mon + 1,
                       local.tm_mday);
}

std::string LogEntry::dateTimeText() const {
    int millis = 0;
    const std::tm local = toLocalTm(time_unix, millis);
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}", local.tm_year + 1900,
                       local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec,
                       millis);
}

std::string LogEntry::shortDateTimeText() const {
    int millis = 0;
    const std::tm local = toLocalTm(time_unix, millis);
    return std::format("{:02d}-{:02d} {:02d}:{:02d}:{:02d}", local.tm_mon + 1, local.tm_mday,
                       local.tm_hour, local.tm_min, local.tm_sec);
}

// ── AppConfig ────────────────────────────────────────────────────────────────

std::string toJsonString(const AppConfig &config) {
    json root = json::object();
    root["schema"] = config.schema;
    root["server"] = serverToJson(config.server);

    json providers = json::array();
    for (const auto &entry : config.providers) {
        providers.push_back(providerToJson(entry));
    }
    root["providers"] = std::move(providers);

    json routes = json::array();
    for (const auto &entry : config.routes) {
        routes.push_back(routeToJson(entry));
    }
    root["routes"] = std::move(routes);

    root["clients"] = json::array();
    for (const auto &client : config.clients) {
        json keys = json::array();
        for (const auto &key : client.keys)
            keys.push_back(json{{"id", key.id}, {"api_key", key.api_key}, {"enabled", key.enabled}});
        root["clients"].push_back(json{{"id", client.id}, {"name", client.name},
            {"enabled", client.enabled}, {"keys", keys}, {"models", client.models},
            {"provider_groups", client.provider_groups}, {"requests_per_minute", client.requests_per_minute},
            {"max_concurrent", client.max_concurrent}, {"requests_per_day", client.requests_per_day},
            {"tokens_per_day", client.tokens_per_day}, {"token_reservation", client.token_reservation}});
    }

    return root.dump(2);
}

std::expected<AppConfig, std::string> appConfigFromJson(std::string_view text) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        return std::unexpected(std::string{"config is not valid JSON"});
    }
    if (!root.is_object()) {
        return std::unexpected(std::string{"config root must be a JSON object"});
    }

    AppConfig out;
    out.schema = readInt(root, "schema", out.schema);

    // Access rules fail closed on malformed types: treating a misspelled
    // allowlist as an empty list would silently grant access to everything.
    try {
        if (root.contains("clients")) {
            const auto &list = root.at("clients");
            if (!list.is_array()) throw std::runtime_error("clients must be an array");
            for (const auto &entry : list) {
                if (!entry.is_object()) throw std::runtime_error("client must be an object");
                ClientConfig c;
                c.id = entry.value("id", std::string{});
                c.name = entry.value("name", std::string{});
                c.enabled = entry.value("enabled", true);
                c.models = entry.value("models", std::vector<std::string>{});
                c.provider_groups = entry.value("provider_groups", std::vector<std::string>{});
                const auto nonnegative = [&](const char *key, std::uint64_t fallback) {
                    if (!entry.contains(key)) return fallback;
                    const auto &v = entry.at(key);
                    if (!v.is_number_unsigned() || v.get<std::uint64_t>() > 9007199254740991ULL)
                        throw std::runtime_error(std::string{key} + " must be a nonnegative safe integer");
                    return v.get<std::uint64_t>();
                };
                const auto rpm = nonnegative("requests_per_minute", 0);
                const auto concurrent = nonnegative("max_concurrent", 0);
                if (rpm > 2147483647ULL || concurrent > 2147483647ULL)
                    throw std::runtime_error("client rate or concurrency is too large");
                c.requests_per_minute = static_cast<int>(rpm);
                c.max_concurrent = static_cast<int>(concurrent);
                c.requests_per_day = nonnegative("requests_per_day", 0);
                c.tokens_per_day = nonnegative("tokens_per_day", 0);
                c.token_reservation = nonnegative("token_reservation", 4096);
                if (entry.contains("keys")) {
                    if (!entry.at("keys").is_array()) throw std::runtime_error("keys must be an array");
                    for (const auto &key : entry.at("keys")) {
                        if (!key.is_object()) throw std::runtime_error("key must be an object");
                        c.keys.push_back(ClientKeyConfig{key.value("id", std::string{}),
                            key.value("api_key", std::string{}), key.value("enabled", true)});
                    }
                }
                out.clients.push_back(std::move(c));
            }
        }
    } catch (const std::exception &e) {
        return std::unexpected(std::string{"invalid clients configuration: "} + e.what());
    }

    if (const auto it = root.find("server"); it != root.end()) {
        if (!it->is_object()) {
            return std::unexpected(std::string{"`server` must be an object"});
        }
        out.server = serverFromJson(*it);
    }

    if (const auto it = root.find("providers"); it != root.end()) {
        if (!it->is_array()) {
            return std::unexpected(std::string{"`providers` must be an array"});
        }
        for (const auto &item : *it) {
            if (!item.is_object()) {
                return std::unexpected(std::string{"every entry in `providers` must be an object"});
            }
            out.providers.push_back(providerFromJson(item));
        }
    }

    if (const auto it = root.find("routes"); it != root.end()) {
        if (!it->is_array()) {
            return std::unexpected(std::string{"`routes` must be an array"});
        }
        for (const auto &item : *it) {
            if (!item.is_object()) {
                return std::unexpected(std::string{"every entry in `routes` must be an object"});
            }
            out.routes.push_back(routeFromJson(item));
        }
    }

    return out;
}

// ── Snapshot ─────────────────────────────────────────────────────────────────

std::string toJsonString(const Snapshot &snapshot) {
    json node = json::object();
    node["running"] = snapshot.running;
    node["host"] = snapshot.host;
    node["port"] = snapshot.port;
    node["base_url"] = snapshot.base_url;
    node["started_unix"] = snapshot.started_unix;
    node["uptime_sec"] = snapshot.uptime_sec;
    node["config_path"] = snapshot.config_path;
    node["version"] = snapshot.version;
    node["total_requests"] = snapshot.total_requests;
    node["total_success"] = snapshot.total_success;
    node["total_failure"] = snapshot.total_failure;
    node["active_requests"] = snapshot.active_requests;
    node["bytes_out"] = snapshot.bytes_out;
    node["cost_usd"] = snapshot.cost_usd;
    node["tokens_prompt"] = snapshot.tokens_prompt;
    node["tokens_completion"] = snapshot.tokens_completion;
    node["latency_ms_avg"] = snapshot.latency_ms_avg;
    node["log_seq"] = snapshot.log_seq;
    node["breakers_open"] = snapshot.breakers_open;
    node["clients"] = json::array();
    for (const auto &usage : snapshot.clients)
        node["clients"].push_back(json::parse(toJsonString(usage)));

    json providers = json::array();
    for (const auto &entry : snapshot.providers) {
        providers.push_back(statToJson(entry));
    }
    node["providers"] = std::move(providers);

    json health = json::array();
    for (const auto &entry : snapshot.health) {
        health.push_back(healthToJson(entry));
    }
    node["health"] = std::move(health);

    // The hourly trend, oldest first. Written unconditionally (empty array when
    // there has been no traffic) so a reader never has to guess whether the key
    // means "no data" or "older build".
    json hourly = json::array();
    for (const auto &bucket : snapshot.hourly) {
        json entry = json::object();
        entry["hour_unix"] = bucket.hour_unix;
        entry["requests"] = bucket.requests;
        entry["successes"] = bucket.successes;
        entry["failures"] = bucket.failures;
        entry["bytes_out"] = bucket.bytes_out;
        entry["tokens_prompt"] = bucket.tokens_prompt;
        entry["tokens_completion"] = bucket.tokens_completion;
        entry["cost_usd"] = bucket.cost_usd;
        hourly.push_back(std::move(entry));
    }
    node["hourly"] = std::move(hourly);

    return node.dump(2);
}

// Its own overload rather than a private branch of the snapshot: the persisted
// telemetry file needs the same shape as the admin API, and a second copy of
// this field list is a field that will eventually be added to only one of them.
std::string toJsonString(const ProviderStat &stat) {
    return statToJson(stat).dump();
}

std::string toJsonString(const LogEntry &entry) {
    json node = json::object();
    node["seq"] = entry.seq;
    node["time_unix"] = entry.time_unix;
    node["time"] = entry.timeText();
    node["date"] = entry.dateText();
    node["datetime"] = entry.dateTimeText();
    node["level"] = entry.level;
    node["request_id"] = entry.request_id;
    node["client_id"] = entry.client_id;
    node["client_key_id"] = entry.client_key_id;
    node["kind"] = entry.kind;
    node["model"] = entry.model;
    node["provider"] = entry.provider;
    node["upstream_model"] = entry.upstream_model;
    node["status"] = entry.status;
    node["stream"] = entry.stream;
    node["failover"] = entry.failover;
    node["attempt"] = entry.attempt;
    node["attempts_total"] = entry.attempts_total;
    node["latency_ms"] = entry.latency_ms;
    node["wait_ms"] = entry.wait_ms;
    node["ttfb_ms"] = entry.ttfb_ms;
    node["stream_ms"] = entry.stream_ms;
    node["bytes"] = entry.bytes;
    node["message"] = entry.message;
    if (!entry.request_body.empty()) {
        node["request_body"] = entry.request_body;
    }
    if (!entry.response_body.empty()) {
        node["response_body"] = entry.response_body;
    }
    return node.dump();
}

std::string toJsonString(const ProviderProbe &probe) {
    json node = json::object();
    node["reachable"] = probe.reachable;
    node["status"] = probe.status;
    node["latency_ms"] = probe.latency_ms;
    node["detail"] = probe.detail;
    node["models"] = probe.models;
    return node.dump(2);
}

} // namespace literouter
