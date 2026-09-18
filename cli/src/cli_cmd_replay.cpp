// `literouter replay` — take a request body a client sent (or one you wrote) and
// run it again against a chosen relay, or against whatever the routing policy
// would pick. That is the question `bench` does not answer: not "which relay is
// fastest" but "what happens to *this* request".
//
// The body is read from a file rather than from the log on purpose. A logged
// body has been truncated to `log_body_limit` and had credentials masked, so
// replaying it would send a different request than the one that was logged —
// which is worse than not replaying it at all.
#include <CLI/CLI.hpp>

#include "cli_context.hpp"
#include "cli_ui.hpp"

#include <fstream>
#include <iterator>

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

struct ReplayOptions {
    std::string file;
    std::string provider;
    std::string model;
    int timeout = 0;
    bool show = false;
    bool json = false;
};

void runReplay(Context &ctx, const ReplayOptions &opts) {
    configureColor(ctx.noColor);
    auto store = loadStore(ctx);
    if (!store) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const literouter::AppConfig &config = store->config();

    if (opts.file.empty()) {
        printError("replay needs a request body: pass --file request.json");
        printHint("the body is an OpenAI chat request, exactly as the client would send it");
        ctx.exitCode = kExitConfig;
        return;
    }

    std::ifstream input{opts.file};
    if (!input) {
        printError(std::format("cannot read {}", opts.file));
        ctx.exitCode = kExitConfig;
        return;
    }
    const std::string body_text{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
    const json body = json::parse(body_text, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        printError(std::format("{} is not a JSON object", opts.file));
        ctx.exitCode = kExitConfig;
        return;
    }

    const std::string model = opts.model.empty() ? body.value("model", std::string{}) : opts.model;
    if (model.empty()) {
        printError("no model to replay: pass --model, or put one in the body");
        ctx.exitCode = kExitConfig;
        return;
    }

    // Which relay: the one asked for, or the first the policy would try. The
    // second is the interesting one, because it shows what the proxy itself
    // would have done with this request.
    const literouter::ProviderConfig *provider = nullptr;
    std::string upstream_model = model;
    const std::vector<literouter::Candidate> chain = routingOrder(config, model);
    if (!opts.provider.empty()) {
        provider = config.provider(opts.provider);
        if (provider == nullptr) {
            printError(std::format("no relay `{}` in {}", opts.provider, configPathFor(ctx).string()));
            ctx.exitCode = kExitConfig;
            return;
        }
        for (const auto &candidate : chain) {
            if (candidate.provider == opts.provider && !candidate.model.empty()) {
                upstream_model = candidate.model;
                break;
            }
        }
    } else if (!chain.empty()) {
        provider = config.provider(chain.front().provider);
        if (!chain.front().model.empty()) {
            upstream_model = chain.front().model;
        }
    }
    if (provider == nullptr) {
        printError(std::format("no relay can serve `{}`", model));
        printHint("add it to a route, or name one with --provider");
        ctx.exitCode = kExitConfig;
        return;
    }

    // The same two steps the proxy takes: translate into the relay's protocol
    // (a no-op for an OpenAI-compatible one) and pick its path.
    json upstream_body = body;
    upstream_body["model"] = upstream_model;
    const std::string payload =
        literouter::adaptChatRequest(*provider, upstream_model, upstream_body.dump(),
                                     /*stream=*/false);
    const std::string path = literouter::resolveChatPath(*provider, upstream_model,
                                                        /*stream=*/false);

    printNote(std::format("replaying against {} ({})", provider->id, provider->base_url), ctx.quiet);
    if (upstream_model != model) {
        printNote(std::format("  the route renames `{}` to `{}` upstream", model, upstream_model),
                  ctx.quiet);
    }

    const literouter::UpstreamResult result =
        literouter::upstreamPost(*provider, path, payload, opts.timeout);
    if (!result.ok) {
        printError(std::format("{}: {}", provider->id, result.error));
        ctx.exitCode = kExitFail;
        return;
    }

    literouter::ProviderStat usage;
    literouter::ProxyServer::accumulateUsage(result.body, usage);
    const double cost = literouter::estimateCost(provider->price_in_per_million,
                                                 provider->price_out_per_million,
                                                 usage.tokens_prompt, usage.tokens_completion);

    if (opts.json) {
        std::println("{}", json{{"provider", provider->id},
                                {"upstream_model", upstream_model},
                                {"status", result.status},
                                {"latency_ms", result.latency_ms},
                                {"bytes", result.body.size()},
                                {"prompt_tokens", usage.tokens_prompt},
                                {"completion_tokens", usage.tokens_completion},
                                {"cost_usd", cost},
                                {"body", opts.show ? json(result.body) : json()}}
                            .dump(2));
        if (result.status < 200 || result.status >= 300) {
            ctx.exitCode = kExitFail;
        }
        return;
    }

    KeyValues info;
    info.add(literouter::i18n::tr("RELAY"), provider->id);
    info.add(literouter::i18n::tr("STATUS"),
             colorStatus(result.status, std::to_string(result.status)));
    info.add(literouter::i18n::tr("LATENCY"), literouter::humanMillis(result.latency_ms));
    info.add(literouter::i18n::tr("BYTES"), literouter::humanBytes(result.body.size()));
    info.add(literouter::i18n::tr("TOKENS"),
             std::format("{} prompt + {} completion", usage.tokens_prompt,
                         usage.tokens_completion));
    if (cost > 0.0) {
        info.add(literouter::i18n::tr("COST"), std::format("${:.4f}", cost));
    }
    info.print();

    // A relay that speaks another protocol answered in that protocol; showing
    // the Chat form keeps one shape on screen whatever the relay is.
    const std::string shown = literouter::adaptChatResponse(*provider, result.body, model);
    if (opts.show) {
        std::println("{}", shown);
    } else {
        printNote("pass --show to print the answer body", ctx.quiet);
    }
    if (result.status < 200 || result.status >= 300) {
        ctx.exitCode = kExitFail;
    }
}

} // namespace

void register_replay(CLI::App &root, Context &ctx) {
    auto opts = std::make_shared<ReplayOptions>();
    CLI::App *sub = root.add_subcommand(
        "replay", std::string(literouter::i18n::tr("Send a saved request body to one relay")));
    sub->add_option("--file", opts->file, "Request body to replay (OpenAI chat JSON)");
    sub->add_option("--provider", opts->provider,
                    "Relay to send it to (default: the first the policy would try)");
    sub->add_option("--model", opts->model, "Model to ask for (default: the body's own)");
    sub->add_option("--timeout", opts->timeout,
                    "Per-request timeout in seconds (default: the relay's own)");
    sub->add_flag("--show", opts->show, "Print the answer body");
    sub->add_flag("--json", opts->json, "Print the outcome as JSON");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runReplay(ctx, *opts); });
}

} // namespace lrcli
