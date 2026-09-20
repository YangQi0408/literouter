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
}

void testTranslations() {
    LR_GROUP("i18n::tr translations and fallbacks");

    // English mode: always returns original text
    LR_CHECK_EQ(tr("Overview", Lang::En), "Overview");
    LR_CHECK_EQ(tr("Settings", Lang::En), "Settings");
    LR_CHECK_EQ(tr("NonExistentString123", Lang::En), "NonExistentString123");

    // Chinese mode: returns translated text
    LR_CHECK_EQ(tr("Overview", Lang::Zh), "概览");
    LR_CHECK_EQ(tr("Providers", Lang::Zh), "中转站");
    LR_CHECK_EQ(tr("Routes", Lang::Zh), "路由");
    LR_CHECK_EQ(tr("Logs", Lang::Zh), "日志");
    LR_CHECK_EQ(tr("Settings", Lang::Zh), "设置");
    LR_CHECK_EQ(tr("Save", Lang::Zh), "保存");
    LR_CHECK_EQ(tr("TOTAL REQUESTS", Lang::Zh), "总请求数");
    LR_CHECK_EQ(tr("SUCCESS RATE", Lang::Zh), "成功率");

    // The strings this round of work added. Each is asserted here rather than
    // only added to the map: a dictionary entry nothing checks is an entry that
    // can silently stop matching the source text, and tr() falls back to English
    // without saying so.
    LR_CHECK_EQ(tr("Daily budget", Lang::Zh), "每日预算");
    LR_CHECK_EQ(tr("Spend today", Lang::Zh), "今日消费");
    LR_CHECK_EQ(tr("a daily budget is a nonnegative number of US dollars; use 0 to disable it",
                   Lang::Zh),
                "每日预算必须是非负的美元金额；填 0 关闭该限制");
    LR_CHECK_EQ(tr("relay concurrency and rate limits cannot be negative", Lang::Zh),
                "中转站并发与速率限制不可为负数");
    LR_CHECK_EQ(tr("a trend needs at least two buckets to show a shape", Lang::Zh),
                "趋势图至少需要两个桶才能显示出形状");
    LR_CHECK_EQ(tr("the entry count cannot be negative", Lang::Zh), "缓存条目数不可为负数");
    LR_CHECK_EQ(tr("credentials_file is not a readable regular file", Lang::Zh),
                "credentials_file 不是可读取的普通文件");
    LR_CHECK_EQ(tr("otlp_endpoint must start with http:// or https://", Lang::Zh),
                "otlp_endpoint 必须以 http:// 或 https:// 开头");
    // Every new key must differ from its source text, or it is a copy that does
    // nothing.
    for (const char *text : {"Daily budget", "Spend today",
                             "a trend needs at least two buckets to show a shape",
                             "the entry count cannot be negative",
                             "credentials_file is not a readable regular file"}) {
        LR_CHECK_MSG(std::string_view(tr(text, Lang::Zh)) != std::string_view(text),
                     std::format("`{}` has no Chinese translation", text));
    }

    // Fallback on missing key
    LR_CHECK_EQ(tr("NonExistentString123", Lang::Zh), "NonExistentString123");

    // C-string overload
    const char* orig = "Overview";
    const char* zh = tr(orig, Lang::Zh);
    LR_CHECK_EQ(std::string_view(zh), std::string_view("概览"));

    // Global state setLang / getLang
    const auto oldLang = getLang();
    setLang(Lang::Zh);
    LR_CHECK(getLang() == Lang::Zh);
    LR_CHECK_EQ(tr("Overview"), "概览");

    setLang(Lang::En);
    LR_CHECK(getLang() == Lang::En);
    LR_CHECK_EQ(tr("Overview"), "Overview");

    setLang(oldLang);
}

// The GUI surface added with the response cache, relay limits, the protocol
// pane and the configurable trend window. Every one of these was shipped with a
// tr() wrapper and no dictionary entry, so a Chinese console rendered them in
// English — which tr() does silently, by design. Listing them here means the
// next such omission fails a test instead of quietly appearing in the UI.
void testGuiSurfaceTranslations() {
    LR_GROUP("i18n: the GUI surface added with the cache, limits and protocol pane");

    const char* kGuiStrings[] = {
        "CACHE HITS",
        "Performance and metrics",
        "What the console keeps, and where metrics go",
        "Trend bucket width (seconds)",
        "60 watches the last minutes; 3600 the last day",
        "Trend buckets kept",
        "Response cache TTL (seconds)",
        "0 disables it; streaming is never cached",
        "Cache entries kept",
        "OTLP metrics endpoint",
        "Empty disables export; metrics go to {endpoint}/v1/metrics",
        "Requests per bucket, over the configured trend window",
        "No traffic in the configured trend window",
        "Protocol settings and limits",
        "Azure api-version / Vertex API version",
        "Azure defaults to 2024-10-21; Vertex to v1.",
        "Bedrock region / Vertex location",
        "Bedrock signs against a region and cannot guess one; Vertex defaults to us-central1.",
        "Vertex project id",
        "Vertex service-account key file",
        "Absolute path on the machine running literouter. Its private key is exchanged for an access token, which is cached.",
        "Bedrock access key id",
        "Bedrock secret access key",
        "Bedrock session token (optional)",
        "Only for temporary credentials.",
        "Resolved only when a request is signed.",
        "A ${VAR} reference stays a reference in the file.",
        "Relay limit: concurrent requests",
        "What literouter itself sends this relay at once; 0 is unlimited. A full relay is skipped, not failed.",
        "Relay limit: requests per minute",
        "Daily quotas and budgets reset at 00:00 UTC. A budget is checked when a request arrives, so one already in flight when the ceiling is reached still completes. Token totals include reservations.",
        "0 disables a limit. Daily quotas and budgets reset at 00:00 UTC; a budget is checked when a request arrives, so one already in flight when the ceiling is reached still completes. Token reservations remain charged when upstream usage is missing.",
        "Models list",
    };
    for (const char* text : kGuiStrings) {
        LR_CHECK_MSG(std::string_view(tr(text, Lang::Zh)) != std::string_view(text),
                     std::format("`{}` has no Chinese translation", text));
    }

    // `open` is the circuit-breaker state, so it must read as "tripped" rather
    // than "switched on". It used to have a second entry meaning the latter,
    // which silently won because unordered_map keeps the last duplicate — the
    // CLI then printed "3 开启" for three open breakers.
    LR_CHECK_EQ(tr("open", Lang::Zh), "熔断");

    // The provider editor's model list and the log-kind filter share the word
    // "models" in English but not in Chinese; keeping them as separate keys is
    // what stops one label from overwriting the other.
    LR_CHECK_EQ(tr("Models", Lang::Zh), "模型");
    LR_CHECK_EQ(tr("Models list", Lang::Zh), "模型列表");
}

} // namespace

int main() {
    testParsing();
    testDetection();
    testTranslations();
    testGuiSurfaceTranslations();
    return LR_SUMMARY("test_i18n");
}
