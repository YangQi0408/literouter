// String, formatting and URL helpers from lr_util.cpp (literouter::trim and
// friends). No sockets, no config: every check here is a pure function call.
#include <filesystem>

#include "lr_test_check.h"

import literouter.core;

namespace {

// Independent of the code under test: reject anything truncateUtf8 produced
// that is not decodable UTF-8, so "cuts on a boundary" is not judged by the
// same routine that made the cut.
bool validUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t follow = 0;
        if (lead < 0x80) {
            follow = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            follow = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            follow = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            follow = 3;
        } else {
            return false;
        }
        if (i + follow >= text.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= follow; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += follow + 1;
    }
    return true;
}

void testTrim() {
    LR_GROUP("trim");
    LR_CHECK_EQ(literouter::trim("  hello  "), "hello");
    LR_CHECK_EQ(literouter::trim("\t\n hi \r\n"), "hi");
    LR_CHECK_EQ(literouter::trim(""), "");
    LR_CHECK_EQ(literouter::trim("   "), "");
    LR_CHECK_EQ(literouter::trim("nospace"), "nospace");
    LR_CHECK_EQ(literouter::trim("  two words inside  "), "two words inside");
}

void testCaseAndAffixes() {
    LR_GROUP("toLower / startsWith / endsWith");
    LR_CHECK_EQ(literouter::toLower("AbC-123_xY"), "abc-123_xy");
    LR_CHECK_EQ(literouter::toLower(""), "");
    LR_CHECK_EQ(literouter::toLower("already lower"), "already lower");

    LR_CHECK(literouter::startsWith("https://api.x.com", "https://"));
    LR_CHECK(!literouter::startsWith("https://api.x.com", "http://"));
    LR_CHECK(literouter::startsWith("abc", "abc"));
    LR_CHECK(literouter::startsWith("abc", ""));
    LR_CHECK(!literouter::startsWith("ab", "abc"));

    LR_CHECK(literouter::endsWith("chunk.json", ".json"));
    LR_CHECK(!literouter::endsWith("chunk.json", ".jsons"));
    LR_CHECK(literouter::endsWith("abc", "abc"));
    LR_CHECK(literouter::endsWith("abc", ""));
    LR_CHECK(!literouter::endsWith("bc", "abc"));
}

void testTruncateUtf8() {
    LR_GROUP("truncateUtf8");

    // Shorter than the limit: returned untouched, and no ellipsis is added.
    LR_CHECK_EQ(literouter::truncateUtf8("short", 64), "short");
    LR_CHECK_EQ(literouter::truncateUtf8("exactly-8", 9), "exactly-8");

    // "日本語テキスト" is seven three-byte characters. A limit of 7 lands inside
    // the third character, so only the first two may survive.
    const std::string japanese = "日本語テキスト";
    LR_CHECK_EQ(japanese.size(), std::size_t{21});
    const std::string cut7 = literouter::truncateUtf8(japanese, 7);
    LR_CHECK_EQ(cut7, "日本…");
    LR_CHECK(validUtf8(cut7));

    // An accent split down the middle: "aé" cut at 2 bytes.
    const std::string accented = "aé";
    const std::string cut2 = literouter::truncateUtf8(accented, 2);
    LR_CHECK_EQ(cut2, "a…");
    LR_CHECK(validUtf8(cut2));

    // A four-byte emoji cannot fit in two bytes at all, so nothing of it is
    // kept — but the result is still decodable.
    const std::string emoji = "🦊🦊";
    const std::string cut1 = literouter::truncateUtf8(emoji, 2);
    LR_CHECK_EQ(cut1, "…");
    LR_CHECK(validUtf8(cut1));

    // Every limit from 0 to the full width yields something decodable.
    bool allBoundariesClean = true;
    for (std::size_t limit = 0; limit <= japanese.size() + 2; ++limit) {
        allBoundariesClean = allBoundariesClean && validUtf8(literouter::truncateUtf8(japanese, limit));
    }
    LR_CHECK_MSG(allBoundariesClean, "a limit on any byte offset produced invalid UTF-8");

    // The ellipsis is U+2026, three bytes, not "...".
    const std::string truncated = literouter::truncateUtf8(japanese, 3);
    LR_CHECK_EQ(truncated, "日…");
    LR_CHECK(literouter::endsWith(truncated, "\xE2\x80\xA6"));
}

