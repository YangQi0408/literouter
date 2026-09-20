#include <CLI/CLI.hpp>

#include <iostream>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

struct AddOptions {
    std::string id;
    std::string baseUrl;
    std::string name;
    std::string key;
    std::string keyEnv;
    bool keyStdin = false;
    int priority = 100;
    int weight = 1;
    int timeout = 120;
    // Dollars per million tokens; 0 leaves the relay unpriced, and an unpriced
    // relay contributes nothing to the cost estimate.
    double priceIn = 0.0;
    double priceOut = 0.0;
    std::vector<std::string> groups;
    std::vector<std::string> models;
    std::vector<std::string> headers;
    bool enabled = true;
    std::string chatPath = "/chat/completions";
    std::string embeddingsPath = "/embeddings";
    std::string protocol = "openai";
    std::string note;
    // Protocol-specific settings. Named rather than generic because each of them
    // belongs to exactly one protocol, and a `--setting k=v` bag would let an
    // operator write one that is silently ignored.
    std::string apiVersion;
    std::string region;
    std::string project;
    std::string credentialsFile;
    std::string awsAccessKey;
    std::string awsSecretKey;
    std::string awsSessionToken;
    // Relay-side protection, distinct from the per-client limits.
    int maxConcurrent = 0;
    int rpm = 0;
};

struct RemoveOptions {
    std::string id;
    bool force = false;
};

struct ToggleOptions {
    std::string id;
};

struct TestOptions {
    std::vector<std::string> ids;
};

json providerJson(const literouter::ProviderConfig &provider) {
    json node = json::object();
    node["id"] = provider.id;
    node["name"] = provider.name.empty() ? provider.id : provider.name;
    node["base_url"] = provider.base_url;
    node["enabled"] = provider.enabled;
    node["protocol"] = provider.protocol;
    node["priority"] = provider.priority;
    node["weight"] = provider.weight;
    node["timeout_sec"] = provider.timeout_sec;
    node["models"] = provider.models;
    node["groups"] = provider.groups;
    node["api_key"] = describeApiKey(provider.api_key);
    node["chat_path"] = provider.chat_path;
    node["embeddings_path"] = provider.embeddings_path;
    node["note"] = provider.note;
    return node;
}

bool saveOrFail(Context &ctx, const literouter::ConfigStore &store) {
    if (auto saved = store.save(); !saved) {
        printError(
            std::format("cannot write config `{}`: {}", store.path().string(), saved.error()));
        ctx.exitCode = kExitFail;
        return false;
    }
    return true;
}

std::string headerTargets(const literouter::AppConfig &config, const std::string &id) {
    std::string routes;
    for (const auto &route : config.routes) {
        for (const auto &target : route.targets) {
            if (target.provider == id) {
                if (!routes.empty()) {
                    routes += ", ";
                }
                routes += route.model;
            }
        }
    }
    return routes;
}

void runList(Context &ctx) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const literouter::AppConfig &config = storeOpt->config();

    if (ctx.json) {
        json array = json::array();
        for (const auto &provider : config.providers) {
            array.push_back(providerJson(provider));
        }
        json root = json::object();
        root["path"] = storeOpt->path().string();
        root["providers"] = std::move(array);
        std::println("{}", root.dump(2));
        return;
    }

    if (config.providers.empty()) {
        printInfo(std::format("no relays configured in {}", storeOpt->path().string()));
        printHint("add one with `literouter providers add <id> --base-url https://…/v1`");
        return;
    }

    Table table;
    table.column("ID");
    table.column("NAME");
    table.column("STATE");
    table.column("PROTOCOL");
    table.column("PRI", Align::Right);
    table.column("MODELS", Align::Right);
    table.column("BASE URL");
    table.column("KEY");
    for (const auto &provider : config.providers) {
        table.row({provider.id,
                   provider.name.empty() ? provider.id : provider.name,
                   colorEnabled(provider.enabled, provider.enabled ? "enabled" : "disabled"),
                   provider.protocol.empty() ? "openai" : provider.protocol,
                   std::to_string(provider.priority),
                   std::to_string(provider.models.size()),
                   provider.base_url,
                   describeApiKey(provider.api_key)});
    }
    table.print();
}

