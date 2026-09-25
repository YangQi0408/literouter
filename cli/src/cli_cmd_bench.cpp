// `literouter bench` — one prompt, every relay that could serve it, side by
// side. The numbers an operator otherwise guesses at: how long each relay takes
// to answer the same question, and what it charges for the tokens it reports.
//
// It reaches upstream directly (through core's single-hop caller) rather than
// through the proxy, because the point is to compare the relays and not the
// routing decision — `replay` is the command for the other question. These are
// real requests against real relays, which is why the command says so before it
// starts.
#include <CLI/CLI.hpp>

#include "cli_context.hpp"
#include "cli_ui.hpp"

#include <algorithm>
#include <numeric>

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"
#include "cli_json.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

struct BenchOptions {
    std::string model;
    std::string prompt = "Reply with the single word: ok";
    int runs = 1;
    int timeout = 30;
    int maxTokens = 16;
};

struct BenchRow {
    std::string provider;
    std::string upstreamModel;
    int runs = 0;
    int failures = 0;
    int lastStatus = 0;
    double latencyMin = 0.0;
    double latencyAvg = 0.0;
    std::uint64_t promptTokens = 0;
    std::uint64_t completionTokens = 0;
    double cost = 0.0;
    std::string error;
};

// The body a client would send, before any protocol adaptation: the smallest
// request that still exercises the relay's real path.
std::string benchBody(const std::string &model, const std::string &prompt, int maxTokens) {
    json body = json::object();
    body["model"] = model;
    body["max_tokens"] = maxTokens;
    body["messages"] = json::array({{{"role", "user"}, {"content", prompt}}});
    return dumpJson(body);
}

BenchRow runBench(const literouter::ProviderConfig &provider, const literouter::Candidate &candidate,
                  const BenchOptions &opts) {
    BenchRow row;
    row.provider = provider.id;
    row.upstreamModel = candidate.model.empty() ? opts.model : candidate.model;

    const std::string chat_body = benchBody(row.upstreamModel, opts.prompt, opts.maxTokens);
    // A relay that speaks another protocol needs the request translated first;
    // for OpenAI-compatible relays adaptChatRequest() hands the body back as it
    // is, so this is one path rather than two.
    const std::string payload = literouter::adaptChatRequest(provider, row.upstreamModel, chat_body,
                                                             /*stream=*/false);
    const std::string path = literouter::resolveChatPath(provider, row.upstreamModel,
                                                         /*stream=*/false);

    double total_latency = 0.0;
    for (int run = 0; run < opts.runs; ++run) {
        const literouter::UpstreamResult result =
            literouter::upstreamPost(provider, path, payload, opts.timeout);
        ++row.runs;
        if (!result.ok) {
            ++row.failures;
            row.error = result.error;
            continue;
        }
        row.lastStatus = result.status;
        total_latency += result.latency_ms;
        if (row.latencyMin == 0.0 || result.latency_ms < row.latencyMin) {
            row.latencyMin = result.latency_ms;
        }
        literouter::ProviderStat usage;
        literouter::ProxyServer::accumulateUsage(result.body, usage);
        row.promptTokens += usage.tokens_prompt;
        row.completionTokens += usage.tokens_completion;
        if (result.status < 200 || result.status >= 300) {
            ++row.failures;
        }
    }
    if (row.runs > 0) {
        row.latencyAvg = total_latency / static_cast<double>(row.runs);
    }
    const auto [p_in, p_out] = provider.pricesFor(opts.model);
    row.cost = literouter::estimateCost(p_in, p_out, row.promptTokens,
                                        row.completionTokens);
    return row;
}

