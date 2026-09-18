// ConfigStore, validate() and the AppConfig helpers from lr_config.cpp and
// lr_util.cpp. Every file this test writes goes into a fresh temp directory, so
// the real config path (and the real home directory) is never touched.
#include "lr_test_check.h"

import literouter.core;

namespace {

using literouter::AppConfig;
using literouter::ConfigStore;
using literouter::ProviderConfig;
using literouter::RouteConfig;
using literouter::RouteTarget;
using literouter::ValidationIssue;
using literouter::ValidationReport;

constexpr auto kError = ValidationIssue::Level::Error;
constexpr auto kWarning = ValidationIssue::Level::Warning;
constexpr auto kInfo = ValidationIssue::Level::Info;

// A directory that removes itself, so a failing check never leaves debris in
// /tmp and a re-run starts clean.
class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("literouter-config-test-" + literouter::hexId(6));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const std::filesystem::path &path() const { return path_; }
    std::filesystem::path file(std::string_view name) const {
        return path_ / std::filesystem::path{name};
    }

private:
    std::filesystem::path path_;
};

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, std::string_view text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
}

const ValidationIssue *findIssue(const ValidationReport &report, std::string_view path,
                                 ValidationIssue::Level level) {
    for (const auto &issue : report.issues) {
        if (issue.path == path && issue.level == level) {
            return &issue;
        }
    }
    return nullptr;
}

const ValidationIssue *findIssueContaining(const ValidationReport &report,
                                           std::string_view needle,
                                           ValidationIssue::Level level) {
    for (const auto &issue : report.issues) {
        if (issue.level == level && issue.message.find(needle) != std::string::npos) {
            return &issue;
        }
    }
    return nullptr;
}

// Two enabled relays, one disabled, every non-default field filled in. Used to
// prove the JSON codec is lossless rather than merely loadable.
AppConfig fullyPopulated() {
    AppConfig config;
    config.schema = 3;
    config.server.host = "0.0.0.0";
    config.server.port = 12345;
    config.server.api_key = "sk-server-key";
    config.server.pass_through_unknown = false;
    config.server.max_attempts = 4;
    config.server.routing_policy = "cheapest";
    config.server.request_deadline_sec = 45;
    config.server.session_affinity_sec = 120;
    config.server.reload_on_change = true;
    config.server.circuit_failure_threshold = 5;
    config.server.circuit_cooldown_sec = 60;
    config.server.skip_open_circuits = false;
    config.server.log_capacity = 128;
    config.server.log_bodies = true;
    config.server.log_body_limit = 512;
    config.server.persist_telemetry = false;
    config.server.web_ui = false;
    config.server.language = "zh";
    config.server.ui_scale = 1.25;

    ProviderConfig primary;
    primary.id = "primary";
    primary.name = "Primary relay";
    primary.base_url = "https://relay.example.com/v1";
    primary.api_key = "sk-primary-literal";
    primary.enabled = true;
    // Priced on purpose: a field only round-trips visibly when it is not zero.
    primary.price_in_per_million = 2.5;
    primary.price_out_per_million = 10.0;
    primary.priority = 7;
    primary.weight = 3;
    primary.timeout_sec = 45;
    primary.connect_timeout_sec = 6;
    primary.supports_stream = true;
    primary.models = {"gpt-4o", "gpt-4o-mini"};
    primary.headers = {{"X-Org", "org-42"}, {"Referer", "https://console.local"}};
    primary.chat_path = "/v1/alternate/chat";
    primary.embeddings_path = "/v1/alternate/embeddings";
    primary.note = "primary hop";

    // The key is a reference on purpose: this is the field whose round trip
    // must stay a reference rather than an expanded secret.
    ProviderConfig backup;
    backup.id = "backup";
    backup.name = "Backup relay";
    backup.base_url = "http://127.0.0.1:9000";
    backup.api_key = "${LITEROUTER_TEST_ENV_KEY}";
    backup.enabled = false;
    backup.priority = 90;
    backup.weight = 1;
    backup.timeout_sec = 30;
    backup.connect_timeout_sec = 3;
    backup.supports_stream = false;
    backup.models = {"claude-3-5-sonnet"};
    backup.headers = {{"x-custom-auth", "custom-value"}};
    backup.chat_path = "/chat/completions";
    backup.embeddings_path = "/embeddings";
    backup.note = "disabled placeholder";

    config.providers = {primary, backup};

    RouteConfig route;
    route.model = "fast";
    // One hop renames the model upstream, the other asks for the logical name
    // unchanged (empty `model`), which is the shorthand the file supports.
    route.targets = {RouteTarget{.provider = "primary", .model = "gpt-4o-2024-08-06"},
                     RouteTarget{.provider = "backup", .model = {}}};
    route.enabled = true;

    RouteConfig disabledRoute;
    disabledRoute.model = "legacy";
    disabledRoute.targets = {RouteTarget{.provider = "primary", .model = {}}};
    disabledRoute.enabled = false;

    config.routes = {route, disabledRoute};
    return config;
}