// The UTF-8 hygiene pass the JSON writers depend on. Before it existed, a path
// or an upstream body that was not valid UTF-8 reached nlohmann's strict
// `dump()`, which throws — from a worker thread with no handler, so the process
// aborted. These checks pin the two halves: the validator, and the repair that
// makes the strict dump safe.
void testUtf8Hygiene() {
    LR_GROUP("isValidUtf8 / toValidUtf8 / pathToUtf8");

    LR_CHECK(literouter::isValidUtf8(""));
    LR_CHECK(literouter::isValidUtf8("plain ascii"));
    LR_CHECK(literouter::isValidUtf8("配置"));
    LR_CHECK(literouter::isValidUtf8("\xE2\x80\xA6"));
    LR_CHECK(literouter::isValidUtf8("\xF0\x9F\xA6\x8A"));

    // The four shapes a naive "is every byte < 0x80 or >= 0xC0" check lets
    // through, each of which nlohmann refuses.
    LR_CHECK(!literouter::isValidUtf8("\xE9"));                 // a lone lead byte
    LR_CHECK(!literouter::isValidUtf8("caf\xE9"));             // the byte a GBK path ends in
    LR_CHECK(!literouter::isValidUtf8("\xE4\xBD"));            // truncated mid-sequence
    LR_CHECK(!literouter::isValidUtf8("\xC0\xAF"));            // overlong for '/'
    LR_CHECK(!literouter::isValidUtf8("\xED\xA0\x80"));       // a UTF-16 surrogate
    LR_CHECK(!literouter::isValidUtf8("\xF5\x80\x80\x80"));  // past U+10FFFF
    LR_CHECK(!literouter::isValidUtf8("\x80"));                 // a bare continuation byte
    LR_CHECK(!literouter::isValidUtf8("\xE4\xBD\xA0\xE4"));  // good, then truncated

    // A valid string comes back byte-identical, not re-encoded.
    LR_CHECK_EQ(literouter::toValidUtf8("配置"), "配置");
    LR_CHECK_EQ(literouter::toValidUtf8(""), "");

    // Every invalid byte becomes U+FFFD, and nothing else moves.
    LR_CHECK_EQ(literouter::toValidUtf8("caf\xE9"), "caf\xEF\xBF\xBD");
    LR_CHECK_EQ(literouter::toValidUtf8("\xE4\xBD"), "\xEF\xBF\xBD\xEF\xBF\xBD");
    LR_CHECK_EQ(literouter::toValidUtf8("a\x80" "b"), "a\xEF\xBF\xBD" "b");

    // The whole point: whatever goes in, what comes out is decodable, so the
    // strict dump on the other side cannot throw.
    bool alwaysRepairable = true;
    for (int byte = 0; byte < 256; ++byte) {
        const std::string one(1, static_cast<char>(byte));
        alwaysRepairable =
            alwaysRepairable && literouter::isValidUtf8(literouter::toValidUtf8(one));
    }
    LR_CHECK_MSG(alwaysRepairable, "some single byte survived toValidUtf8 as invalid UTF-8");

    // pathToUtf8 is the conversion the whole program uses instead of
    // `path::string()`. It never throws and never returns invalid UTF-8.
    LR_CHECK_EQ(literouter::pathToUtf8(std::filesystem::path{"/tmp/plain.json"}),
                "/tmp/plain.json");
    LR_CHECK_EQ(literouter::pathToUtf8(std::filesystem::path{"/tmp/配置/config.json"}),
                "/tmp/配置/config.json");
    const std::string gbk = std::string{"/tmp/"} + "\xC4\xE3\xBA\xC3" + "/config.json";
    const std::string repaired = literouter::pathToUtf8(std::filesystem::path{gbk});
    LR_CHECK(literouter::isValidUtf8(repaired));
    LR_CHECK(repaired.find("config.json") != std::string::npos);
    LR_CHECK(repaired.find('\xEF') != std::string::npos);
}

