// literouter-tray — a tray icon that starts, stops and opens the local gateway.
//
// ── What this program is, and what it deliberately is not ────────────────────
//
// There is no window. eui-neo's tray support is reachable without ever creating
// one (`core::platform::initializeTray` / `pollTray` touch no GL context and no
// window handle), which is what makes a tray-only program possible at all.
//
// The menu, however, is NOT ours: eui-neo hard-codes exactly three entries —
// "Show", a separator, and "Exit" — in all three of its backends (AppKit, Win32
// and the Linux SNI one), and exposes the outcome as two booleans
// (`consumeTrayShowRequested` / `consumeTrayExitRequested`). There is no API for
// adding items. So this program maps the two entries it can actually observe:
//
//   Show  → open the built-in web console in the default browser
//   Exit  → stop the gateway (if this program started it) and quit
//
// Starting and stopping the gateway therefore hangs off the process lifetime
// rather than a menu entry: the gateway is spawned when this program starts and
// stopped when it exits. That is the whole supervision model, and it is why
// `serve` is started with SIGTERM as the stop signal — the CLI installs a
// handler for it and shuts the listener down cleanly.
//
// ── Why a child process instead of linking core ──────────────────────────────
//
// Linking `literouter.core` would put the proxy inside this binary, but the
// console URL, the config path and the state directory all follow the CLI's own
// resolution rules, and the operator may already be running `literouter serve`
// from a shell. Driving the same `literouter` binary keeps one source of truth
// for all of it: this program starts the gateway exactly the way the operator
// would, and an already-running instance is detected and adopted rather than
// fought with.

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "eui_neo.h"
#include "eui/platform.h"

extern char **environ;

namespace {

using namespace std::chrono_literals;

// ── locating the gateway ─────────────────────────────────────────────────────

// The binary to supervise. Resolution order is deliberately boring: an explicit
// override first, then whatever is on PATH (which is how a package or a systemd
// unit would install it), then the in-tree build product. The last one keeps
// this usable straight out of a checkout without an install step.
std::string gatewayBinary() {
    if (const char *explicitPath = std::getenv("LITEROUTER_BIN");
        explicitPath != nullptr && *explicitPath != '\0') {
        return explicitPath;
    }
    if (const char *onPath = std::getenv("PATH"); onPath != nullptr) {
        std::string_view rest = onPath;
        while (!rest.empty()) {
            const auto colon = rest.find(':');
            const auto dir = rest.substr(0, colon);
            if (!dir.empty()) {
                std::filesystem::path candidate{std::string{dir}};
                candidate /= "literouter";
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate, ec) &&
                    ::access(candidate.c_str(), X_OK) == 0) {
                    return candidate.string();
                }
            }
            if (colon == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(colon + 1);
        }
    }
    // In-tree fallback: target/<triple>/<hash>/bin/cli/literouter. The hash is
    // not knowable here, so the newest one wins — the same rule CI's own
    // "newest binary" lookups use.
    std::error_code ec;
    const std::filesystem::path target{"target"};
    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    if (std::filesystem::is_directory(target, ec)) {
        for (const auto &triple : std::filesystem::directory_iterator(target, ec)) {
            if (!triple.is_directory()) continue;
            for (const auto &build : std::filesystem::directory_iterator(triple.path(), ec)) {
                if (!build.is_directory()) continue;
                const auto candidate = build.path() / "bin" / "cli" / "literouter";
                if (!std::filesystem::is_regular_file(candidate, ec)) continue;
                const auto stamp = std::filesystem::last_write_time(candidate, ec);
                if (best.empty() || stamp > bestTime) {
                    best = candidate;
                    bestTime = stamp;
                }
            }
        }
    }
    return best.string();
}

// ── gateway supervision ──────────────────────────────────────────────────────

class Gateway {
public:
    ~Gateway() { stop(); }

    Gateway(const Gateway &) = delete;
    Gateway &operator=(const Gateway &) = delete;
    Gateway() = default;

