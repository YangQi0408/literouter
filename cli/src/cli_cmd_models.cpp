#include <CLI/CLI.hpp>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

struct Hop {
    std::string provider;
    std::string model; // empty means pass the logical name through
    bool enabled = true;
};

struct ModelRow {
    std::string model;
    std::string source; // "route" | "pass-through"
    bool enabled = true;
    std::vector<Hop> hops;
};

std::vector<Hop> passThroughProviders(const literouter::AppConfig &config,
                                      const std::string &model) {
    std::vector<const literouter::ProviderConfig *> candidates;
    for (const auto &provider : config.providers) {
        if (!provider.enabled) {
            continue;
        }
        if (std::ranges::find(provider.models, model) != provider.models.end()) {
            candidates.push_back(&provider);
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto *a, const auto *b) { return a->priority < b->priority; });
    std::vector<Hop> hops;
    for (const auto *provider : candidates) {
        hops.push_back(Hop{.provider = provider->id, .model = {}, .enabled = true});
    }
    return hops;
}

std::vector<ModelRow> buildRows(const literouter::AppConfig &config, bool showAll = false) {
    std::vector<ModelRow> rows;
    for (const auto &model : showAll ? config.allModels() : config.logicalModels()) {
        ModelRow row;
        row.model = model;
        if (const literouter::RouteConfig *route = config.route(model); route != nullptr) {
            row.source = route->enabled ? "route" : "route (disabled)";
            row.enabled = route->enabled;
            for (const auto &target : route->targets) {
                const literouter::ProviderConfig *provider = config.provider(target.provider);
                row.hops.push_back(Hop{.provider = target.provider,
                                       .model = target.model,
                                       .enabled = provider != nullptr && provider->enabled});
            }
        } else {
            row.source = "pass-through";
            row.hops = passThroughProviders(config, model);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string renderChain(const std::vector<Hop> &hops) {
    if (hops.empty()) {
        return dim("(no relay advertises this name)");
    }
    std::string out;
    for (std::size_t i = 0; i < hops.size(); ++i) {
        if (i > 0) {
            out += " → ";
        }
        std::string hop = hops[i].provider;
        if (!hops[i].model.empty()) {
            hop += ":" + hops[i].model;
        }
        if (!hops[i].enabled) {
            out += dim(hop) + dim(" (disabled)");
        } else {
            out += hop;
        }
    }
    return out;
}

void runModels(Context &ctx, bool showAll = false) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const literouter::AppConfig &config = storeOpt->config();
    const std::vector<ModelRow> rows = buildRows(config, showAll);

    if (ctx.json) {
        json array = json::array();
        for (const auto &row : rows) {
            json hops = json::array();
            for (const auto &hop : row.hops) {
                json node = json::object();
                node["provider"] = hop.provider;
                node["model"] = hop.model;
                node["disabled"] = !hop.enabled;
                hops.push_back(std::move(node));
            }
            json node = json::object();
            node["model"] = row.model;
            node["source"] = row.source;
            node["enabled"] = row.enabled;
            node["chain"] = std::move(hops);
            array.push_back(std::move(node));
        }
        json root = json::object();
        root["models"] = std::move(array);
        std::println("{}", root.dump(2));
        return;
    }

    if (rows.empty()) {
        printInfo("no logical models: add a relay with `literouter providers add`, or a route "
                  "with `literouter routes add`");
        return;
    }

    Table table;
    table.column("MODEL");
    table.column("SOURCE");
    table.column("FAILOVER CHAIN");
    for (const auto &row : rows) {
        table.row({row.model, row.source, renderChain(row.hops)});
    }
    table.print();
}

struct ModelsOptions {
    bool all = false;
};

} // namespace

void register_models(CLI::App &root, Context &ctx) {
    auto opts = std::make_shared<ModelsOptions>();
    CLI::App *sub = root.add_subcommand(
        "models", std::string(literouter::i18n::tr("List every logical model and the ordered relays that can serve it")));
    sub->add_flag("-a,--all", opts->all, "Include unrouted provider models in pass-through mode");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runModels(ctx, opts->all); });
}

} // namespace lrcli