// A working directory that no longer exists must not abort the process.
// `userHome()` fell back to the throwing `current_path()` overload whenever HOME
// was unset, and that call sits under `defaultConfigPath()` — a ConfigStore
// member initializer, so every command that builds a store runs it. A shell
// whose cwd had been deleted got `terminate` out of `literouter config path`,
// i.e. the same abort P0 describes, without needing a single unusual byte in a
// path.
//
// POSIX-only: the way to make `getcwd` fail is to remove the directory the
// process is standing in, and Windows keeps an open handle on the cwd of a
// running process, so the removal would fail there rather than the lookup.
#ifndef _WIN32
void testUserHomeWithoutHomeOrCwd() {
    LR_GROUP("userHome() answers with a path when there is no home and no cwd");

    const auto original = std::filesystem::current_path();
    const auto scratch = std::filesystem::temp_directory_path() / "literouter-userhome-probe";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    std::filesystem::current_path(scratch);
    std::filesystem::remove(scratch);

    const lr_test::EnvGuard homeEnv{"HOME"};
    homeEnv.clear();

    // The property under test is that this returns at all: the throwing
    // overload turns the missing directory into a `filesystem_error`, and
    // nothing above here catches it.
    const auto home = literouter::userHome();
    LR_CHECK_MSG(home.empty(), "with no home and no cwd, userHome() invented a directory");

    // The callers that run above any handler have to survive it too.
    const auto configPath = literouter::defaultConfigPath();
    LR_CHECK_MSG(!configPath.empty(), "the default config path resolved to nothing");
    LR_CHECK_MSG(!literouter::defaultStateDir().empty(),
                 "the default state dir resolved to nothing");

    std::filesystem::current_path(original);
    std::filesystem::remove_all(scratch);
}
#endif

void testHumanCount() {
    LR_GROUP("humanCount");
    LR_CHECK_EQ(literouter::humanCount(0), "0");
    LR_CHECK_EQ(literouter::humanCount(1), "1");
    LR_CHECK_EQ(literouter::humanCount(999), "999");
    // Band edge: 1000 is the first value the k band covers.
    LR_CHECK_EQ(literouter::humanCount(1000), "1.0k");
    LR_CHECK_EQ(literouter::humanCount(1200), "1.2k");
    LR_CHECK_EQ(literouter::humanCount(1500), "1.5k");
    LR_CHECK_EQ(literouter::humanCount(9500), "9.5k");
    // Two significant digits are dropped once the mantissa reaches 10.
    LR_CHECK_EQ(literouter::humanCount(10000), "10k");
    LR_CHECK_EQ(literouter::humanCount(15000), "15k");
    // Rounding boundaries, not accidents: the printer rounds, so a mantissa of
    // 999.5+ would have printed as "1000k"; the extra division keeps the label
    // the smallest one that renders correctly. Locked down so a future
    // "simplification" cannot reintroduce "1000k".
    LR_CHECK_EQ(literouter::humanCount(999499), "999k");
    LR_CHECK_EQ(literouter::humanCount(999999), "1.0M");
    LR_CHECK_EQ(literouter::humanCount(1000000), "1.0M");
    LR_CHECK_EQ(literouter::humanCount(2500000), "2.5M");
    LR_CHECK_EQ(literouter::humanCount(1000000000ull), "1.0G");
    LR_CHECK_EQ(literouter::humanCount(1000000000000ull), "1.0T");
    // Past the largest unit the mantissa just keeps growing rather than
    // inventing a P band.
    LR_CHECK_EQ(literouter::humanCount(1000000000000000ull), "1000T");
}

