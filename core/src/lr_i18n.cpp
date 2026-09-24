module;

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

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

    // ── CLI option descriptions ─────────────────────────────────────────────
    {"A model id this relay advertises (repeatable)",
     "该中转站对外声明的模型 ID（可重复）"},
    {"Add a relay to the config file",
     "向配置文件添加中转站"},
    {"Add a route, or append hops to an existing one",
     "新增路由，或为已有路由追加候选链路"},
    {"Add the route disabled",
     "新增时先禁用该路由"},
    {"Ask every relay that serves a model the same question",
     "向所有可提供该模型的中转站发送同一个问题"},
    {"Azure: the api-version query; Vertex: the API version segment",
     "Azure：api-version 查询参数；Vertex：API 版本路径段"},
    {"Bedrock STS session token, for temporary credentials",
     "Bedrock STS 会话令牌，用于临时凭证"},
    {"Bedrock SigV4 access key id (a ${VAR} reference is stored as written)",
     "Bedrock SigV4 访问密钥 ID（${VAR} 引用按原样存储）"},
    {"Bedrock SigV4 secret access key",
     "Bedrock SigV4 私密访问密钥"},
    {"Bedrock region (required there), or the Vertex location",
     "Bedrock 区域（该协议必填），或 Vertex 位置"},
    {"Chat completions path (default /chat/completions)",
     "Chat completions 路径（默认 /chat/completions）"},
    {"Destination file; omit or `-` for stdout",
     "目标文件；省略或写 `-` 则输出到标准输出"},
    {"Disable a route",
     "禁用一条路由"},
    {"Do not persist counters and the request log for this run only",
     "仅本次运行不持久化计数器与请求日志"},
    {"Do not serve the built-in console at /ui for this run only",
     "仅本次运行不提供 /ui 内置控制台"},
    {"Dump the effective config JSON and exit",
     "输出生效后的配置 JSON 并退出"},
    {"Embeddings path (default /embeddings)",
     "Embeddings 路径（默认 /embeddings）"},
    {"Enable a route",
     "启用一条路由"},
    {"Enable the relay (default)",
     "启用该中转站（默认）"},
    {"Extra upstream header, `Name: value` (repeatable)",
     "附加的上游请求头，`名称: 值`（可重复）"},
    {"Free-form note",
     "自由备注"},
    {"Human label (defaults to the id)",
     "显示名称（默认与 id 相同）"},
    {"Include unrouted provider models in pass-through mode",
     "在透传模式下包含未配置路由的中转站模型"},
    {"Input price in dollars per million tokens (0 = unknown)",
     "输入价格，美元/百万 Token（0 表示未知）"},
    {"List configured relays",
     "列出已配置的中转站"},
    {"List the route table",
     "列出路由表"},
    {"Literal API key",
     "直接填写 API 密钥"},
    {"Load a whole config, or merge named entries into the current one",
     "加载整份配置，或把文件中点名的条目合并进当前配置"},
    {"Load and validate, then exit without binding",
     "加载并校验后退出，不绑定端口"},
    {"Lower wins (default 100)",
     "数值越小优先级越高（默认 100）"},
    {"Mark a relay disabled",
     "禁用该中转站"},
    {"Mark a relay enabled",
     "启用该中转站"},
    {"Maximum entries to fetch (default 50)",
     "最多获取的条目数（默认 50）"},
    {"Model to ask for (default: the body's own)",
     "请求使用的模型（默认沿用请求体中的）"},
    {"Model to benchmark (required)",
     "要压测的模型（必填）"},
    {"Most requests literouter sends this relay at once; 0 is unlimited",
     "literouter 同时发往该中转站的请求上限；0 表示不限"},
    {"Most requests literouter starts on this relay per minute; 0 is unlimited",
     "literouter 每分钟在该中转站发起的请求上限；0 表示不限"},
    {"Only entries at this level",
     "仅显示该级别的条目"},
    {"Output price in dollars per million tokens (0 = unknown)",
     "输出价格，美元/百万 Token（0 表示未知）"},
    {"Override server.host for this run only",
     "仅本次运行覆盖 server.host"},
    {"Override server.port for this run only",
     "仅本次运行覆盖 server.port"},
    {"Overwrite an existing file",
     "覆盖已存在的文件"},
    {"Path to the config file (default: $LITEROUTER_CONFIG, else ~/.config/literouter/config.json)",
     "配置文件路径（默认取 $LITEROUTER_CONFIG，否则 ~/.config/literouter/config.json）"},
    {"Per-request timeout in seconds (default 120)",
     "单次请求超时秒数（默认 120）"},
    {"Per-request timeout in seconds (default 30)",
     "单次请求超时秒数（默认 30）"},
    {"Per-request timeout in seconds (default: the relay's own)",
     "单次请求超时秒数（默认沿用中转站自身的设置）"},
    {"Persist counters and the request log for this run only",
     "仅本次运行持久化计数器与请求日志"},
    {"Poll every 250ms and print new entries until Ctrl-C",
     "每 250ms 轮询并打印新条目，直到按 Ctrl-C"},
    {"Print the answer body",
     "打印响应正文"},
    {"Print the config file as it is on disk",
     "原样打印磁盘上的配置文件"},
    {"Print the outcome as JSON",
     "以 JSON 格式输出结果"},
    {"Print the resolved config path",
     "打印解析后的配置路径"},
    {"Print the whole config, or write it to a file",
     "打印整份配置，或写入文件"},
    {"Probe relays with GET {base_url}/models",
     "用 GET {base_url}/models 探测中转站"},
    {"Prompt to send (default: a one-word request)",
     "发送的提示词（默认是一个单词的请求）"},
    {"Read one line from stdin",
     "从标准输入读取一行"},
    {"Relay id",
     "中转站 id"},
    {"Relay id (used by routes and logs)",
     "中转站 id（供路由与日志引用）"},
    {"Relay ids to probe (default: every enabled relay)",
     "要探测的中转站 id（默认探测所有已启用的）"},
    {"Relay to send it to (default: the first the policy would try)",
     "发送目标中转站（默认取路由策略的首选）"},
    {"Remove a relay from the config file",
     "从配置文件中删除中转站"},
    {"Remove a route",
     "删除路由"},
    {"Remove even while a route still targets it",
     "即使仍有路由指向它也强制删除"},
    {"Replace only the relays and routes the file names",
     "只替换文件中点名的中转站与路由"},
    {"Request body to replay (OpenAI chat JSON)",
     "要重放的请求体（OpenAI chat JSON）"},
    {"Requests per relay (default 1)",
     "每个中转站的请求次数（默认 1）"},
    {"Rewrite an older config onto this build's schema",
     "把旧版配置改写到当前构建的 schema"},
    {"Run even when validate() reports errors",
     "即使 validate() 报错也照常运行"},
    {"Serve the built-in console at /ui for this run only",
     "仅本次运行在 /ui 提供内置控制台"},
    {"Source file; `-` reads standard input",
     "源文件；写 `-` 表示从标准输入读取"},
    {"Start after this sequence number (0 = newest)",
     "从该序号之后开始（0 表示最新）"},
    {"Store ${VAR}; the key is read at request time",
     "存为 ${VAR}；请求时再读取密钥"},
    {"The model name a client sends",
     "客户端发送的模型名称"},
    {"The route's model name",
     "路由的模型名称"},
    {"Tie-break weight within a priority (default 1)",
     "同一优先级内的权重（默认 1）"},
    {"Upstream API protocol: openai, anthropic, gemini, openai_responses, azure, vertex, "
     "bedrock, ollama (default openai)",
     "上游 API 协议：openai、anthropic、gemini、openai_responses、azure、vertex、bedrock、"
     "ollama（默认 openai）"},
    {"Upstream root, e.g. https://api.openai.com/v1",
     "上游根地址，例如 https://api.openai.com/v1"},
    {"Validate and report without writing",
     "只校验并报告，不写入"},
    {"Validate the config and print every issue",
     "校验配置并打印全部问题"},
    {"Vertex service-account JSON key path",
     "Vertex 服务账号 JSON 密钥路径"},
    {"Write the seed config",
     "写入初始配置"},
    {"max_tokens to ask for (default 16)",
     "请求使用的 max_tokens（默认 16）"},
    {"provider[:upstream-model] (repeatable, order = failover order)",
     "provider[:upstream-model]（可重复，顺序即故障转移顺序）"},
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

