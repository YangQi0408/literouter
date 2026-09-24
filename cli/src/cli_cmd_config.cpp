#include <CLI/CLI.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"
#include "cli_json.hpp"

namespace lrcli {
namespace {

using json = nlohmann::json;

struct InitOptions {
    bool force = false;
};

bool pathExists(const std::filesystem::path &path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

void runPath(Context &ctx) {
    configureColor(ctx.noColor);
    const std::filesystem::path path = configPathFor(ctx);
    const bool exists = pathExists(path);
    if (ctx.json) {
        json root = json::object();
        root["path"] = literouter::pathToUtf8(path);
        root["exists"] = exists;
        std::println("{}", dumpJson(root, 2));
        return;
    }
    std::println("{}", literouter::pathToUtf8(path));
    if (exists) {
        std::println("{}", paint("exists", "32"));
    } else {
        std::println("{}", dim("does not exist — create it with `literouter config init`"));
    }
}

void runShow(Context &ctx) {
    configureColor(ctx.noColor);
    const std::filesystem::path path = configPathFor(ctx);

    if (!pathExists(path)) {
        const literouter::AppConfig seed = literouter::ConfigStore::seedDefault();
        if (ctx.json) {
            std::println("{}", literouter::toJsonString(seed));
            return;
        }
        std::println("{}", literouter::toJsonString(seed));
        printNote(std::format("\n  `{}` does not exist; that is the built-in seed. Write it with "
                              "`literouter config init`.",
                              literouter::pathToUtf8(path)),
                  ctx.quiet);
        return;
    }

    std::ifstream input{path, std::ios::binary};
    if (!input) {
        printError(std::format("cannot read config `{}`", literouter::pathToUtf8(path)));
        printHint("check the file's permissions");
        ctx.exitCode = kExitConfig;
        return;
    }
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    if (ctx.json) {
        // The file is meant to be JSON; validate that it still parses before
        // handing it to a machine reader.
        if (auto parsed = literouter::appConfigFromJson(text); !parsed) {
            printError(std::format("config `{}` is not valid JSON: {}",
                                   literouter::pathToUtf8(path), parsed.error()));
            ctx.exitCode = kExitConfig;
            return;
        }
    }
    std::print("{}", text);
    if (!text.empty() && text.back() != '\n') {
        std::print("\n");
    }
}

void runInit(Context &ctx, const InitOptions &opts) {
    configureColor(ctx.noColor);
    const std::filesystem::path path = configPathFor(ctx);

    if (pathExists(path) && !opts.force) {
        printError(std::format("refusing to overwrite existing config `{}`",
                               literouter::pathToUtf8(path)));
        printHint(std::format("re-run with `literouter config init --force` to replace it"));
        ctx.exitCode = kExitConfig;
        return;
    }

    literouter::ConfigStore store;
    if (auto loaded = literouter::ConfigStore::loadOrSeed(path); loaded) {
        store = std::move(*loaded);
    } else {
        printError(std::format("cannot write config `{}`: {}", literouter::pathToUtf8(path),
                               loaded.error()));
        ctx.exitCode = kExitFail;
        return;
    }
    store.config() = literouter::ConfigStore::seedDefault();
    if (auto saved = store.save(); !saved) {
        printError(std::format("cannot write config `{}`: {}", literouter::pathToUtf8(path),
                               saved.error()));
        ctx.exitCode = kExitFail;
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["path"] = literouter::pathToUtf8(path);
        root["wrote"] = true;
        std::println("{}", dumpJson(root, 2));
        return;
    }
    printInfo(std::format("wrote seed config to {}", literouter::pathToUtf8(path)));
    printNote("  edit it, or use `literouter providers add` / `literouter routes add`.", ctx.quiet);
}

void runValidate(Context &ctx) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const literouter::ValidationReport report = literouter::validate(storeOpt->config());

    if (ctx.json) {
        json issues = json::array();
        for (const auto &issue : report.issues) {
            issues.push_back(json{{"level", issue.levelName()},
                                  {"path", issue.path},
                                  {"message", issue.message}});
        }
        json root = json::object();
        root["path"] = literouter::pathToUtf8(storeOpt->path());
        root["ok"] = report.ok();
        root["summary"] = report.summary();
        root["issues"] = std::move(issues);
        std::println("{}", dumpJson(root, 2));
    } else {
        printIssues(report, std::cout);
    }
    if (!report.ok()) {
        ctx.exitCode = kExitConfig;
    }
}

void registerPath(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("path", lrcli::tr("Print the resolved config path"));
    sub->fallthrough();
    sub->callback([&ctx] { runPath(ctx); });
}

void registerShow(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("show",
                                          lrcli::tr("Print the config file as it is on disk"));
    sub->fallthrough();
    sub->callback([&ctx] { runShow(ctx); });
}

void registerInit(CLI::App &parent, Context &ctx) {
    auto opts = std::make_shared<InitOptions>();
    CLI::App *sub = parent.add_subcommand("init", lrcli::tr("Write the seed config"));
    sub->add_flag("--force", opts->force, lrcli::tr("Overwrite an existing file"));
    sub->fallthrough();
    sub->callback([&ctx, opts] { runInit(ctx, *opts); });
}

void registerValidate(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("validate",
                                          lrcli::tr("Validate the config and print every issue"));
    sub->fallthrough();
    sub->callback([&ctx] { runValidate(ctx); });
}

// Rewrites an older config onto this build's schema. Loading already migrates in
// memory; this is the explicit way to write the migrated form back, which is
// what makes the file match what the proxy is actually doing.
void runMigrate(Context &ctx) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::ConfigStore &store = *storeOpt;
    if (!store.needsMigration()) {
        if (ctx.json) {
            json root = json::object();
            root["path"] = literouter::pathToUtf8(store.path());
            root["schema"] = literouter::kConfigSchema;
            root["migrated"] = false;
            root["notes"] = json::array();
            std::println("{}", dumpJson(root, 2));
            return;
        }
        printInfo(std::format("{} is already on schema {}", literouter::pathToUtf8(store.path()),
                              literouter::kConfigSchema));
        return;
    }

