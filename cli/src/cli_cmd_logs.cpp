#include <CLI/CLI.hpp>

#include <atomic>
#include <csignal>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

std::atomic<bool> g_stop_requested{false};

extern "C" void handleLogsSignal(int) { g_stop_requested.store(true); }

struct LogsOptions {
    std::size_t limit = 50;
    std::uint64_t since = 0;
    bool follow = false;
    std::string level;
};

std::string entryModelRelay(const literouter::LogEntry &entry) {
    if (entry.model.empty() && entry.provider.empty()) {
        return "—";
    }
    if (entry.provider.empty()) {
        return entry.model;
    }
    if (entry.model.empty()) {
        return "→ " + entry.provider;
    }
    return entry.model + " → " + entry.provider;
}

std::vector<literouter::LogEntry> filterByLevel(std::vector<literouter::LogEntry> entries,
                                                const std::string &level) {
    if (level.empty()) {
        return entries;
    }
    std::erase_if(entries, [&](const literouter::LogEntry &entry) { return entry.level != level; });
    return entries;
}

json entriesJson(const std::vector<literouter::LogEntry> &entries) {
    json array = json::array();
    for (const auto &entry : entries) {
        json node = json::parse(literouter::toJsonString(entry), nullptr, false);
        if (!node.is_discarded()) {
            array.push_back(std::move(node));
        }
    }
    return array;
}

void renderEntries(const std::vector<literouter::LogEntry> &entries) {
    if (entries.empty()) {
        printNote("  no log entries", false);
        return;
    }
    Table table;
    table.column("TIME");
    table.column("LEVEL");
    table.column("REQUEST");
    table.column("KIND");
    table.column("MODEL → RELAY");
    table.column("STATUS", Align::Right);
    table.column("LATENCY", Align::Right);
    table.column("BYTES", Align::Right);
    table.column("MESSAGE");
    for (const auto &entry : entries) {
        table.row({std::format("{} {}", entry.dateText(), entry.timeText()),
                   colorLevel(entry.level, entry.level),
                   entry.request_id.empty() ? "—" : entry.request_id,
                   entry.kind.empty() ? "—" : entry.kind,
                   entryModelRelay(entry),
                   entry.status == 0 ? "—" : colorStatus(entry.status, std::to_string(entry.status)),
                   literouter::humanMillis(entry.latency_ms),
                   literouter::humanBytes(entry.bytes),
                   literouter::truncateUtf8(entry.message, 60)});
    }
    table.print();
}

// Explains an unreachable server and sets the exit-4 code. Shared by the one-shot
// and follow paths so the message cannot drift.
void reportUnreachable(Context &ctx, const literouter::AdminLogs &page, const std::string &url,
                       const std::string &configPath) {
    if (ctx.json) {
        json root = json::object();
        root["reachable"] = false;
        root["url"] = url;
        root["error"] = page.error;
        std::println("{}", root.dump(2));
    } else {
        printError(std::format("cannot read the log from {} ({})", url, page.error));
        printHint(std::format("start it with `literouter serve`, or pass --config pointing at the "
                              "file it uses (currently `{}`)",
                              configPath));
    }
    ctx.exitCode = kExitUnreachable;
}

void runLogs(Context &ctx, const LogsOptions &opts) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const std::string url = baseUrlOf(storeOpt->config());
    const std::string apiKey = storeOpt->config().server.api_key;

    if (!opts.follow) {
        const literouter::AdminLogs page =
            literouter::fetchLogs(url, opts.since, opts.limit, apiKey);
        if (!page.reachable) {
            reportUnreachable(ctx, page, url, storeOpt->path().string());
            return;
        }
        const auto entries = filterByLevel(page.entries, opts.level);
        if (ctx.json) {
            json root = json::object();
            root["seq"] = page.seq;
            root["entries"] = entriesJson(entries);
            std::println("{}", root.dump(2));
        } else {
            renderEntries(entries);
        }
        return;
    }

    // --follow: poll the admin log endpoint and print whatever is new. since==0
    // asks core for the tail on the first poll; afterwards `seq` is fed back so
    // each poll returns only what came after. Ctrl-C ends the loop. With --json
    // each new entry is printed as its own JSON object on its own line
    // (newline-delimited JSON), the usual shape for a stream.
    g_stop_requested.store(false);
    std::signal(SIGINT, handleLogsSignal);
    std::signal(SIGTERM, handleLogsSignal);

    std::uint64_t since = opts.since;
    bool first = true;
    while (!g_stop_requested.load()) {
        const literouter::AdminLogs page =
            literouter::fetchLogs(url, since, opts.limit, apiKey);
        if (!page.reachable) {
            printError(std::format("log stream from {} stopped ({})", url, page.error));
            ctx.exitCode = kExitUnreachable;
            return;
        }
        const auto entries = filterByLevel(page.entries, opts.level);
        if (!entries.empty()) {
            if (ctx.json) {
                for (const auto &entry : entries) {
                    std::println("{}", literouter::toJsonString(entry));
                }
            } else {
                if (first) {
                    std::println("{}", dim(std::format("following {} … (Ctrl-C to stop)", url)));
                }
                renderEntries(entries);
            }
            first = false;
            for (const auto &entry : entries) {
                since = std::max(since, entry.seq);
            }
            // Keep a redirected `--follow` stream live rather than buffered.
            std::cout.flush();
        }
        since = std::max(since, page.seq);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!ctx.quiet && !ctx.json) {
        printInfo("\nstopped following");
    }
}

} // namespace

void register_logs(CLI::App &root, Context &ctx) {
    auto opts = std::make_shared<LogsOptions>();
    CLI::App *sub = root.add_subcommand(
        "logs", std::string(literouter::i18n::tr("Read (and optionally follow) the request log")));
    sub->add_option("--limit", opts->limit, "Maximum entries to fetch (default 50)")
        ->default_val(50);
    sub->add_option("--since", opts->since, "Start after this sequence number (0 = newest)");
    sub->add_flag("--follow", opts->follow, "Poll every 250ms and print new entries until Ctrl-C");
    sub->add_option("--level", opts->level, "Only entries at this level")
        ->check(CLI::IsMember({"info", "warn", "error"}));
    sub->fallthrough();
    sub->callback([&ctx, opts] { runLogs(ctx, *opts); });
}

} // namespace lrcli
