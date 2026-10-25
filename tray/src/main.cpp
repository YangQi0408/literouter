// literouter — the desktop app.
//
// One process, no window of its own. It starts the gateway in-process, puts an
// icon in the system tray, opens the built-in web console in the default
// browser, and stops the gateway when it exits. The console is the entire UI;
// this program is the thing you double-click.
//
// ── Why the gateway runs in this process ────────────────────────────────────
//
// The alternative — spawn `literouter serve` as a child and supervise it — was
// the first design, and it cost a platform-specific process layer on every
// platform: posix_spawn or CreateProcessW to start, kill(SIGTERM) or
// GenerateConsoleCtrlEvent to stop, waitpid or WaitForSingleObject to reap, and
// a pipe or an inherited handle to read the child's status. Five behaviours
// that differ per platform, all of them only exercised on the platform they
// were written for.
//
// literouter.core is a library. Linking it means start() and stop() are plain
// function calls, the snapshot is a struct read, and there is no child process
// to leak, orphan or mis-signal. That removes the entire platform layer: what
// is left below is C++ and EUI-NEO's tray API, both of which already work on
// all three platforms. The cost is that this binary carries the proxy engine
// (~19 MB with eui-neo's graphics closure) instead of shelling out to a 2 MB
// one, which is the right trade for an application the user installs.
//
// ── The tray menu is two entries, and that is not a choice ──────────────────
//
// EUI-NEO hard-codes Show / separator / Exit in all three of its backends
// (AppKit, Win32, StatusNotifierItem) and exposes the outcome as two booleans.
// There is no API for adding items. So:
//
//   Show → open the console in the browser
//   Exit → stop the gateway and quit
//
// Starting is implicit: the gateway is up for as long as this program runs.
// That is also why autostart is the natural way to keep the gateway always
// available, and why this program never writes an autostart entry unless the
// user asks for one — see autostart.cpp.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "eui_neo.h"
#include "eui/platform.h"

import literouter.core;

#include "tray_autostart.hpp"

namespace {

// Autostart is not a tray menu entry because EUI-NEO's menu cannot gain one —
// see the header comment. It is a flag instead, which also means it can be set
// from an installer or a script without a GUI:
//
//   literouter-tray --autostart          write the entry and keep running
//   literouter-tray --no-autostart       remove it and keep running
//   literouter-tray --autostart-status   report it and exit
//
// Nothing is written unless one of these is given.
struct Options {
    enum class Action { Run, EnableAutostart, DisableAutostart, ReportAutostart } action =
        Action::Run;
};

Options parseArgs(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--autostart") {
            options.action = Options::Action::EnableAutostart;
        } else if (arg == "--no-autostart") {
            options.action = Options::Action::DisableAutostart;
        } else if (arg == "--autostart-status") {
            options.action = Options::Action::ReportAutostart;
        }
    }
    return options;
}

const char *describe(lrtray::AutostartState state) {
    switch (state) {
    case lrtray::AutostartState::Enabled:     return "enabled";
    case lrtray::AutostartState::Disabled:    return "disabled";
    case lrtray::AutostartState::Unsupported: return "unsupported";
    }
    return "unknown";
}

using namespace std::chrono_literals;

volatile std::sig_atomic_t g_quit = 0;

extern "C" void handleSignal(int) { g_quit = 1; }

// The console lives at /ui/ on whatever the gateway is actually listening on.
// Reading it back from the server rather than from the config is what makes
// `port: 0` (kernel-assigned) work.
std::string consoleUrl(const literouter::ProxyServer &server) {
    const int port = server.boundPort();
    if (port <= 0) {
        return {};
    }
    return std::format("http://127.0.0.1:{}/ui/", port);
}

std::string iconPath() {
    // resolveResourcePath() looks next to the executable first, which is where
    // an installed bundle keeps it. The fallbacks cover a run straight out of
    // the build tree.
    const std::string resolved =
        core::platform::resolveResourcePath("assets/literouter-tray.png");
    std::error_code ec;
    if (!resolved.empty() && std::filesystem::is_regular_file(resolved, ec)) {
        return resolved;
    }
    const std::filesystem::path beside =
        std::filesystem::current_path(ec) / "assets" / "literouter-tray.png";
    if (std::filesystem::is_regular_file(beside, ec)) {
        return beside.string();
    }
    return {};
}

