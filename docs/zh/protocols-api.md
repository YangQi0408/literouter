# 接口与协议规范

本文档全面介绍 `literouter` 对外提供的客户端兼容端点、支持的上游协议类型、跨协议自动转换与零开销直通机制，以及内部管理 API。

---

## 目录

- [概览](#概览)
- [客户端入口端点 (Client Inbound Endpoints)](#客户端入口端点-client-inbound-endpoints)
  - [OpenAI 对话补全 (Chat Completions)](#openai-对话补全-chat-completions)
  - [Anthropic Claude 原生接口 (Messages API)](#anthropic-claude-原生接口-messages-api)
  - [Google Gemini 原生接口 (GenerateContent)](#google-gemini-原生接口-generatecontent)
  - [OpenAI Responses API](#openai-responses-api)
  - [向量嵌入 (Embeddings) 与旧版补全 (Completions)](#向量嵌入-embeddings-与旧版补全-completions)
  - [模型查询与健康检查](#模型查询与健康检查)
- [上游提供商协议 (Upstream Protocols)](#上游提供商协议-upstream-protocols)
  - [支持的协议类型](#支持的协议类型)
  - [端点路径自动推导规则](#端点路径自动推导规则)
- [跨协议适配与零开销直通 (Zero-overhead Passthrough)](#跨协议适配与零开销直通-zero-overhead-passthrough)
  - [同协议零开销直通 (Fast Path)](#同协议零开销直通-fast-path)
  - [跨协议双向转换 (Cross-Protocol Translation)](#跨协议双向转换-cross-protocol-translation)
- [内部管理端点 (Admin API)](#内部管理端点-admin-api)
- [内置 Web 控制台 (Web Console)](#内置-web-控制台-web-console)

---

## 概览

`literouter` 不仅是一个单一协议的重试代理，也是一个**多协议接入网关**。无论你的客户端习惯使用 OpenAI SDK、Anthropic Claude SDK 还是 Google GenAI SDK，均可直接连接到 `literouter` 本地端口，网关会智能将请求转换为目标上游所需的报文格式。

---

## 客户端入口端点 (Client Inbound Endpoints)

所有客户端端点均支持携带 Bearer Token 进行鉴权（若配置文件中的 `server.api_key` 非空）：
```http
Authorization: Bearer <your-server-api-key>
```

### OpenAI 对话补全 (Chat Completions)

- **端点路径**：
  - `POST /v1/chat/completions`
  - `POST /chat/completions`（适配省去 `/v1` 前缀的轻量客户端）
- **流式透传**：请求体包含 `"stream": true` 时，以标准 `text/event-stream` SSE 格式输出。

**请求示例**：
```bash
curl http://127.0.0.1:8787/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gpt-4o",
    "messages": [
      {"role": "user", "content": "你好！"}
    ],
    "stream": true
  }'
```

### Anthropic Claude 原生接口 (Messages API)

- **端点路径**：
  - `POST /v1/messages`
  - `POST /messages`
- **说明**：可直接接入支持 Claude 官方协议的工具与客户端（如 Claude Dev, Cline 等）。支持 `x-api-key` 或 `Authorization: Bearer` 鉴权头。

**请求示例**：
```bash
curl http://127.0.0.1:8787/v1/messages \
  -H "Content-Type: application/json" \
  -H "anthropic-version: 2023-06-01" \
  -d '{
    "model": "claude-3-5-sonnet",
    "max_tokens": 1024,
    "messages": [
      {"role": "user", "content": "Hello Claude!"}
    ]
  }'
```

### Google Gemini 原生接口 (GenerateContent)

- **端点路径**：
  - 非流式：`POST /v1beta/models/{model}:generateContent`、`POST /v1/models/{model}:generateContent`
  - 流式：`POST /v1beta/models/{model}:streamGenerateContent`、`POST /v1/models/{model}:streamGenerateContent`
- **说明**：支持 Google Gemini 官方 Python/Node SDK 直接配置 `http://127.0.0.1:8787` 作为根端点。

### OpenAI Responses API

- **端点路径**：
  - `POST /v1/responses`
  - `POST /responses`

### 向量嵌入 (Embeddings) 与旧版补全 (Completions)

- **向量嵌入**：`POST /v1/embeddings`
- **传统补全**：`POST /v1/completions`

### 模型查询与健康检查

| 方法 | 端点路径 | 作用说明 |
|---|---|---|
| `GET` | `/v1/models`, `/models`, `/v1beta/models` | 列出聚合器聚合的所有逻辑模型列表，附加每个模型的候选链路拓扑 |
| `GET` | `/v1/models/{id}`, `/models/{id}` | 查询单个模型的详细元数据与候选站健康状态 |
| `GET` | `/health` | 服务存活探测端点，免鉴权，健康时返回 `{"status":"ok"}` |

---

## 上游提供商协议 (Upstream Protocols)

在配置文件的 `providers[].protocol` 字段中指定上游中转站的通信协议规范：

### 支持的协议类型

1. **`openai` (默认)**：
   - 适用于：OpenAI 官方、DeepSeek、Moonshot (Kimi)、SiliconFlow、Groq、Ollama、vLLM 以及绝大多数第三方中转聚合站。
   - 默认鉴权：`Authorization: Bearer <api_key>`。
2. **`anthropic`**：
   - 适用于：Anthropic 官方（`api.anthropic.com`）或提供原生 Claude Messages API 的服务商。
   - 自动注入请求头：`x-api-key: <api_key>` 与 `anthropic-version: 2023-06-01`。
3. **`gemini`**：
   - 适用于：Google AI Studio（`generativelanguage.googleapis.com`）。
   - 自动注入请求头：`x-goog-api-key: <api_key>`。
4. **`openai_responses`**：
   - 适用于支持 OpenAI 新一代 Responses 协议的上游端点。

### 端点路径自动推导规则

若中转站配置中的 `chat_path` 为空，系统依据 `protocol` 自动推导对话端点路径：

| 协议 (`protocol`) | 默认对话补全路径 (`chat_path`) |
|---|---|
| `openai` | `/chat/completions`（若 `base_url` 不含 `/v1`，请求时会自动组装） |
| `anthropic` | `/v1/messages` |
| `gemini` | `/v1beta/models/{model}:generateContent` (流式为 `:streamGenerateContent?alt=sse`) |
| `openai_responses` | `/v1/responses` |

---

## 跨协议适配与零开销直通 (Zero-overhead Passthrough)

### 同协议零开销直通 (Fast Path)

当**客户端请求的协议**与**选中的上游中转站协议**完全一致时（例如：客户端发送 OpenAI `/v1/chat/completions`，上游提供商 `protocol` 亦为 `openai`）：

- **零解析开销**：入站请求体无需经过完整的 JSON DOM 树构建，直接流向目标上游套接字（仅在需要重命名模型时做轻量替换）；
- **零缓冲透传**：SSE 数据块到达后即刻推入客户端套接字，内存无额外拼装拷贝，延迟达到硬件和网络极限。

> **上游连接复用**：缓冲与流式两条路径都从**每工作线程一份**的上游连接池里取连接（每个中转站一条，键包含根地址、两个超时值与 CA 包），所以连续请求不会重复支付 TCP/TLS 握手——流式路径此前每次尝试都新建连接，等于把握手成本压在了流量最大的那条路上。传输干净结束时连接留在池中供下次复用；**被中止或因传输错误结束的传输会把它退役**，交给下一个请求的永远是干净 socket。httplib 在复用前会探测 socket 存活（含 TLS 对端关闭判定），因此中转站关掉空闲连接只会多一次重连，不会让请求失败。

> **流式 token 统计**：为统计用量，流经的每个数据块只做一次子串匹配；**在事件边界结束且不含 `usage` 的块完全不拷贝、不解析**（快路径），命中 `usage` / `usageMetadata` 的块才解析（OpenAI 通常只有最后一个块），只有跨块被切断的事件才进入有界的接缝缓冲。因此上面的“零解析开销”与“无额外拼装拷贝”在语义上仍然成立。计数按出现过的**最大值**合并：Anthropic 把输入与输出分别放在 `message_start` 与 `message_delta`，Gemini 在每个块里累计上报，OpenAI 仅在客户端开启 `stream_options.include_usage` 时于流尾给出一次（`literouter` **不会**替你注入该参数，以免改动客户端看到的报文）。若上游始终未报告 usage，该次请求的 token 计为 0，不做估算或猜测。

### 跨协议双向转换 (Cross-Protocol Translation)

当客户端与上游协议不一致时，`literouter` 内部协议适配层（`core/src/lr_protocol.cpp`）无缝执行双向协议转换：

```text
客户端 (如 OpenAI SDK)               literouter                上游 (如 Anthropic 官方)
    /v1/chat/completions                 │                           /v1/messages
             │                           │                                │
             │─── OpenAI Request JSON ──▶│                                │
             │                           │── 转换 messages/system/tools ─▶│
             │                           │── 注入 anthropic-version 头 ──▶│
             │                           │                                │
             │                           │◀── Anthropic SSE Stream Chunk ─│
             │                           │── 转换为 standard OpenAI SSE ──│
             │◀── data: {delta:...} ─────│                                │
```

适配层支持双向转换的能力覆盖：
- 角色映射（`developer`/`system` 提示词自动提纯，`tool_calls` 与 tool result 转换）；
- 常用控制参数自动映射（`max_tokens`, `temperature`, `top_p`, `top_k`, `stop`）；
- SSE 流式数据事件双向互转与结束标记对齐；
- Token 计费统计（`usage`）结构对齐。

> **推理/思考内容**：转换时**保留**模型的推理内容而不是丢弃——上游为 Anthropic 时 `thinking` 块映射为 OpenAI 侧的 `reasoning_content`（`redacted_thinking` 内容加密，不解析）；上游为 Gemini 时带 `thought: true` 的部分归入 `reasoning_content`（此前它被错误地拼进了正文，等于把思考当成回答）；上游为 Responses API 时 `reasoning` 条目（读 `summary[].text`，兼容把明文放在 `content[].text` 的中转站）同样归入 `reasoning_content`。反向则分别变成 Anthropic 的**前置 `thinking` 块**、Responses 的**前置 `reasoning` 条目**、Gemini 的 `thought: true` 部分（均为响应方向）。流式方向上，Anthropic 的 `thinking_delta` 与 Responses 的 `response.reasoning_summary_text.delta` 都会转成 `reasoning_content` 增量。
>
> **两处刻意的不对称**（都不是遗漏）：
> 1. **请求方向不合成 `thinking` 块**。Anthropic 只接受带**原始签名**的 thinking 块、Gemini 只接受带 `thoughtSignature` 的思考部分，伪造签名会把本可成功的请求变成 400，因此客户端的 `reasoning_content` 在通往这两家的请求里被丢弃；反向（Anthropic/Gemini/Responses → Chat）则把客户端给的推理内容作为 `reasoning_content` 原样带过去。
> 2. **流式反向（OpenAI → Anthropic）暂不合成 thinking 块**。那需要第二个内容块及其索引与开始/结束帧，块序列错乱对严格客户端比"没有思考内容"更糟。

---

## 内部管理端点 (Admin API)

管理端点统一以 `/__literouter` 为路径前缀，供 CLI、GUI 或自动化运维脚本调用。当配置了 `server.api_key` 时，管理接口同样要求携带 Bearer 鉴权头。

管理接口与 Web 控制台**不会发送 CORS 头**，只有面向客户端的端点保留 `Access-Control-Allow-Origin: *`——请求日志可能包含用户的提示词，允许任意网页跨域读取等于把它们交出去。所有管理响应一律带 `Cache-Control: no-store`。

### 1. 获取系统状态快照

- **请求**：`GET /__literouter/status`
- **响应**：
  ```json
  {
    "running": true,
    "host": "127.0.0.1",
    "port": 8787,
    "base_url": "http://127.0.0.1:8787",
    "version": "0.1.0",
    "config_path": "/home/you/.config/literouter/config.json",
    "started_unix": 1758000000.0,
    "uptime_sec": 3600.5,
    "active_requests": 0,
    "total_requests": 1420,
    "total_success": 1410,
    "total_failure": 10,
    "bytes_out": 12345678,
    "tokens_prompt": 120000,
    "tokens_completion": 45000,
    "latency_ms_avg": 345.2,
    "log_seq": 1420,
    "breakers_open": 0,
    "providers": [
      {
        "provider": "openai-official",
        "requests": 1200,
        "successes": 1198,
        "failures": 2,
        "aborted": 0,
        "retries_in": 5,
        "bytes_in": 120000,
        "bytes_out": 900000,
        "tokens_prompt": 110000,
        "tokens_completion": 40000,
        "latency_ms_last": 310.5,
        "latency_ms_avg": 345.2,
        "latency_ms_p95": 0.0,
        "last_used_unix": 1758003600.5
      }
    ],
    "health": [
      {
        "provider": "openai-official",
        "state": "healthy",
        "consecutive_failures": 0,
        "total_failures": 2,
        "last_error": "",
        "cooldown_remaining": 0.0
      }
    ]
  }
  ```

  `providers` 是累计统计（自上次 `POST /__literouter/reset-stats` 或遥测文件恢复起），其中 `latency_ms_p95` 是最近 64 次尝试的最近秩（nearest-rank）p95——取窗口内实际出现过的样本值，没有样本时为 0；`health` 是熔断器当前状态（`state` 取 `unknown` / `healthy` / `degraded` / `open`）。`uptime_sec` 每次请求实时计算，因此 Web 控制台与 GUI 的运行时长会逐秒跳动；显示格式为 `1h 2m 5s`，且**始终保留秒**（`humanUptime`），而一般的时长显示（如熔断冷却剩余时间）仍使用会向上归整到分钟的 `humanDuration`。

### 2. 获取增量请求日志

- **请求**：`GET /__literouter/logs?since=<id>&limit=<n>`
- **参数**：
  - `since`：拉取日志 ID 大于此值的增量记录；
  - `limit`：最多返回条数（默认 100）。
- **响应**：包含请求摘要、方法、目标模型、上游站点、HTTP 状态码、耗时（毫秒）、Token 统计等。

  每条日志的**耗时拆成三段**（`wait_ms` / `ttfb_ms` / `stream_ms`），按时间发生顺序：

  | 字段 | 含义 |
  |---|---|
  | `wait_ms` | 从请求进入到**本中转站被尝试**之间的时间：包含排队与前面候选站消耗的时间，因此它是"故障转移链有多贵"的直接证据 |
  | `ttfb_ms` | 从中转站被尝试到**首个响应字节**到达：这一站自己的思考时间。缓冲式回复无法再细分（httplib 把建连与应答合为一个数），此时该值即为整次调用耗时 |
  | `stream_ms` | 从首字节到最后一个字节：流式应答的传输时长；非流式恒为 0 |

  因此流式请求是唯一能把"这一站多久开口"与"说完用了多久"分开的地方——这正是比较不同中转站时最需要的两个数。

### 3. 配置热重载

- **请求**：`POST /__literouter/reload`
- **响应**：`{"ok": true, "message": "reloaded successfully"}`

### 4. 重置遥测统计与熔断器

- **请求**：`POST /__literouter/reset-stats`
- **作用**：清空所有中转站的成功/失败计数、延迟统计，并将所有熔断器强制恢复为 Healthy。

### 5. 存活探测与模型扫描

- **请求**：`POST /__literouter/probe`
- **请求体**（二选一）：
  - `{"provider": "<id>"}`：探测**已配置**的中转站；密钥在服务端解析（`resolveSecret`），不会经由网络传输；
  - 中转站配置 JSON（`base_url`, `api_key`, `protocol`, `headers`, `timeout_sec`）：探测尚未保存的临时条目，供表单使用。
- **作用**：即时向该中转站发起探测，测试网络连通性，并自动拉取上游所支持的模型列表。

### 6. Prometheus 指标导出

- **请求**：`GET /__literouter/metrics`
- **响应**：Prometheus 文本格式（`text/plain; version=0.0.4`），内容与 `/__literouter/status` **同源**——控制台给人看，这里给图看，两者读的是同一份快照。
- **指标**：`literouter_running`、`literouter_uptime_seconds`、`literouter_requests_total`/`successes_total`/`failures_total`、`literouter_active_requests`、`literouter_breakers_open`、`literouter_bytes_out_total`、`literouter_tokens_*_total`、`literouter_latency_ms_avg`，以及逐中转站的 `literouter_relay_*{relay="<id>"}`（请求/成功/失败/客户端中断/吸收重试/进出字节/Token/`latency_ms_last|avg|p95`/`last_used_unixtime`/`healthy`/`cooldown_seconds`）。
- **鉴权**：与其它管理端点一致受 `server.api_key` 保护（Prometheus 侧用 `bearer_token` / `authorization` 配置即可）。
- **示例**（`prometheus.yml`）：
  ```yaml
  scrape_configs:
    - job_name: literouter
      authorization:
        credentials_file: /etc/literouter/key
      static_configs:
        - targets: ["127.0.0.1:8787"]
      metrics_path: /__literouter/metrics
  ```

### 7. 安全关闭服务

- **请求**：`POST /__literouter/shutdown`
- **作用**：通知正在运行的后台服务安全释放资源并优雅退出进程。

### 8. 读取运行配置（脱敏）

- **请求**：`GET /__literouter/config`
- **响应**：
  ```json
  {
    "path": "/home/me/.config/literouter/config.json",
    "exists": true,
    "config": { "server": { "...": "..." }, "providers": [ "..." ], "routes": [ "..." ] },
    "validation": { "ok": false, "summary": "0 errors, 1 warning", "issues": [ "..." ] }
  }
  ```
- **脱敏规则**：`api_key` 是 `${VAR}` 引用时原样返回（它只是变量名，不是密钥）；是字面量密钥时替换为空字符串，并以 `"api_key_source": "literal"` 标记。**真实密钥不会经过网络。**

### 9. 更新运行配置与持久化

- **请求**：`PUT /__literouter/config`
- **请求体**：`{ "config": <AppConfig> }` 或裸 `<AppConfig>` 对象
- **校验与原子性**：先执行 JSON 反序列化，再执行语义校验（`validate()`）。若存在任何 `error` 级别错误，返回 **422 Unprocessable Entity**，且不改动内存和磁盘配置。
- **密钥保留规则**：
  - provider 的 `api_key` 为空字符串：自动保留内存中该 provider 现有的密钥（支持将脱敏 GET 的结果安全原样回传）；
  - `api_key_clear: true`：显式清空该 provider 的密钥；
  - 非空字符串：更新为新值（支持环境变量占位符 `${VAR}`）。
- **持久化**：有配置文件路径时使用临时文件重命名原子落盘，并调用 `updateConfig()`；无路径时仅更新内存并返回 `"saved": false`。
- **响应**：`{"ok": true, "saved": true, "path": "...", "summary": "...", "issues": [...]}`

---

## 内置 Web 控制台 (Web Console)

`literouter serve` 运行时，同一个端口就带一份基于 **React + Vite + Tailwind + shadcn/ui** 开发的现代 Web 控制台，不依赖任何外部 CDN 资源，非常适合无桌面环境的服务器与容器部署。

| 路径 | 说明 |
|---|---|
| `GET /` | 302 跳转到 `/ui/`（控制台开启时） |
| `GET /ui/` | 控制台单页应用入口（`index.html`） |
| `GET /ui/app.js`、`/ui/app.css`、`/ui/favicon.svg` | 前端构建产物 |
| `GET /favicon.ico` | 302 跳转到 `/ui/favicon.svg` |

- **开关控制**：由 `server.web_ui`（布尔值，默认 `true`）控制。也可以在启动时通过 CLI 参数 `serve --web-ui` 或 `serve --no-web-ui` 显式覆盖。关闭后访问 `/ui/` 返回 404（错误码 `console_disabled`）。配置修改（通过 reload 或 PUT）后即刻生效，无需重启进程。
- **鉴权**：静态外壳不含任何数据，因此无需密钥即可加载；页面随后调用的 `/__literouter/*` 与 `/v1/*` 一样受 `server.api_key` 保护。浏览器首次访问会弹出密钥输入框，密钥只保存在本机 localStorage。
- **打包方式**：前端构建产物（`web/dist/` 下的 `index.html`、`app.js`、`app.css`、`favicon.svg`）通过 C++23 `#embed` 编译进二进制，服务器上只拷贝一个 `literouter` 即可。若编译器不支持 `#embed`（例如 ISO 严格模式下的 GCC），改用环境变量 `LITEROUTER_WEB_DIR` 指向包含这四个文件的构建产物目录（如 `web/dist`）。
- **能力**：实时指标磁贴（请求/成功率/Token 速率等）、中转站健康矩阵与一键探测、路由候选链排序与编辑、可视化全局配置编辑与安全回传保存、增量日志流过滤（按级别/类型/关键字过滤、暂停/清空）、暗亮主题切换与中英双语国际化。
- **安全性**：与控制台同源，不向跨域请求开放管理端点；密钥仅用于浏览器到本机服务的同源请求，且字面量密钥绝不出网。
