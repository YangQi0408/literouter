#pragma once
// Declarations that name literouter.core types. This header must be included
// AFTER `import literouter.core;` — the import is what brings `std` and the
// core namespace into scope for it.
//
// Keeping it separate is what lets cli_ui.hpp / cli_context.hpp stay
// core-free and be included before the import, per the workspace's verified
// include/import ordering.

namespace lrcli {

struct Context;

// --config when given, otherwise literouter::defaultConfigPath(). Honours
// LITEROUTER_CONFIG through the core helper.
std::filesystem::path configPathFor(const Context &ctx);

// Loads the config for reading or editing. On a parse failure this prints an
// error naming the file and the parse error, and returns nullopt so the caller
// can exit with kExitConfig.
std::optional<literouter::ConfigStore> loadStore(const Context &ctx);
std::optional<literouter::ConfigStore> loadStoreAt(const std::filesystem::path &path);

// "http://host:port" for the config's server section.
std::string baseUrlOf(const literouter::AppConfig &config);

// "http://host:port/v1" — the base an OpenAI-compatible client points at.
std::string clientBaseUrlOf(const literouter::AppConfig &config);

// Every issue, one table row each, then the summary line. Written to `out` so
// a caller can keep stdout clean for a JSON payload.
void printIssues(const literouter::ValidationReport &report, std::ostream &out);

// Renders an env-backed key as `$NAME`, a literal one masked, an empty one as
// "(none)". Never expands the reference.
std::string describeApiKey(const std::string &raw);

} // namespace lrcli