void runAdd(Context &ctx, const AddOptions &opts) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::AppConfig &config = storeOpt->config();
    if (config.provider(opts.id) != nullptr) {
        printError(std::format("relay `{}` already exists in {}", opts.id,
                               storeOpt->path().string()));
        printHint(std::format("remove it first with `literouter providers remove {}`", opts.id));
        ctx.exitCode = kExitConfig;
        return;
    }

    literouter::ProviderConfig provider;
    provider.id = opts.id;
    provider.name = opts.name.empty() ? opts.id : opts.name;
    provider.base_url = opts.baseUrl;
    provider.protocol = opts.protocol;
    provider.enabled = opts.enabled;
    provider.priority = opts.priority;
    provider.weight = opts.weight;
    provider.timeout_sec = opts.timeout;
    provider.price_in_per_million = opts.priceIn;
    provider.price_out_per_million = opts.priceOut;
    provider.models = opts.models;
    provider.groups = opts.groups;
    provider.chat_path = opts.chatPath;
    provider.embeddings_path = opts.embeddingsPath;
    provider.note = opts.note;
    provider.api_version = opts.apiVersion;
    provider.region = opts.region;
    provider.project = opts.project;
    provider.credentials_file = opts.credentialsFile;
    provider.aws_access_key = opts.awsAccessKey;
    provider.aws_secret_key = opts.awsSecretKey;
    provider.aws_session_token = opts.awsSessionToken;
    provider.max_concurrent = opts.maxConcurrent;
    provider.requests_per_minute = opts.rpm;

    if (!opts.key.empty()) {
        provider.api_key = opts.key;
    } else if (!opts.keyEnv.empty()) {
        provider.api_key = std::format("${{{}}}", opts.keyEnv);
    } else if (opts.keyStdin) {
        std::string line;
        if (!std::getline(std::cin, line) || line.empty()) {
            printError(std::format("--key-stdin was given but no key arrived on stdin for relay `{}`",
                                   opts.id));
            printHint("pipe the key in, e.g. `echo $KEY | literouter providers add … --key-stdin`");
            ctx.exitCode = kExitFail;
            return;
        }
        provider.api_key = line;
    }

    for (const auto &header : opts.headers) {
        const auto sep = header.find(':');
        if (sep == std::string::npos) {
            printError(std::format("--header `{}` is not in `Name: value` form for relay `{}`",
                                   header, opts.id));
            ctx.exitCode = kExitFail;
            return;
        }
        const std::string name = literouter::trim(header.substr(0, sep));
        const std::string value = literouter::trim(header.substr(sep + 1));
        if (name.empty()) {
            printError(std::format("--header `{}` has an empty name for relay `{}`", header,
                                   opts.id));
            ctx.exitCode = kExitFail;
            return;
        }
        provider.headers[name] = value;
    }

    config.providers.push_back(provider);
    if (!saveOrFail(ctx, *storeOpt)) {
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["changed"] = "provider-added";
        root["path"] = storeOpt->path().string();
        root["provider"] = providerJson(provider);
        std::println("{}", root.dump(2));
        return;
    }

    KeyValues info;
    info.add("added relay", provider.id);
    info.add("base url", provider.base_url);
    info.add("state", colorEnabled(provider.enabled, provider.enabled ? "enabled" : "disabled"));
    info.add("priority", std::format("{} (weight {})", provider.priority, provider.weight));
    info.add("key", describeApiKey(provider.api_key));
    info.add("models", provider.models.empty() ? "(none advertised)"
                                               : std::format("{}", provider.models.size()));
    info.add("saved to", storeOpt->path().string());
    info.print();
    printNote(std::format("  test it with `literouter providers test {}`", provider.id), ctx.quiet);
}

