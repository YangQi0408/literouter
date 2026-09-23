module literouter.core;

import std;

namespace literouter::i18n {

namespace {

const std::unordered_map<std::string_view, const char*> kZhTranslations = {
    {"PEM certificate chain absolute path for HTTPS", "HTTPS 的 PEM 证书链绝对路径"},
    {"Unencrypted PEM private key absolute path for HTTPS", "HTTPS 的未加密 PEM 私钥绝对路径"},
    {"tls_cert_file and tls_key_file must both be set, or both empty for HTTP", "tls_cert_file 与 tls_key_file 必须同时填写，或同时留空使用 HTTP"},
    {"TLS file must be an existing regular file with an absolute path", "TLS 路径必须是已存在的普通文件绝对路径"},
    {"cannot load the PEM certificate chain", "无法加载 PEM 证书链"},
    {"TLS certificate is expired, not yet valid, or has invalid dates", "TLS 证书已过期、尚未生效或有效期无效"},
    {"cannot load an unencrypted PEM private key matching the certificate", "无法加载与证书匹配的未加密 PEM 私钥"},
    {"cannot initialize HTTPS listener from TLS files", "无法使用 TLS 文件初始化 HTTPS 监听"},
    // Relay-side protection.
    {"relay concurrency and rate limits cannot be negative", "中转站并发与速率限制不可为负数"},
    // The traffic trend's window.
    {"a trend needs at least two buckets to show a shape", "趋势图至少需要两个桶才能显示出形状"},
    // The local response cache.
    {"the entry count cannot be negative", "缓存条目数不可为负数"},
    // Observability.
    {"otlp_endpoint must start with http:// or https://", "otlp_endpoint 必须以 http:// 或 https:// 开头"},
    // The protocols that need more than a base_url and a key.
    {"a Vertex relay needs `project` (and `credentials_file`)",
     "Vertex 中转站需要 `project`（以及 `credentials_file`）"},
    {"credentials_file is not a readable regular file", "credentials_file 不是可读取的普通文件"},
    {"Bedrock signs every request against a region and cannot guess one",
     "Bedrock 的每个请求都要按区域签名，无法自行推断区域"},
    // ── Navigation & Shell ──────────────────────────────────────────────────
    {"Test", "测试"},

    // ── Overview & Metrics ──────────────────────────────────────────────────
    {"TOKENS", "Token 数"},
    {"wait", "等待"},
    {"stream", "流式"},
    {"healthy", "正常"},
    {"degraded", "降级"},
    {"open", "熔断"},
    {"unknown", "未知"},

    // ── Providers ───────────────────────────────────────────────────────────
    {"MODELS", "模型"},
    {"REQUESTS", "请求数"},
    {"SUCCESS", "成功数"},
    {"disabled", "已禁用"},
    {"enabled", "已启用"},

    // ── Routes ─────────────────────────────────────────────────────────────
    {"route", "条路由"},
    {"routes", "条路由"},
    {"missing", "中转站不存在"},

    // ── Logs ────────────────────────────────────────────────────────────────
    {"TIME", "时间"},
    {"LEVEL", "级别"},
    {"KIND", "类型"},
    {"STATUS", "状态"},
    {"LATENCY", "延迟"},
    {"MODEL", "模型"},
    {"RELAY", "中转站"},
    {"COST", "花费"},
    {"BEST", "最快"},
    {"MESSAGE", "详细信息"},

    // ── Settings ────────────────────────────────────────────────────────────

    // ── Overlays & Modals ───────────────────────────────────────────────────
    {"BYTES", "流量"},

    // ── CLI & Diagnostics ───────────────────────────────────────────────────
    {"literouter — one local endpoint that fans a request out across your configured relays",
     "literouter — 将本地请求智能分发至多个 AI 中转站的聚合网关"},
    {"Emit machine-readable JSON", "以机器可读的 JSON 格式输出"},
    {"Never emit ANSI colour", "禁用 ANSI 彩色输出"},
    {"Suppress non-essential output", "仅输出必要信息"},
    {"Language (auto, en, zh)", "界面语言 (auto, en, zh)"},
    {"Run the proxy in the foreground", "在前台运行代理服务"},
    {"Query a running instance for its telemetry", "查询正在运行的实例状态与遥测数据"},
    {"List every logical model and the ordered relays that can serve it",
     "列出所有逻辑模型及可提供服务的候选链路"},
    {"Manage relays (edits the config file, no server needed)",
     "管理中转站配置（直接编辑文件，无需运行服务）"},
    {"Manage routes (edits the config file, no server needed)",
     "管理模型路由规则（直接编辑文件，无需运行服务）"},
    {"Inspect and edit the config file", "检查与管理配置文件"},
    {"Diagnose the config and the environment", "诊断配置与运行环境健康状态"},
    {"Read (and optionally follow) the request log", "查看（或持续跟踪）代理请求日志"},
    {"config file", "配置文件"},
    {"validation", "配置校验"},
    {"secret references", "密钥引用"},
    {"enabled relays", "已启用的中转站"},
    {"route table", "路由表"},
    {"listener", "监听服务"},
    {"at least one hop is disabled and will be skipped",
     "至少有一个中继处于禁用状态，将被跳过"},
    {"run `literouter config init` to write the seed",
     "运行 `literouter config init` 写入初始配置"},
    {"run `literouter config validate` to see the warnings",
     "运行 `literouter config validate` 查看警告详情"},
    {"doctor found at least one FAIL", "环境体检发现至少一项失败"},
    {"all checks passed", "所有体检项均通过"},
    {"STATE", "状态"},
    {"AVG", "平均"},
    {"Send a saved request body to one relay", "把保存好的请求体发给指定中转站重放"},
    {"FAILOVER", "故障转移"},
    {"requests", "请求总数"},
    {"avg latency", "平均延迟"},
    {"tokens", "Token 总数"},
    {"bytes out", "下发流量"},
    {"breakers", "熔断器"},
    // ── provider editor, protocol pane ───────────────────────────────────
    {"Vertex project id", "Vertex 项目 ID"},
};

std::atomic<Lang> g_current_lang{Lang::Auto};

} // namespace

Lang parseLang(std::string_view code) {
    std::string lower;
    lower.reserve(code.size());
    for (char c : code) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "zh" || lower.starts_with("zh_") || lower.starts_with("zh-") ||
        lower == "chinese" || lower == "cn") {
        return Lang::Zh;
    }
    if (lower == "en" || lower.starts_with("en_") || lower.starts_with("en-") ||
        lower == "english" || lower == "us" || lower == "uk") {
        return Lang::En;
    }
    return Lang::Auto;
}