void runBench(Context &ctx, const BenchOptions &opts) {
    configureColor(ctx.noColor);
    auto store = loadStore(ctx);
    if (!store) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const literouter::AppConfig &config = store->config();

    if (opts.model.empty()) {
        printError("bench needs the model to ask every relay for");
        printHint("`literouter models list` shows what the configured relays advertise");
        ctx.exitCode = kExitConfig;
        return;
    }

    const std::vector<literouter::Candidate> chain = routingOrder(config, opts.model);
    if (chain.empty()) {
        printError(std::format("no relay can serve `{}`", opts.model));
        printHint("add it to a route, or list it in a relay's `models` array");
        ctx.exitCode = kExitConfig;
        return;
    }

    if (ctx.json) {
        json rows = json::array();
        for (const auto &candidate : chain) {
            const literouter::ProviderConfig *provider = config.provider(candidate.provider);
            if (provider == nullptr) {
                continue;
            }
            const BenchRow row = runBench(*provider, candidate, opts);
            rows.push_back({{"provider", row.provider},
                            {"upstream_model", row.upstreamModel},
                            {"runs", row.runs},
                            {"failures", row.failures},
                            {"status", row.lastStatus},
                            {"latency_ms_min", row.latencyMin},
                            {"latency_ms_avg", row.latencyAvg},
                            {"prompt_tokens", row.promptTokens},
                            {"completion_tokens", row.completionTokens},
                            {"cost_usd", row.cost},
                            {"error", row.error}});
        }
        const json summary{{"model", opts.model}, {"runs", opts.runs}, {"results", rows}};
        std::println("{}", dumpJson(summary, 2));
        return;
    }

    // stdout is block-buffered once it is a pipe or a file, so without this the
    // warning (stderr) would land after the table it is warning about.
    std::cout.flush();
    printWarn(std::format("sending {} real request(s) per relay for `{}` — this costs money on "
                          "every account it touches",
                          opts.runs, opts.model));
    std::cout.flush();
    Table table;
    table.column(literouter::i18n::tr("RELAY"));
    table.column(literouter::i18n::tr("STATUS"));
    table.column(literouter::i18n::tr("LATENCY"), Align::Right);
    table.column(literouter::i18n::tr("BEST"), Align::Right);
    table.column(literouter::i18n::tr("TOKENS"), Align::Right);
    table.column(literouter::i18n::tr("COST"), Align::Right);

    std::vector<BenchRow> rows;
    for (const auto &candidate : chain) {
        const literouter::ProviderConfig *provider = config.provider(candidate.provider);
        if (provider == nullptr) {
            continue;
        }
        BenchRow row = runBench(*provider, candidate, opts);
        const bool ok = row.failures == 0 && row.lastStatus >= 200 && row.lastStatus < 300;
        table.row({row.provider + (row.upstreamModel != opts.model
                                       ? std::format(" → {}", row.upstreamModel)
                                       : std::string{}),
                   ok ? colorStatus(row.lastStatus, std::to_string(row.lastStatus))
                      : colorStatus(row.lastStatus,
                                    row.error.empty() ? std::format("{} failed", row.failures)
                                                      : row.error),
                   ok ? literouter::humanMillis(row.latencyAvg) : "—",
                   ok ? literouter::humanMillis(row.latencyMin) : "—",
                   ok ? literouter::humanCount(row.promptTokens + row.completionTokens) : "—",
                   row.cost > 0.0 ? std::format("${:.4f}", row.cost) : "—"});
        rows.push_back(std::move(row));
    }
    table.print();

    // The two questions the table is for, answered in one line each.
    const auto fastest = std::ranges::min_element(rows, [](const BenchRow &a, const BenchRow &b) {
        if (a.failures > 0) return false;
        if (b.failures > 0) return true;
        return a.latencyAvg < b.latencyAvg;
    });
    const auto cheapest = std::ranges::min_element(rows, [](const BenchRow &a, const BenchRow &b) {
        if (a.cost <= 0.0) return false;
        if (b.cost <= 0.0) return true;
        return a.cost < b.cost;
    });
    if (fastest != rows.end() && fastest->failures == 0) {
        printInfo(std::format("fastest: {} ({})", fastest->provider,
                              literouter::humanMillis(fastest->latencyAvg)));
    }
    if (cheapest != rows.end() && cheapest->cost > 0.0) {
        printInfo(std::format("cheapest here: {} (${:.4f})", cheapest->provider, cheapest->cost));
    }
    bool priced = false;
    for (const auto &row : rows) {
        if (row.cost > 0.0) {
            priced = true;
        }
    }
    if (!priced) {
        printNote("no relay has a price written down, so the cost column is empty: add "
                  "price_in_per_million / price_out_per_million to compare spend",
                  ctx.quiet);
    }
}

} // namespace

void register_bench(CLI::App &root, Context &ctx) {
    auto opts = std::make_shared<BenchOptions>();
    CLI::App *sub = root.add_subcommand(
        "bench", std::string(literouter::i18n::tr("Ask every relay that serves a model the same "
                                                  "question")));
    sub->add_option("--model", opts->model, lrcli::tr("Model to benchmark (required)"));
    sub->add_option("--prompt", opts->prompt,
                    lrcli::tr("Prompt to send (default: a one-word request)"));
    sub->add_option("--runs", opts->runs, lrcli::tr("Requests per relay (default 1)"));
    sub->add_option("--timeout", opts->timeout,
                    lrcli::tr("Per-request timeout in seconds (default 30)"));
    sub->add_option("--max-tokens", opts->maxTokens,
                    lrcli::tr("max_tokens to ask for (default 16)"));
    sub->fallthrough();
    sub->callback([&ctx, opts] { runBench(ctx, *opts); });
}

} // namespace lrcli