void checkServerEqual(const literouter::ServerConfig &actual,
                      const literouter::ServerConfig &expected) {
    LR_CHECK_EQ(actual.host, expected.host);
    LR_CHECK_EQ(actual.port, expected.port);
    LR_CHECK_EQ(actual.api_key, expected.api_key);
    LR_CHECK_EQ(actual.pass_through_unknown, expected.pass_through_unknown);
    LR_CHECK_EQ(actual.max_attempts, expected.max_attempts);
    LR_CHECK_EQ(actual.routing_policy, expected.routing_policy);
    LR_CHECK_EQ(actual.request_deadline_sec, expected.request_deadline_sec);
    LR_CHECK_EQ(actual.session_affinity_sec, expected.session_affinity_sec);
    LR_CHECK_EQ(actual.reload_on_change, expected.reload_on_change);
    LR_CHECK_EQ(actual.circuit_failure_threshold, expected.circuit_failure_threshold);
    LR_CHECK_EQ(actual.circuit_cooldown_sec, expected.circuit_cooldown_sec);
    LR_CHECK_EQ(actual.skip_open_circuits, expected.skip_open_circuits);
    LR_CHECK_EQ(actual.log_capacity, expected.log_capacity);
    LR_CHECK_EQ(actual.log_bodies, expected.log_bodies);
    LR_CHECK_EQ(actual.log_body_limit, expected.log_body_limit);
    LR_CHECK_EQ(actual.persist_telemetry, expected.persist_telemetry);
    LR_CHECK_EQ(actual.web_ui, expected.web_ui);
    LR_CHECK_EQ(actual.language, expected.language);
    LR_CHECK_EQ(actual.ui_scale, expected.ui_scale);
}

void checkProviderEqual(const ProviderConfig &actual, const ProviderConfig &expected,
                        std::string_view label) {
    const auto where = [label](std::string_view field) {
        return std::string{label} + "." + std::string{field};
    };
    LR_CHECK_MSG(actual.id == expected.id, where("id"));
    LR_CHECK_MSG(actual.name == expected.name, where("name"));
    LR_CHECK_MSG(actual.base_url == expected.base_url, where("base_url"));
    LR_CHECK_MSG(actual.api_key == expected.api_key, where("api_key"));
    LR_CHECK_MSG(actual.enabled == expected.enabled, where("enabled"));
    LR_CHECK_MSG(actual.priority == expected.priority, where("priority"));
    LR_CHECK_MSG(actual.weight == expected.weight, where("weight"));
    LR_CHECK_MSG(actual.timeout_sec == expected.timeout_sec, where("timeout_sec"));
    LR_CHECK_MSG(actual.connect_timeout_sec == expected.connect_timeout_sec,
                 where("connect_timeout_sec"));
    LR_CHECK_MSG(actual.supports_stream == expected.supports_stream, where("supports_stream"));
    LR_CHECK_MSG(actual.models == expected.models, where("models"));
    LR_CHECK_MSG(actual.headers == expected.headers, where("headers"));
    LR_CHECK_MSG(actual.chat_path == expected.chat_path, where("chat_path"));
    LR_CHECK_MSG(actual.embeddings_path == expected.embeddings_path, where("embeddings_path"));
    LR_CHECK_MSG(actual.price_in_per_million == expected.price_in_per_million,
                 where("price_in_per_million"));
    LR_CHECK_MSG(actual.price_out_per_million == expected.price_out_per_million,
                 where("price_out_per_million"));
    LR_CHECK_MSG(actual.note == expected.note, where("note"));
}

