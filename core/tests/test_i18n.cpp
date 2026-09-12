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

} // namespace

int main() {
    testParsing();
    testDetection();
    testTranslations();
    return LR_SUMMARY("test_i18n");
}