    // True when something is already serving the configured port. Checked by
    // asking the CLI rather than by probing the port, because `status` knows
    // which config file it would have used and whether the answer came from a
    // real literouter.
    bool alreadyRunning() const {
        std::string out;
        // `status --json` exits non-zero when nothing answers; the exit code is
        // the signal here, not the body.
        return runCaptured({"status", "--json", "--quiet"}, out) == 0;
    }

    // Spawns `literouter serve` and remembers the pid. Returns an empty string
    // on success, or a human-readable reason on failure.
    std::string start() {
        if (pid_ > 0) {
            return {};
        }
        const std::string binary = gatewayBinary();
        if (binary.empty()) {
            return "cannot find the `literouter` binary (set LITEROUTER_BIN to point at it)";
        }

        std::array<char *, 4> argv{};
        argv[0] = const_cast<char *>(binary.c_str());
        argv[1] = const_cast<char *>("serve");
        argv[2] = const_cast<char *>("--quiet");
        argv[3] = nullptr;

        // Its stdout/stderr go to this program's, so the operator sees the
        // gateway's own startup banner and errors in the terminal they started
        // the tray from. Detached from the terminal's signal group (POSIX_SPAWN_
        // SETSIGDEF + a separate process group) so Ctrl-C on the tray does not
        // race the graceful path below.
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        sigset_t defaultSignals;
        sigemptyset(&defaultSignals);
        sigaddset(&defaultSignals, SIGINT);
        sigaddset(&defaultSignals, SIGTERM);
        posix_spawnattr_setsigdefault(&attr, &defaultSignals);
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);

        pid_t child = 0;
        const int rc = posix_spawn(&child, binary.c_str(), nullptr, &attr,
                                   argv.data(), environ);
        posix_spawnattr_destroy(&attr);
        if (rc != 0) {
            return std::string{"cannot start `"} + binary + " serve`: " + std::strerror(rc);
        }
        pid_ = child;
        return {};
    }

    // SIGTERM, then wait. `literouter serve` installs a handler for it and
    // shuts the listener down cleanly; SIGKILL would leave the pid file behind
    // and skip the telemetry flush.
    void stop() {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        // 5 seconds is generous for a listener that is not mid-request. A
        // request in flight gets that long to finish before we escalate.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            const pid_t done = ::waitpid(pid_, &status, WNOHANG);
            if (done == pid_ || done < 0) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(50ms);
        }
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    pid_t pid() const { return pid_; }

