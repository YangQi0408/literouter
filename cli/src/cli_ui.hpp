#pragma once
// Output primitives for the CLI: colour, box-drawing tables, key/value blocks
// and the error/warning printers.
//
// This header names no core type on purpose, so it can be the first include in
// every translation unit — before `import literouter.core;` — and keep the
// verified include/import ordering intact.

#include <atomic>
#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace lrcli {

// Puts the Windows console into UTF-8 for the life of the process, so the
// box-drawing tables and the Chinese dictionary render as themselves instead of
// as mojibake under the default code page (cp936, cp437, …). A no-op elsewhere.
// Called once from main(), before any command runs.
void configureConsoleEncoding();

// Windows delivers a console close, a logoff and a Ctrl-Break as console
// control events rather than as SIGINT/SIGTERM, so `std::signal` never sees
// them and closing the window kills the process outright — no `server.stop()`,
// no last telemetry flush. This installs a handler that sets `flag` for those
// events, which is the same flag the SIGINT handler sets, so the existing stop
// loop is the graceful path on both platforms. A no-op elsewhere.
//
// Both live in cli_console_win.cpp, which is the only file that includes
// windows.h.
void installConsoleStopHandler(std::atomic<bool> *flag);

// Colour is off when --no-color was given, when NO_COLOR is in the environment,
// or when stdout is not a TTY. `configureColor` is called once per command from
// the parsed global flags; `colorEnabled` caches the answer.
void configureColor(bool disabled);
bool colorEnabled();

enum class Align { Left, Right };

struct Column {
    std::string header;
    Align align = Align::Left;
};

// A small box-drawing table. Cells may already carry ANSI sequences; column
// widths are measured on the visible text so colour never breaks alignment.
class Table {
public:
    void column(std::string header, Align align = Align::Left);
    void row(std::vector<std::string> cells);
    void print() const;
    void print(std::ostream &out) const;
    bool empty() const { return rows_.empty(); }

private:
    std::vector<Column> columns_;
    std::vector<std::vector<std::string>> rows_;
};

// An aligned "key  value" block, optionally with section headings.
class KeyValues {
public:
    void section(std::string title);
    void add(std::string key, std::string value);
    void print() const;
    void print(std::ostream &out) const;

private:
    struct Item {
        bool isSection = false;
        std::string key;
        std::string value;
    };
    std::vector<Item> items_;
};

std::string paint(std::string_view text, std::string_view sgr);
std::string bold(std::string_view text);
std::string dim(std::string_view text);

std::string colorHealth(std::string_view state, std::string_view text);
std::string colorLevel(std::string_view level, std::string_view text);
std::string colorStatus(int status, std::string_view text);
std::string colorEnabled(bool enabled, std::string_view text);

std::string stripAnsi(std::string_view text);
std::size_t visibleWidth(std::string_view text);

// "literouter: error: …" on stderr.
void printError(const std::string &message);
void printWarn(const std::string &message);
// A secondary "  hint: …" line, printed right after an error/warning.
void printHint(const std::string &message);
// Informational line on stdout.
void printInfo(const std::string &message);
// Printed only when --quiet was not given.
void printNote(const std::string &message, bool quiet);

} // namespace lrcli