void testHumanMillis() {
    LR_GROUP("humanMillis");
    LR_CHECK_EQ(literouter::humanMillis(0.0), "—");
    LR_CHECK_EQ(literouter::humanMillis(-5.0), "—");
    LR_CHECK_EQ(literouter::humanMillis(0.4), "0ms");
    LR_CHECK_EQ(literouter::humanMillis(1.0), "1ms");
    LR_CHECK_EQ(literouter::humanMillis(820.4), "820ms");
    LR_CHECK_EQ(literouter::humanMillis(999.0), "999ms");
    // Band edge: >= 1000ms switches to seconds with two decimals.
    LR_CHECK_EQ(literouter::humanMillis(1000.0), "1.00s");
    LR_CHECK_EQ(literouter::humanMillis(12400.0), "12.40s");
}

void testHumanBytes() {
    LR_GROUP("humanBytes");
    LR_CHECK_EQ(literouter::humanBytes(0), "0B");
    LR_CHECK_EQ(literouter::humanBytes(1023), "1023B");
    LR_CHECK_EQ(literouter::humanBytes(1024), "1.0KB");
    LR_CHECK_EQ(literouter::humanBytes(1536), "1.5KB");
    LR_CHECK_EQ(literouter::humanBytes(2048), "2.0KB");
    LR_CHECK_EQ(literouter::humanBytes(10 * 1024), "10KB");
    // Rounding boundaries, as in humanCount: 1048575 renders as "1.0MB" rather
    // than "1024KB" because that is what the printed value would have been.
    LR_CHECK_EQ(literouter::humanBytes(1048575), "1.0MB");
    LR_CHECK_EQ(literouter::humanBytes(1024ull * 1024), "1.0MB");
    LR_CHECK_EQ(literouter::humanBytes(3670016), "3.5MB");
    LR_CHECK_EQ(literouter::humanBytes(1024ull * 1024 * 1024), "1.0GB");
    LR_CHECK_EQ(literouter::humanBytes(1099511627776ull), "1.0TB");
}

void testHumanDuration() {
    LR_GROUP("humanDuration");
    LR_CHECK_EQ(literouter::humanDuration(-1.0), "—");
    LR_CHECK_EQ(literouter::humanDuration(0.0), "0s");
    LR_CHECK_EQ(literouter::humanDuration(0.9), "0s");
    LR_CHECK_EQ(literouter::humanDuration(59.0), "59s");
    LR_CHECK_EQ(literouter::humanDuration(60.0), "1m 0s");
    LR_CHECK_EQ(literouter::humanDuration(192.0), "3m 12s");
    LR_CHECK_EQ(literouter::humanDuration(3599.0), "59m 59s");
    LR_CHECK_EQ(literouter::humanDuration(3600.0), "1h 0m");
    LR_CHECK_EQ(literouter::humanDuration(3720.0), "1h 2m");
    LR_CHECK_EQ(literouter::humanDuration(86399.0), "23h 59m");
    LR_CHECK_EQ(literouter::humanDuration(86400.0), "1d 0h");
    LR_CHECK_EQ(literouter::humanDuration(90061.0), "1d 1h");
}

