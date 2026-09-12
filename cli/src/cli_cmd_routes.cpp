#include <CLI/CLI.hpp>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

struct AddOptions {
    std::string model;
    std::vector<std::string> targets;
    bool noEnable = false;
};

struct ToggleOptions {
    std::string model;
};

std::string renderTargets(const literouter::RouteConfig &route,
                          const literouter::AppConfig &config) {
    std::string out;
    for (std::size_t i = 0; i < route.targets.size(); ++i) {
        if (i > 0) {
            out += " → ";
        }
        std::string hop = route.targets[i].provider;
        if (!route.targets[i].model.empty()) {
            hop += ":" + route.targets[i].model;
        }
        const literouter::ProviderConfig *provider = config.provider(route.targets[i].provider);
        if (provider == nullptr || !provider->enabled) {
            out += dim(hop) + dim(" (disabled)");
        } else {
            out += hop;
        }
    }
    return out.empty() ? dim("(no targets)") : out;
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
        for (const auto &route : config.routes) {
            json targets = json::array();
            for (const auto &target : route.targets) {
                const literouter::ProviderConfig *provider = config.provider(target.provider);
                json node = json::object();
                node["provider"] = target.provider;
                node["model"] = target.model;
                node["disabled"] = provider == nullptr || !provider->enabled;
                targets.push_back(std::move(node));
            }
            json node = json::object();
            node["model"] = route.model;
            node["enabled"] = route.enabled;
            node["targets"] = std::move(targets);
            array.push_back(std::move(node));
        }
        json root = json::object();
        root["path"] = storeOpt->path().string();
        root["routes"] = std::move(array);
        std::println("{}", root.dump(2));
        return;
    }

    if (config.routes.empty()) {
        printInfo(std::format("no routes configured in {}", storeOpt->path().string()));
        printHint("add one with `literouter routes add <model> --target <relay>`");
        return;
    }

    Table table;
    table.column("MODEL");
    table.column("STATE");
    table.column("FAILOVER CHAIN");
    for (const auto &route : config.routes) {
        table.row({route.model,
                   colorEnabled(route.enabled, route.enabled ? "enabled" : "disabled"),
                   renderTargets(route, config)});
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

    // `add` is additive: a model that already has a route gets the new hops
    // appended to its failover chain, a model without one gets a fresh route.
    literouter::RouteConfig *route = nullptr;
    for (auto &candidate : config.routes) {
        if (candidate.model == opts.model) {
            route = &candidate;
            break;
        }
    }
    const bool created = route == nullptr;
    literouter::RouteConfig fresh;
    if (created) {
        fresh.model = opts.model;
        fresh.enabled = !opts.noEnable;
        route = &fresh;
    }

    std::size_t added = 0;
    std::size_t skipped = 0;
    for (const auto &spec : opts.targets) {
        const auto sep = spec.find(':');
        literouter::RouteTarget target;
        target.provider = sep == std::string::npos ? spec : spec.substr(0, sep);
        target.model = sep == std::string::npos ? std::string{} : spec.substr(sep + 1);
        if (target.provider.empty()) {
            printError(std::format("--target `{}` has no relay id for route `{}`", spec,
                                   opts.model));
            ctx.exitCode = kExitConfig;
            return;
        }
        if (config.provider(target.provider) == nullptr) {
            printError(std::format("--target `{}` names relay `{}`, which is not in {}", spec,
                                   target.provider, storeOpt->path().string()));
            printHint(std::format("add it first with `literouter providers add {} --base-url …`",
                                  target.provider));
            ctx.exitCode = kExitConfig;
            return;
        }
        const bool duplicate = std::ranges::any_of(
            route->targets, [&](const literouter::RouteTarget &existing) {
                return existing.provider == target.provider && existing.model == target.model;
            });
        if (duplicate) {
            ++skipped;
            continue;
        }
        route->targets.push_back(std::move(target));
        ++added;
    }

    if (created) {
        config.routes.push_back(std::move(fresh));
    }
    if (!saveOrFail(ctx, *storeOpt)) {
        return;
    }

    const literouter::RouteConfig &saved = *config.route(opts.model);
    if (ctx.json) {
        json targets = json::array();
        for (const auto &target : saved.targets) {
            targets.push_back(json{{"provider", target.provider}, {"model", target.model}});
        }
        json root = json::object();
        root["changed"] = created ? "route-added" : "route-extended";
        root["path"] = storeOpt->path().string();
        root["model"] = saved.model;
        root["enabled"] = saved.enabled;
        root["hops_added"] = added;
        root["hops_skipped"] = skipped;
        root["targets"] = std::move(targets);
        std::println("{}", root.dump(2));
        return;
    }

    KeyValues info;
    info.add(created ? "added route" : "extended route", saved.model);
    info.add("hops added", std::format("{}", added));
    if (skipped > 0) {
        info.add("hops already present", std::format("{}", skipped));
    }
    info.add("state", colorEnabled(saved.enabled, saved.enabled ? "enabled" : "disabled"));
    info.add("chain", renderTargets(saved, config));
    info.add("saved to", storeOpt->path().string());
    info.print();
}

