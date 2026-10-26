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
#if defined(__linux__)
#include <dlfcn.h>
#endif

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
    bool help = false;
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
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else {
            throw std::runtime_error("unknown desktop argument: " + std::string{arg});
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
    const auto snapshot = server.snapshot();
    if (snapshot.port <= 0) {
        return {};
    }
    auto listener = server.config().server;
    listener.host = snapshot.host;
    listener.port = snapshot.port;
    if (literouter::startsWith(snapshot.base_url, "https://")) listener.tls_cert_file = "active-https";
    else listener.tls_cert_file.clear();
    const auto base = literouter::runningServerBaseUrl(listener, {});
    return base ? *base + "/ui/" : std::string{};
}

std::string iconPath() {
#if defined(_WIN32)
    // The Win32 tray backend calls ExtractIconEx, which expects the executable's
    // embedded icon resource rather than a standalone PNG/ICO file.
    return literouter::pathToUtf8(lrtray::appExecutablePath());
#endif
    const auto besideExecutable = lrtray::appExecutablePath().parent_path() / "assets" /
        "literouter-tray.png";
    std::error_code error;
    if (std::filesystem::is_regular_file(besideExecutable, error))
        return literouter::pathToUtf8(besideExecutable);
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
    if (options.help) {
        std::printf("%s\n", std::string{literouter::i18n::tr(
            "literouter-tray [--autostart|--no-autostart|--autostart-status|--help]")}.c_str());
        return 0;
    }
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

#if defined(__linux__)
    // The backend's initializeTray() succeeds without a watcher and waits
    // indefinitely. Check first, before starting a windowless gateway. Resolve
    // GIO dynamically from the same process dependency, without publishing
    // third-party types through core or shelling out to the user's desktop.
    void *gio = ::dlopen("libgio-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    using Bus = void *(*)(int, void *, void **);
    using Call = void *(*)(void *, const char *, const char *, const char *, const char *,
                          void *, const void *, int, int, void *, void **);
    const auto bus = gio ? reinterpret_cast<Bus>(::dlsym(gio, "g_bus_get_sync")) : nullptr;
    const auto call = gio ? reinterpret_cast<Call>(::dlsym(gio, "g_dbus_connection_call_sync")) : nullptr;
    void *connection = bus ? bus(2, nullptr, nullptr) : nullptr; // G_BUS_TYPE_SESSION
    void *reply = connection && call ? call(connection, "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher", "org.freedesktop.DBus.Peer", "Ping", nullptr, nullptr,
        1, 1000, nullptr, nullptr) : nullptr; // no auto-start of another program
    const bool watcher = reply != nullptr;
    if (reply) {
        const auto unref = reinterpret_cast<void (*)(void *)>(::dlsym(gio, "g_variant_unref"));
        if (unref) unref(reply);
    }
    if (connection) {
        const auto unref = reinterpret_cast<void (*)(void *)>(::dlsym(gio, "g_object_unref"));
        if (unref) unref(connection);
    }
    if (gio) ::dlclose(gio);
    if (!watcher) {
        std::fprintf(stderr, "literouter: %s\n", std::string{literouter::i18n::tr(
            "no StatusNotifierWatcher is running; enable a tray panel or use the CLI gateway")}.c_str());
        return 1;
    }
#endif

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // A first run writes the seed config and keeps going, which is
    // what "double-click the app" has to mean. A config that exists but cannot
    // be parsed is a different case and is reported rather than replaced — the
    // user may have hand-edited it.
    auto store = literouter::ConfigStore::load(literouter::defaultConfigPath());
    if (!store) {
        std::fprintf(stderr, "literouter: %s\n", store.error().c_str());
        return 1;
    }

    literouter::AppConfig config = store->config();
    const auto validation = literouter::validate(config);
    if (!validation.ok()) {
        std::fprintf(stderr, "literouter: %s\n", validation.summary().c_str());
        return 1;
    }
    if (!store->existsOnDisk()) {
        const auto saved = store->save();
        if (!saved) {
            std::fprintf(stderr, "literouter: %s\n", saved.error().c_str());
            return 1;
        }
        store->setExistsOnDisk(true);
    }

    // ── Adopt an instance that is already serving, instead of losing to it ──
    //
    // The first version of this program exited when the port was taken, and told
    // the user to open the console themselves. That is wrong for an application
    // with no window: there is nowhere to tell them. stdout is discarded when a
    // GUI program is launched from a file manager, eui-neo's tray tooltip is
    // thrown away inside the library, and there is no notification API — so a
    // failure message reaches nobody, and the user sees an app that did nothing.
    //
    // The behaviour that actually works is to join the running gateway: open its
    // console, keep the icon, and leave it alone on exit because this program did
    // not start it. Show then always opens a live page, which is the only channel
    // a windowless program has.
    bool ownsGateway = true;
    std::string url;

    const auto resolved = literouter::runningServerBaseUrl(config.server, store->path());
    if (!resolved && resolved.error() != "no running instance found for this config's dynamic port") {
        std::fprintf(stderr, "literouter: %s\n", std::string{literouter::i18n::tr(resolved.error())}.c_str());
        return 1; // ambiguity must not start a third instance on another random port
    }
    const std::string probeUrl = resolved ? *resolved : std::string{};
    const literouter::AdminStatus existing =
        resolved ? literouter::fetchStatus(probeUrl, config.server.api_key) : literouter::AdminStatus{};
    if (existing.reachable && existing.snapshot.running) {
        ownsGateway = false;
        // The snapshot's own base_url, not the probe URL: when the config says
        // port 0 the running instance is on a kernel-assigned port, and only the
        // instance knows which.
        const std::string &base = existing.snapshot.base_url.empty()
                                      ? probeUrl
                                      : existing.snapshot.base_url;
        url = base + "/ui/";
    }

    literouter::ProxyServer server;
    if (ownsGateway) {
        server.setConfigPath(literouter::pathToUtf8(literouter::defaultConfigPath()));
        if (const auto started = server.start(config); !started) {
            // There is no live console to open. Report the error to a terminal
            // caller and exit, rather than keeping a dead tray/browser URL.
            std::fprintf(stderr, "literouter: %s\n", started.error().c_str());
            return 1; // no dead tray or browser URL when the listener could not start
        } else {
            url = consoleUrl(server);
        }
    }

    core::platform::TrayOptions trayOptions;
    trayOptions.tooltip = "literouter";
    trayOptions.iconPath = iconPath();

    if (!core::platform::initializeTray(trayOptions)) {
        if (ownsGateway) {
            server.stop();
        }
        std::fprintf(stderr,
                     "literouter: cannot create a tray icon. On Linux this needs a "
                     "StatusNotifierWatcher on the session bus (KDE, or a GNOME extension "
                     "such as AppIndicator).\n");
        return 1;
    }

    // Opening the console on launch is the point of the app: there is no window
    // of its own, so without this the user double-clicks and sees nothing.
    //
    // The return value is deliberately NOT checked. eui-neo's openUrl runs
    // `xdg-open … &` through std::system(), so the shell backgrounds it and
    // system() reports success the moment the fork happens — it returns true
    // whether or not a browser ever appears. Treating that as a failure signal
    // would be checking a constant. Measured: a foreground `xdg-open` on this
    // machine prints zygote errors and still exits 0.
    (void)core::platform::openUrl(url);

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
        // would open a dead page. Only meaningful for a gateway this program
        // started: an adopted instance is not ours to outlive.
        if (ownsGateway && !server.running()) {
            std::printf("the gateway was stopped from the console; exiting\n");
            std::fflush(stdout);
            break;
        }
        std::this_thread::sleep_for(100ms);
    }

    core::platform::shutdownTray();
    // Only a gateway this program started is this program's to stop. When the
    // tray adopted an instance that was already serving, exiting must leave it
    // running — the user may be using it from a terminal or a systemd unit.
    if (ownsGateway) {
        server.stop();
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        lrtray::setExecutablePath(literouter::pathFromUtf8(argv[0]));
        return run(parseArgs(argc, argv));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "literouter: %s\n", error.what());
        return 1;
    }
}
