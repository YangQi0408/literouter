#include "cli_context.hpp"
#include "cli_ui.hpp"

import literouter.core;

#include "cli_core.hpp"

namespace lrcli {

std::filesystem::path configPathFor(const Context &ctx) {
    if (!ctx.configPath.empty()) {
        return std::filesystem::path{ctx.configPath};
    }
    return literouter::defaultConfigPath();
}

std::optional<literouter::ConfigStore> loadStoreAt(const std::filesystem::path &path) {
    auto loaded = literouter::ConfigStore::load(path);
    if (!loaded) {
        printError(std::format("cannot use config `{}`: {}", literouter::pathToUtf8(path),
                               loaded.error()));
        printHint(std::format("fix the file, or rewrite it with `literouter config init --force "
                              "--config {}`",
                              literouter::pathToUtf8(path)));
        return std::nullopt;
    }
    return std::move(*loaded);
}

std::optional<literouter::ConfigStore> loadStore(const Context &ctx) {
    return loadStoreAt(configPathFor(ctx));
}

std::string baseUrlOf(const literouter::AppConfig &config) {
    return literouter::serverBaseUrl(config.server);
}

std::string clientBaseUrlOf(const literouter::AppConfig &config) {
    return literouter::serverBaseUrl(config.server, "/v1");
}

void printIssues(const literouter::ValidationReport &report, std::ostream &out) {
    if (report.issues.empty()) {
        out << paint("no validation issues", "32") << "\n";
        return;
    }
    Table table;
    table.column("LEVEL");
    table.column("PATH");
    table.column("MESSAGE");
    for (const auto &issue : report.issues) {
        const std::string level = issue.levelName();
        table.row({colorLevel(level, level), issue.path, std::string(literouter::i18n::tr(issue.message))});
    }
    table.print(out);
    out << report.summary() << "\n";
}

std::vector<literouter::Candidate> routingOrder(const literouter::AppConfig &config,
                                                const std::string &model) {
    literouter::Router router;
    router.setConfig(config);
    return router.candidatesFor(model);
}

std::string describeApiKey(const std::string &raw) {
    if (literouter::isSecretReference(raw)) {
        const std::string name = literouter::secretReferenceName(raw);
        return name.empty() ? std::string{"$<malformed>"} : "$" + name;
    }
    if (raw.empty()) {
        return "(none)";
    }
    return literouter::maskSecret(raw);
}

} // namespace lrcli
