#include <CLI/CLI.hpp>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

json snapshotJson(const literouter::AdminStatus &status, const std::string &url) {
    json root = json::parse(literouter::toJsonString(status.snapshot), nullptr, false);
    if (root.is_discarded()) {
        root = json::object();
    }
    root["reachable"] = status.reachable;
    root["url"] = url;
    return root;
}

void printMetrics(const literouter::Snapshot &s) {
    KeyValues metrics;
    metrics.add(std::string(literouter::i18n::tr("requests")),
                std::format("{}  (successes {}  failures {}  in-flight {})",
                            s.total_requests, s.total_success, s.total_failure,
                            s.active_requests));
    metrics.add(std::string(literouter::i18n::tr("avg latency")),
                s.total_requests == 0 ? "—" : literouter::humanMillis(s.latency_ms_avg));
    metrics.add(std::string(literouter::i18n::tr("tokens")),
                std::format("{} prompt + {} completion",
                            literouter::humanCount(s.tokens_prompt),
                            literouter::humanCount(s.tokens_completion)));
    metrics.add(std::string(literouter::i18n::tr("bytes out")), literouter::humanBytes(s.bytes_out));
    metrics.add(std::string(literouter::i18n::tr("breakers")),
                std::format("{} {}", s.breakers_open, literouter::i18n::tr("open")));
    metrics.print();
}

void printRelays(const literouter::Snapshot &s) {
    if (s.providers.empty() && s.health.empty()) {
        printNote("  no relay has served a request yet.", false);
        return;
    }
    std::map<std::string, const literouter::ProviderHealth *> health;
    for (const auto &entry : s.health) {
        health.emplace(entry.provider, &entry);
    }

    Table table;
    table.column(std::string(literouter::i18n::tr("RELAY")));
    table.column(std::string(literouter::i18n::tr("STATE")));
    table.column(std::string(literouter::i18n::tr("REQUESTS")), Align::Right);
    table.column(std::string(literouter::i18n::tr("SUCCESS")), Align::Right);
    table.column(std::string(literouter::i18n::tr("AVG")), Align::Right);
    table.column(std::string(literouter::i18n::tr("FAILOVER")), Align::Right);

    // Union of both vectors, ordered as the server reported them.
    std::vector<std::string> ids;
    for (const auto &stat : s.providers) {
        ids.push_back(stat.provider);
    }
    for (const auto &h : s.health) {
        if (std::ranges::find(ids, h.provider) == ids.end()) {
            ids.push_back(h.provider);
        }
    }
    for (const auto &id : ids) {
        const literouter::ProviderStat *stat = nullptr;
        for (const auto &candidate : s.providers) {
            if (candidate.provider == id) {
                stat = &candidate;
                break;
            }
        }
        const std::string state = health.contains(id) ? health.at(id)->stateName() : "unknown";
        const std::uint64_t requests = stat ? stat->requests : 0;
        const std::uint64_t successes = stat ? stat->successes : 0;
        const std::string success =
            requests == 0 ? "—" : std::format("{:.0f}%", 100.0 * static_cast<double>(successes) /
                                                          static_cast<double>(requests));
        const std::string avg = (stat && stat->requests > 0)
                                    ? literouter::humanMillis(stat->latency_ms_avg)
                                    : "—";
        table.row({id, colorHealth(state, state), literouter::humanCount(requests), success, avg,
                   stat ? literouter::humanCount(stat->retries_in) : "0"});
    }
    table.print();
}

void runStatus(Context &ctx) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const literouter::AppConfig &config = storeOpt->config();
    const std::string url = baseUrlOf(config);

    const literouter::AdminStatus status = literouter::fetchStatus(url, config.server.api_key);

    if (ctx.json) {
        json root = snapshotJson(status, url);
        if (!status.reachable) {
            root["error"] = status.error;
        }
        std::println("{}", root.dump(2));
        if (!status.reachable) {
            ctx.exitCode = kExitUnreachable;
        }
        return;
    }

    if (!status.reachable) {
        printError(std::format("no literouter instance answered at {} ({})", url, status.error));
        printHint(std::format("start one with `literouter serve`, or pass --config pointing at "
                              "the file it uses (currently `{}`)",
                              storeOpt->path().string()));
        ctx.exitCode = kExitUnreachable;
        return;
    }

    const literouter::Snapshot &s = status.snapshot;
    KeyValues header;
    header.add("address", url);
    header.add("version", s.version.empty() ? "—" : s.version);
    header.add("uptime", literouter::humanUptime(s.uptime_sec));
    header.add("config", s.config_path.empty() ? "—" : s.config_path);
    header.print();

    std::println("");
    printMetrics(s);
    std::println("");
    printRelays(s);
}

} // namespace

void register_status(CLI::App &root, Context &ctx) {
    CLI::App *sub = root.add_subcommand(
        "status", std::string(literouter::i18n::tr("Query a running instance for its telemetry")));
    sub->fallthrough();
    sub->callback([&ctx] { runStatus(ctx); });
}

} // namespace lrcli
