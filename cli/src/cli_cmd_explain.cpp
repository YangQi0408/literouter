#include <CLI/CLI.hpp>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"
#include "cli_json.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

struct ExplainOptions {
    std::string model;
};

std::string encodeQuery(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[ch >> 4]);
            encoded.push_back(hex[ch & 0x0f]);
        }
    }
    return encoded;
}

std::string reasonText(const json &candidate) {
    const std::string reason = candidate.value("reason", std::string{});
    if (reason == "circuit_open") return std::string{literouter::i18n::tr("breaker open; deferred")};
    if (reason == "measured_latency") {
        std::string text{literouter::i18n::tr("ordered by measured p95")};
        if (candidate.contains("latency_ms_p95")) {
            text += " (" + literouter::humanMillis(candidate.at("latency_ms_p95").get<double>()) + ")";
        } else {
            text += " (" + std::string{literouter::i18n::tr("not measured yet")} + ")";
        }
        return text;
    }
    if (reason == "configured_price") {
        std::string text{literouter::i18n::tr("ordered by configured price")};
        if (candidate.contains("price_usd_per_million")) {
            text += std::format(" (${:.4f}/M)", candidate.at("price_usd_per_million").get<double>());
        } else {
            text += " (" + std::string{literouter::i18n::tr("unpriced")} + ")";
        }
        return text;
    }
    if (reason == "weighted_round_robin") return std::string{literouter::i18n::tr("weighted round-robin selection")};
    return std::string{literouter::i18n::tr("declared candidate order")};
}

void runExplain(Context &ctx, const ExplainOptions &opts) {
    configureColor(ctx.noColor);
    auto store = loadStore(ctx);
    if (!store) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const auto reply = literouter::adminGet(
        baseUrlOf(store->config()), "/explain?model=" + encodeQuery(opts.model),
        store->config().server.api_key);
    if (!reply.reachable || reply.status < 200 || reply.status >= 300) {
        const std::string error = reply.reachable
            ? std::format("HTTP {}: {}", reply.status, reply.body)
            : reply.error;
        if (ctx.json) {
            json output = json::object();
            output["reachable"] = reply.reachable;
            output["status"] = reply.status;
            output["error"] = error;
            std::println("{}", dumpJson(output, 2));
        } else {
            printError(std::format("{} ({})", literouter::i18n::tr("could not explain route"), error));
            printHint(std::format("{} `{}`", literouter::i18n::tr("start the matching literouter instance"),
                                  literouter::pathToUtf8(store->path())));
        }
        ctx.exitCode = reply.reachable ? kExitFail : kExitUnreachable;
        return;
    }

    const json explanation = json::parse(reply.body, nullptr, false);
    if (explanation.is_discarded() || !explanation.is_object()) {
        printError(literouter::i18n::tr("the server returned an invalid route explanation"));
        ctx.exitCode = kExitFail;
        return;
    }
    if (ctx.json) {
        std::println("{}", dumpJson(explanation, 2));
        return;
    }

    KeyValues summary;
    summary.add(std::string{literouter::i18n::tr("model")}, opts.model);
    summary.add(std::string{literouter::i18n::tr("routing policy")},
                explanation.value("policy", std::string{"priority"}));
    summary.add(std::string{literouter::i18n::tr("attempt limit")},
                explanation.value("max_attempts", 0) == 0
                    ? std::string{literouter::i18n::tr("all candidates")}
                    : std::to_string(explanation.value("max_attempts", 0)));
    summary.print();

    const auto candidates = explanation.value("candidates", json::array());
    if (!candidates.is_array() || candidates.empty()) {
        printNote(std::string{literouter::i18n::tr("no relay can serve this model")}, false);
        return;
    }
    Table table;
    table.column(std::string{literouter::i18n::tr("ORDER")}, Align::Right);
    table.column(std::string{literouter::i18n::tr("RELAY")});
    table.column(std::string{literouter::i18n::tr("UPSTREAM MODEL")});
    table.column(std::string{literouter::i18n::tr("WEIGHT")}, Align::Right);
    table.column(std::string{literouter::i18n::tr("STATE")});
    table.column(std::string{literouter::i18n::tr("REASON")});
    for (const auto &candidate : candidates) {
        const bool skipped = candidate.value("skipped", false);
        const bool selected = candidate.value("selected", false);
        const std::string state = skipped
            ? std::string{literouter::i18n::tr("deferred")}
            : selected ? std::string{literouter::i18n::tr("first choice")}
                       : std::string{literouter::i18n::tr("failover")};
        table.row({std::to_string(candidate.value("rank", 0)),
                   candidate.value("provider", std::string{}),
                   candidate.value("model", std::string{}),
                   std::to_string(candidate.value("weight", 1)),
                   state,
                   reasonText(candidate)});
    }
    table.print();
    printNote(std::string{literouter::i18n::tr(
        "preview does not advance round-robin state or apply conversation affinity")}, false);
}

} // namespace

void register_explain(CLI::App &root, Context &ctx) {
    auto opts = std::make_shared<ExplainOptions>();
    CLI::App *sub = root.add_subcommand(
        "explain", std::string{literouter::i18n::tr("Explain how a model is routed")});
    sub->add_option("model", opts->model, lrcli::tr("Logical model name to inspect"))->required();
    sub->fallthrough();
    sub->callback([&ctx, opts] { runExplain(ctx, *opts); });
}

} // namespace lrcli
