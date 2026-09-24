// The CLI's pure output layer: `cli/src/cli_ui.cpp` and the helpers in
// `cli/src/cli_core.cpp`. No socket, no config file, no CLI11 parse — every
// check here is a function call and a string comparison.
//
// This is the first test target under `cli/`. The three smoke commands CI runs
// (`--version`, `config init`, `config validate`) exercise the wiring but none
// of this: the table renderer, the ANSI stripper and the East-Asian width
// arithmetic are exactly the code where an off-by-one shows up as a misaligned
// table rather than as a failing command, so nothing caught it before.
#include "cli_ui.hpp"

#include <cstdio>
#include <sstream>
#include <string>

#include "lr_test_check.h"

import literouter.core;
import nlohmann.json;

#include "cli_core.hpp"
#include "cli_json.hpp"

namespace {

using lrcli::Align;
using lrcli::KeyValues;
using lrcli::Table;

void testStripAnsi() {
    LR_GROUP("stripAnsi: SGR sequences go, everything else stays");

    LR_CHECK_EQ(lrcli::stripAnsi("plain"), "plain");
    LR_CHECK_EQ(lrcli::stripAnsi("\x1b[31mred\x1b[0m"), "red");
    LR_CHECK_EQ(lrcli::stripAnsi("\x1b[1;32mbold green\x1b[0m"), "bold green");
    // The text between two sequences is not lost, and a sequence in the middle
    // does not swallow what follows.
    LR_CHECK_EQ(lrcli::stripAnsi("a\x1b[0mb"), "ab");
    // An ESC that is not the start of a CSI is ordinary text, not a sequence.
    LR_CHECK_EQ(lrcli::stripAnsi("a\x1b" "b"), "a\x1b" "b");
    // An unterminated sequence consumes the rest rather than reading past it.
    LR_CHECK_EQ(lrcli::stripAnsi("a\x1b[31"), "a");
    LR_CHECK_EQ(lrcli::stripAnsi(""), "");
}

void testVisibleWidth() {
    LR_GROUP("visibleWidth: ANSI is free, wide characters cost two");

    LR_CHECK_EQ(lrcli::visibleWidth("abc"), 3u);
    LR_CHECK_EQ(lrcli::visibleWidth(""), 0u);
    // Colour is not layout.
    LR_CHECK_EQ(lrcli::visibleWidth("\x1b[31mabc\x1b[0m"), 3u);

    // The East-Asian cases the tables depend on: a Chinese label is two columns
    // per character, so a column of them lines up with one of ASCII.
    LR_CHECK_EQ(lrcli::visibleWidth("配置"), 4u);
    LR_CHECK_EQ(lrcli::visibleWidth("中转站"), 6u);
    LR_CHECK_EQ(lrcli::visibleWidth("a配置b"), 6u);
    // Fullwidth forms.
    LR_CHECK_EQ(lrcli::visibleWidth("（）"), 4u);
    // "…" (U+2026) is East-Asian *ambiguous*: one column in a Latin terminal,
    // two in a CJK one. This table treats it as narrow, which is the only
    // choice that is right in the common case and is why truncateUtf8()'s
    // ellipsis can look one column off in a CJK terminal.
    LR_CHECK_EQ(lrcli::visibleWidth("…"), 1u);
    // A 4-byte sequence is decoded rather than counted as four bytes.
    LR_CHECK_EQ(lrcli::visibleWidth("\xF0\x9F\x98\x80"), 1u);

    // A truncated multi-byte sequence must not be counted as a character, and
    // must not read past the end of the buffer either.
    LR_CHECK_EQ(lrcli::visibleWidth("\xE9"), 0u);
    LR_CHECK_EQ(lrcli::visibleWidth("\xE4\xBD"), 0u);
    LR_CHECK_EQ(lrcli::visibleWidth("\xF0\x9F\x98"), 0u);
}

void testPaint() {
    LR_GROUP("paint: colour is off when it is not a TTY");

    // The test binary's stdout is a pipe, so configureColor(false) turns colour
    // off; every painter must then return its input unchanged, which is what
    // keeps the width arithmetic above true in a redirected run.
    lrcli::configureColor(false);
    LR_CHECK(!lrcli::colorEnabled());
    LR_CHECK_EQ(lrcli::paint("x", "31"), "x");
    LR_CHECK_EQ(lrcli::bold("x"), "x");
    LR_CHECK_EQ(lrcli::dim("x"), "x");
    LR_CHECK_EQ(lrcli::colorStatus(200, "ok"), "ok");
    LR_CHECK_EQ(lrcli::colorStatus(500, "boom"), "boom");
    LR_CHECK_EQ(lrcli::colorLevel("error", "e"), "e");
    LR_CHECK_EQ(lrcli::colorEnabled(true, "on"), "on");

    // --no-color and NO_COLOR are the same switch, and it must stick: a later
    // call cannot turn colour back on for a process that asked for none.
    lrcli::configureColor(true);
    LR_CHECK(!lrcli::colorEnabled());
}

void testTable() {
    LR_GROUP("Table: borders, alignment and column sizing");

    std::ostringstream out;
    Table table;
    table.column("NAME");
    table.column("N", Align::Right);
    table.row({"alpha", "1"});
    table.row({"b", "22"});
    table.print(out);

    const std::string text = out.str();
    LR_CHECK(text.find("┌") != std::string::npos);
    LR_CHECK(text.find("└") != std::string::npos);
    // Every column is sized by its widest cell — "alpha" for the first, "22"
    // for the second — and the right-aligned one pads on the left.
    LR_CHECK(text.find("│ NAME  │  N │") != std::string::npos);
    LR_CHECK(text.find("│ alpha │  1 │") != std::string::npos);
    LR_CHECK(text.find("│ b     │ 22 │") != std::string::npos);
    // Every line of a box has the same visible width, which is the property the
    // alignment bugs break.
    const auto lines = [&text] {
        std::vector<std::string> result;
        std::string current;
        for (const char c : text) {
            if (c == '\n') {
                result.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        return result;
    }();
    LR_CHECK(!lines.empty());
    const std::size_t width = lrcli::visibleWidth(lines.front());
    for (const auto &line : lines) {
        LR_CHECK_EQ(lrcli::visibleWidth(line), width);
    }

    // A table with no columns prints nothing at all rather than an empty box.
    std::ostringstream empty;
    Table none;
    none.print(empty);
    LR_CHECK_EQ(empty.str(), "");
}

void testTableWideCells() {
    LR_GROUP("Table: a Chinese cell sizes the column like an ASCII one");

    std::ostringstream out;
    Table table;
    table.column("MODEL");
    table.row({"配置"});
    table.row({"abcd"});
    table.print(out);

    const std::string text = out.str();
    // "配置" measures four columns, so it is padded to the width of "MODEL"
    // with one space — the same amount "abcd" gets. The point is that the two
    // data rows have the same visible width, which is what a table that
    // counted the Chinese cell as six bytes would get wrong.
    LR_CHECK(text.find("│ 配置  │") != std::string::npos);
    LR_CHECK(text.find("│ abcd  │") != std::string::npos);
    LR_CHECK_EQ(lrcli::visibleWidth("│ 配置  │"), lrcli::visibleWidth("│ abcd  │"));
}

void testKeyValues() {
    LR_GROUP("KeyValues: keys are padded to the widest, values follow");

    std::ostringstream out;
    KeyValues info;
    info.add("a", "1");
    info.add("longer-key", "2");
    info.print(out);
    LR_CHECK_EQ(lrcli::stripAnsi(out.str()), "  a           1\n  longer-key  2\n");

    // A section heading is its own line, with a blank line before every one
    // after the first.
    std::ostringstream sections;
    KeyValues grouped;
    grouped.section("first");
    grouped.add("k", "v");
    grouped.section("second");
    grouped.add("k2", "v2");
    grouped.print(sections);
    LR_CHECK_EQ(lrcli::stripAnsi(sections.str()),
                "first\n  k   v\n\nsecond\n  k2  v2\n");

    // The width comes from visibleWidth(), so a Chinese key pads to the same
    // column as an ASCII one of the same display width.
    std::ostringstream wide;
    KeyValues mixed;
    mixed.add("中转站", "1");
    mixed.add("ab", "2");
    mixed.print(wide);
    LR_CHECK_EQ(lrcli::stripAnsi(wide.str()), "  中转站  1\n  ab      2\n");
}

void testBaseUrls() {
    LR_GROUP("baseUrlOf / clientBaseUrlOf");

    literouter::AppConfig config;
    config.server.host = "127.0.0.1";
    config.server.port = 8787;

    LR_CHECK_EQ(lrcli::baseUrlOf(config), "http://127.0.0.1:8787");
    LR_CHECK_EQ(lrcli::clientBaseUrlOf(config), "http://127.0.0.1:8787/v1");

    // TLS switches the scheme, and that is what a client has to be pointed at.
    config.server.tls_cert_file = "/tmp/cert.pem";
    config.server.tls_key_file = "/tmp/key.pem";
    LR_CHECK_EQ(lrcli::baseUrlOf(config), "https://127.0.0.1:8787");
    LR_CHECK_EQ(lrcli::clientBaseUrlOf(config), "https://127.0.0.1:8787/v1");
}

void testDescribeApiKey() {
    LR_GROUP("describeApiKey: references are named, literals are masked");

    // A ${VAR} reference is shown as the reference, never expanded — the whole
    // point of the placeholder scheme is that the CLI can print the config
    // without ever holding the secret.
    LR_CHECK_EQ(lrcli::describeApiKey("${OPENAI_API_KEY}"), "$OPENAI_API_KEY");
    LR_CHECK_EQ(lrcli::describeApiKey("$OPENAI_API_KEY"), "$OPENAI_API_KEY");
    // The :-fallback form names the variable and drops the fallback.
    LR_CHECK_EQ(lrcli::describeApiKey("${VAR:-sk-fallback}"), "$VAR");
    // A malformed reference says so rather than printing half of it.
    LR_CHECK_EQ(lrcli::describeApiKey("${UNCLOSED"), "$<malformed>");

    LR_CHECK_EQ(lrcli::describeApiKey(""), "(none)");

    // A literal key is masked: the ends survive so two keys are still
    // distinguishable in a list, the middle does not.
    const std::string masked = lrcli::describeApiKey("sk-abcdefghijklmnop");
    LR_CHECK(masked.find("sk-a") == 0);
    LR_CHECK(masked.find("mnop") != std::string::npos);
    LR_CHECK(masked.find("efghijkl") == std::string::npos);
    LR_CHECK(masked.find("\xE2\x80\xA6") != std::string::npos);

    // A short key has no safe ends to show, so it is all asterisks.
    LR_CHECK_EQ(lrcli::describeApiKey("short"), "*****");
}

// The regression the P0 fix exists for, at the layer the crash happened: a
// config path whose bytes are not UTF-8 (a Chinese Windows user name narrowed
// through cp936) put through the exact composition `config path --json` uses.
// Before the fix that reached nlohmann's strict dump, threw type_error.316 and
// aborted the process; these checks are the same three calls in the same order.
void testJsonWithInvalidUtf8Path() {
    LR_GROUP("a --json payload survives a path that is not valid UTF-8");

    // "你好" as cp936 bytes, which is what path::string() yields for a Chinese
    // user name on a DBCS Windows code page.
    const std::filesystem::path gbk{std::string{"/tmp/"} + "\xC4\xE3\xBA\xC3" +
                                    "/config.json"};
    LR_CHECK(!literouter::isValidUtf8(gbk.string()));

    // Exactly what runPath()/runList()/runValidate() build.
    nlohmann::json root = nlohmann::json::object();
    root["exists"] = false;
    root["path"] = literouter::pathToUtf8(gbk);

    const std::string text = dumpJson(root, 2);
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    LR_CHECK_MSG(!parsed.is_discarded(), "the JSON did not parse");
    if (!parsed.is_discarded()) {
        const std::string path = parsed.at("path").get<std::string>();
        LR_CHECK(literouter::isValidUtf8(path));
        // The tail is still readable, so the caller can still tell which file
        // this is even though the directory name could not be represented.
        LR_CHECK(path.find("config.json") != std::string::npos);
    }

    // The other shape the same fix covers: a relay id or model name from a
    // config file with a stray byte in it, which is what `providers list
    // --json` renders.
    nlohmann::json listing = nlohmann::json::object();
    listing["path"] = literouter::pathToUtf8(gbk);
    listing["providers"] = nlohmann::json::array();
    listing["providers"].push_back({{"id", std::string{"relay"} + "\xE9"}});
    const nlohmann::json listingParsed =
        nlohmann::json::parse(dumpJson(listing, 2), nullptr, false);
    LR_CHECK_MSG(!listingParsed.is_discarded(), "the provider listing did not parse");
    if (!listingParsed.is_discarded()) {
        LR_CHECK(literouter::isValidUtf8(
            listingParsed.at("providers").at(0).at("id").get<std::string>()));
    }

    // Indent 0 and the default (compact) form take the same path.
    LR_CHECK(!dumpJson(root, 0).empty());
    LR_CHECK(!dumpJson(root).empty());
}

void testTrAlias() {
    LR_GROUP("lrcli::tr returns an owning string in the current language");

    // CLI11 takes a `std::string` for every description, and core's tr()
    // returns a view — the alias exists for that conversion, so it is checked
    // in both languages rather than only that it compiles.
    literouter::i18n::setLang(literouter::i18n::Lang::Zh);
    LR_CHECK_EQ(lrcli::tr("config file"), "配置文件");
    LR_CHECK_EQ(lrcli::tr("no such string"), "no such string");

    literouter::i18n::setLang(literouter::i18n::Lang::En);
    LR_CHECK_EQ(lrcli::tr("config file"), "config file");
}

void testPrintIssues() {
    LR_GROUP("printIssues: the clean case says so in one line");

    literouter::ValidationReport report;
    std::ostringstream out;
    lrcli::printIssues(report, out);
    LR_CHECK_EQ(lrcli::stripAnsi(out.str()), "no validation issues\n");
}

} // namespace

int main() {
    testStripAnsi();
    testVisibleWidth();
    testPaint();
    testTable();
    testTableWideCells();
    testKeyValues();
    testBaseUrls();
    testDescribeApiKey();
    testJsonWithInvalidUtf8Path();
    testTrAlias();
    testPrintIssues();
    return LR_SUMMARY("test_ui");
}
