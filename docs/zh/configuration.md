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
  "schema": 1,

  "server": {
    "host": "127.0.0.1",          // 监听地址（绑定到公网非回环地址且未设 api_key 时将触发安全告警）
    "port": 8787,                 // 监听端口；设为 0 表示由系统随机分配空闲端口
    "api_key": "",                // 客户端访问本代理的鉴权 Bearer Token；留空表示不校验
    "pass_through_unknown": true, // 路由表未显式收录的模型，自动按优先级透传给拥有该模型的中转站
    "max_attempts": 0,            // 单次请求最多重试尝试的中转站数量；0 表示遍历所有可用候选站
    "request_deadline_sec": 0,    // 整个请求的时间上限（秒）；0 表示不限制
    "circuit_failure_threshold": 3, // 连续失败几次触发中转站进入熔断冷却
    "circuit_cooldown_sec": 30,     // 熔断后的冷却保持时长（秒）
    "skip_open_circuits": true,   // 路由调度时是否优先跳过熔断冷却中的中转站
    "log_capacity": 200,          // 内存环形日志最大缓冲条数
    "log_bodies": false,          // 是否记录请求体与响应体报文（默认关闭以保护提示词隐私）
    "log_body_limit": 2048,       // 记录报文时截断保存的最大字节数
    "persist_telemetry": true,    // 是否将计数器与请求日志持久化到状态目录（详见下文“遥测持久化”）
    "language": "auto",           // 界面语言：auto / en / zh
    "ui_scale": 1.0,              // GUI 界面缩放：0.8 ~ 1.5（0.0 或 1.0 表示默认）
    "web_ui": true                // 是否在 /ui 提供内置 Web 控制台（非回环地址且未设 api_key 时将告警）
  },

  "providers": [
    {
      "id": "openai-official",                 // 唯一标识符（全局唯一，路由与统计均以此为键）
      "name": "OpenAI 官方直连",                // 控制台显示名
      "protocol": "openai",                    // 上游协议：openai / anthropic / gemini / openai_responses
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
| `api_key` | `string` | `""` | 代理本身的客户端访问鉴权密钥。客户端需在请求头携带 `Authorization: Bearer <api_key>`；若为空则不校验。 |
| `pass_through_unknown` | `bool` | `true` | 若客户端请求的模型未在 `routes` 表中定义，是否自动按优先级透传至声称拥有该模型的中转站。 |
| `max_attempts` | `size_t` | `0` | 一次请求最多尝试的中转站数量上限。`0` 表示不限（尝试链路中所有健康候选站）。 |
| `request_deadline_sec` | `int` | `0` | 整个请求的时间上限（秒），`0` 表示不限制。与 `timeout_sec`（单次尝试）和 `max_attempts`（尝试次数）不同，它约束的是客户端真正在意的那个数：一次请求最多等多久。在**两次尝试之间**以及**等待流式首字节时**检查；一旦应答已开始下发就不再中断（此时截断比让回答跑完更糟）。小于 5 秒会让故障转移失去意义，校验时会告警。 |
| `circuit_failure_threshold`| `uint32` | `3` | 连续遭遇几次网络故障或 5xx/429 可重试错误后触发中转站熔断。 |
| `circuit_cooldown_sec` | `uint32` | `30` | 熔断冷却期时长（秒）。冷却期间该中转站被自动降级至候选链最末端。 |
| `skip_open_circuits` | `bool` | `true` | 路由调度构建候选链时，是否跳过处于熔断 Open 状态的中转站（除非无其他候选可用）。 |
| `log_capacity` | `size_t` | `200` | 内存环形日志缓冲区的最大条目容量。超出会自动覆盖最旧记录。 |
| `log_bodies` | `bool` | `false` | 是否在内存日志中抓取并保存请求体和响应体文本。 |
| `log_body_limit` | `size_t` | `2048` | 开启 `log_bodies` 时单个报文体截断保存的最大字节数。 |
| `persist_telemetry` | `bool` | `true` | 是否把计数器、逐中转站统计与请求日志持久化到状态目录，重启后自动读回。详见下方“遥测持久化”。 |
| `language` | `string` | `"auto"` | 界面语言：`auto`（跟随系统区域）/ `en` / `zh`。CLI 与 GUI 共用同一份字典。 |
| `ui_scale` | `double` | `1.0` | GUI 初始矢量缩放比例，可用范围约 `0.25` ~ `4.0`（推荐 `0.8` ~ `1.5`）；`0.0` 与 `1.0` 均表示默认。 |
| `web_ui` | `bool` | `true` | 是否启用内置 Web 控制台。修改后即刻生效；若 `host` 设为非回环地址（如 `0.0.0.0`）且未设置 `api_key`，校验时将产生安全警告。 |

### 遥测持久化

`persist_telemetry` 为 `true`（默认）时，服务会把以下数据写入 **状态目录**（见[环境变量参考手册](environment.md)）下的 `telemetry-<port>.json`（`<port>` 为实例实际绑定的端口），并在下一次启动时读回，因此 `literouter status`、`/ui` 控制台与 GUI 不会在重启后归零：

- 全局计数器：`total_requests` / `total_success` / `total_failure` / `bytes_out` / `tokens_*` / 平均延迟；
- 逐中转站统计：请求数、成功/失败/中断数、重试吸收数、进出字节、token 数与延迟；
- 最近 **500** 条请求日志（`log.seq` 序号跨重启继续递增，不会倒退）。

写入方式与配置保持一致：先写同目录临时文件再原子重命名，文件权限 `0600`。刷写由后台线程按需进行（有变更时最多 3 秒一次），并在 `stop()` 时补齐最后一次。文件损坏或版本不符时会被忽略并记录一条系统日志，绝不会阻止服务启动；磁盘写入失败只记录一次错误日志，不影响请求处理。

> ⚠️ **隐私提示**：若同时开启 `log_bodies`，报文体（即提示词）也会被一并写入磁盘。校验时会就此组合给出告警。如需每次运行都不留痕迹，请把 `persist_telemetry` 设为 `false`。

### 中转站配置 (`providers`)

`providers` 为数组，定义所有可用上游中转站。每个对象包含：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `id` | `string` | 必填 | 唯一标识符（英文字母、数字、下划线、减号），用于路由与指标统计绑定。 |
| `name` | `string` | `id` | 控制台与日志中展示的友好名称。 |
| `protocol` | `string` | `"openai"` | 上游通信协议：`openai`（默认）、`anthropic`、`gemini`、`openai_responses`。 |
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
| `chat_path` | `string` | 依协议 | 自定义对话补全端点路径。留空时依据 `protocol` 自动推导为标准路径。 |
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
2. **绝对禁止回写明文**：无论是在 GUI 界面中修改配置并点击保存，还是通过 CLI `literouter config save`，`ConfigStore` 序列化时**始终保留原始的环境变量占位符**，杜绝将敏感明文意外沉淀到磁盘配置文件中。
3. **明文保存权限加固**：如果配置文件中确实包含了直接书写的明文密钥（如 `sk-...`），系统在保存文件时会自动将文件权限收紧为 `0600`（仅当前用户可读写）。

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
  - 服务监听在非回环地址（如 `0.0.0.0`）却未配置客户端鉴权密钥 `server.api_key`。
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

# 方式 3：在 GUI 控制台的“设置”面板中点击【从磁盘重载】按钮
```
