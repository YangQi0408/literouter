#pragma once
// Options every command shares, the exit codes the task pins down, and the one
// entry point per command family.

namespace CLI {
class App;
}

#include <string>

namespace lrcli {

// 0 clean, 1 runtime failure (bind failed, probe failed), 3 config/validation
// error, 4 no instance answered the admin API.
inline constexpr int kExitOk = 0;
inline constexpr int kExitFail = 1;
inline constexpr int kExitConfig = 3;
inline constexpr int kExitUnreachable = 4;

struct Context {
    std::string configPath; // global --config; empty means the default path
    std::string lang;       // global --lang / -l; "auto", "en", "zh"
    bool json = false;
    bool noColor = false;
    bool quiet = false;
    int exitCode = kExitOk;
};

void register_serve(CLI::App &root, Context &ctx);
void register_status(CLI::App &root, Context &ctx);
void register_models(CLI::App &root, Context &ctx);
void register_clients(CLI::App &root, Context &ctx);
void register_providers(CLI::App &root, Context &ctx);
void register_routes(CLI::App &root, Context &ctx);
void register_config(CLI::App &root, Context &ctx);
void register_doctor(CLI::App &root, Context &ctx);
void register_logs(CLI::App &root, Context &ctx);
void register_bench(CLI::App &root, Context &ctx);
void register_replay(CLI::App &root, Context &ctx);

} // namespace lrcli
