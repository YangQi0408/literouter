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
#  include <cstdio>
#  include <windows.h>
// _setmode / _O_BINARY: the CRT's translation mode, which windows.h does not
// declare and which this file is the only place in the CLI to touch.
#  include <fcntl.h>
#  include <io.h>

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

    // The CRT's default translation mode is text, which rewrites every '\n' on
    // its way to a pipe or a file as "\r\n". That is right for a console and
    // wrong for every machine-readable thing this program emits: `--json`
    // output is parsed by `jq`, by CI, and by the Docker healthcheck, and a
    // stray CR inside a JSON string value — which is where a rewritten newline
    // lands once the document is pretty-printed — is either invalid JSON or a
    // silently different value.
    //
    // Binary mode on both standard streams is the fix, and it is set once here
    // rather than at each writer because there are many writers and this is the
    // one place that already owns the console. Input is included for the same
    // reason `--key-stdin` needs it: a CRLF from a Windows pipe should be
    // trimmed by the reader that knows to trim it, not by the CRT behind its
    // back.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
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