    // Written only after the migrated model validates: replacing a readable file
    // with one this build refuses to run would be worse than leaving it alone.
    const literouter::ValidationReport report = literouter::validate(store.config());
    if (!report.ok()) {
        printIssues(report, std::cerr);
        printHint("the config did not validate, so nothing was written");
        ctx.exitCode = kExitConfig;
        return;
    }
    if (auto saved = store.save(); !saved) {
        printError(saved.error());
        ctx.exitCode = kExitFail;
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["path"] = literouter::pathToUtf8(store.path());
        root["from_schema"] = store.migration().from_schema;
        root["to_schema"] = store.migration().to_schema;
        root["migrated"] = true;
        root["notes"] = store.migration().notes;
        std::println("{}", dumpJson(root, 2));
        return;
    }
    printInfo(std::format("migrated {} from schema {} to {}", literouter::pathToUtf8(store.path()),
                          store.migration().from_schema, store.migration().to_schema));
    for (const auto &note : store.migration().notes) {
        printNote("  " + note, ctx.quiet);
    }
}

// A whole-config dump and load, for moving a working setup between machines or
// into version control. Deliberately the same bytes `config show` prints and
// `config load` accepts, so a round trip is not a format of its own.
void runExport(Context &ctx, const std::filesystem::path &destination) {
    configureColor(ctx.noColor);
    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    const std::string text = storeOpt->toJson() + "\n";
    if (destination.empty() || destination == "-") {
        std::print("{}", text);
    } else if (auto saved = storeOpt->saveAs(destination); !saved) {
        printError(saved.error());
        ctx.exitCode = kExitFail;
        return;
    } else if (!ctx.quiet) {
        printInfo(std::format("wrote {} ({} bytes)", literouter::pathToUtf8(destination),
                              text.size()));
        printNote("  secrets are exported as the references they are, never expanded.", ctx.quiet);
    }
}

void runLoad(Context &ctx, const std::filesystem::path &source, bool merge, bool dry_run) {
    configureColor(ctx.noColor);
    // `-` is standard input, read through `std::cin` rather than by opening
    // `/dev/stdin`: that path is a POSIX convention and does not exist on
    // Windows, where the command would fail with a plain "cannot read -".
    std::string text;
    if (source == "-") {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        text = buffer.str();
    } else {
        std::ifstream input{source, std::ios::binary};
        if (!input) {
            printError(std::format("cannot read {}", literouter::pathToUtf8(source)));
            ctx.exitCode = kExitConfig;
            return;
        }
        text = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    auto storeOpt = loadStore(ctx);
    if (!storeOpt) {
        ctx.exitCode = kExitConfig;
        return;
    }
    literouter::ConfigStore &store = *storeOpt;

    // Migrated first, so a file exported by an older build loads here.
    std::string rewritten;
    if (auto migration = literouter::migrateConfigJson(text, rewritten); !migration) {
        printError(migration.error());
        ctx.exitCode = kExitConfig;
        return;
    }
    auto incoming = literouter::appConfigFromJson(rewritten);
    if (!incoming) {
        printError(incoming.error());
        ctx.exitCode = kExitConfig;
        return;
    }

    literouter::AppConfig result = *incoming;
    if (merge) {
        // Merge by id: the relays and routes the incoming file names replace the
        // ones already configured, and everything else is kept. This is the "add
        // these three relays to my working setup" operation, which a whole-file
        // replace cannot express.
        result = store.config();
        for (const auto &provider : incoming->providers) {
            if (auto *existing = result.provider(provider.id); existing != nullptr) {
                *existing = provider;
            } else {
                result.providers.push_back(provider);
            }
        }
        for (const auto &route : incoming->routes) {
            const auto found = std::ranges::find(result.routes, route.model, &literouter::RouteConfig::model);
            if (found != result.routes.end()) {
                *found = route;
            } else {
                result.routes.push_back(route);
            }
        }
    }

    const literouter::ValidationReport report = literouter::validate(result);
    if (!report.ok()) {
        printIssues(report, std::cerr);
        printHint("nothing was written");
        ctx.exitCode = kExitConfig;
        return;
    }
    if (dry_run) {
        if (ctx.json) {
            json root = json::object();
            root["path"] = literouter::pathToUtf8(store.path());
            root["applied"] = false;
            root["ok"] = true;
            root["summary"] = report.summary();
            std::println("{}", dumpJson(root, 2));
            return;
        }
        printInfo(std::format("{} would load and validate ({})", literouter::pathToUtf8(source),
                              report.summary()));
        return;
    }

    store.config() = std::move(result);
    if (auto saved = store.save(); !saved) {
        printError(saved.error());
        ctx.exitCode = kExitFail;
        return;
    }
    if (ctx.json) {
        json root = json::object();
        root["path"] = literouter::pathToUtf8(store.path());
        root["applied"] = true;
        root["ok"] = true;
        root["merged"] = merge;
        root["summary"] = report.summary();
        std::println("{}", dumpJson(root, 2));
        return;
    }
    printInfo(std::format("{} {} into {}", merge ? "merged" : "loaded",
                          literouter::pathToUtf8(source), literouter::pathToUtf8(store.path())));
    printNote("  reload the running proxy, or enable reload_on_change, to apply it.", ctx.quiet);
}

void registerMigrate(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand(
        "migrate", lrcli::tr("Rewrite an older config onto this build's schema"));
    sub->fallthrough();
    sub->callback([&ctx] { runMigrate(ctx); });
}

void registerExport(CLI::App &parent, Context &ctx) {
    auto destination = std::make_shared<std::filesystem::path>();
    CLI::App *sub = parent.add_subcommand(
        "export", lrcli::tr("Print the whole config, or write it to a file"));
    sub->add_option("file", *destination, lrcli::tr("Destination file; omit or `-` for stdout"));
    sub->fallthrough();
    sub->callback([&ctx, destination] { runExport(ctx, *destination); });
}

void registerLoad(CLI::App &parent, Context &ctx) {
    struct Options {
        std::filesystem::path file;
        bool merge = false;
        bool dryRun = false;
    };
    auto opts = std::make_shared<Options>();
    CLI::App *sub = parent.add_subcommand(
        "load", lrcli::tr("Load a whole config, or merge named entries into the current one"));
    sub->add_option("file", opts->file,
                    lrcli::tr("Source file; `-` reads standard input"))->required();
    sub->add_flag("--merge", opts->merge,
                  lrcli::tr("Replace only the relays and routes the file names"));
    sub->add_flag("--dry-run", opts->dryRun, lrcli::tr("Validate and report without writing"));
    sub->fallthrough();
    sub->callback([&ctx, opts] { runLoad(ctx, opts->file, opts->merge, opts->dryRun); });
}

} // namespace

void register_config(CLI::App &root, Context &ctx) {
    CLI::App *config = root.add_subcommand(
        "config", std::string(literouter::i18n::tr("Inspect and edit the config file")));
    config->require_subcommand(1);
    config->fallthrough();
    registerPath(*config, ctx);
    registerShow(*config, ctx);
    registerInit(*config, ctx);
    registerValidate(*config, ctx);
    registerMigrate(*config, ctx);
    registerExport(*config, ctx);
    registerLoad(*config, ctx);
}

} // namespace lrcli