std::string_view langCode(Lang lang) {
    switch (lang) {
        case Lang::Auto: return "auto";
        case Lang::En: return "en";
        case Lang::Zh: return "zh";
    }
    return "auto";
}

std::string_view langDisplayName(Lang lang) {
    switch (lang) {
        case Lang::Auto: return "Auto (System)";
        case Lang::En: return "English";
        case Lang::Zh: return "简体中文";
    }
    return "Auto (System)";
}

Lang detectSystemLang() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* val = std::getenv(var); val != nullptr && *val != '\0') {
            std::string_view s{val};
            if (s.starts_with("zh") || s.starts_with("ZH")) {
                return Lang::Zh;
            }
            if (s.starts_with("en") || s.starts_with("EN")) {
                return Lang::En;
            }
        }
    }
    return Lang::En;
}

Lang resolveLang(Lang lang) {
    if (lang == Lang::Auto) {
        return detectSystemLang();
    }
    return lang;
}

void setLang(Lang lang) {
    g_current_lang.store(lang, std::memory_order_relaxed);
}

Lang getLang() {
    return g_current_lang.load(std::memory_order_relaxed);
}

std::string_view tr(std::string_view text, Lang lang) {
    Lang effective = (lang == Lang::Auto) ? resolveLang(Lang::Auto) : lang;
    if (effective == Lang::En) {
        return text;
    }
    auto it = kZhTranslations.find(text);
    if (it != kZhTranslations.end()) {
        return it->second;
    }
    return text;
}

std::string_view tr(std::string_view text) {
    return tr(text, getLang());
}

const char* tr(const char* text, Lang lang) {
    Lang effective = (lang == Lang::Auto) ? resolveLang(Lang::Auto) : lang;
    if (effective == Lang::En) {
        return text;
    }
    auto it = kZhTranslations.find(std::string_view{text});
    if (it != kZhTranslations.end()) {
        return it->second;
    }
    return text;
}

const char* tr(const char* text) {
    return tr(text, getLang());
}

} // namespace literouter::i18n
