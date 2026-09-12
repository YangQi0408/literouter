#include <CLI/CLI.hpp>

#include <fstream>
#include <iterator>

#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"

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
        root["path"] = path.string();
        root["exists"] = exists;
        std::println("{}", root.dump(2));
        return;
    }
    std::println("{}", path.string());
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
                              path.string()),
                  ctx.quiet);
        return;
    }

    std::ifstream input{path, std::ios::binary};
    if (!input) {
        printError(std::format("cannot read config `{}`", path.string()));
        printHint("check the file's permissions");
        ctx.exitCode = kExitConfig;
        return;
    }
    std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

    if (ctx.json) {
        // The file is meant to be JSON; validate that it still parses before
        // handing it to a machine reader.
        if (auto parsed = literouter::appConfigFromJson(text); !parsed) {
            printError(std::format("config `{}` is not valid JSON: {}", path.string(),
                                   parsed.error()));
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
        printError(std::format("refusing to overwrite existing config `{}`", path.string()));
        printHint(std::format("re-run with `literouter config init --force` to replace it"));
        ctx.exitCode = kExitConfig;
        return;
    }

    literouter::ConfigStore store;
    if (auto loaded = literouter::ConfigStore::loadOrSeed(path); loaded) {
        store = std::move(*loaded);
    } else {
        printError(std::format("cannot write config `{}`: {}", path.string(), loaded.error()));
        ctx.exitCode = kExitFail;
        return;
    }
    store.config() = literouter::ConfigStore::seedDefault();
    if (auto saved = store.save(); !saved) {
        printError(std::format("cannot write config `{}`: {}", path.string(), saved.error()));
        ctx.exitCode = kExitFail;
        return;
    }

    if (ctx.json) {
        json root = json::object();
        root["path"] = path.string();
        root["wrote"] = true;
        std::println("{}", root.dump(2));
        return;
    }
    printInfo(std::format("wrote seed config to {}", path.string()));
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
        root["path"] = storeOpt->path().string();
        root["ok"] = report.ok();
        root["summary"] = report.summary();
        root["issues"] = std::move(issues);
        std::println("{}", root.dump(2));
    } else {
        printIssues(report, std::cout);
    }
    if (!report.ok()) {
        ctx.exitCode = kExitConfig;
    }
}

void registerPath(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("path", "Print the resolved config path");
    sub->fallthrough();
    sub->callback([&ctx] { runPath(ctx); });
}

void registerShow(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("show", "Print the config file as it is on disk");
    sub->fallthrough();
    sub->callback([&ctx] { runShow(ctx); });
}

void registerInit(CLI::App &parent, Context &ctx) {
    auto opts = std::make_shared<InitOptions>();
    CLI::App *sub = parent.add_subcommand("init", "Write the seed config");
    sub->add_flag("--force", opts->force, "Overwrite an existing file");
    sub->fallthrough();
    sub->callback([&ctx, opts] { runInit(ctx, *opts); });
}

void registerValidate(CLI::App &parent, Context &ctx) {
    CLI::App *sub = parent.add_subcommand("validate", "Validate the config and print every issue");
    sub->fallthrough();
    sub->callback([&ctx] { runValidate(ctx); });
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
}

} // namespace lrcli