int run(const Options &options) {
    // The autostart actions are decided before the gateway is started: asking
    // "is autostart on?" should not bind a port, and it should work on a machine
    // where the port is already taken by another instance.
    if (options.action == Options::Action::ReportAutostart) {
        const lrtray::AutostartStatus status = lrtray::queryAutostart();
        std::printf("%s\n", describe(status.state));
        if (!status.path.empty()) {
            std::printf("%s\n", status.path.c_str());
        }
        if (!status.message.empty()) {
            std::fprintf(stderr, "literouter: %s\n", status.message.c_str());
        }
        return status.state == lrtray::AutostartState::Unsupported ? 1 : 0;
    }
    if (options.action == Options::Action::EnableAutostart ||
        options.action == Options::Action::DisableAutostart) {
        const lrtray::AutostartStatus status =
            options.action == Options::Action::EnableAutostart ? lrtray::enableAutostart()
                                                              : lrtray::disableAutostart();
        std::printf("autostart %s: %s\n", describe(status.state), status.path.c_str());
        if (!status.message.empty()) {
            std::fprintf(stderr, "literouter: %s\n", status.message.c_str());
        }
        if (status.state == lrtray::AutostartState::Unsupported) {
            return 1;
        }
        // Falls through: setting autostart is usually done once, then the user
        // expects the app to be running. Exiting here would make
        // `literouter-tray --autostart` a no-op from their point of view.
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // loadOrSeed: a first run writes the seed config and keeps going, which is
    // what "double-click the app" has to mean. A config that exists but cannot
    // be parsed is a different case and is reported rather than replaced — the
    // user may have hand-edited it.
    const auto store = literouter::ConfigStore::loadOrSeed(literouter::defaultConfigPath());
    if (!store) {
        std::fprintf(stderr, "literouter: %s\n", store.error().c_str());
        return 1;
    }

    literouter::ProxyServer server;
    server.setConfigPath(literouter::pathToUtf8(literouter::defaultConfigPath()));

    literouter::AppConfig config = store->config();
    if (const auto started = server.start(config); !started) {
        std::fprintf(stderr, "literouter: %s\n", started.error().c_str());
        std::fprintf(stderr,
                     "literouter: another instance may already be listening; open the "
                     "console at http://127.0.0.1:%d/ui/ to use it\n",
                     config.server.port);
        return 1;
    }

    const std::string url = consoleUrl(server);
    std::printf("literouter is serving at %s\n", url.c_str());
    std::fflush(stdout);

    core::platform::TrayOptions trayOptions;
    trayOptions.tooltip = "literouter";
    trayOptions.iconPath = iconPath();

    if (!core::platform::initializeTray(trayOptions)) {
        server.stop();
        std::fprintf(stderr,
                     "literouter: cannot create a tray icon. On Linux this needs a "
                     "StatusNotifierWatcher on the session bus (KDE, or a GNOME extension "
                     "such as AppIndicator).\n");
        return 1;
    }

    // Opening the console on launch is the point of the app: there is no window
    // of its own, so without this the user double-clicks and sees nothing.
    if (!core::platform::openUrl(url)) {
        std::fprintf(stderr, "literouter: cannot open %s; open it by hand\n", url.c_str());
    }

    // Polled, not callback-driven: pollTray drains whatever the desktop queued
    // and the two consume* calls return and clear one-shot flags. 100 ms keeps a
    // click feeling immediate without spinning a core.
    while (g_quit == 0) {
        core::platform::pollTray(false);

        if (core::platform::consumeTrayShowRequested()) {
            if (!core::platform::openUrl(url)) {
                std::fprintf(stderr, "literouter: cannot open %s\n", url.c_str());
            }
        }
        if (core::platform::consumeTrayExitRequested()) {
            break;
        }
        // The console's own shutdown button stops the listener from a handler
        // thread. Without this the icon would outlive the gateway and Show
        // would open a dead page.
        if (!server.running()) {
            std::printf("the gateway was stopped from the console; exiting\n");
            std::fflush(stdout);
            break;
        }
        std::this_thread::sleep_for(100ms);
    }

    core::platform::shutdownTray();
    server.stop();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        return run(parseArgs(argc, argv));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "literouter: %s\n", error.what());
        return 1;
    }
}
