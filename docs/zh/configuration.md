# 配置文件与密钥管理

本文档详细介绍 `literouter` 的配置文件结构、字段含义、密钥安全展开机制以及配置校验规则。

---

## 目录

- [配置文件存储位置](#配置文件存储位置)
- [完整配置示例](#完整配置示例)
- [顶层字段详解](#顶层字段详解)
  - [Server 配置 (`server`)](#server-配置-server)
  - [中转站配置 (`providers`)](#中转站配置-providers)
  - [路由规则配置 (`routes`)](#路由规则配置-routes)
- [密钥安全解析机制](#密钥安全解析机制)
  - [支持的语法格式](#支持的语法格式)
  - [绝对不可逆（零泄露）原则](#绝对不可逆零泄露原则)
- [配置校验引擎 (Validation)](#配置校验引擎-validation)
- [配置热重载 (Hot Reload)](#配置热重载-hot-reload)

---

## 配置文件存储位置

`literouter` 依据跨平台操作系统规范自动探测并创建默认配置目录：

| 操作系统 | 默认配置文件路径 | 说明 |
|---|---|---|
| **Linux** | `~/.config/literouter/config.json` | 遵循 XDG Base Directory 规范（`$XDG_CONFIG_HOME` 优先） |
| **macOS** | `~/.config/literouter/config.json` | 与 Linux 一致（同样优先读取 `$XDG_CONFIG_HOME`） |
| **Windows** | `%APPDATA%\literouter\config.json` | 如 `C:\Users\<User>\AppData\Roaming\literouter\config.json` |

### 显式指定配置路径

你可以通过以下两种方式随时覆盖默认路径：

1. **CLI 命令行选项**：
   ```bash
   literouter serve --config /path/to/custom_config.json
   ```
2. **环境变量**：
   ```bash
   export LITEROUTER_CONFIG="/path/to/custom_config.json"
   ```

---

## 完整配置示例

```jsonc
{
  "schema": 3,

  "server": {
    "host": "127.0.0.1",          // 监听地址（绑定到公网非回环地址且未设 api_key 时将触发安全告警）
    "port": 8787,                 // 监听端口；设为 0 表示由系统随机分配空闲端口
    "tls_cert_file": "",         // PEM 证书链绝对路径；两项 TLS 路径都为空时保持 HTTP
    "tls_key_file": "",          // 匹配证书的未加密 PEM 私钥绝对路径；重启生效
    "api_key": "",                // 访问密钥，同时用于模型接口与管理接口；留空表示关闭鉴权
    "pass_through_unknown": true, // 路由表未显式收录的模型，自动按优先级透传给拥有该模型的中转站
    "max_attempts": 0,            // 单次请求最多重试尝试的中转站数量；0 表示遍历所有可用候选站
    "routing_policy": "priority", // 候选链排序：priority（默认）/ fastest（实测最快）/ cheapest（单价最低）/ round_robin（平均分流）
    "request_deadline_sec": 0,    // 整个请求的时间上限（秒）；0 表示不限制
    "session_affinity_sec": 0,    // 会话粘性时长（秒）；0 表示关闭
    "circuit_failure_threshold": 3, // 连续失败几次触发中转站进入熔断冷却
    "circuit_cooldown_sec": 30,     // 熔断后的冷却保持时长（秒）
    "skip_open_circuits": true,   // 路由调度时是否优先跳过熔断冷却中的中转站
    "log_capacity": 200,          // 内存环形日志最大缓冲条数
    "log_bodies": false,          // 是否记录请求体与响应体报文（默认关闭以保护提示词隐私）
    "log_body_limit": 2048,       // 记录报文时截断保存的最大字节数
    "persist_telemetry": true,    // 是否将计数器与请求日志持久化到状态目录（详见下文“遥测持久化”）
    "web_ui": true,               // 是否在 /ui 提供内置 Web 控制台（非回环地址且未设 api_key 时将告警）
    "reload_on_change": false,    // 配置文件在磁盘上变化时自动应用（默认关闭）
    "traffic_bucket_sec": 3600,   // 流量趋势每个桶的宽度（秒）；60 即按分钟观察最近一段时间
    "traffic_bucket_count": 24,   // 趋势保留的桶数量；3600 x 24 即“最近一天，按小时”
    "response_cache_ttl_sec": 0,  // 本地应答缓存有效期（秒）；0 表示关闭
    "response_cache_max_entries": 128, // 缓存条目上限，超出时淘汰最久未使用的一条
    "otlp_endpoint": ""           // OpenTelemetry OTLP/HTTP 指标端点，如 http://127.0.0.1:4318；留空关闭
  },

  "providers": [
    {
      "id": "openai-official",                 // 唯一标识符（全局唯一，路由与统计均以此为键）
      "name": "OpenAI 官方直连",                // 控制台显示名
      "protocol": "openai",                    // 上游协议：openai / anthropic / gemini / openai_responses
      "price_in_per_million": 2.5,             // 输入单价（美元 / 百万 token）；0 或省略表示未填写，不计入花费估算
      "price_out_per_million": 10.0,           // 输出单价（美元 / 百万 token）
      "base_url": "https://api.openai.com/v1", // 上游根地址（保留路径前缀）
      "api_key": "${OPENAI_API_KEY}",          // 支持直接写明文，或使用环境变量占位符
      "enabled": true,                         // 是否启用该中转站
      "priority": 10,                          // 优先级，数值越小越优先尝试（此处为示例值，字段默认值为 100）
      "weight": 1,                             // 同优先级下的权重（权重数值大者排在前面）
      "timeout_sec": 120,                      // 响应超时时间（秒）
      "connect_timeout_sec": 15,               // TCP/TLS 连接建立超时（秒）
      "supports_stream": true,                 // 是否支持流式传输（SSE）
      "models": ["gpt-4o", "text-embedding-3-small"], // 声明该中转站支持的模型列表
      "headers": {                             // 附加向上游发送的自定义 HTTP 请求头
        "HTTP-Referer": "https://example.com"
      },
      "chat_path": "/chat/completions",        // 自定义对话端点相对路径（留空则依据 protocol 自动推导）
      "embeddings_path": "/embeddings",        // 自定义向量嵌入端点相对路径
      "note": "主要高优先级出口"
    },
    {
      "id": "anthropic-direct",
      "name": "Anthropic 官方",
      "protocol": "anthropic",                 // 原生 Claude Messages 协议
      "base_url": "https://api.anthropic.com",
      "api_key": "${ANTHROPIC_API_KEY}",
      "enabled": true,
      "priority": 20,
      "models": ["claude-3-5-sonnet-20241022", "claude-3-5-haiku-20241022"],
      "chat_path": "/v1/messages"
    },
    {
      "id": "relay-backup",
      "name": "第三方低价中转站",
      "protocol": "openai",
      "base_url": "https://api.relay-example.com/v1",
      "api_key": "${RELAY_KEY:-sk-fallback-test}",
      "enabled": true,
      "priority": 50,
      "models": ["gpt-4o", "claude-3-5-sonnet-20241022"]
    }
  ],

  "routes": [
    {
      "model": "gpt-4o",             // 客户端请求的逻辑模型名称
      "enabled": true,
      "targets": [                   // 有序候选链路：第 0 项优先，后续为故障转移兜底
        { "provider": "openai-official", "model": "gpt-4o-2024-08-06" }, // 可映射到上游的具体物理模型名
        { "provider": "relay-backup" } // 省略 model 字段表示透传原逻辑模型名
      ]
    },
    {
      "model": "claude-3-5-sonnet",
      "enabled": true,
      "targets": [
        { "provider": "anthropic-direct", "model": "claude-3-5-sonnet-20241022" },
        { "provider": "relay-backup", "model": "claude-3-5-sonnet-20241022" }
      ]
    }
  ]
}
```

---

## 顶层字段详解

### Server 配置 (`server`)

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `host` | `string` | `"127.0.0.1"` | 本地服务监听地址。如需局域网共享可设为 `"0.0.0.0"`，但强烈建议同时配置 `api_key`。 |
| `port` | `uint16` | `8787` | 服务监听端口。设置为 `0` 表示由内核自动分配空闲端口。 |
| `tls_cert_file` | `string` | `""` | PEM 证书链绝对路径；与 `tls_key_file` 同时填写启用 HTTPS，同时留空保持 HTTP。启动时校验文件、证书有效期与密钥匹配；监听设置重启后生效。 |
| `tls_key_file` | `string` | `""` | 与证书匹配的未加密 PEM 私钥绝对路径。文件内容不会写入配置或返回 Web 控制台。 |
| `api_key` | `string` | `""` | 模型接口与管理接口共用的访问密钥，支持环境变量引用。仅当此项为空时关闭鉴权。监听非回环地址或开启 Web 控制台且此项为空时都会告警，因为那会暴露请求日志。 |
| `pass_through_unknown` | `bool` | `true` | 若客户端请求的模型未在 `routes` 表中定义，是否自动按优先级透传至声称拥有该模型的中转站。 |
| `max_attempts` | `size_t` | `0` | 一次请求最多尝试的中转站数量上限。`0` 表示不限（尝试链路中所有健康候选站）。 |
| `routing_policy` | `string` | `"priority"` | 首次尝试前**候选链的排序方式**：`priority`（默认，按 `priority`/`weight` 声明顺序）、`fastest`（有实测延迟者优先，按 p95 排序——通常快、偶尔极慢的中转站不该排在前面）、`cheapest`（已填价者优先，按输入+输出单价之和排序；未填价者排在最后，因为它的花费是未知而非零）、`round_robin`（在候选链中的各中转站之间轮换首选站，让平级站平均分担流量；同一站的模型回退链保持连续）。平局保持原优先级顺序；**会话亲和仍然优先于排序策略**（哪个站已经见过这段对话是更具体的事实）。 |
| `request_deadline_sec` | `int` | `0` | 整个请求的时间上限（秒），`0` 表示不限制。与 `timeout_sec`（单次尝试）和 `max_attempts`（尝试次数）不同，它约束的是客户端真正在意的那个数：一次请求最多等多久。在**两次尝试之间**以及**等待流式首字节时**检查；一旦应答已开始下发就不再中断（此时截断比让回答跑完更糟）。小于 5 秒会让故障转移失去意义，校验时会告警。 |
| `session_affinity_sec` | `int` | `0` | 同一会话粘在**已应答过它**的中转站上的时长（秒），`0` 表示关闭。开启后，同一会话的后续轮次会优先走上次成功应答的中转站，以便上游命中**提示词缓存**（长上下文下这是实打实的钱与延迟，而优先级排序无从得知“哪家是热的”）。只在成功应答后记录；熔断中的中转站不会被粘住；表内条目会过期且总量有上限。 |
| `reload_on_change` | `bool` | `false` | 是否**监听配置文件并在其变化时自动应用**（默认关闭）。开启后，编辑并保存 `config.json` 会在数秒内生效（搭在遥测刷写的 3 秒节拍上），无需点击重载或重启；控制台自己保存配置时不会产生多余的“已重载”日志。默认关闭是因为“正在运行的代理的配置被改掉了”应当是操作者主动要求的。 |
| `circuit_failure_threshold`| `uint32` | `3` | 连续遭遇几次网络故障或 5xx/429 可重试错误后触发中转站熔断。 |
| `circuit_cooldown_sec` | `uint32` | `30` | 熔断冷却期时长（秒）。冷却期间该中转站被自动降级至候选链最末端。 |
| `skip_open_circuits` | `bool` | `true` | 路由调度构建候选链时，是否跳过处于熔断 Open 状态的中转站（除非无其他候选可用）。 |
| `log_capacity` | `size_t` | `200` | 内存环形日志缓冲区的最大条目容量。超出会自动覆盖最旧记录。 |
| `log_bodies` | `bool` | `false` | 是否在内存日志中抓取并保存请求体和响应体文本。**开启时仍会对明显凭据打码**（`sk-`/`AIza`/`ghp_` 等已知前缀、`Bearer <token>`、JWT、私钥块、`api_key: <长值>`），因为它们常常就贴在提示词里；打码发生在截断之前，因此不会只留下一截密钥。 |
| `log_body_limit` | `size_t` | `2048` | 开启 `log_bodies` 时单个报文体截断保存的最大字节数。 |
| `persist_telemetry` | `bool` | `true` | 是否把计数器、逐中转站统计与请求日志持久化到状态目录，重启后自动读回。详见下方“遥测持久化”。 |
| `web_ui` | `bool` | `true` | 是否启用内置 Web 控制台。修改后即刻生效；若 `host` 设为非回环地址（如 `0.0.0.0`）且未设置 `api_key`，校验时将产生安全警告。 |
| `traffic_bucket_sec` | `int` | `3600` | 流量趋势**每个桶的宽度（秒）**，小于 60 会被提升到 60。默认 `3600 × 24` 就是历史行为“最近一天按小时”；改成 `60 × 120` 就是“最近两小时按分钟”，正在排查某个中转站时更有用。每个桶都带自己的 `bucket_sec`，因此从旧遥测文件恢复的历史不会被按新宽度误读。 |
| `traffic_bucket_count` | `int` | `24` | 趋势保留的桶数量。调小会立刻裁剪已持有的数据，不必等到下一个桶边界。 |
| `response_cache_ttl_sec` | `int` | `0` | **本地应答缓存**的有效期（秒），`0` 表示关闭（默认）。开启后，完全相同（同协议、同模型、同请求体）的**非流式**请求在有效期内直接由内存作答，不再上行——这是最直接的省钱方式。流式请求永不缓存：把缓存体当作事件流回放需要凭空编造分块与时序，客户端会察觉。 |
| `response_cache_max_entries` | `int` | `128` | 缓存条目上限，超出时淘汰**最久未使用**的一条（先清已过期的）。每条目是一整份回答，因此这是内存上限。 |
| `otlp_endpoint` | `string` | `""` | OpenTelemetry **OTLP/HTTP** 指标端点，例如 `http://127.0.0.1:4318`；留空关闭。开启后每 30 秒向 `{endpoint}/v1/metrics` 推送一次 OTLP JSON（计数器按 CUMULATIVE + monotonic 上报），适合没有 Prometheus 抓取端的环境。导出失败只在日志里报告一次，不会刷屏。 |

### HTTPS 监听

默认监听 `http://127.0.0.1:8787`。若要让监听端走 HTTPS，将 `server.tls_cert_file` 设置为服务端证书链的绝对路径（叶证书在前，随后是中间证书），并将 `server.tls_key_file` 设置为对应的未加密 PEM 私钥绝对路径。证书的 SAN 必须覆盖客户端实际连接的域名或 IP。证书缺失、过期、尚未生效或私钥不匹配时拒绝启动，`serve --force` 也不会绕过 TLS 检查。

填写后，同一监听端口上的模型接口、健康检查和 `/ui/` 管理控制台全部使用 HTTPS。主机、端口、TLS 路径以及原路径上的证书文件更新均在重启监听后生效；保存或热重载不会中断正在传输的请求。状态接口始终返回当前实际监听的协议和地址。

公有 CA 证书使用系统信任库。私有 CA 部署可在运行 CLI 的环境中设置 `LITEROUTER_CA_BUNDLE=/absolute/path/ca-bundle.pem`，`status`、`logs` 和管理命令会校验证书链及主机名；该 CA 包也用于上游请求，必要时包含系统根证书。浏览器和其他客户端需独立信任该 CA。绑定 `0.0.0.0` 或 `::` 时，远程客户端使用证书覆盖的实际服务域名；CLI 可使用仅将 `server.host` 改为该域名的本地管理配置。项目不会自动申请或续期证书。

### 成本估算与按模型定价

literouter 支持为每个中转站设置全局默认单价，同时也支持**按模型单独配置差异化单价**（`model_prices`）。

`price_in_per_million` / `price_out_per_million` 是该中转站的全局输入/输出每百万 token 单价（美元）。而在同一个中转站下，不同模型的调用成本往往天差地别（例如 `gpt-4o` 与 `gpt-4o-mini`，或 `claude-3-5-sonnet` 与 `claude-3-5-haiku`）。通过 `model_prices` 字典，可以为特定模型覆盖单独的价格：

```jsonc
{
  "price_in_per_million": 2.5,   // 默认兜底输入单价
  "price_out_per_million": 10.0, // 默认兜底输出单价
  "model_prices": {
    "gpt-4o-mini": {
      "price_in_per_million": 0.15,
      "price_out_per_million": 0.60
    }
  }
}
```

价格匹配与回退规则：
1. 先以发往上游的物理模型名匹配 `model_prices`；
2. 若未匹配，再以客户端请求的逻辑模型名匹配 `model_prices`；
3. 若仍未匹配，回退使用该中转站的全局 `price_in_per_million` / `price_out_per_million` 默认单价。

计算与计费原则：
- 只在**有 token 上报**时累计：中转站不回 `usage`（或流里不带）就计 0，不做猜测；
- 只在**填了价格**时累计：未填价的中转站计 0，因此总计是"已知花费的下限"而非全部支出；
- 花费在**记账时**就按当时的价格累计，事后改价格不会重算历史；
- 候选链排序策略如果配置为 `cheapest`，会自动根据请求模型的实际单价之和对可用链路排序，动态优选最划算链路。
- 候选链排序策略如果配置为 `round_robin`，会在候选链包含的多个中转站之间轮换首选站；请求量足够时，各平级站会自然接近平均分担。

因此它既能精细反映每种模型的真实花费，也适合回答"哪家更贵、今天花了多少"这类相对问题。

### 本地应答缓存

`server.response_cache_ttl_sec` 打开后，**完全相同**的非流式请求在有效期内由内存直接作答，不再上行。命中时响应会带 `X-Literouter-Cache: hit`（存储时是 `miss`），日志里 `provider` 一列记为 `cache`，请求总数照常 +1，但**任何中转站统计都不动、不计 token**——这正是缓存的意义。

缓存键是「入站协议 + 逻辑模型 + 归一化后的请求体」的 SHA-256，字段长度前缀拼接，因此把字符挪过字段边界也是不同的键。

**不缓存**的情况：流式请求（缓存体无法在不编造时序的前提下当作事件流回放）、音频与图像（请求是 multipart、应答可能是二进制）、以及任何非 2xx 应答（把中转站一次 400 缓存成 TTL 内的常态是灾难）。缓存关闭时会把已持有的条目丢弃，重新打开不会拿到关闭之前的旧答案。

### 中转站侧保护

`providers[].max_concurrent` / `requests_per_minute` 管的是“literouter 自己往这个站发多少”：**中转站满了不是中转站坏了**，因此达到上限的候选会被跳过并尝试下一个（不计熔断失败、不会打开熔断器），日志里写 `skipping a full relay`；整条链都满时返回 `429` 并带真实的 `Retry-After`（滚动窗口最早一次启动离开窗口的秒数）。

每次启动都会记录进窗口，即使当时没有设限。只在设限时记录会让操作者打开限制的第一分钟“白送”——而那正是他最需要限制的时刻。窗口有固定上限，因此计数带来的内存是每站一个常数上界，与流量无关。

### 流量趋势窗口

`traffic_bucket_sec × traffic_bucket_count` 决定控制台趋势图的窗口与粒度。默认 `3600 × 24` 是“最近一天，按小时”；排查某个中转站时改成 `60 × 120` 就是“最近两小时，按分钟”。桶按 UTC 取整，每个桶都记录自己的 `bucket_sec`，因此重启后从遥测文件恢复的历史不会被按新宽度误读。

### OpenTelemetry 导出

设置 `server.otlp_endpoint`（如 `http://127.0.0.1:4318`）后，literouter 每 30 秒向 `{endpoint}/v1/metrics` 推送一次 **OTLP/HTTP JSON**：`literouter.requests` 等计数器按 CUMULATIVE + monotonic 上报，逐中转站与逐客户端指标带 `relay=` / `client=` 属性。适合没有 Prometheus 抓取端的部署；导出失败只记一条日志，不会持续刷屏。

### 遥测持久化

`persist_telemetry` 为 `true`（默认）时，服务会把以下数据写入 **状态目录**（见[环境变量参考手册](environment.md)）下的 `telemetry-<port>.json`（`<port>` 为实例实际绑定的端口），并在下一次启动时读回，因此 `literouter status` 与 `/ui` 控制台不会在重启后归零：

- 全局计数器：`total_requests` / `total_success` / `total_failure` / `bytes_out` / `tokens_*` / 平均延迟；
- 逐中转站统计：请求数、成功/失败/中断数、重试吸收数、进出字节、token 数与延迟；
- 最近 **500** 条请求日志（`log.seq` 序号跨重启继续递增，不会倒退）。

写入方式与配置保持一致：先写同目录临时文件再原子重命名，文件权限 `0600`。刷写由后台线程按需进行（有变更时最多 3 秒一次），并在 `stop()` 时补齐最后一次。文件损坏或版本不符时会被忽略并记录一条系统日志，绝不会阻止服务启动；磁盘写入失败只记录一次错误日志，不影响请求处理。

> ⚠️ **隐私提示**：若同时开启 `log_bodies`，报文体（即提示词）也会被一并写入磁盘。校验时会就此组合给出告警。若不需要保存遥测，把 `persist_telemetry` 设为 `false`。

### 中转站配置 (`providers`)

`providers` 为数组，定义所有可用上游中转站。每个对象包含：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `id` | `string` | 必填 | 唯一标识符（英文字母、数字、下划线、减号），用于路由与指标统计绑定。 |
| `name` | `string` | `id` | 控制台与日志中展示的友好名称。 |
| `protocol` | `string` | `"openai"` | 上游通信协议：`openai`（默认）、`anthropic`、`gemini`、`openai_responses`、`azure`、`vertex`、`bedrock`、`ollama`。代理按**报文形态**而不是协议名判断是否需要转换，因此 `azure` 与 OpenAI 同形（零解析直通），`vertex` 与 Gemini 同形。 |
| `api_version` | `string` | `""` | `azure`：`api-version` 查询参数（留空用 `2024-10-21`）；`vertex`：路径中的 API 版本段（留空用 `v1`）。 |
| `region` | `string` | `""` | `bedrock`：AWS 区域（**必填**，SigV4 无法推断）；`vertex`：位置（留空用 `us-central1`）。 |
| `project` | `string` | `""` | `vertex`：项目 ID（**必填**）。 |
| `credentials_file` | `string` | `""` | `vertex`：service-account JSON 密钥路径（**必填**）。其私钥用于签发 RS256 断言以换取访问令牌，令牌在过期前会被缓存复用。 |
| `aws_access_key` | `string` | `""` | `bedrock`：SigV4 Access Key ID（**必填**），支持 `${VAR}` 引用，只在签名瞬间解析。 |
| `aws_secret_key` | `string` | `""` | `bedrock`：SigV4 Secret Access Key（**必填**），同样支持 `${VAR}`。 |
| `aws_session_token` | `string` | `""` | `bedrock`：临时凭据的 STS 会话令牌，会一并参与签名。 |
| `max_concurrent` | `int` | `0` | **中转站侧**在途请求上限，`0` 不限制。它约束的是 literouter 自己往这个站发多少，而不是调用方能问多少。达到上限的站会像熔断一样被跳过并尝试下一个候选（**不**计熔断失败），整条链都满时返回 `429` 并带 `Retry-After`。 |
| `requests_per_minute` | `int` | `0` | **中转站侧**滚动 60 秒启动上限，`0` 不限制。每次启动都会计入窗口，即使当时未设限——否则操作者刚打开限制的那一分钟会白送。 |
| `price_in_per_million` | `double` | `0` | 该中转站默认**输入** token 的单价（美元 / 百万 token）。`0` 表示未填写：未填写的中转站**不计入**花费估算，而不是按 0 元计。 |
| `price_out_per_million` | `double` | `0` | 该中转站默认**输出** token 的单价（美元 / 百万 token）。 |
| `model_prices` | `object` | `{}` | 为特定模型单独配置计费单价，未单独配置的模型回退到默认单价。格式为 `{"模型名": {"price_in_per_million": 数值, "price_out_per_million": 数值}}`。 |
| `base_url` | `string` | 必填 | 上游服务基础地址，例如 `https://api.openai.com/v1`。必须为合法 HTTP/HTTPS URL。 |
| `api_key` | `string` | `""` | 上游鉴权密钥，支持静态明文或环境变量占位符。 |
| `enabled` | `bool` | `true` | 是否启用该中转站。禁用后不会参与任何请求调度。 |
| `priority` | `int32` | `100` | 调度优先级，**数值越小越优先**（例如 1 优于 10，10 优于 20）。 |
| `weight` | `uint32` | `1` | 同一优先级下的相对权重，**权重越大越优先**。 |
| `timeout_sec` | `uint32` | `120` | HTTP 响应超时（秒），超过此时间请求自动终止并尝试下一个候选。 |
| `connect_timeout_sec` | `uint32` | `15` | TCP / TLS 握手连接超时（秒）。 |
| `supports_stream` | `bool` | `true` | 该中转站是否支持 Server-Sent Events (SSE) 流式传输。 |
| `models` | `string[]`| `[]` | 声明该站点支持的模型列表。自动透传与一键探测时使用。 |
| `headers` | `object` | `{}` | 自定义请求头字典（键值对），每个发往该站点的请求都会自动附带。 |
| `chat_path` | `string` | 依协议（`openai`, `anthropic`, `gemini`, `openai_responses`, `azure`, `vertex`, `bedrock`, `ollama`） | 自定义对话补全端点路径。留空时依据 `protocol` 自动推导为标准路径（例如 `anthropic` → `/v1/messages`，`vertex` → `/v1/projects/…/publishers/google/models/{model}:generateContent`，`bedrock` → `/model/{model}/converse`）。 |
| `embeddings_path`| `string` | `"/embeddings"`| 自定义向量嵌入端点路径。 |
| `note` | `string` | `""` | 备注说明信息。 |

### 路由规则配置 (`routes`)

`routes` 显式定义客户端逻辑模型名称到上游中转站物理链路的映射规则。

每个路由包含：
- `model` (`string`, 必填)：客户端请求的逻辑模型名称（如 `gpt-4o`, `claude-3-5-sonnet`）。
- `enabled` (`bool`, 默认 `true`)：是否启用此规则。
- `targets` (`Target[]`, 必填)：有序候选链路列表。
  - `provider` (`string`, 必填)：引用的中转站 `id`。
  - `model` (`string`, 可选)：向上游中转站发送的实际物理模型名称。若留空，则透传客户端请求的逻辑模型名称。

---

## 密钥安全解析机制

### 支持的语法格式

在配置文件的任意 `api_key` 字段中，literouter 支持以下 5 种声明格式：

```text
1. "${VAR_NAME}"          // 从环境变量读取，未设置时视为空字符串
2. "${VAR:-fallback}"     // 从环境变量读取，若环境变量未设置或为空，则回退为 fallback
3. "$VAR_NAME"            // 简易环境变量引用
4. "sk-abc123456"         // 明文静态密钥
5. ""                     // 留空，不发送 Authorization 头（适用于本地/内网免鉴权网关）
```

### 绝对不可逆（零泄露）原则

在安全实现上，`literouter` 始终坚守**密钥零泄露保障**：

1. **按需展开**：配置载入内存时保留原始占位符字符串。密钥**仅在实际发起网络请求瞬间**由内部模块调用 `resolveSecret()` 解析为明文并写入 HTTP 请求头。
2. **绝对禁止回写明文**：无论是通过 `/ui` 控制台保存配置，还是通过 CLI `literouter config save`，`ConfigStore` 序列化时**始终保留原始的环境变量占位符**，杜绝将敏感明文意外沉淀到磁盘配置文件中。
3. **保存权限加固**：每次保存都会把文件收紧到仅当前用户可访问——POSIX 上是 `0600`；Windows 上则把 DACL 替换为仅含当前用户一条 ACE（配置里可能有明文密钥，而它并不只可能放在 `%APPDATA%`）。

---

## 配置校验引擎 (Validation)

配置载入或保存前，系统规则引擎会对配置结构执行多维度体检：

- 🔴 **Error 严重错误**（导致配置不可用）：
  - 中转站 `id` 为空或重复；
  - 中转站 `base_url` 格式非法（非 `http://` 或 `https://`）；
  - `routes` 中引用的 `provider` 不存在；
  - 存在多条相同 `model` 名的重复路由；
  - 端口号超出合法范围 (`> 65535`)。
- 🟡 **Warning 警告**（配置可运行，但存在潜在隐患）：
  - 引用的环境变量在当前系统环境中未被定义；
  - 路由规则的目标链路中包含了已禁用的中转站；
  - 服务监听在非回环地址（如 `0.0.0.0`）却未配置访问密钥 `server.api_key`；
  - Web 控制台开在非回环地址上却未配置 `server.api_key`，能访问该端口的人就能读到请求日志。
- 🔵 **Info 提示**（常规提示）：
  - 中转站未声明任何模型列表（只能通过路由规则显式调度）；
  - 监听端口设为 `0`（动态分配端口）。

你可以随时使用命令行执行体检：
```bash
mcpp run -p cli -- config validate
mcpp run -p cli -- doctor
```

---

## 配置热重载 (Hot Reload)

当外部编辑器修改了 `config.json` 后，无需重启进程即可生效：

```bash
# 方式 1：CLI 发送重载命令
mcpp run -p cli -- config reload

# 方式 2：调用管理端点
curl -X POST http://127.0.0.1:8787/__literouter/reload

# 方式 3：在 Web 控制台的“设置”面板中点击【从磁盘重载】按钮
```