private:
    // Runs `literouter <args>` to completion and captures stdout. Used only for
    // the short-lived `status` probe; the server itself is spawned detached.
    static int runCaptured(const std::vector<std::string> &args, std::string &out) {
        const std::string binary = gatewayBinary();
        if (binary.empty()) {
            return -1;
        }
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>(binary.c_str()));
        for (const auto &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        int pipeFds[2];
        if (::pipe(pipeFds) != 0) {
            return -1;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
        posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

        pid_t child = 0;
        const int rc = posix_spawn(&child, binary.c_str(), &actions, nullptr,
                                   argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipeFds[1]);
        if (rc != 0) {
            ::close(pipeFds[0]);
            return -1;
        }
        char buffer[4096];
        ssize_t got = 0;
        while ((got = ::read(pipeFds[0], buffer, sizeof buffer)) > 0) {
            out.append(buffer, static_cast<std::size_t>(got));
        }
        ::close(pipeFds[0]);
        int status = 0;
        ::waitpid(child, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    pid_t pid_ = -1;
};

// ── console URL ──────────────────────────────────────────────────────────────

// The console lives at /ui/ on whatever the gateway is actually listening on.
// `status --json` is the only honest source for that: the port can come from
// the config file, from `--port`, or from the kernel when the config says 0.
std::string consoleUrl() {
    Gateway probe;
    std::string body;
    if (probe.alreadyRunning()) {
        // Re-run capturing stdout; alreadyRunning() above only wanted the exit
        // code, and this keeps the two concerns separate.
        const std::string binary = gatewayBinary();
        if (!binary.empty()) {
            std::FILE *pipe = ::popen((binary + " status --json").c_str(), "r");
            if (pipe != nullptr) {
                char buffer[4096];
                std::size_t got = 0;
                while ((got = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
                    body.append(buffer, got);
                }
                ::pclose(pipe);
            }
        }
    }
    // Minimal, dependency-free extraction of "base_url": "http://127.0.0.1:8787".
    // A full JSON parser would be the wrong trade here: this reads one field out
    // of a document this repository itself produces.
    const auto key = body.find("\"base_url\"");
    if (key != std::string::npos) {
        const auto colon = body.find(':', key);
        const auto open = body.find('"', colon);
        const auto close = body.find('"', open + 1);
        if (colon != std::string::npos && open != std::string::npos &&
            close != std::string::npos && close > open + 1) {
            return body.substr(open + 1, close - open - 1) + "/ui/";
        }
    }
    // Nothing is listening: point at the documented default so Show still does
    // something sensible, and the browser's own error says the rest.
    return "http://127.0.0.1:8787/ui/";
}

// ── tray loop ────────────────────────────────────────────────────────────────

volatile std::sig_atomic_t g_quit = 0;

extern "C" void handleSignal(int) { g_quit = 1; }

std::string iconPath() {
    // resolveResourcePath() looks next to the executable first, which is where
    // an installed bundle would keep the icon. The explicit fallbacks cover a
    // run straight out of the checkout.
    const std::string resolved = core::platform::resolveResourcePath("assets/literouter-tray.png");
    std::error_code ec;
    if (!resolved.empty() && std::filesystem::is_regular_file(resolved, ec)) {
        return resolved;
    }
    const std::filesystem::path beside{std::filesystem::current_path(ec) / "assets" /
                                       "literouter-tray.png"};
    if (std::filesystem::is_regular_file(beside, ec)) {
        return beside.string();
    }
    return {};
}

int run() {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    Gateway gateway;

    // Adopt an existing instance rather than starting a second one: the CLI
    // refuses to share a port anyway, and a second spawn would only produce an
    // error banner for a gateway that is already fine.
    if (gateway.alreadyRunning()) {
        std::fprintf(stderr, "literouter-tray: a gateway is already running; "
                             "the tray will not manage it\n");
    } else if (const std::string error = gateway.start(); !error.empty()) {
        std::fprintf(stderr, "literouter-tray: %s\n", error.c_str());
        return 1;
    }

    core::platform::TrayOptions options;
    options.tooltip = "literouter";
    options.iconPath = iconPath();

    if (!core::platform::initializeTray(options)) {
        // The gateway is already running by now; leaving it up would strand it
        // with no way to stop it from here. Undo the start before failing.
        gateway.stop();
        std::fprintf(stderr,
                     "literouter-tray: cannot create a tray icon. On Linux this needs a "
                     "StatusNotifierWatcher on the session bus (KDE, or a GNOME extension "
                     "such as AppIndicator); see the README's troubleshooting note.\n");
        return 1;
    }

    // The tray API is polled, not callback-driven: `pollTray(false)` drains
    // whatever the desktop queued and the two consume* calls return and clear
    // one-shot flags. 100 ms keeps a click feeling immediate without spinning.
    while (g_quit == 0) {
        core::platform::pollTray(false);

        if (core::platform::consumeTrayShowRequested()) {
            const std::string url = consoleUrl();
            if (!core::platform::openUrl(url)) {
                std::fprintf(stderr, "literouter-tray: cannot open %s in a browser\n",
                             url.c_str());
            }
        }
        if (core::platform::consumeTrayExitRequested()) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }

    core::platform::shutdownTray();
    gateway.stop();
    return 0;
}

} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "literouter-tray: %s\n", error.what());
        return 1;
    }
}