void testSeedDefault() {
    LR_GROUP("ConfigStore::seedDefault");
    const AppConfig seed = ConfigStore::seedDefault();
    const ValidationReport report = literouter::validate(seed);

    LR_CHECK_EQ(seed.schema, 1);
    LR_CHECK_EQ(seed.server.language, "auto");
    LR_CHECK_EQ(static_cast<long long>(seed.providers.size()), 2);
    LR_CHECK_EQ(static_cast<long long>(seed.routes.size()), 1);
    LR_CHECK_MSG(report.ok(), "the shipped seed must be usable as-is");
    LR_CHECK_EQ(static_cast<long long>(report.count(kError)), 0);
    LR_NOTE("seed summary: " + report.summary());

    // The seed ships an environment reference, not a key: it must stay
    // commit-able.
    bool hasReference = false;
    for (const auto &provider : seed.providers) {
        hasReference = hasReference || literouter::isSecretReference(provider.api_key);
    }
    LR_CHECK(hasReference);
    LR_CHECK(literouter::toJsonString(seed).find("${OPENAI_API_KEY}") != std::string::npos);
}

void testJsonRoundTrip() {
    LR_GROUP("toJsonString / appConfigFromJson round trip");
    const AppConfig original = fullyPopulated();
    const std::string text = literouter::toJsonString(original);

    auto parsed = literouter::appConfigFromJson(text);
    if (!parsed) {
        LR_CHECK_MSG(false, "a config written by toJsonString must parse: " + parsed.error());
        return;
    }
    const AppConfig &back = *parsed;

    LR_CHECK_EQ(back.schema, original.schema);
    checkServerEqual(back.server, original.server);
    LR_CHECK_EQ(static_cast<long long>(back.providers.size()),
                static_cast<long long>(original.providers.size()));
    LR_CHECK_EQ(static_cast<long long>(back.routes.size()),
                static_cast<long long>(original.routes.size()));
    if (back.providers.size() == 2) {
        checkProviderEqual(back.providers[0], original.providers[0], "providers[0]");
        checkProviderEqual(back.providers[1], original.providers[1], "providers[1]");
    }
    if (back.routes.size() == 2) {
        LR_CHECK_EQ(back.routes[0].model, original.routes[0].model);
        LR_CHECK_EQ(back.routes[0].enabled, original.routes[0].enabled);
        LR_CHECK_EQ(static_cast<long long>(back.routes[0].targets.size()), 2);
        if (back.routes[0].targets.size() == 2) {
            LR_CHECK_EQ(back.routes[0].targets[0].provider, "primary");
            LR_CHECK_EQ(back.routes[0].targets[0].model, "gpt-4o-2024-08-06");
            LR_CHECK_EQ(back.routes[0].targets[1].provider, "backup");
            LR_CHECK_EQ(back.routes[0].targets[1].model, "");
        }
        LR_CHECK_EQ(back.routes[1].model, "legacy");
        LR_CHECK_EQ(back.routes[1].enabled, false);
    }

    // A second round trip through the codec is stable, which catches a codec
    // that silently drops a field it can read.
    LR_CHECK_EQ(literouter::toJsonString(back), text);
}

void testSecretStaysReferenced() {
    LR_GROUP("a ${ENV} reference is never expanded into the file");
    const lr_test::EnvGuard guard{"LITEROUTER_TEST_ENV_KEY"};
    guard.assign("sk-expanded-value");

    const AppConfig config = fullyPopulated();
    const std::string text = literouter::toJsonString(config);
    LR_CHECK(text.find("${LITEROUTER_TEST_ENV_KEY}") != std::string::npos);
    LR_CHECK_MSG(text.find("sk-expanded-value") == std::string::npos,
                 "the resolved secret leaked into the serialised config");

    TempDir dir;
    auto store = ConfigStore::load(dir.file("config.json"));
    LR_CHECK(store.has_value());
    if (!store) {
        return;
    }
    store->config() = config;
    LR_CHECK(store->save().has_value());
    const std::string onDisk = readFile(dir.file("config.json"));
    LR_CHECK(onDisk.find("${LITEROUTER_TEST_ENV_KEY}") != std::string::npos);
    LR_CHECK(onDisk.find("sk-expanded-value") == std::string::npos);
    LR_CHECK(store->toJson().find("${LITEROUTER_TEST_ENV_KEY}") != std::string::npos);
    LR_CHECK(store->toJson().find("sk-expanded-value") == std::string::npos);
}

