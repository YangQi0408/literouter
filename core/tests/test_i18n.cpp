#include "lr_test_check.h"

import literouter.core;

namespace {

using literouter::i18n::Lang;
using literouter::i18n::parseLang;
using literouter::i18n::langCode;
using literouter::i18n::langDisplayName;
using literouter::i18n::detectSystemLang;
using literouter::i18n::resolveLang;
using literouter::i18n::setLang;
using literouter::i18n::getLang;
using literouter::i18n::tr;

void testParsing() {
    LR_GROUP("i18n::parseLang and langCode");

    LR_CHECK(parseLang("auto") == Lang::Auto);
    LR_CHECK(parseLang("") == Lang::Auto);
    LR_CHECK(parseLang("unknown") == Lang::Auto);

    LR_CHECK(parseLang("en") == Lang::En);
    LR_CHECK(parseLang("EN") == Lang::En);
    LR_CHECK(parseLang("en-US") == Lang::En);
    LR_CHECK(parseLang("en_GB.UTF-8") == Lang::En);
    LR_CHECK(parseLang("english") == Lang::En);

    LR_CHECK(parseLang("zh") == Lang::Zh);
    LR_CHECK(parseLang("ZH") == Lang::Zh);
    LR_CHECK(parseLang("zh-CN") == Lang::Zh);
    LR_CHECK(parseLang("zh_CN.UTF-8") == Lang::Zh);
    LR_CHECK(parseLang("zh-TW") == Lang::Zh);
    LR_CHECK(parseLang("chinese") == Lang::Zh);

    LR_CHECK_EQ(langCode(Lang::Auto), "auto");
    LR_CHECK_EQ(langCode(Lang::En), "en");
    LR_CHECK_EQ(langCode(Lang::Zh), "zh");

    LR_CHECK(langDisplayName(Lang::En).find("English") != std::string_view::npos);
    LR_CHECK(langDisplayName(Lang::Zh).find("中文") != std::string_view::npos);
}

void testDetection() {
    LR_GROUP("i18n::detectSystemLang and resolveLang");

    lr_test::EnvGuard guardAll("LC_ALL");
    lr_test::EnvGuard guardMsg("LC_MESSAGES");
    lr_test::EnvGuard guardLang("LANG");

    guardMsg.clear();
    guardLang.clear();

    guardAll.assign("zh_CN.UTF-8");
    LR_CHECK(detectSystemLang() == Lang::Zh);
    LR_CHECK(resolveLang(Lang::Auto) == Lang::Zh);

    guardAll.assign("en_US.UTF-8");
    LR_CHECK(detectSystemLang() == Lang::En);
    LR_CHECK(resolveLang(Lang::Auto) == Lang::En);

    // Explicit setting is never changed by resolution
    LR_CHECK(resolveLang(Lang::Zh) == Lang::Zh);
    LR_CHECK(resolveLang(Lang::En) == Lang::En);

    // A language this build has no dictionary for is not a match: the search
    // carries on to the next variable rather than settling on English, so
    // LC_ALL=fr_FR with LANG=zh_CN is Chinese and not the first variable's
    // value taken literally.
    guardAll.assign("fr_FR.UTF-8");
    guardLang.assign("zh_CN.UTF-8");
    LR_CHECK(detectSystemLang() == Lang::Zh);
    guardLang.assign("en_US.UTF-8");
    LR_CHECK(detectSystemLang() == Lang::En);

    // Nothing recognisable anywhere falls back to English, which is the
    // language the source strings are written in.
    guardAll.assign("de_DE.UTF-8");
    guardLang.assign("ja_JP.UTF-8");
    LR_CHECK(detectSystemLang() == Lang::En);
}

void testTranslations() {
    LR_GROUP("i18n::tr translations and fallbacks");

    // English mode: always returns original text
    LR_CHECK_EQ(tr("MODELS", Lang::En), "MODELS");
    LR_CHECK_EQ(tr("NonExistentString123", Lang::En), "NonExistentString123");

    // Chinese mode: the strings the CLI itself renders resolve.
    LR_CHECK_EQ(tr("config file", Lang::Zh), "配置文件");
    LR_CHECK_EQ(tr("validation", Lang::Zh), "配置校验");
    LR_CHECK_EQ(tr("route table", Lang::Zh), "路由表");
    LR_CHECK_EQ(tr("listener", Lang::Zh), "监听服务");
    LR_CHECK_EQ(tr("all checks passed", Lang::Zh), "所有体检项均通过");
    LR_CHECK_EQ(tr("RELAY", Lang::Zh), "中转站");
    LR_CHECK_EQ(tr("LATENCY", Lang::Zh), "延迟");

    // The strings added with the cache, relay limits and the outbound
    // protocols. Each is asserted here rather than only added to the map: a
    // dictionary entry nothing checks is an entry that can silently stop
    // matching the source text, and tr() falls back to English without saying
    // so.
    LR_CHECK_EQ(tr("relay concurrency and rate limits cannot be negative", Lang::Zh),
                "中转站并发与速率限制不可为负数");
    LR_CHECK_EQ(tr("a trend needs at least two buckets to show a shape", Lang::Zh),
                "趋势图至少需要两个桶才能显示出形状");
    LR_CHECK_EQ(tr("the entry count cannot be negative", Lang::Zh), "缓存条目数不可为负数");
    LR_CHECK_EQ(tr("credentials_file is not a readable regular file", Lang::Zh),
                "credentials_file 不是可读取的普通文件");
    LR_CHECK_EQ(tr("otlp_endpoint must start with http:// or https://", Lang::Zh),
                "otlp_endpoint 必须以 http:// 或 https:// 开头");
    LR_CHECK_EQ(tr("Vertex project id", Lang::Zh), "Vertex 项目 ID");

    // Fallback on missing key
    LR_CHECK_EQ(tr("NonExistentString123", Lang::Zh), "NonExistentString123");

    // C-string overload
    const char* orig = "config file";
    const char* zh = tr(orig, Lang::Zh);
    LR_CHECK_EQ(std::string_view(zh), std::string_view("配置文件"));

    // Global state setLang / getLang
    const auto oldLang = getLang();
    setLang(Lang::Zh);
    LR_CHECK(getLang() == Lang::Zh);
    LR_CHECK_EQ(tr("config file"), "配置文件");

    setLang(Lang::En);
    LR_CHECK(getLang() == Lang::En);
    LR_CHECK_EQ(tr("config file"), "config file");

    setLang(oldLang);
}

// The CLI's own surface: every string the command line renders through tr()
// must have a dictionary entry, because tr() falls back to the English text
// silently. Listing them here is what turns a missing translation into a test
// failure instead of English appearing in a Chinese run.
void testCliTranslations() {
    LR_GROUP("i18n: the CLI surface resolves to Chinese");

    const char* kCliStrings[] = {
        "literouter — one local endpoint that fans a request out across your configured relays",
        "config file",
        "validation",
        "secret references",
        "enabled relays",
        "route table",
        "listener",
        "run `literouter config init` to write the seed",
        "run `literouter config validate` to see the warnings",
        "doctor found at least one FAIL",
        "all checks passed",
        "at least one hop is disabled and will be skipped",
        "Run the proxy in the foreground",
        "Query a running instance for its telemetry",
        "List every logical model and the ordered relays that can serve it",
        "Manage relays (edits the config file, no server needed)",
        "Manage routes (edits the config file, no server needed)",
        "Inspect and edit the config file",
        "Diagnose the config and the environment",
        "Read (and optionally follow) the request log",
        "Send a saved request body to one relay",
        "Emit machine-readable JSON",
        "Never emit ANSI colour",
        "Suppress non-essential output",
        "Language (auto, en, zh)",
        "PEM certificate chain absolute path for HTTPS",
        "Unencrypted PEM private key absolute path for HTTPS",
        "tls_cert_file and tls_key_file must both be set, or both empty for HTTP",
        "relay concurrency and rate limits cannot be negative",
        "a trend needs at least two buckets to show a shape",
        "the entry count cannot be negative",
        "otlp_endpoint must start with http:// or https://",
        "Vertex project id",
        "credentials_file is not a readable regular file",
        // The option descriptions every subcommand renders. They were bare
        // string literals once, so `LITEROUTER_LANG=zh` printed English for
        // every one of them; the list is here so the next bare literal is a
        // failing test rather than an English line in a Chinese run.
        "A model id this relay advertises (repeatable)",
        "Add a relay to the config file",
        "Add a route, or append hops to an existing one",
        "Add the route disabled",
        "Ask every relay that serves a model the same question",
        "Azure: the api-version query; Vertex: the API version segment",
        "Bedrock STS session token, for temporary credentials",
        "Bedrock SigV4 access key id (a ${VAR} reference is stored as written)",
        "Bedrock SigV4 secret access key",
        "Bedrock region (required there), or the Vertex location",
        "Chat completions path (default /chat/completions)",
        "Destination file; omit or `-` for stdout",
        "Disable a route",
        "Do not persist counters and the request log for this run only",
        "Do not serve the built-in console at /ui for this run only",
        "Dump the effective config JSON and exit",
        "Embeddings path (default /embeddings)",
        "Enable a route",
        "Enable the relay (default)",
        "Extra upstream header, `Name: value` (repeatable)",
        "Free-form note",
        "Human label (defaults to the id)",
        "Include unrouted provider models in pass-through mode",
        "Input price in dollars per million tokens (0 = unknown)",
        "List configured relays",
        "List the route table",
        "Literal API key",
        "Load a whole config, or merge named entries into the current one",
        "Load and validate, then exit without binding",
        "Lower wins (default 100)",
        "Mark a relay disabled",
        "Mark a relay enabled",
        "Maximum entries to fetch (default 50)",
        "Model to ask for (default: the body's own)",
        "Model to benchmark (required)",
        "Most requests literouter sends this relay at once; 0 is unlimited",
        "Most requests literouter starts on this relay per minute; 0 is unlimited",
        "Only entries at this level",
        "Output price in dollars per million tokens (0 = unknown)",
        "Override server.host for this run only",
        "Override server.port for this run only",
        "Overwrite an existing file",
        "Path to the config file (default: $LITEROUTER_CONFIG, else "
        "~/.config/literouter/config.json)",
        "Per-request timeout in seconds (default 120)",
        "Per-request timeout in seconds (default 30)",
        "Per-request timeout in seconds (default: the relay's own)",
        "Persist counters and the request log for this run only",
        "Poll every 250ms and print new entries until Ctrl-C",
        "Print the answer body",
        "Print the config file as it is on disk",
        "Print the outcome as JSON",
        "Print the resolved config path",
        "Print the whole config, or write it to a file",
        "Probe relays with GET {base_url}/models",
        "Prompt to send (default: a one-word request)",
        "Read one line from stdin",
        "Relay id",
        "Relay id (used by routes and logs)",
        "Relay ids to probe (default: every enabled relay)",
        "Relay to send it to (default: the first the policy would try)",
        "Remove a relay from the config file",
        "Remove a route",
        "Remove even while a route still targets it",
        "Replace only the relays and routes the file names",
        "Request body to replay (OpenAI chat JSON)",
        "Requests per relay (default 1)",
        "Rewrite an older config onto this build's schema",
        "Run even when validate() reports errors",
        "Serve the built-in console at /ui for this run only",
        "Source file; `-` reads standard input",
        "Start after this sequence number (0 = newest)",
        "Store ${VAR}; the key is read at request time",
        "The model name a client sends",
        "The route's model name",
        "Tie-break weight within a priority (default 1)",
        "Upstream API protocol: openai, anthropic, gemini, openai_responses, azure, vertex, "
        "bedrock, ollama (default openai)",
        "Upstream root, e.g. https://api.openai.com/v1",
        "Validate and report without writing",
        "Validate the config and print every issue",
        "Vertex service-account JSON key path",
        "Write the seed config",
        "max_tokens to ask for (default 16)",
        "provider[:upstream-model] (repeatable, order = failover order)",
    };
    for (const char* text : kCliStrings) {
        LR_CHECK_MSG(std::string_view(tr(text, Lang::Zh)) != std::string_view(text),
                     std::format("`{}` has no Chinese translation", text));
    }

    // `open` is the circuit-breaker state, so it must read as "tripped" rather
    // than "switched on". It used to have a second entry meaning the latter,
    // which silently won because unordered_map keeps the last duplicate — the
    // CLI then printed "3 开启" for three open breakers.
    LR_CHECK_EQ(tr("open", Lang::Zh), "熔断");

    // The status and bench tables' own labels. They reach the dictionary from
    // `cli_cmd_status.cpp` and `cli_cmd_bench.cpp` rather than from an option
    // description, so the list above does not reach them; they are asserted
    // here for the same reason it exists — a label with no entry prints English
    // inside an otherwise Chinese table, and nothing else would say so.
    const char* kTableLabels[] = {
        "requests", "tokens", "avg latency", "bytes out", "breakers",
        "REQUESTS", "STATE", "SUCCESS", "AVG", "FAILOVER",
        "RELAY", "STATUS", "LATENCY", "TOKENS", "COST", "BYTES", "BEST",
    };
    for (const char* text : kTableLabels) {
        LR_CHECK_MSG(std::string_view(tr(text, Lang::Zh)) != std::string_view(text),
                     std::format("`{}` has no Chinese translation", text));
    }
}

} // namespace

int main() {
    testParsing();
    testDetection();
    testTranslations();
    testCliTranslations();
    return LR_SUMMARY("test_i18n");
}
