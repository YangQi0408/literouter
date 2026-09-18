#include <CLI/CLI.hpp>
#include <csignal>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;

#include "cli_core.hpp"

namespace lrcli {
namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handleSignal(int) { g_stop_requested.store(true); }

struct ServeOptions {
    std::string host;
    int port = -1;
    std::string tlsCertFile;
    std::string tlsKeyFile;
    // Neither flag given keeps server.web_ui from the file, so a one-off run
    // never silently overrides what the operator wrote down.
    bool webUiOn = false;
    bool webUiOff = false;
    // Same for server.persist_telemetry: --no-persist is the one that matters,
    // a one-off run that must leave nothing on disk.
    bool persistOn = false;
    bool persistOff = false;
    bool force = false;
    bool printConfig = false;
    bool check = false;
};

int enabledRelays(const literouter::AppConfig &config) {
    return static_cast<int>(
        std::ranges::count_if(config.providers, [](const auto &p) { return p.enabled; }));
}

void runServe(Context &ctx, const ServeOptions &opts) {
    configureColor(ctx.noColor);
    const std::filesystem::path path = configPathFor(ctx);

    auto storeOpt = loadStoreAt(path);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::ConfigStore &store = *storeOpt;

    literouter::AppConfig config = store.config();
    if (opts.webUiOn) {
        config.server.web_ui = true;
    }
    if (opts.webUiOff) {
        config.server.web_ui = false;
    }
    if (opts.persistOn) {
        config.server.persist_telemetry = true;
    }
    if (opts.persistOff) {
        config.server.persist_telemetry = false;
    }
    if (!opts.host.empty()) {
        config.server.host = opts.host;
    }
    if (!opts.tlsCertFile.empty()) config.server.tls_cert_file = opts.tlsCertFile;
    if (!opts.tlsKeyFile.empty()) config.server.tls_key_file = opts.tlsKeyFile;
    if (opts.port >= 0) {
        config.server.port = opts.port;
    }

    if (!store.existsOnDisk()) {
        printWarn(std::format("config `{}` does not exist; running with the built-in seed",
                              path.string()));
        printNote("  create it with `literouter config init` once you have a relay to point at.",
                  ctx.quiet);
    }

    const literouter::ValidationReport report = literouter::validate(config);
    if (!report.issues.empty()) {
        printIssues(report, std::cerr);
    }
    if (!report.ok() && !opts.force) {
        printError(std::format("config `{}` did not pass validation ({}); refusing to serve",
                               path.string(), report.summary()));
        printHint("fix the issues above, or run `literouter serve --force` to start anyway");
        ctx.exitCode = kExitConfig;
        return;
    }

    if (opts.printConfig) {
        std::println("{}", literouter::toJsonString(config));
        return;
    }

    if (opts.check) {
        printInfo(std::format("{}: {} — {}", path.string(), report.summary(),
                              report.ok() ? paint("ok", "32") : paint("errors", "31")));
        std::println("listen would be {}", baseUrlOf(config));
        return;
    }

    literouter::ProxyServer server;
    server.setConfigPath(path.string());
    if (auto started = server.start(config); !started) {
        printError(std::format("cannot start the proxy on {}: {}", baseUrlOf(config),
                               literouter::i18n::tr(started.error())));
        printHint(std::format("check for another process on that port, or change `server.port` "
                              "in {}",
                              path.string()));
        ctx.exitCode = kExitFail;
        return;
    }

    // The port may have been kernel-assigned when the config said 0, so report
    // the address actually bound rather than the requested one.
    const std::string address = server.snapshot().base_url;

    KeyValues info;
    info.add("listen", address);
    info.add("client base", std::format("{}/v1", address));
    info.add("config", path.string());
    info.add("relays", std::format("{} enabled of {}", enabledRelays(config),
                                   config.providers.size()));
    info.print();
    std::println("");
    printInfo(paint(std::format("point your OpenAI-compatible client at {}/v1", address), "32"));
    printNote("serving in the foreground; press Ctrl-C to stop.", ctx.quiet);
    // stdout is block-buffered when redirected to a file; without this the
    // startup block would only appear when the process exits.
    std::cout.flush();

    g_stop_requested.store(false);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    while (!g_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!ctx.quiet) {
        std::println("");
    }
    printInfo("stopping…");
    server.stop();
    printInfo(paint("stopped cleanly", "32"));
}

} // namespace

void register_serve(CLI::App &root, Context &ctx) {
    auto opts = std::make_shared<ServeOptions>();
    CLI::App *sub = root.add_subcommand(
        "serve", std::string(literouter::i18n::tr("Run the proxy in the foreground")));
    sub->add_option("--host", opts->host, "Override server.host for this run only");
    sub->add_option("--port", opts->port, "Override server.port for this run only");
    sub->add_option("--tls-cert", opts->tlsCertFile,
        std::string(literouter::i18n::tr("PEM certificate chain absolute path for HTTPS")));
    sub->add_option("--tls-key", opts->tlsKeyFile,
        std::string(literouter::i18n::tr("Unencrypted PEM private key absolute path for HTTPS")));
    CLI::Option *no_web_ui =
        sub->add_flag("--no-web-ui", opts->webUiOff,
                      "Do not serve the built-in console at /ui for this run only");
    sub->add_flag("--web-ui", opts->webUiOn,
                  "Serve the built-in console at /ui for this run only")
        ->excludes(no_web_ui);
    CLI::Option *no_persist =
        sub->add_flag("--no-persist", opts->persistOff,
                      "Do not persist counters and the request log for this run only");
    sub->add_flag("--persist", opts->persistOn,
                  "Persist counters and the request log for this run only")
        ->excludes(no_persist);
    sub->add_flag("--force", opts->force, "Run even when validate() reports errors");
    sub->add_flag("--print-config", opts->printConfig,
                  "Dump the effective config JSON and exit");
    sub->add_flag("--check", opts->check, "Load and validate, then exit without binding");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runServe(ctx, *opts); });
}

} // namespace lrcli