void testMinimalDocuments() {
    LR_GROUP("hand-written minimal documents keep their defaults");
    {
        auto parsed = literouter::appConfigFromJson("{}");
        LR_CHECK(parsed.has_value());
        if (parsed) {
            LR_CHECK_EQ(parsed->schema, 1);
            LR_CHECK_EQ(parsed->server.host, "127.0.0.1");
            LR_CHECK_EQ(parsed->server.port, 8787);
            LR_CHECK_EQ(parsed->server.max_attempts, 0);
            LR_CHECK_EQ(parsed->server.routing_policy, "priority");
            // No deadline by default: bounding a long generation by default
            // would break the case the proxy exists to serve.
            LR_CHECK_EQ(parsed->server.request_deadline_sec, 0);
            LR_CHECK_EQ(parsed->server.session_affinity_sec, 0);
            LR_CHECK_EQ(parsed->server.reload_on_change, false);
            LR_CHECK_EQ(parsed->server.circuit_failure_threshold, 3);
            LR_CHECK_EQ(parsed->server.circuit_cooldown_sec, 30);
            LR_CHECK_EQ(parsed->server.pass_through_unknown, true);
            LR_CHECK_EQ(parsed->server.skip_open_circuits, true);
            LR_CHECK_EQ(parsed->server.log_capacity, 200);
            LR_CHECK_EQ(parsed->server.log_bodies, false);
            // On by default: a restart that silently reset the counters and
            // emptied the log is what this field exists to prevent.
            LR_CHECK_EQ(parsed->server.persist_telemetry, true);
            LR_CHECK_EQ(parsed->server.ui_scale, 1.0);
            LR_CHECK_EQ(static_cast<long long>(parsed->providers.size()), 0);
            LR_CHECK_EQ(static_cast<long long>(parsed->routes.size()), 0);
        }
    }
    {
        auto parsed = literouter::appConfigFromJson(R"({"server":{"port":1234}})");
        LR_CHECK(parsed.has_value());
        if (parsed) {
            LR_CHECK_EQ(parsed->server.port, 1234);
            // One field set means every sibling keeps its documented default.
            LR_CHECK_EQ(parsed->server.host, "127.0.0.1");
            LR_CHECK_EQ(parsed->server.log_capacity, 200);
        }
    }
}

void testStructurallyWrongInput() {
    LR_GROUP("structurally wrong input is refused with a message");

    struct Case {
        const char *text;
        const char *expectedFragment;
    };
    const Case cases[] = {
        {"[]", "root"},
        {"\"a string\"", "root"},
        {"12", "root"},
        {"not json", "not valid JSON"},
        {"{\"providers\": {}}", "providers"},
        {"{\"providers\": [1]}", "providers"},
        {"{\"server\": []}", "server"},
        {"{\"routes\": {}}", "routes"},
        {"{\"routes\": [\"x\"]}", "routes"},
    };

    for (const Case &entry : cases) {
        auto parsed = literouter::appConfigFromJson(entry.text);
        LR_CHECK_MSG(!parsed.has_value(),
                     std::string{"expected a refusal for ["} + entry.text + "]");
        if (!parsed) {
            LR_CHECK_MSG(parsed.error().find(entry.expectedFragment) != std::string::npos,
                         std::string{"error for ["} + entry.text + "] was: " + parsed.error());
        }
    }

    // fromJson must leave the previous model alone when it refuses.
    ConfigStore store;
    const int portBefore = store.config().server.port;
    const auto refused = store.fromJson("[]");
    LR_CHECK(!refused.has_value());
    LR_CHECK_EQ(store.config().server.port, portBefore);

    // Unknown keys are tolerated so a file written by a newer build still loads.
    auto forward = literouter::appConfigFromJson(
        R"({"schema":1,"future":{"nested":true},"server":{"port":9999}})");
    LR_CHECK(forward.has_value());
    if (forward) {
        LR_CHECK_EQ(forward->server.port, 9999);
    }
}