void testHumanUptime() {
    LR_GROUP("humanUptime never drops the seconds");
    LR_CHECK_EQ(literouter::humanUptime(-1.0), "—");
    LR_CHECK_EQ(literouter::humanUptime(0.0), "0s");
    LR_CHECK_EQ(literouter::humanUptime(0.9), "0s");
    LR_CHECK_EQ(literouter::humanUptime(59.0), "59s");
    LR_CHECK_EQ(literouter::humanUptime(60.0), "1m 0s");
    LR_CHECK_EQ(literouter::humanUptime(192.0), "3m 12s");
    // Where humanDuration stops at "1h 2m", this keeps counting: a live tile
    // that only moves once a minute reads as a frozen one.
    LR_CHECK_EQ(literouter::humanUptime(3599.0), "59m 59s");
    LR_CHECK_EQ(literouter::humanUptime(3600.0), "1h 0m 0s");
    LR_CHECK_EQ(literouter::humanUptime(3725.0), "1h 2m 5s");
    LR_CHECK_EQ(literouter::humanUptime(86399.0), "23h 59m 59s");
    LR_CHECK_EQ(literouter::humanUptime(86400.0), "1d 0h 0m 0s");
    LR_CHECK_EQ(literouter::humanUptime(90061.0), "1d 1h 1m 1s");
}

void testRedactSecrets() {
    LR_GROUP("redactSecrets masks credentials and leaves prose alone");
    using literouter::redactSecrets;

    // The shapes people paste into prompts: a curl with a bearer token, a key
    // from a provider dashboard, a JWT, a labelled assignment, a PEM block.
    LR_CHECK_EQ(redactSecrets("curl -H 'Authorization: Bearer sk-abcdefghijklmnopqrstuvwx' x"),
                "curl -H 'Authorization: Bearer [redacted]' x");
    LR_CHECK_EQ(redactSecrets(R"({"api_key":"sk-proj-abcdefghijklmnopqrstuv"})"),
                R"({"api_key":"sk-proj-[redacted]"})");
    LR_CHECK(redactSecrets("token eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.abcdefghijklmnop")
                 .find("[redacted jwt]") != std::string::npos);
    LR_CHECK(redactSecrets("api_key: abcdefghijklmnopqrstuvwxyz012345")
                 .find("[redacted]") != std::string::npos);
    const std::string pem =
        "-----BEGIN RSA PRIVATE KEY-----\nMIIEowIBAAKCAQEA1234\n-----END RSA PRIVATE KEY-----";
    LR_CHECK_EQ(redactSecrets(pem), "[redacted private key]");

    // Prose is not a credential: a bare prefix, a short token and ordinary
    // words must survive untouched, or the log stops being evidence.
    LR_CHECK_EQ(redactSecrets("the sk- prefix marks an OpenAI key"), "the sk- prefix marks an OpenAI key");
    LR_CHECK_EQ(redactSecrets("Bearer short"), "Bearer short");
    LR_CHECK_EQ(redactSecrets("password: hunter2"), "password: hunter2");
    LR_CHECK_EQ(redactSecrets(""), "");
    LR_CHECK_EQ(redactSecrets("nothing to hide here"), "nothing to hide here");
}

void testHexId() {
    LR_GROUP("hexId");
    const std::string id = literouter::hexId();
    LR_CHECK_EQ(id.size(), std::size_t{16});
    LR_CHECK_EQ(literouter::hexId(4).size(), std::size_t{8});
    LR_CHECK_EQ(literouter::hexId(0).size(), std::size_t{0});

    const auto isHex = [](std::string_view value) {
        for (const char c : value) {
            const bool digit = c >= '0' && c <= '9';
            const bool lower = c >= 'a' && c <= 'f';
            if (!digit && !lower) {
                return false;
            }
        }
        return true;
    };
    LR_CHECK(isHex(id));

    // 3000 sixteen-hex-character draws from a 64-bit space: a repeat would be
    // a genuine entropy failure, not bad luck.
    std::unordered_set<std::string> seen;
    bool alphabetClean = true;
    for (int i = 0; i < 3000; ++i) {
        const std::string candidate = literouter::hexId();
        alphabetClean = alphabetClean && isHex(candidate);
        seen.insert(candidate);
    }
    LR_CHECK_EQ(static_cast<long long>(seen.size()), 3000);
    LR_CHECK_MSG(alphabetClean, "a generated id left the hex alphabet");
}