void runRemove(Context &ctx, const RemoveOptions &opts) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::AppConfig &config = storeOpt->config();
    if (config.provider(opts.id) == nullptr) {
        printError(
            std::format("no relay `{}` in {}", opts.id, storeOpt->path().string()));
        ctx.exitCode = kExitConfig;
        return;
    }

    const std::string routes = headerTargets(config, opts.id);
    if (!routes.empty() && !opts.force) {
        printError(std::format("refusing to remove relay `{}`: route(s) {} still target it",
                               opts.id, routes));
        printHint(std::format("remove those routes first, or re-run with "
                              "`literouter providers remove {} --force`",
                              opts.id));
        ctx.exitCode = kExitConfig;
        return;
    }

    const auto before = config.providers.size();
    std::erase_if(config.providers,
                  [&](const literouter::ProviderConfig &p) { return p.id == opts.id; });
    if (!saveOrFail(ctx, *storeOpt)) {
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["changed"] = "provider-removed";
        root["path"] = storeOpt->path().string();
        root["id"] = opts.id;
        root["providers_before"] = before;
        root["providers_after"] = config.providers.size();
        std::println("{}", root.dump(2));
        return;
    }
    printInfo(std::format("removed relay `{}` from {} ({} relay(s) remain)", opts.id,
                          storeOpt->path().string(), config.providers.size()));
}

void runToggle(Context &ctx, const ToggleOptions &opts, bool enable) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::AppConfig &config = storeOpt->config();
    literouter::ProviderConfig *provider = config.provider(opts.id);
    if (provider == nullptr) {
        printError(std::format("no relay `{}` in {}", opts.id, storeOpt->path().string()));
        ctx.exitCode = kExitConfig;
        return;
    }
    provider->enabled = enable;
    if (!saveOrFail(ctx, *storeOpt)) {
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["changed"] = enable ? "provider-enabled" : "provider-disabled";
        root["path"] = storeOpt->path().string();
        root["id"] = opts.id;
        std::println("{}", root.dump(2));
        return;
    }
    printInfo(std::format("relay `{}` is now {} in {}",
                          opts.id, colorEnabled(enable, enable ? "enabled" : "disabled"),
                          storeOpt->path().string()));
}

void runTest(Context &ctx, const TestOptions &opts) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const literouter::AppConfig &config = storeOpt->config();

    std::vector<literouter::ProviderConfig> selected;
    if (opts.ids.empty()) {
        for (const auto &provider : config.providers) {
            if (provider.enabled) {
                selected.push_back(provider);
            }
        }
        if (selected.empty()) {
            printError(std::format("no enabled relay in {} to test", storeOpt->path().string()));
            printHint("enable one with `literouter providers enable <id>` or name it explicitly");
            ctx.exitCode = kExitFail;
            return;
        }
    } else {
        for (const auto &id : opts.ids) {
            const literouter::ProviderConfig *provider = config.provider(id);
            if (provider == nullptr) {
                printError(std::format("no relay `{}` in {}", id, storeOpt->path().string()));
                ctx.exitCode = kExitConfig;
                return;
            }
            selected.push_back(*provider);
        }
    }

    int failures = 0;
    json array = json::array();
    Table table;
    table.column("RELAY");
    table.column("REACHABLE");
    table.column("HTTP", Align::Right);
    table.column("LATENCY", Align::Right);
    table.column("MODELS", Align::Right);
    table.column("SAMPLE");
    table.column("DETAIL");

    for (const auto &provider : selected) {
        const literouter::ProviderProbe probe = literouter::probeProvider(provider);
        // A non-retryable 4xx is the caller's bad request, not a relay failure:
        // the probe reached the relay, so it counts as reachable. Only a
        // transport failure or a retryable status (5xx, 408/409/425/429) fails.
        const bool failed =
            !probe.reachable || literouter::Router::retryableStatus(probe.status);
        if (failed) {
            ++failures;
        }
        std::string sample;
        for (std::size_t i = 0; i < probe.models.size() && i < 3; ++i) {
            if (i > 0) {
                sample += ", ";
            }
            sample += probe.models[i];
        }
        if (probe.models.size() > 3) {
            sample += ", …";
        }
        table.row({provider.id,
                   probe.reachable ? colorEnabled(true, "yes") : colorEnabled(false, "no"),
                   probe.status == 0 ? "—" : colorStatus(probe.status, std::to_string(probe.status)),
                   literouter::humanMillis(probe.latency_ms),
                   std::to_string(probe.models.size()),
                   sample.empty() ? "—" : literouter::truncateUtf8(sample, 48),
                   literouter::truncateUtf8(probe.detail, 60)});

        if (ctx.json) {
            json node = json::parse(literouter::toJsonString(probe), nullptr, false);
            if (node.is_discarded()) {
                node = json::object();
            }
            node["id"] = provider.id;
            node["base_url"] = provider.base_url;
            node["ok"] = !failed;
            array.push_back(std::move(node));
        }
    }

    if (ctx.json) {
        json root = json::object();
        root["failures"] = failures;
        root["probes"] = std::move(array);
        std::println("{}", root.dump(2));
    } else {
        table.print();
        if (failures > 0) {
            printWarn(std::format("{} of {} relay(s) did not answer cleanly", failures,
                                  selected.size()));
        } else {
            printInfo(paint(std::format("all {} relay(s) answered", selected.size()), "32"));
        }
    }
    if (failures > 0) {
        ctx.exitCode = kExitFail;
    }
}

