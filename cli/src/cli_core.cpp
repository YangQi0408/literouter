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
        printError(std::format("cannot use config `{}`: {}", path.string(), loaded.error()));
        printHint(std::format("fix the file, or rewrite it with `literouter config init --force "
                              "--config {}`",
                              path.string()));
        return std::nullopt;
    }
    return std::move(*loaded);
}

std::optional<literouter::ConfigStore> loadStore(const Context &ctx) {
    return loadStoreAt(configPathFor(ctx));
}

std::string baseUrlOf(const literouter::AppConfig &config) {
    return std::format("http://{}:{}", config.server.host, config.server.port);
}

std::string clientBaseUrlOf(const literouter::AppConfig &config) {
    return std::format("http://{}:{}/v1", config.server.host, config.server.port);
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
        table.row({colorLevel(level, level), issue.path, issue.message});
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