void testSecureEquals() {
    LR_GROUP("secureEquals");
    LR_CHECK(literouter::secureEquals("sk-abc", "sk-abc"));
    LR_CHECK(literouter::secureEquals("", ""));
    LR_CHECK(!literouter::secureEquals("sk-abc", "sk-abd"));
    LR_CHECK(!literouter::secureEquals("sk-abc", "sk-ab"));
    LR_CHECK(!literouter::secureEquals("sk-ab", "sk-abc"));
    LR_CHECK(!literouter::secureEquals("", "sk-abc"));
    // A difference in the final byte must still be caught, not only a prefix.
    LR_CHECK(!literouter::secureEquals("sk-abcx", "sk-abcy"));
}

void testSplitBaseUrl() {
    LR_GROUP("splitBaseUrl");
    struct Case {
        const char *input;
        bool ok;
        const char *root;
        const char *prefix;
        const char *scheme;
    };
    const Case cases[] = {
        {"https://api.x.com/v1", true, "https://api.x.com", "/v1", "https"},
        {"https://api.x.com/v1/", true, "https://api.x.com", "/v1", "https"},
        {"https://api.x.com", true, "https://api.x.com", "", "https"},
        {"https://api.x.com/", true, "https://api.x.com", "", "https"},
        {"http://h:8080", true, "http://h:8080", "", "http"},
        {"http://h:8080/", true, "http://h:8080", "", "http"},
        {"http://h:8080/a/b", true, "http://h:8080", "/a/b", "http"},
        {"http://h:8080/a/b/", true, "http://h:8080", "/a/b", "http"},
        // No scheme at all is accepted and read as https, which is what a
        // hand-typed relay address looks like.
        {"api.x.com", true, "https://api.x.com", "", "https"},
        // Case in the scheme is normalised.
        {"HTTP://Host:1234/V1", true, "http://Host:1234", "/V1", "http"},
        // Surrounding whitespace is tolerated.
        {"  https://api.x.com/v1  ", true, "https://api.x.com", "/v1", "https"},
        // A bare host with a query is still split at the first slash.
        {"example.com/search?q=1", true, "https://example.com", "/search?q=1", "https"},
        // Rejections.
        {"", false, "", "", ""},
        {"   ", false, "", "", ""},
        {"https://", false, "", "", ""},
        {"://host/x", false, "", "", ""},
        {"ftp://host/x", false, "", "", ""},
        {"file:///tmp/x", false, "", "", ""},
    };

    for (const Case &entry : cases) {
        std::string root;
        std::string prefix;
        std::string scheme;
        const bool ok = literouter::splitBaseUrl(entry.input, root, prefix, scheme);
        LR_CHECK_MSG(ok == entry.ok,
                     std::string{"input ["} + entry.input + "] expected ok=" +
                         (entry.ok ? "true" : "false"));
        if (entry.ok) {
            LR_CHECK_MSG(root == entry.root,
                         std::string{"root for ["} + entry.input + "] was [" + root + "]");
            LR_CHECK_MSG(prefix == entry.prefix,
                         std::string{"prefix for ["} + entry.input + "] was [" + prefix + "]");
            LR_CHECK_MSG(scheme == entry.scheme,
                         std::string{"scheme for ["} + entry.input + "] was [" + scheme + "]");
        }
    }

    // A URL rejected for having no host at all must not leave a connection root
    // or path behind from an earlier parse. (`scheme` is documented as an out
    // parameter of a *successful* split, so it is not asserted here.)
    std::string root = "stale";
    std::string prefix = "stale";
    std::string scheme = "stale";
    LR_CHECK(!literouter::splitBaseUrl("https://", root, prefix, scheme));
    LR_CHECK_EQ(root, "");
    LR_CHECK_EQ(prefix, "");
}