namespace {

// "zh-CN", "zh_CN.UTF-8", "ZH" — anything whose primary subtag is the one we
// know. Returns Auto for a language this build has no dictionary for, so the
// caller can keep looking rather than settling on English.
Lang langFromTag(std::string_view tag) {
    if (tag.starts_with("zh") || tag.starts_with("ZH")) {
        return Lang::Zh;
    }
    if (tag.starts_with("en") || tag.starts_with("EN")) {
        return Lang::En;
    }
    return Lang::Auto;
}

#if defined(_WIN32)
// The locale a Windows user actually configured. The POSIX variables below are
// not set there — a shell may export LANG, but nothing else does — so without
// this branch `--lang auto` is always English on Windows, which is not what
// docs/environment.md promises.
//
// GetUserDefaultUILanguage() is the display language of the user's account and
// is the closest match to "what language does this person read"; the lower ten
// bits are the LANGID, and 0x0804 is zh-CN, 0x0404 zh-TW, 0x0C04 zh-HK, 0x1004
// zh-SG — every Chinese LANGID has 0x04 as its primary language.
Lang detectWindowsLang() {
    const LANGID langid = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(langid) == LANG_CHINESE) {
        return Lang::Zh;
    }
    return Lang::Auto;
}
#endif

} // namespace

Lang detectSystemLang() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* val = std::getenv(var); val != nullptr && *val != '\0') {
            if (const Lang found = langFromTag(val); found != Lang::Auto) {
                return found;
            }
        }
    }
#if defined(_WIN32)
    if (const Lang found = detectWindowsLang(); found != Lang::Auto) {
        return found;
    }
#endif
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
