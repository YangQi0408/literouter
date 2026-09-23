#include <CLI/CLI.hpp>

#include <iostream>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;

int main(int argc, char **argv) {
    literouter::ensureLocalTimezone();
    std::string explicitLang;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--lang" && i + 1 < argc) {
            explicitLang = argv[i + 1];
            break;
        } else if (arg.starts_with("--lang=")) {
            explicitLang = arg.substr(7);
            break;
        } else if (arg == "-l" && i + 1 < argc) {
            explicitLang = argv[i + 1];
            break;
        }
    }
    if (explicitLang.empty()) {
        if (const char *envLang = std::getenv("LITEROUTER_LANG"); envLang != nullptr && *envLang != '\0') {
            explicitLang = envLang;
        }
    }
    if (!explicitLang.empty()) {
        literouter::i18n::setLang(literouter::i18n::parseLang(explicitLang));
    } else {
        literouter::i18n::setLang(literouter::i18n::detectSystemLang());
    }

    using literouter::i18n::tr;

    CLI::App app{
        std::string(tr("literouter — one local endpoint that fans a request out across your configured relays")),
        "literouter"};
    app.set_version_flag("-V,--version", std::string{literouter::kVersion});
    app.require_subcommand(1);

    lrcli::Context ctx;
    ctx.lang = explicitLang;
    app.add_option("--config", ctx.configPath,
                   std::string(tr("Path to the config file (default: $LITEROUTER_CONFIG, else "
                                  "~/.config/literouter/config.json)")));
    app.add_option("-l,--lang", ctx.lang, std::string(tr("Language (auto, en, zh)")));
    app.add_flag("--json", ctx.json, std::string(tr("Emit machine-readable JSON")));
    app.add_flag("--no-color", ctx.noColor, std::string(tr("Never emit ANSI colour")));
    app.add_flag("-q,--quiet", ctx.quiet, std::string(tr("Suppress non-essential output")));

    lrcli::register_serve(app, ctx);
    lrcli::register_status(app, ctx);
    lrcli::register_models(app, ctx);
    lrcli::register_providers(app, ctx);
    lrcli::register_routes(app, ctx);
    lrcli::register_config(app, ctx);
    lrcli::register_doctor(app, ctx);
    lrcli::register_logs(app, ctx);
    lrcli::register_bench(app, ctx);
    lrcli::register_replay(app, ctx);

    if (argc < 2) {
        std::cout << app.help() << "\n";
        return lrcli::kExitOk;
    }

    CLI11_PARSE(app, argc, argv);
    if (!ctx.lang.empty()) {
        literouter::i18n::setLang(literouter::i18n::parseLang(ctx.lang));
    }
    return ctx.exitCode;
}