void testLoadAndSave() {
    LR_GROUP("ConfigStore::load / save / saveAs");
    TempDir dir;
    const auto path = dir.file("config.json");

    // A missing file is a state, not an error: seeded default, not on disk.
    auto missing = ConfigStore::load(path);
    LR_CHECK(missing.has_value());
    if (!missing) {
        return;
    }
    LR_CHECK(!missing->existsOnDisk());
    LR_CHECK(missing->loadError().empty());
    LR_CHECK_EQ(missing->config().server.port, 8787);
    LR_CHECK_EQ(missing->path().string(), path.string());

    // Save, then load it back. The seed's own entries are replaced, so a
    // reload that returned three providers would prove the file was ignored.
    missing->config().server.port = 1234;
    missing->config().providers.clear();
    missing->config().providers.push_back(ProviderConfig{.id = "one",
                                                         .base_url = "https://relay.example/v1",
                                                         .models = {"m"}});
    const auto saved = missing->save();
    LR_CHECK_MSG(saved.has_value(), saved ? "" : saved.error());
    LR_CHECK(std::filesystem::exists(path));

    auto loaded = ConfigStore::load(path);
    LR_CHECK(loaded.has_value());
    if (!loaded) {
        LR_CHECK_MSG(false, loaded.error());
        return;
    }
    LR_CHECK(loaded->existsOnDisk());
    LR_CHECK_EQ(loaded->config().server.port, 1234);
    LR_CHECK_EQ(static_cast<long long>(loaded->config().providers.size()), 1);
    LR_CHECK(loaded->mtime != std::filesystem::file_time_type{});

    // Overwriting replaces the content: load() afterwards must see the new
    // value, and the rename must not leave the temp file behind.
    loaded->config().server.port = 4321;
    LR_CHECK(loaded->save().has_value());
    auto again = ConfigStore::load(path);
    LR_CHECK(again.has_value());
    if (again) {
        LR_CHECK_EQ(again->config().server.port, 4321);
    }
    int leftoverTemp = 0;
    for (const auto &entry : std::filesystem::directory_iterator{dir.path()}) {
        if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
            ++leftoverTemp;
        }
    }
    LR_CHECK_EQ(leftoverTemp, 0);

    // saveAs writes elsewhere without rebinding the store, and creates the
    // parent directories a first-time user does not have yet.
    const auto nested = dir.file("deep/deeper/config.json");
    LR_CHECK(!std::filesystem::exists(nested));
    const auto written = loaded->saveAs(nested);
    LR_CHECK_MSG(written.has_value(), written ? "" : written.error());
    LR_CHECK(std::filesystem::exists(nested));
    LR_CHECK_EQ(loaded->path().string(), path.string());
    auto nestedLoaded = ConfigStore::load(nested);
    LR_CHECK(nestedLoaded.has_value());
    if (nestedLoaded) {
        LR_CHECK_EQ(nestedLoaded->config().server.port, 4321);
    }

    // Hand-written file: load() keeps whatever the file says.
    const auto handwritten = dir.file("handwritten.json");
    writeFile(handwritten, R"({"server":{"port":7777,"log_bodies":true}})");
    auto handwrittenStore = ConfigStore::load(handwritten);
    LR_CHECK(handwrittenStore.has_value());
    if (handwrittenStore) {
        LR_CHECK_EQ(handwrittenStore->config().server.port, 7777);
        LR_CHECK_EQ(handwrittenStore->config().server.log_bodies, true);
        LR_CHECK_EQ(handwrittenStore->config().server.host, "127.0.0.1");
    }

    // A corrupt file is an error load() reports, and loadOrSeed() turns into a
    // usable seeded store that remembers the parse error.
    const auto corrupt = dir.file("corrupt.json");
    writeFile(corrupt, "{oops");
    LR_CHECK(!ConfigStore::load(corrupt).has_value());
    auto rescued = ConfigStore::loadOrSeed(corrupt);
    LR_CHECK(rescued.has_value());
    if (rescued) {
        LR_CHECK(!rescued->loadError().empty());
        LR_CHECK(!rescued->existsOnDisk());
        LR_CHECK_EQ(rescued->config().server.port, 8787);
    }

#ifndef _WIN32
    // Keys live in this file, so it must not be group/world readable.
    const auto perms = std::filesystem::status(path).permissions();
    const bool othersCanRead =
        (perms & (std::filesystem::perms::group_read | std::filesystem::perms::others_read)) !=
        std::filesystem::perms::none;
    LR_CHECK_MSG(!othersCanRead, "the saved config is readable by other users");
#endif
}