void registerList(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("list", "List configured relays");
    sub->fallthrough();
    sub->callback([&ctx] { runList(ctx); });
}

void registerAdd(CLI::App &parent, Context &ctx) {
    auto opts = std::make_shared<AddOptions>();
    CLI::App *sub = parent.add_subcommand("add", "Add a relay to the config file");
    sub->add_option("id", opts->id, "Relay id (used by routes and logs)")->required();
    sub->add_option("--base-url", opts->baseUrl, "Upstream root, e.g. https://api.openai.com/v1")
        ->required();
    sub->add_option("--name", opts->name, "Human label (defaults to the id)");
    CLI::Option *key = sub->add_option("--key", opts->key, "Literal API key");
    CLI::Option *keyEnv =
        sub->add_option("--key-env", opts->keyEnv, "Store ${VAR}; the key is read at request time");
    CLI::Option *keyStdin = sub->add_flag("--key-stdin", opts->keyStdin, "Read one line from stdin");
    key->excludes(keyEnv)->excludes(keyStdin);
    keyEnv->excludes(keyStdin);
    sub->add_option("--priority", opts->priority, "Lower wins (default 100)");
    sub->add_option("--weight", opts->weight, "Tie-break weight within a priority (default 1)");
    sub->add_option("--timeout", opts->timeout, "Per-request timeout in seconds (default 120)");
    sub->add_option("--price-in", opts->priceIn,
                    "Input price in dollars per million tokens (0 = unknown)");
    sub->add_option("--price-out", opts->priceOut,
                    "Output price in dollars per million tokens (0 = unknown)");
    sub->add_option("--group", opts->groups, std::string(literouter::i18n::tr("Provider group, repeatable")));
    sub->add_option("--model", opts->models, "A model id this relay advertises (repeatable)");
    sub->add_option("--header", opts->headers, "Extra upstream header, `Name: value` (repeatable)");
    sub->add_flag("--enable,!--no-enable", opts->enabled, "Enable the relay (default)")
        ->default_val(true);
    sub->add_option("--chat-path", opts->chatPath, "Chat completions path (default /chat/completions)");
    sub->add_option("--embeddings-path", opts->embeddingsPath,
                    "Embeddings path (default /embeddings)");
    sub->add_option("--protocol", opts->protocol,
                    "Upstream API protocol: openai, anthropic, gemini, openai_responses, "
                    "azure, vertex, bedrock, ollama (default openai)");
    sub->add_option("--api-version", opts->apiVersion,
                    "Azure: the api-version query; Vertex: the API version segment");
    sub->add_option("--region", opts->region,
                    "Bedrock region (required there), or the Vertex location");
    sub->add_option("--project", opts->project, "Vertex project id");
    sub->add_option("--credentials-file", opts->credentialsFile,
                    "Vertex service-account JSON key path");
    sub->add_option("--aws-access-key", opts->awsAccessKey,
                    "Bedrock SigV4 access key id (a ${VAR} reference is stored as written)");
    sub->add_option("--aws-secret-key", opts->awsSecretKey, "Bedrock SigV4 secret access key");
    sub->add_option("--aws-session-token", opts->awsSessionToken,
                    "Bedrock STS session token, for temporary credentials");
    sub->add_option("--max-concurrent", opts->maxConcurrent,
                    "Most requests literouter sends this relay at once; 0 is unlimited")
        ->check(CLI::NonNegativeNumber);
    sub->add_option("--rpm", opts->rpm,
                    "Most requests literouter starts on this relay per minute; 0 is unlimited")
        ->check(CLI::NonNegativeNumber);
    sub->add_option("--note", opts->note, "Free-form note");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runAdd(ctx, *opts); });
}