void testExtractApiKey() {
    LR_GROUP("extractApiKey");
    using Headers = std::map<std::string, std::string, std::less<>>;

    LR_CHECK_EQ(literouter::extractApiKey(Headers{}), "");
    LR_CHECK_EQ(literouter::extractApiKey(Headers{{"authorization", "Bearer sk-abc"}}), "sk-abc");
    // The scheme is matched case-insensitively; the key itself is not touched.
    LR_CHECK_EQ(literouter::extractApiKey(Headers{{"authorization", "bearer sk-abc"}}), "sk-abc");
    LR_CHECK_EQ(literouter::extractApiKey(Headers{{"authorization", "BEARER   sk-abc  "}}),
                "sk-abc");
    // A relay that sends the key without a scheme still authenticates.
    LR_CHECK_EQ(literouter::extractApiKey(Headers{{"authorization", "  sk-abc  "}}), "sk-abc");
    LR_CHECK_EQ(literouter::extractApiKey(Headers{{"x-api-key", " sk-xyz "}}), "sk-xyz");
    // authorization wins when both are present, which is the order the proxy
    // documents and the order real clients rely on.
    LR_CHECK_EQ(
        literouter::extractApiKey(Headers{{"authorization", "Bearer sk-first"},
                                         {"x-api-key", "sk-second"}}),
        "sk-first");
    // An unrelated header is not a key.
    LR_CHECK_EQ(literouter::extractApiKey(Headers{{"accept", "application/json"}}), "");
}

void testJoinPath() {
    LR_GROUP("joinPath");
    LR_CHECK_EQ(literouter::joinPath("", "/chat/completions"), "/chat/completions");
    LR_CHECK_EQ(literouter::joinPath("/v1", "/chat/completions"), "/v1/chat/completions");
    LR_CHECK_EQ(literouter::joinPath("/v1/", "/chat/completions"), "/v1/chat/completions");
    LR_CHECK_EQ(literouter::joinPath("/v1", "chat/completions"), "/v1/chat/completions");
    LR_CHECK_EQ(literouter::joinPath("", "chat/completions"), "/chat/completions");
    LR_CHECK_EQ(literouter::joinPath("", ""), "/");
    LR_CHECK_EQ(literouter::joinPath("/v1", ""), "/v1");
    LR_CHECK_EQ(literouter::joinPath("/v1/", ""), "/v1");

    // Segment deduplication: avoids "/v1/v1/messages" when base_url prefix is "/v1"
    LR_CHECK_EQ(literouter::joinPath("/v1", "/v1/messages"), "/v1/messages");
    LR_CHECK_EQ(literouter::joinPath("/v1/", "/v1/messages"), "/v1/messages");
    LR_CHECK_EQ(literouter::joinPath("", "/v1/messages"), "/v1/messages");
    LR_CHECK_EQ(literouter::joinPath("/v1beta", "/v1beta/models/gemini"), "/v1beta/models/gemini");
    LR_CHECK_EQ(literouter::joinPath("/v1beta/", "/v1beta/models/gemini"), "/v1beta/models/gemini");
    LR_CHECK_EQ(literouter::joinPath("/v1", "/v1"), "/v1");
    // Nested prefixes like "/alpha/v1" preserve the sub-path
    LR_CHECK_EQ(literouter::joinPath("/alpha/v1", "/v1/messages"), "/alpha/v1/v1/messages");
}

} // namespace

int main() {
    testTrim();
    testCaseAndAffixes();
    testTruncateUtf8();
    testUtf8Hygiene();
#ifndef _WIN32
    testUserHomeWithoutHomeOrCwd();
#endif
    testHumanCount();
    testHumanMillis();
    testHumanBytes();
    testHumanDuration();
    testHumanUptime();
    testRedactSecrets();
    testHexId();
    testSecureEquals();
    testSplitBaseUrl();
    testJoinPath();
    testExtractApiKey();
    return LR_SUMMARY("test_util");
}