void runRemove(Context &ctx, const ToggleOptions &opts) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::AppConfig &config = storeOpt->config();
    if (config.route(opts.model) == nullptr) {
        printError(
            std::format("no route `{}` in {}", opts.model, storeOpt->path().string()));
        ctx.exitCode = kExitConfig;
        return;
    }
    std::erase_if(config.routes,
                  [&](const literouter::RouteConfig &r) { return r.model == opts.model; });
    if (!saveOrFail(ctx, *storeOpt)) {
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["changed"] = "route-removed";
        root["path"] = storeOpt->path().string();
        root["model"] = opts.model;
        std::println("{}", root.dump(2));
        return;
    }
    printInfo(std::format("removed route `{}` from {} ({} route(s) remain)", opts.model,
                          storeOpt->path().string(), config.routes.size()));
}

void runToggle(Context &ctx, const ToggleOptions &opts, bool enable) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::AppConfig &config = storeOpt->config();
    literouter::RouteConfig *route = nullptr;
    for (auto &candidate : config.routes) {
        if (candidate.model == opts.model) {
            route = &candidate;
            break;
        }
    }
    if (route == nullptr) {
        printError(std::format("no route `{}` in {}", opts.model, storeOpt->path().string()));
        ctx.exitCode = kExitConfig;
        return;
    }
    route->enabled = enable;
    if (!saveOrFail(ctx, *storeOpt)) {
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["changed"] = enable ? "route-enabled" : "route-disabled";
        root["path"] = storeOpt->path().string();
        root["model"] = opts.model;
        std::println("{}", root.dump(2));
        return;
    }
    printInfo(std::format("route `{}` is now {} in {}", opts.model,
                          colorEnabled(enable, enable ? "enabled" : "disabled"),
                          storeOpt->path().string()));
}

void registerList(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("list", "List the route table");
    sub->fallthrough();
    sub->callback([&ctx] { runList(ctx); });
}

void registerAdd(CLI::App &parent, Context &ctx) {
    auto opts = std::make_shared<AddOptions>();
    CLI::App *sub = parent.add_subcommand(
        "add", "Add a route, or append hops to an existing one");
    sub->add_option("model", opts->model, "The model name a client sends")->required();
    sub->add_option("--target", opts->targets,
                    "provider[:upstream-model] (repeatable, order = failover order)")
        ->required()
        ->expected(-1);
    sub->add_flag("--no-enable", opts->noEnable, "Add the route disabled");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runAdd(ctx, *opts); });
}

void registerRemove(CLI::App &parent, Context &ctx) {
    auto opts = std::make_shared<ToggleOptions>();
    CLI::App *sub = parent.add_subcommand("remove", "Remove a route");
    sub->add_option("model", opts->model, "The route's model name")->required();
    sub->fallthrough();
    sub->callback([&ctx, opts] { runRemove(ctx, *opts); });
}

void registerToggle(CLI::App &parent, Context &ctx, bool enable) {
    auto opts = std::make_shared<ToggleOptions>();
    const std::string name = enable ? "enable" : "disable";
    CLI::App *sub =
        parent.add_subcommand(name, enable ? "Enable a route" : "Disable a route");
    sub->add_option("model", opts->model, "The route's model name")->required();
    sub->fallthrough();
    sub->callback([&ctx, opts, enable] { runToggle(ctx, *opts, enable); });
}

} // namespace

void register_routes(CLI::App &root, Context &ctx) {
    CLI::App *routes = root.add_subcommand(
        "routes",
        std::string(literouter::i18n::tr("Manage routes (edits the config file, no server needed)")));
    routes->require_subcommand(1);
    routes->fallthrough();
    registerList(*routes, ctx);
    registerAdd(*routes, ctx);
    registerRemove(*routes, ctx);
    registerToggle(*routes, ctx, true);
    registerToggle(*routes, ctx, false);
}

} // namespace lrcli