void registerRemove(CLI::App &parent, Context &ctx) {
    auto opts = std::make_shared<RemoveOptions>();
    CLI::App *sub = parent.add_subcommand("remove", "Remove a relay from the config file");
    sub->add_option("id", opts->id, "Relay id")->required();
    sub->add_flag("--force", opts->force, "Remove even while a route still targets it");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runRemove(ctx, *opts); });
}

void registerToggle(CLI::App &parent, Context &ctx, bool enable) {
    auto opts = std::make_shared<ToggleOptions>();
    const std::string name = enable ? "enable" : "disable";
    CLI::App *sub = parent.add_subcommand(
        name, enable ? "Mark a relay enabled" : "Mark a relay disabled");
    sub->add_option("id", opts->id, "Relay id")->required();
    sub->fallthrough();
    sub->callback([&ctx, opts, enable] { runToggle(ctx, *opts, enable); });
}

void registerTest(CLI::App &parent, Context &ctx) {
    auto opts = std::make_shared<TestOptions>();
    CLI::App *sub = parent.add_subcommand("test", "Probe relays with GET {base_url}/models");
    sub->add_option("ids", opts->ids,
                    "Relay ids to probe (default: every enabled relay)");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runTest(ctx, *opts); });
}

} // namespace

void registerProviderGroups(CLI::App& parent, Context& ctx) {
    struct Options { std::string id; std::vector<std::string> groups; bool clear = false; };
    auto opts = std::make_shared<Options>();
    auto* sub = parent.add_subcommand("groups", std::string(literouter::i18n::tr("Set provider groups")));
    sub->fallthrough();
    sub->add_option("id", opts->id, std::string(literouter::i18n::tr("Provider ID")))->required();
    auto* groups = sub->add_option("--group", opts->groups, std::string(literouter::i18n::tr("Provider group, repeatable")));
    sub->add_flag("--clear", opts->clear, std::string(literouter::i18n::tr("Clear provider groups")))->excludes(groups);
    sub->callback([&ctx, opts] {
        auto store = loadStore(ctx);
        if (!store) { ctx.exitCode = kExitConfig; return; }
        auto* provider = store->config().provider(opts->id);
        if (!provider || (opts->groups.empty() && !opts->clear)) {
            printError(std::string(literouter::i18n::tr("Choose an existing provider and --group or --clear")));
            ctx.exitCode = kExitConfig;
            return;
        }
        provider->groups = opts->groups;
        if (!saveOrFail(ctx, *store)) return;
        if (ctx.json) std::println("{}", json{{"id", opts->id}, {"groups", provider->groups}}.dump(2));
        else printInfo(std::string(literouter::i18n::tr("Provider groups saved")));
    });
}

void register_providers(CLI::App &root, Context &ctx) {
    CLI::App *providers = root.add_subcommand(
        "providers",
        std::string(literouter::i18n::tr("Manage relays (edits the config file, no server needed)")));
    providers->require_subcommand(1);
    providers->fallthrough();
    registerProviderGroups(*providers, ctx);
    registerList(*providers, ctx);
    registerAdd(*providers, ctx);
    registerRemove(*providers, ctx);
    registerToggle(*providers, ctx, true);
    registerToggle(*providers, ctx, false);
    registerTest(*providers, ctx);
}

} // namespace lrcli
