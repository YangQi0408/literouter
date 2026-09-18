module literouter.core;

import std;

namespace literouter::i18n {

namespace {

const std::unordered_map<std::string_view, const char*> kZhTranslations = {
    // ── Navigation & Shell ──────────────────────────────────────────────────
    {"CONSOLE", "控制台"},
    {"Overview", "概览"},
    {"Providers", "中转站"},
    {"Routes", "路由"},
    {"Logs", "日志"},
    {"Settings", "设置"},
    {"Live traffic, relay health and recent activity", "实时流量、中转站健康状态与近期活动"},
    {"Upstream relays, credentials and reachability", "上游中转站、凭据与连通性"},
    {"Model mapping and the ordered failover chain", "模型映射与有序故障转移链路"},
    {"Every request this proxy has handled", "代理处理的所有历史请求"},
    {"Listener, circuit breaker and logging", "监听端口、熔断器与日志记录"},
    {"Start proxy", "启动代理"},
    {"Stop proxy", "停止代理"},
    {"Running", "运行中"},
    {"Stopped", "已停止"},
    {"Starting", "启动中"},
    {"Stopping", "停止中"},
    {"active request", "个处理中请求"},
    {"active requests", "个处理中请求"},
    {"idle", "空闲"},
    {"offline", "离线"},
    {"req", "请求"},
    {"up ", "运行 "},
    {"listener closed", "监听已关闭"},
    {"Save", "保存"},
    {"Reload", "重新加载"},
    {"Reset counters", "重置计数器"},
    {"Clear log", "清空日志"},
    {"Delete", "删除"},
    {"Edit", "编辑"},
    {"Copy", "复制"},
    {"Cancel", "取消"},
    {"Apply", "应用"},
    {"Close", "关闭"},
    {"Test", "测试"},
    {"Test all", "全部测试"},
    {"Testing...", "测试中..."},
    {"Pause", "暂停"},
    {"Resume", "继续"},
    {"Dismiss", "忽略"},
    {"The configuration on disk could not be parsed — this session is running on the seed defaults.",
     "磁盘上的配置文件解析失败 — 当前会话运行在种子默认配置上。"},
    {"No config file yet. This session uses the seeded example until you create one.",
     "尚未创建配置文件。当前会话使用种子示例配置，直到您创建配置文件。"},
    {"Create config file", "创建配置文件"},
    {"The configuration file changed on disk. Reload to pick up the new routing model.",
     "磁盘上的配置文件已更改。重新加载以应用新的路由模型。"},
    {"Reload from disk", "从磁盘重新加载"},
    {"You have unsaved changes.", "存在未保存的修改。"},
    {"Save now", "立即保存"},

    // ── Overview & Metrics ──────────────────────────────────────────────────
    {"TOTAL REQUESTS", "总请求数"},
    {"SUCCESS RATE", "成功率"},
    {"IN FLIGHT", "处理中"},
    {"AVG LATENCY", "平均延迟"},
    {"TOKENS", "Token 数"},
    {"BYTES OUT", "下发流量"},
    {"TIMING", "耗时分布"},
    {"wait", "等待"},
    {"first byte", "首字节"},
    {"stream", "流式"},
    {"BYTES IN / OUT", "进出流量"},
    {"RELAY TRAFFIC", "中转流量"},
    {"CIRCUIT BREAKER", "熔断告警"},
    {"RELAYS", "中转站列表"},
    {"RECENT ACTIVITY", "最近活动"},
    {"Recent activity", "最近活动"},
    {"No requests yet", "暂无请求"},
    {"No activity recorded yet", "暂无活动记录"},
    {"View all logs", "查看全部日志"},
    {"All circuits healthy", "所有中转站运行正常"},
    {"circuit breaker is open", "个中转站熔断中"},
    {"circuit breakers are open", "个中转站熔断中"},
    {"relay breaker is open", "个中转站熔断中"},
    {"relay breakers are open", "个中转站熔断中"},
    {"Healthy", "正常"},
    {"Degraded", "降级"},
    {"Open", "熔断"},
    {"Unknown", "未知"},
    {"healthy", "正常"},
    {"degraded", "降级"},
    {"open", "熔断"},
    {"unknown", "未知"},
    {" left", " 剩余"},
    {"remaining", "剩余"},
    {"No relay is currently eligible.", "当前没有可用的中转站。"},
    {"(no base url)", "(未配置根地址)"},
    {"No recent upstream errors", "近期无上游错误"},
    {"last error · ", "最近错误 · "},
    {"newest first · full log on Logs", "按时间倒序 · 完整记录见日志页"},
    {"No requests yet — start the server and point a client at it.",
     "暂无请求 — 请启动代理服务并将客户端指向本端点。"},
    {" configured · ordered by priority", " 个已配置 · 按优先级排序"},
    {"Open Providers", "打开中转站页面"},
    {"A relay is one upstream zhong-zhuan endpoint. Add one on the Providers page and its models become routable.",
     "中转站是上游 API 节点。在中转站页面添加后，其模型即可参与路由转发。"},

    // ── Providers ───────────────────────────────────────────────────────────
    {"Add provider", "添加中转站"},
    {"Edit provider", "编辑中转站"},
    {"Delete provider?", "删除中转站？"},
    {"Delete relay", "删除中转站"},
    {"Save relay", "保存中转站"},
    {"Add relay", "添加中转站"},
    {"Provider ID", "中转站 ID"},
    {"Display name", "显示名称"},
    {"Base URL", "接口根地址"},
    {"API Key", "API 密钥"},
    {"API key", "API 密钥"},
    {"Priority", "优先级"},
    {"Weight", "权重"},
    {"Timeout", "超时时间"},
    {"Connect timeout", "连接超时"},
    {"Supports streaming", "支持流式传输 (SSE)"},
    {"Protocol", "上游协议"},
    {"Models", "模型列表"},
    {"Headers", "自定义请求头"},
    {"Note", "备注"},
    {"PRIORITY", "优先级"},
    {"TIMEOUT", "超时"},
    {"MODELS", "模型"},
    {"REQUESTS", "请求数"},
    {"SUCCESS", "成功数"},
    {"FAILURES", "失败数"},
    {"disabled", "已禁用"},
    {"enabled", "已启用"},
    {"No providers configured", "尚未配置中转站"},
    {"Relay", "中转站"},
    {"Relays", "中转站"},
    {"Select", "请选择"},
    {"No relays configured", "尚未配置中转站"},
    {"Providers define the upstream relays this proxy forwards to.",
     "中转站定义了本代理可转发调用的上游 API 服务。"},
    {"Give the relay an id before testing it.", "测试前请先指定中转站 ID。"},
    {"An id is required — it is what routes and logs reference.", "必须填写 ID — 路由与日志将通过 ID 引用该中转站。"},
    {"Another relay already uses the id \"", "已有其他中转站使用了 ID “"},
    {"Server started", "代理服务已启动"},
    {"Listening on ", "正在监听 "},
    {"Could not start server", "无法启动代理服务"},
    {"Server stopped", "代理服务已停止"},
    {"The listener is closed; in-flight replies drained.", "监听服务已关闭，进行中的请求已全部处理完毕。"},
    {"Relay reachable", "中转站连通正常"},
    {"Relay unreachable", "中转站无法连通"},
    {"answered /models", "成功响应 /models"},
    {"no response", "无响应"},
    {"Probe finished", "探测完成"},
    {"Reachability refreshed for every enabled relay.", "已刷新所有启用中转站的连通性状态。"},
    {"Save failed", "保存失败"},
    {"Saved to ", "已保存至 "},
    {"Configuration saved", "配置已保存"},
    {"Reload failed", "重新加载失败"},
    {"Reloaded from disk", "已从磁盘重新加载"},
    {"Configuration reloaded", "配置已重新加载"},
    {"Counters reset", "计数器已重置"},
    {"Traffic totals and breaker state cleared.", "流量统计与熔断状态已清零。"},
    {"Log cleared", "日志已清空"},
    {"The on-disk ring buffer is empty.", "内存与磁盘日志缓冲区已清空。"},
    {"Relay removed", "中转站已移除"},
    {" is no longer part of the routing model.", " 已不再包含于路由模型中。"},
    {"Route removed", "路由已删除"},
    {" will no longer be advertised.", " 将不再对外发布。"},
    {"Add a relay on the Providers page first.", "请先在中转站页面添加至少一个中转站。"},
    {"no key", "无密钥"},
    {" (set)", " (已配置)"},
    {" (missing)", " (缺失)"},

    // ── Routes ─────────────────────────────────────────────────────────────
    {"Add route", "添加路由"},
    {"Edit route", "编辑路由"},
    {"Save route", "保存路由"},
    {"Delete route?", "删除路由？"},
    {"Delete route", "删除路由"},
    {"Logical model", "逻辑模型"},
    {"Logical model name", "逻辑模型名称"},
    {"Model name clients will request", "客户端发起请求时填写的模型名称"},
    {"Enable route", "启用此路由"},
    {"Failover targets", "故障转移链路"},
    {"No routes yet", "暂无路由配置"},
    {"Hops", "跳步"},
    {"Add hop", "添加中继"},
    {"Edit hop", "编辑中继"},
    {"Save hop", "保存中继"},
    {"Add hop to ", "添加中继到 "},
    {"Upstream model", "上游模型"},
    {"Available models on relay", "该中转站已配置的模型"},
    {"Quick select model", "快捷选用模型"},
    {"Leave empty to use client model name", "留空则透传客户端请求的模型名称"},
    {"Pass-through unknown models", "透传未匹配模型"},
    {"Auto-forward unrouted requests to matching relays", "未配置专属路由的模型将自动匹配中转站转发"},
    {"Model name cannot be empty", "模型名称不能为空"},
    {"A route with this model name already exists", "已存在相同模型名称的路由"},
    {"(unnamed route)", "(未命名路由)"},
    {"route", "条路由"},
    {"routes", "条路由"},
    {"hop", "跳"},
    {"hops", "跳"},
    {"each name is one client-facing model", "每个名称对应一个对外逻辑模型"},
    {"advertised", "已对外发布"},
    {"not advertised", "未对外发布"},
    {"No hops yet — add the first relay this model should be tried on.", "暂无中继 — 请添加此模型首选的中转站。"},
    {"(same model name)", "(同名透传)"},
    {"missing", "中转站不存在"},
    {"breaker open", "熔断开启"},
    {"A client asking for ", "客户端请求 "},
    {" has no relay to try yet.", " 暂无可用中转站。"},
    {" will try ", " 将依次尝试调用 "},
    {", then ", "，失败则切换至 "},
    {"Clients asking for \"", "请求模型 “"},
    {"\" will no longer be matched by this route.", "” 的客户端将不再匹配此路由。"},
    {"Configure relay and model for route: ", "配置该路由的中继节点与模型映射: "},
    {"Append failover relay for route: ", "为该路由追加故障转移中转站: "},
    {"Modify the upstream relay or model mapping for this hop.", "修改此中继跳步对应的上游中转站或模型名称。"},
    {"The hop is appended at the end; reorder it with < and > on the route card.", "中继将追加到末尾；可在路由卡片中使用 < 和 > 调整顺序。"},
    {"Add a relay on the Providers page before adding a hop.", "在添加中继前，请先在中转站页面添加至少一个中转站。"},
    {"e.g. gpt-4o-2024-11-20", "例如 gpt-4o-2024-11-20"},
    {"e.g. gpt-4o, claude-3-5-sonnet", "例如 gpt-4o, claude-3-5-sonnet"},
    {"Clients (Cursor, Chatbox, etc.) request this name.", "客户端（如 Cursor、Chatbox 等）发起调用时使用此名称。"},
    {"A route maps a model name your clients ask for onto an ordered chain of relays. Add one, then add hops to it.",
     "路由将客户端请求的模型名称映射到有序的中转站候选链路。添加路由后可向其追加中继跳步。"},

    // ── Logs ────────────────────────────────────────────────────────────────
    {"Filter model, relay or message", "搜索模型、中转站或日志内容"},
    {"All", "全部"},
    {"All levels", "全部"},
    {"Info", "信息"},
    {"Warn", "警告"},
    {"Error", "错误"},
    {"All kinds", "所有类型"},
    {"Chat", "对话"},
    {"Embeddings", "嵌入"},
    {"Models", "模型"},
    {"Admin", "管理"},
    {"System", "系统"},
    {"No log entries", "暂无日志条目"},
    {"No matching log entries", "无匹配的日志条目"},
    {"Try adjusting your level, kind or search filter.", "请尝试调整级别、类型或搜索筛选条件。"},
    {"Reset filters", "重置筛选"},
    {"Requests appear here as the proxy handles them.",
     "代理处理请求时将在此处实时显示日志。"},
    {"TIME", "时间"},
    {"LEVEL", "级别"},
    {"KIND", "类型"},
    {"STATUS", "状态"},
    {"LATENCY", "延迟"},
    {"MODEL", "模型"},
    {"RELAY", "中转站"},
    {"MESSAGE", "详细信息"},
    {"REQUEST BODY", "请求报文"},
    {"RESPONSE BODY", "响应报文"},
    {"Log detail", "日志详情"},

    // ── Settings ────────────────────────────────────────────────────────────
    {"Listener", "监听服务"},
    {"Where this proxy accepts client connections", "本地代理监听地址与客户端连接设置"},
    {"Bind host", "监听主机"},
    {"127.0.0.1 keeps the proxy on this machine", "127.0.0.1 仅限本机访问"},
    {"Port", "端口"},
    {"Max attempts", "最大尝试次数"},
    {"Request deadline (seconds)", "整请求最长耗时（秒）"},
    {"Session affinity (seconds)", "会话粘性（秒）"},
    {"0 disables it. Keeps a conversation on the relay that answered it, so the provider can reuse its cached prompt.", "0 表示关闭。让同一个会话继续走已应答过它的中转站，以便复用上游的提示词缓存。"},
    {"Price in ($ / 1M tokens)", "输入单价（美元 / 百万 token）"},
    {"Price out ($ / 1M tokens)", "输出单价（美元 / 百万 token）"},
    {"What this relay charges for input tokens. 0 leaves it unknown, and an unknown price contributes nothing to the cost estimate.", "该中转站输入 token 的单价。0 表示未填写；未填写的中转站不计入花费估算。"},
    {"What this relay charges for output tokens.", "该中转站输出 token 的单价。"},
    {"0 disables it. Bounds the whole request, not one attempt.", "0 表示不限制；限制的是整个请求，而非单次尝试。"},
    {"Client API key", "客户端 API 密钥"},
    {"Empty disables the check. Set it and clients must send Authorization: Bearer <key>.",
     "留空表示不校验密钥。设置后客户端必须携带 Authorization: Bearer <key>。"},
    {"Pass through unknown models to matching relays",
     "未配置路由的模型自动透传至包含该模型的可用中转站"},
    {"Skip relays whose breaker is open", "跳过处于熔断状态的中转站"},
    {"Circuit breaker", "熔断保护"},
    {"When a relay fails repeatedly, back off before retrying it",
     "当中转站连续故障时触发熔断，冷却后再尝试恢复"},
    {"Consecutive failures before opening", "触发熔断的连续失败次数"},
    {"Cooldown (seconds)", "熔断冷却时间（秒）"},
    {"Logging", "日志设置"},
    {"How much request history the console keeps", "控制台保留的历史请求数量及体积限制"},
    {"Ring buffer depth (entries)", "环形缓冲区容量（条目数）"},
    {"Retained body bytes per entry", "单条日志保留报文体字节数"},
    {"Retain request / response bodies", "保留请求与响应报文体"},
    {"Keep telemetry across restarts", "重启后保留遥测数据"},
    {"Language", "界面语言"},
    {"Interface language", "界面语言"},
    {"Choose interface language", "选择控制台与界面的显示语言"},
    {"Auto (System)", "跟随系统"},
    {"English", "English"},
    {"Simplified Chinese", "简体中文"},
    {"How to use it", "接入指引"},
    {"Point any OpenAI-compatible client at this proxy",
     "将任何兼容 OpenAI 的客户端指向本代理"},
    {"LOCAL BASE URL", "本地基础端点"},
    {"(start the server to see the bound address)", "(启动代理后显示绑定地址)"},
    {"Everything here is written to a single JSON file. Edits are held in memory until you save.",
     "所有配置均保存于单一 JSON 文件中。在点击保存前修改暂存于内存中。"},
    {"exists on disk", "配置文件已存在"},
    {"not created yet", "尚未创建文件"},
    {"In sync with disk", "已与磁盘同步"},
    {"Unsaved changes", "存在未保存的修改"},
    {"Not yet written to disk", "尚未写入磁盘"},
    {"Reset counters?", "重置计数器？"},
    {"Traffic totals and circuit-breaker state go back to zero. The listener keeps running.",
     "流量统计指标与熔断状态将被清零，监听服务保持运行。"},
    {"leave empty to accept any key", "留空表示接受任何密钥"},
    {"Every model in a route is advertised under /models. Unknown models pass through to any enabled relay that lists them.",
     "路由中的每个模型均暴露于 /models。未知模型将自动透传至声明了该模型的已启选中转站。"},
    {"Choose display language", "选择控制台界面语言"},
    {"Display", "界面显示"},
    {"Display and appearance settings", "界面语言与页面缩放设置"},
    {"Page scale", "页面缩放"},
    {"Zoom in", "放大"},
    {"Zoom out", "缩小"},
    {"Reset zoom", "重置缩放"},
    {"Keyboard shortcuts: Ctrl +/- to zoom, Ctrl 0 to reset.", "快捷键: Ctrl +/- 缩放，Ctrl 0 重置。"},
    {"Zoom: ", "缩放: "},

    // ── Overlays & Modals ───────────────────────────────────────────────────
    {"Edit relay", "编辑中转站"},
    {"A relay is one upstream endpoint. Routes reference it by id, so the id is stable and unique.",
     "中转站代表一个上游服务节点。路由通过 ID 引用它，因此 ID 应当保持唯一且稳定。"},
    {"Id", "ID"},
    {"e.g. openai", "例如 openai"},
    {"e.g. OpenAI official", "例如 OpenAI official"},
    {"Includes the /v1 root of the relay.", "包含中转站的 /v1 根路径。"},
    {"Includes /v1 root", "包含 /v1 根路径"},
    {"Literal, or ${ENV_VAR} / $ENV_VAR to read it from the environment at request time.",
     "明文字符串，或使用 ${ENV_VAR} / $ENV_VAR 在请求时从环境变量动态读取。"},
    {"Supports ${ENV_VAR}", "支持 ${ENV_VAR} 环境变量"},
    {"Lower wins; ties fall back to declaration order.", "数值越小越优先；相同时按声明顺序排序。"},
    {"Tie-break inside one priority band.", "同一优先级区间内的权重选择。"},
    {"Request timeout (seconds)", "请求超时（秒）"},
    {"Connect timeout (seconds)", "连接超时（秒）"},
    {"Comma or newline separated. Used by /v1/models and for pass-through matching.",
     "以逗号或换行分隔。用于 /v1/models 展示及未知模型透传匹配。"},
    {"Extra headers", "附加请求头"},
    {"One per line, \"Name: value\".", "每行一个，格式为 \"Name: value\"。"},
    {"Enabled", "已启用"},
    {"Fill in the fields, then Save to write the config.", "填写各项信息，点击保存以写入配置。"},
    {"Editing an existing relay. Save writes the config file.", "正在编辑已有中转站。点击保存将更新配置文件。"},
    {"Use discovered models", "使用探测到的模型"},
    {"Appended to the end of the failover chain for ", "追加到此故障转移链路的末尾: "},
    {"this route", "此路由"},
    {"No relays configured", "未配置中转站"},
    {"Select", "选择"},
    {"Leave empty to ask for the route's name unchanged.", "留空表示使用与路由相同的模型名称。"},
    {"Add a relay on the Providers page before adding a hop.", "在添加中继前，请先在中转站页面添加至少一个中转站。"},
    {"The hop is appended at the end; reorder it with < and > on the route card.",
     "中继将追加到末尾；可在路由卡片中使用 < 和 > 调整顺序。"},
    {"Confirm", "确认"},
    {"Log entry #", "日志条目 #"},
    {"no request id", "无请求 ID"},
    {"request ", "请求 "},
    {"MODEL (AS ASKED)", "请求模型"},
    {"UPSTREAM MODEL", "上游模型"},
    {"BYTES", "流量"},
    {"ATTEMPT", "尝试次数"},
    {"after failover", "故障转移后"},
    {"streamed", "流式"},
    {"(no message)", "(无信息)"},
    {"REQUEST / RESPONSE BODY", "请求 / 响应报文"},
    {"BODIES", "报文内容"},
    {"Body retention is off (server.log_bodies). Turn it on in Settings to inspect prompts and responses here.",
     "报文体记录已关闭 (server.log_bodies)。在设置中开启后可在此查看完整提示词与回复内容。"},
    {"models discovered", "个可用模型"},
    {"probe · unreachable", "探测 · 无法连通"},
    {"probe · running…", "探测 · 运行中..."},
    {"Filled in ", "已填入 "},
    {" discovered models.", " 个探测到的模型。"},
    {"Delete route", "删除路由"},

    // ── CLI & Diagnostics ───────────────────────────────────────────────────
    {"literouter — one local endpoint that fans a request out across your configured relays",
     "literouter — 将本地请求智能分发至多个 AI 中转站的聚合网关"},
    {"Path to the config file (default: $LITEROUTER_CONFIG, else ~/.config/literouter/config.json)",
     "配置文件路径 (默认: $LITEROUTER_CONFIG，或 ~/.config/literouter/config.json)"},
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
    {"the port is free", "端口空闲可用"},
    {"run `literouter config init` to write the seed",
     "运行 `literouter config init` 写入初始配置"},
    {"run `literouter config validate` to see the warnings",
     "运行 `literouter config validate` 查看警告详情"},
    {"doctor found at least one FAIL", "环境体检发现至少一项失败"},
    {"all checks passed", "所有体检项均通过"},
    {"RELAY", "中转站"},
    {"STATE", "状态"},
    {"AVG", "平均"},
    {"FAILOVER", "故障转移"},
    {"TARGETS", "候选链路"},
    {"CONFIG", "配置"},
    {"requests", "请求总数"},
    {"avg latency", "平均延迟"},
    {"tokens", "Token 总数"},
    {"bytes out", "下发流量"},
    {"breakers", "熔断器"},
    {"open", "开启"}
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