void testDefaultPaths() {
    LR_GROUP("default paths follow LITEROUTER_CONFIG / LITEROUTER_STATE_DIR");
    TempDir dir;
    const lr_test::EnvGuard configEnv{"LITEROUTER_CONFIG"};
    const lr_test::EnvGuard stateEnv{"LITEROUTER_STATE_DIR"};

    const auto configPath = dir.file("config.json");
    const auto stateDir = dir.file("state");
    configEnv.assign(configPath.string());
    stateEnv.assign(stateDir.string());

    LR_CHECK_EQ(literouter::defaultConfigPath().string(), configPath.string());
    LR_CHECK_EQ(literouter::defaultStateDir().string(), stateDir.string());
    LR_CHECK_EQ(literouter::defaultPidPath(8787).string(), (stateDir / "literouter-8787.pid").string());
    // The telemetry file is per instance too: one file per port, so two
    // instances cannot replace each other's counters.
    LR_CHECK_EQ(literouter::defaultTelemetryPath(8787).string(),
                (stateDir / "telemetry-8787.json").string());
    // One file per port: two instances on two ports are two instances, and the
    // file is what tells a second start who is holding the one it wants.
    LR_CHECK_EQ(literouter::defaultPidPath(0).string(), (stateDir / "literouter-0.pid").string());

    // loadDefault() on a directory that has no config yields the seed.
    auto loaded = ConfigStore::loadDefault();
    LR_CHECK(loaded.has_value());
    if (loaded) {
        LR_CHECK(!loaded->existsOnDisk());
        LR_CHECK_EQ(loaded->path().string(), configPath.string());
        LR_CHECK_EQ(loaded->config().server.port, 8787);
    }
    // Nothing was created: reading must not have side effects.
    LR_CHECK(!std::filesystem::exists(configPath));
}

void testValidateProviders() {
    LR_GROUP("validate(): providers");
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "dup", .base_url = "https://a.example/v1"},
                            ProviderConfig{.id = "dup", .base_url = "https://b.example/v1"}};
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(!report.ok());
        LR_CHECK(findIssue(report, "providers[1].id", kError) != nullptr);
        LR_CHECK_EQ(static_cast<long long>(report.count(kError)), 1);
        LR_CHECK_EQ(report.summary(), "1 error");
    }
    {
        // Prices are dollars per million tokens: a negative one is a typo, not a
        // discount.
        AppConfig config;
        config.providers = {ProviderConfig{.id = "priced",
                                           .base_url = "https://relay.example/v1",
                                           .price_in_per_million = -1.0}};
        LR_CHECK(findIssue(literouter::validate(config), "providers[0].price_in_per_million",
                           kError) != nullptr);
        config.providers[0].price_in_per_million = 0.0;
        LR_CHECK(literouter::validate(config).ok());
    }
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "bad", .base_url = "ftp://relay.example/v1"}};
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(findIssue(report, "providers[0].base_url", kError) != nullptr);
        LR_CHECK(!report.ok());
    }
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "bad", .base_url = "relay.example/v1"}};
        LR_CHECK(findIssue(literouter::validate(config), "providers[0].base_url", kError) !=
                 nullptr);
    }
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "", .base_url = "https://a.example/v1"}};
        LR_CHECK(findIssue(literouter::validate(config), "providers[0].id", kError) != nullptr);
    }
    {
        // A malformed reference is an error; an unset-but-well-formed one is
        // only a warning, so a config still loads on a machine without the key.
        const lr_test::EnvGuard guard{"LITEROUTER_TEST_ABSENT_KEY"};
        guard.clear();
        AppConfig config;
        ProviderConfig provider;
        provider.id = "ref";
        provider.base_url = "https://a.example/v1";
        provider.api_key = "${LITEROUTER_TEST_ABSENT_KEY}";
        config.providers = {provider};
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(report.ok());
        LR_CHECK(findIssue(report, "providers[0].api_key", kWarning) != nullptr);

        config.providers[0].api_key = "${UNTERMINATED";
        const ValidationReport malformed = literouter::validate(config);
        LR_CHECK(!malformed.ok());
        LR_CHECK(findIssueContaining(malformed, "malformed", kError) != nullptr);
    }
    {
        AppConfig config;
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(report.ok());
        LR_CHECK(findIssue(report, "providers", kWarning) != nullptr);
    }
}

