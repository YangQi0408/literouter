// The Windows console, kept in its own translation unit.
//
// `windows.h` is a header of macros — `min`, `max`, `ERROR`, `small` — and this
// file is the only one in the CLI that includes it, so nothing else has to
// defend itself against them. It also must not be included next to a module
// import, which is the other reason this is a `.cpp` of its own rather than a
// block inside cli_ui.cpp: that file imports literouter.core.
//
// On every other platform the two entry points are no-ops, so the callers need
// no #if of their own.
#include "cli_ui.hpp"

#if defined(_WIN32)

// Guarded: libstdc++'s os_defines.h already defines NOMINMAX on mingw, and a
// bare #define would warn about the redefinition on every build.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <atomic>
#  include <windows.h>

namespace lrcli {
namespace {

// The same flag the SIGINT handler sets, so a console control event and a
// Ctrl-C land in the same stop loop.
std::atomic<bool> *g_stop_flag = nullptr;

BOOL WINAPI consoleCtrlHandler(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_stop_flag != nullptr) {
            g_stop_flag->store(true);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

} // namespace

void configureConsoleEncoding() {
    // Both halves: output so our UTF-8 bytes are decoded as UTF-8 rather than
    // through the default code page, input so a key typed or piped in for
    // `--key-stdin` arrives in the same encoding.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void installConsoleStopHandler(std::atomic<bool> *flag) {
    g_stop_flag = flag;
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
}

} // namespace lrcli

#else

namespace lrcli {

void configureConsoleEncoding() {}

void installConsoleStopHandler(std::atomic<bool> *) {}

} // namespace lrcli

#endif