void testValidateServer() {
    LR_GROUP("validate(): server");
    {
        AppConfig config;
        config.server.port = 70000;
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(!report.ok());
        LR_CHECK(findIssue(report, "server.port", kError) != nullptr);
    }
    {
        AppConfig config;
        config.server.port = -1;
        LR_CHECK(findIssue(literouter::validate(config), "server.port", kError) != nullptr);
    }
    {
        // Port 0 is legal — the kernel picks one — and only informational.
        AppConfig config;
        config.server.port = 0;
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(report.ok());
        LR_CHECK(findIssue(report, "server.port", kInfo) != nullptr);
        LR_CHECK(findIssue(report, "server.port", kError) == nullptr);
    }
    {
        AppConfig config;
        config.server.host = "";
        LR_CHECK(findIssue(literouter::validate(config), "server.host", kError) != nullptr);
    }
    {
        // A deadline is a choice; a negative one is a mistake.
        AppConfig config;
        config.server.request_deadline_sec = -1;
        LR_CHECK(findIssue(literouter::validate(config), "server.request_deadline_sec", kError) !=
                 nullptr);
        config.server.request_deadline_sec = 3;
        LR_CHECK(findIssueContaining(literouter::validate(config), "no room for a second relay",
                                     kWarning) != nullptr);
        config.server.request_deadline_sec = 30;
        LR_CHECK(literouter::validate(config).ok());
        config.server.session_affinity_sec = -5;
        LR_CHECK(findIssue(literouter::validate(config), "server.session_affinity_sec", kError) !=
                 nullptr);
        config.server.session_affinity_sec = 0;
        config.server.routing_policy = "sometimes";
        LR_CHECK(findIssueContaining(literouter::validate(config), "not a routing policy",
                                     kWarning) != nullptr);
        config.server.routing_policy = "fastest";
        LR_CHECK(literouter::validate(config).ok());
        config.server.routing_policy = "priority";
        LR_CHECK(findIssue(literouter::validate(config), "server.request_deadline_sec", kWarning) ==
                 nullptr);
    }
    {
        AppConfig config;
        config.server.host = "0.0.0.0";
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(report.ok());
        LR_CHECK(findIssueContaining(report, "no client key", kWarning) != nullptr);
    }
    {
        // Persisting is fine and logging bodies is fine; the pair is what puts
        // prompts on disk, so it is the combination that gets flagged.
        AppConfig config;
        config.server.log_bodies = true;
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(report.ok());
        LR_CHECK(findIssueContaining(report, "outlive the process", kWarning) != nullptr);

        config.server.persist_telemetry = false;
        LR_CHECK(findIssueContaining(literouter::validate(config), "outlive the process", kWarning) ==
                 nullptr);
    }
}

void testValidateRoutes() {
    LR_GROUP("validate(): routes");
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "a", .base_url = "https://a.example/v1"}};
        RouteConfig route;
        route.model = "m";
        route.targets = {RouteTarget{.provider = "ghost", .model = {}}};
        config.routes = {route};
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(!report.ok());
        LR_CHECK(findIssue(report, "routes[0].targets[0].provider", kError) != nullptr);
    }
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "a", .base_url = "https://a.example/v1"}};
        RouteConfig route;
        route.model = "m";
        config.routes = {route};
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(!report.ok());
        LR_CHECK(findIssue(report, "routes[0].targets", kError) != nullptr);
    }
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "a", .base_url = "https://a.example/v1"}};
        RouteConfig first;
        first.model = "same";
        first.targets = {RouteTarget{.provider = "a", .model = {}}};
        RouteConfig second = first;
        config.routes = {first, second};
        const ValidationReport report = literouter::validate(config);
        LR_CHECK(!report.ok());
        LR_CHECK(findIssue(report, "routes[1].model", kError) != nullptr);
    }
    {
        AppConfig config;
        config.providers = {ProviderConfig{.id = "a", .base_url = "https://a.example/v1"}};
        RouteConfig route;
        route.model = "";
        route.targets = {RouteTarget{.provider = "a", .model = {}}};
        config.routes = {route};
        LR_CHECK(findIssue(literouter::validate(config), "routes[0].model", kError) != nullptr);
    }
    {
        // A hop through a disabled relay is a warning, not an error: the rest
        // of the chain still works and the user only needs to know it is
        // skipped.
        AppConfig config;
        config.providers = {ProviderConfig{.id = "live", .base_url = "https://live.example/v1"},
                            ProviderConfig{.id = "off",
                                           .base_url = "https://off.example/v1",
                                           .enabled = false}};
        RouteConfig route;
        route.model = "m";
        route.targets = {RouteTarget{.provider = "off", .model = {}}};
        config.routes = {route};
        const ValidationReport report = literouter::validate(config);
        LR_CHECK_MSG(report.ok(), "a disabled hop must not make the config unusable");
        LR_CHECK(findIssue(report, "routes[0].targets[0].provider", kWarning) != nullptr);
        LR_CHECK(findIssueContaining(report, "is disabled", kWarning) != nullptr);
        LR_CHECK_EQ(report.summary(), "1 warning");
    }
}

void testModelHelpers() {
    LR_GROUP("AppConfig::provider / route / logicalModels");

    AppConfig config;
    config.providers = {
        ProviderConfig{.id = "p1", .name = "one", .base_url = "https://p1.example/v1",
                       .models = {"b", "a"}},
        ProviderConfig{.id = "p2", .name = "two", .base_url = "https://p2.example/v1",
                       .models = {"a", "c"}},
        ProviderConfig{.id = "p3", .name = "three", .base_url = "https://p3.example/v1",
                       .enabled = false, .models = {"zzz"}},
    };
    RouteConfig enabled;
    enabled.model = "zz";
    enabled.targets = {RouteTarget{.provider = "p1", .model = {}}};
    RouteConfig disabled;
    disabled.model = "aa";
    disabled.enabled = false;
    disabled.targets = {RouteTarget{.provider = "p1", .model = {}}};
    RouteConfig empty;
    empty.model = "";
    empty.targets = {RouteTarget{.provider = "p1", .model = {}}};
    config.routes = {enabled, disabled, empty};

    const AppConfig &constConfig = config;
    LR_CHECK(constConfig.provider("p1") != nullptr);
    LR_CHECK_EQ(constConfig.provider("p1")->name, "one");
    LR_CHECK(constConfig.provider("nope") == nullptr);
    LR_CHECK(config.provider("p2") != nullptr);
    LR_CHECK(config.provider("nope") == nullptr);
    LR_CHECK(config.provider("p1") != nullptr && config.provider("p1")->id == "p1");

    LR_CHECK(constConfig.route("zz") != nullptr);
    LR_CHECK(constConfig.route("nope") == nullptr);
    // A disabled route is still findable: the router decides what to do with it.
    LR_CHECK(constConfig.route("aa") != nullptr);

    // Only configured and enabled routes are returned by logicalModels() / routedModels().
    // Disabled routes and empty route names contribute nothing.
    const std::vector<std::string> expectedRoutes{"zz"};
    LR_CHECK(config.logicalModels() == expectedRoutes);
    LR_CHECK(config.routedModels() == expectedRoutes);

    // allModels() includes both routed and provider-advertised models, de-duplicated and sorted.
    const std::vector<std::string> expectedAll{"a", "b", "c", "zz"};
    LR_CHECK(config.allModels() == expectedAll);

    // Sorted and unique by construction, so sorting again changes nothing.
    auto models = config.logicalModels();
    std::ranges::sort(models);
    models.erase(std::ranges::unique(models).begin(), models.end());
    LR_CHECK(models == config.logicalModels());
}

void testValidationReportShape() {
    LR_GROUP("ValidationReport and ValidationIssue shape");
    AppConfig config;
    config.server.port = 70000;
    config.server.log_capacity = 4;
    const ValidationReport report = literouter::validate(config);
    LR_CHECK(!report.ok());
    LR_CHECK_EQ(static_cast<long long>(report.count(kError)), 1);
    LR_CHECK(report.count(kWarning) >= 1);
    LR_CHECK(report.summary().find("1 error") == 0);
    LR_CHECK(report.summary().find("warning") != std::string::npos);

    LR_CHECK_EQ(ValidationIssue{.level = kInfo}.levelName(), "info");
    LR_CHECK_EQ(ValidationIssue{.level = kWarning}.levelName(), "warning");
    LR_CHECK_EQ(ValidationIssue{.level = kError}.levelName(), "error");

    const ValidationReport clean = literouter::validate(AppConfig{});
    LR_CHECK(clean.count(kError) == 0);
}

} // namespace

int main() {
    testSeedDefault();
    testJsonRoundTrip();
    testSecretStaysReferenced();
    testMinimalDocuments();
    testStructurallyWrongInput();
    testLoadAndSave();
    testDefaultPaths();
    testValidateProviders();
    testValidateServer();
    testValidateRoutes();
    testModelHelpers();
    testValidationReportShape();
    return LR_SUMMARY("test_config");
}
