# 接口与协议规范

所有接口也可由同一监听端口提供 HTTPS：配置 `server.tls_cert_file` 与 `server.tls_key_file`，重启后将下文 URL 的 `http://` 改为 `https://`。管理客户端仍校验证书链和主机名，私有 CA 使用 `LITEROUTER_CA_BUNDLE`；详见[配置文档](configuration.md#https-监听)。

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
  - [音频与图像](#音频与图像)
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

模型端点接受 `server.api_key`。仅当此项为空时关闭鉴权：
```http
Authorization: Bearer <your-api-key>
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

### 音频与图像

| 端点（`POST`） | 请求 | 响应 |
|---|---|---|
| `/v1/audio/transcriptions` | `multipart/form-data`，包含 `file` 与 `model` | JSON、文本/字幕格式；`stream=true` 时可透传上游 SSE |
| `/v1/audio/translations` | `multipart/form-data`，包含 `file` 与 `model` | 上游 JSON 或文本/字幕格式 |
| `/v1/audio/speech` | JSON，包含 `model`、`input`、`voice`，可选 `response_format` | 音频二进制或上游 SSE，随到随发 |
| `/v1/images/generations` | JSON，包含 `prompt` 及模型支持的参数 | JSON；`stream=true` 时可透传上游 SSE |
| `/v1/images/edits` | 图片/蒙版 Multipart，或上游支持的 JSON | JSON；`stream=true` 时可透传上游 SSE |
| `/v1/images/variations` | 图片及参数 Multipart | 上游 JSON |

这些端点要求中转站采用 **`openai` 协议**，并且上游实现相应端点。其他协议的候选站会在计算尝试次数前排除；模型已有路由但没有兼容站时，返回 `400`，错误码为 `unsupported_media_protocol`。音频、图像请求不会被转换成 Chat、Anthropic、Gemini 或 Responses 请求。具体模型支持范围和参数校验由上游决定。

`base_url` 是 API 前缀，例如 `https://api.openai.com/v1` 加上 `/audio/speech`。`chat_path` 和 `embeddings_path` 不影响这些路径。`model` 沿用现有模型路由与重命名规则；图像请求未填写模型时，按 OpenAI 默认的 `dall-e-2` 路由，音频请求必须填写模型。Multipart 按原顺序保留重复字段（如 `image[]`、时间戳选项）、文件名、分段 MIME 头与二进制内容；只重新生成 MIME 边界并替换模型字段。上传内容会缓冲以支持重试，网关请求体上限为 **64 MiB**；上游可能有更低限制。

媒体请求共用鉴权、路由策略、故障转移、熔断、日志和用量统计。非流式响应完整接收后才提交。语音生成及显式流式请求沿用响应头闸门：提交前的连接失败、`429` 和可重试 `5xx` 可切换中转站；成功响应头一旦提交，上游音频/SSE 中断就会终止客户端连接并记录失败，绝不切换站点拼接另一段内容。二进制响应及上游 `Content-Type` 保持原样；网关自身错误使用 OpenAI JSON 格式。无法确认上游是否已处理的传输失败可能造成重复操作或计费，不能接受重放时可设置 `max_attempts: 1`。

Token 统计只采纳上游 JSON/SSE 实际返回的用量；Token 单价字段不会估算音频时长费用或按张图像费用。二进制上传和媒体响应不进入正文日志；Multipart 日志仅保存模型、流式标志和分段元数据（字段名、文件名、MIME 类型、字节数）。Web 控制台的日志筛选支持音频与图像。

```bash
curl http://127.0.0.1:8787/v1/audio/transcriptions \
  -H "Authorization: Bearer $LITEROUTER_KEY" \
  -F model=whisper-1 -F file=@recording.wav

curl http://127.0.0.1:8787/v1/audio/speech \
  -H "Authorization: Bearer $LITEROUTER_KEY" -H "Content-Type: application/json" \
  -d '{"model":"tts-1","input":"你好","voice":"alloy","response_format":"mp3"}' \
  --output speech.mp3

curl http://127.0.0.1:8787/v1/images/generations \
  -H "Authorization: Bearer $LITEROUTER_KEY" -H "Content-Type: application/json" \
  -d '{"model":"gpt-image-1","prompt":"水彩山丘","size":"1024x1024"}'
```

### 模型查询与健康检查

| 方法 | 端点路径 | 作用说明 |
|---|---|---|
| `GET` | `/v1/models`, `/models`, `/v1beta/models` | 列出聚合器聚合的所有逻辑模型列表，附加每个模型的候选链路拓扑 |
| `GET` | `/v1/models/{id}`, `/models/{id}` | 查询单个模型的详细元数据与候选站健康状态 |
| `GET` | `/health`, `/health/live` | **存活探测**：免鉴权，健康时返回 `{"status":"ok"}`。回答的是"这个进程还活着吗"，因此只看监听器是否在跑。 |
| `GET` | `/health/ready` | **就绪探测**：免鉴权。回答的是"能不能把流量发到这里"，因此配置里没有任何已启用中转站时返回 `503` 与 `{"status":"not_ready","reason":"..."}`——一个能接受连接却答不了任何请求的实例，对负载均衡器来说不是"就绪"。 |

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
   - 默认鉴权：`Authorization: Bearer <api_key>`；路径 `/v1/responses`。
5. **`azure`**（Azure OpenAI）：
   - 报文与 `openai` **完全同形**，因此同协议直通、零解析；差别只在路径与鉴权：模型名是路径里的 **deployment**，必须带 `api-version` 查询参数，密钥走 `api-key` 请求头（Azure 拒绝 bearer）。
   - 路径：`/openai/deployments/{model}/chat/completions?api-version={api_version}`，`api_version` 留空时用 `2024-10-21`。
   - `base_url` 填资源根地址（如 `https://my-resource.openai.azure.com`）。
6. **`vertex`**（Vertex AI）：
   - 报文与 `gemini` 同形（同一套 generateContent JSON），因此与 Gemini 共享请求/响应适配。
   - 路径：`/{api_version}/projects/{project}/locations/{region}/publishers/google/models/{model}:generateContent`（流式为 `:streamGenerateContent?alt=sse`）。
   - 鉴权：用 `credentials_file` 指向的 service-account JSON 私钥签发 RS256 断言，向 `token_uri` 换取 OAuth2 访问令牌，再以 `Authorization: Bearer` 发出。令牌会被缓存到接近过期才续签——每次请求都换一次令牌等于给每个请求加一次往返。
   - 必填：`project`、`credentials_file`；`region` 留空时用 `us-central1`。
7. **`bedrock`**（AWS Bedrock Converse API）：
   - **自己的报文格式**，不是 OpenAI 的：`system` 是独立的顶层列表而非消息，内容是有类型的块（`text` / `image` / `toolUse` / `toolResult`），采样参数收在 `inferenceConfig` 下。
   - 路径：`/model/{model}/converse`（流式为 `/model/{model}/converse-stream`）。
   - 鉴权：**SigV4**，签名覆盖方法、路径、被签头集合与请求体的 SHA-256。必填 `region`、`aws_access_key`、`aws_secret_key`；临时凭据再填 `aws_session_token`。三者都支持 `${VAR}` 引用，且只在签名瞬间解析。
   - 流式不是 SSE：Bedrock 用二进制 event-stream 分帧（长度前缀 + 双向 CRC32），literouter 校验校验和并转成标准 OpenAI SSE；校验和不符的分帧会被拒绝而不是"按看起来像长度的字节重新对齐"。
   - 模型目录在另一个主机、另一套签名服务上（控制面 `bedrock` 而非数据面 `bedrock-runtime`），因此一键探测只能报告可达性，无法列出模型。
8. **`ollama`**（Ollama 原生 `/api/chat`）：
   - 自己的报文：采样参数在 `options` 下且名字不同（`num_predict` 而不是 `max_tokens`），没有 `developer` 角色，图片是裸 base64 字符串而不是 data URL。
   - 流式是**按行分隔的 JSON**（NDJSON），不是 SSE——每行一个完整对象，没有 `data:` 前缀也没有空行终止符。
   - 默认无鉴权（本地 `http://127.0.0.1:11434`）；模型列表在 `/api/tags`。

### 端点路径自动推导规则

若中转站配置中的 `chat_path` 为空，系统依据 `protocol` 自动推导对话端点路径：

| 协议 (`protocol`) | 默认对话补全路径 (`chat_path`) |
|---|---|
| `openai` | `/chat/completions`（若 `base_url` 不含 `/v1`，请求时会自动组装） |
| `anthropic` | `/v1/messages` |
| `gemini` | `/v1beta/models/{model}:generateContent` (流式为 `:streamGenerateContent?alt=sse`) |
| `openai_responses` | `/v1/responses` |
| `azure` | `/openai/deployments/{model}/chat/completions?api-version={api_version}` |
| `vertex` | `/{api_version}/projects/{project}/locations/{region}/publishers/google/models/{model}:generateContent` |
| `bedrock` | `/model/{model}/converse` (流式为 `/model/{model}/converse-stream`) |
| `ollama` | `/api/chat`（模型列表 `/api/tags`） |

---

## 跨协议适配与零开销直通 (Zero-overhead Passthrough)

### 同协议零开销直通 (Fast Path)

当**客户端请求的协议**与**选中的上游中转站协议**完全一致时（例如：客户端发送 OpenAI `/v1/chat/completions`，上游提供商 `protocol` 亦为 `openai`）：

- **零解析开销**：入站请求体无需经过完整的 JSON DOM 树构建，直接流向目标上游套接字（仅在需要重命名模型时做轻量替换）；
- **零缓冲透传**：SSE 数据块到达后即刻推入客户端套接字，内存无额外拼装拷贝，延迟达到硬件和网络极限。

请求默认透传端到端业务头，包括 `User-Agent`、`originator`、请求/会话标识、追踪头及未知自定义头，无需登记请求头或增加配置。入站同名头重复时采用第一个值，但 `anthropic-beta` 和 `openai-beta` 的列表值会以逗号合并。`providers[].headers` 中的显式配置优先，覆盖时不区分大小写，也会覆盖已合并的 beta 列表。缓冲和流式请求遵循同一规则。

以下属于本地连接的入站头不会直接转发：

- 本地凭据（`Authorization`、`x-api-key`、`api-key`、`x-goog-api-key`）、Cookie、代理转发地址及 AWS 签名字段。上游鉴权使用选中中转站的配置。
- `Host`、逐跳请求头，以及入站 `Connection` 头中列出的全部字段；字段名匹配不区分大小写。
- `Accept`、`Accept-Encoding`、请求体的类型/长度/编码、摘要及 `Expect`。出站请求会按目标协议与实际报文重新生成传输信息。

已知 OpenAI、Anthropic 协商及账号选择头仅在出口属于对应 API 家族时保留；其他自定义业务头在协议转换时仍会透传。客户端 API 的 CORS 预检接受客户端声明的自定义请求头；管理 API 和控制台仍保持同源边界。

开启响应缓存后，缓存键也包含经中转站配置覆盖后的实际转发请求头。同一 JSON 携带不同业务头或 `User-Agent` 时不会串用缓存回答；仅请求头大小写或排列顺序不同则不会产生额外缓存版本。

流式聊天/协议请求会在计算尝试次数前跳过 `supports_stream: false` 的中转站，因此即使 `max_attempts: 1`，支持流式的备用站仍可接手。若所有候选均不支持流式，返回 HTTP 400，错误码为 `unsupported_stream`；非流式请求仍可使用这些站点。二进制媒体接口保持自身的响应处理方式。

对于较大的提示词，网关直接发送请求体，不自动启用 `Expect: 100-continue`。部分中转站无法正确完成这一握手，会出现小请求成功、大请求约一秒后报读取失败的现象。HTTP 429 则仍按上游限流处理；两个中转站条目若使用同一账号，可能共享并发限制。

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
> 2. **流式反向（OpenAI → Anthropic）会合成 `thinking` 块**，但只在**响应方向**：上游的推理增量先开一个 `content_block_start`（`type: thinking`，索引 0），推理结束后关闭该块，再以索引 1 开启文本块。块是**惰性开启**的——先推理就先开 thinking 块，直接出正文就只有文本块，两者都不会让一个块悬着不关。流被中途截断（没有 `finish_reason`）时，`finish()` 关闭的是**实际打开的那个索引**，而不是写死的 0。之所以这里可以合成而请求方向不行，是因为这一侧的块由 literouter 自己生成、没有上游签名需要复现。

---

## 内部管理端点 (Admin API)

管理端点统一以 `/__literouter` 为路径前缀，供 CLI、Web 控制台或自动化运维脚本调用。当配置了 `server.api_key` 时，管理接口同样要求携带 Bearer 鉴权头。

管理接口与 Web 控制台**不会发送 CORS 头**，只有面向客户端的端点保留 `Access-Control-Allow-Origin: *`——请求日志可能包含用户的提示词，允许任意网页跨域读取等于把它们交出去。所有管理响应一律带 `Cache-Control: no-store`。

### 1. 获取系统状态快照

- **请求**：`GET /__literouter/status`
- **响应**：
  ```json
{
  "active_requests": 0,
  "base_url": "http://127.0.0.1:8787",
  "breakers_open": 0,
  "bytes_out": 12345678,
  "cache_enabled": false,
  "cache_entries": 0,
  "cache_hits": 0,
  "cache_misses": 0,
  "config_path": "/home/you/.config/literouter/config.json",
  "cost_usd": 0.075,
  "health": [
    {
      "provider": "openai-official",
      "state": "healthy",
      "consecutive_failures": 0,
      "total_failures": 2,
      "last_error": "",
      "cooldown_remaining": 0.0
    }
  ],
  "host": "127.0.0.1",
  "hourly": [
    {
      "hour_unix": 1758000000.0,
      "requests": 12,
      "successes": 11,
      "failures": 1,
      "bytes_out": 184320,
      "tokens_prompt": 9000,
      "tokens_completion": 3200,
      "cost_usd": 0.075,
      "bucket_sec": 3600
    }
  ],
  "latency_ms_avg": 345.2,
  "log_seq": 1420,
  "port": 8787,
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
  "running": true,
  "started_unix": 1758000000.0,
  "tokens_completion": 45000,
  "tokens_prompt": 120000,
  "total_failure": 10,
  "total_requests": 1420,
  "total_success": 1410,
  "traffic_bucket_count": 24,
  "traffic_bucket_sec": 3600,
  "uptime_sec": 3600.5,
  "version": "0.3.0"
}
```

  `hourly` 是最近 24 个**小时桶**（`hour_unix` 为该小时起点，UTC 整点），旧到新排列：控制台的趋势图就是它，重启后会从遥测文件恢复，因此"今天的样子"不会因为重启而消失。没有流量时该数组为空。

  `providers` 是累计统计（自上次 `POST /__literouter/reset-stats` 或遥测文件恢复起），其中 `latency_ms_p95` 是最近 64 次尝试的最近秩（nearest-rank）p95——取窗口内实际出现过的样本值，没有样本时为 0；`health` 是熔断器当前状态（`state` 取 `unknown` / `healthy` / `degraded` / `open`）。`uptime_sec` 每次请求实时计算，因此 Web 控制台的运行时长会逐秒跳动；显示格式为 `1h 2m 5s`，且**始终保留秒**（`humanUptime`），而一般的时长显示（如熔断冷却剩余时间）仍使用会向上归整到分钟的 `humanDuration`。

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
- **指标**：`literouter_build_info`、`literouter_running`、`literouter_uptime_seconds`、`literouter_requests_total`/`successes_total`/`failures_total`、`literouter_active_requests`、`literouter_breakers_open`、`literouter_log_entries_total`、`literouter_bytes_out_total`、`literouter_tokens_*_total`、`literouter_latency_ms_avg`、`literouter_cost_usd_total`；
- **逐中转站**：`literouter_relay_*{relay="<id>"}`（请求/成功/失败/客户端中断/吸收重试/进出字节/Token/`latency_ms_last|avg|p95`/`last_used_unixtime`/`healthy`/`cooldown_seconds`/`cost_usd_total`）；
- **应答缓存**：`literouter_cache_enabled`、`literouter_cache_hits_total`、`literouter_cache_misses_total`、`literouter_cache_entries`——「已开启但零命中」与「已关闭」从配置上看是一样的，这四个指标是区分它们的方式。
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
- **脱敏规则**：`api_key` 是 `${VAR}` 引用时原样返回（它只是变量名，不是密钥）；带默认凭据的 `${VAR:-fallback}` 和字面量密钥均隐藏。字面量密钥替换为空字符串，并以 `"api_key_source": "literal"` 标记。配置 GET 不返回已保存的明文密钥。

### 9. 更新运行配置与持久化

- **请求**：`PUT /__literouter/config`
- **请求体**：`{ "config": <AppConfig> }` 或裸 `<AppConfig>` 对象
- **校验与原子性**：先执行 JSON 反序列化，再执行语义校验（`validate()`）。若存在任何 `error` 级别错误，返回 **422 Unprocessable Entity**，且不改动内存和磁盘配置。
- **密钥保留规则**：
  - 服务器或中转站的 `api_key` 为空字符串：自动保留已保存的密钥（支持将脱敏 GET 的结果安全原样回传）。中转站按 ID 匹配；
  - `api_key_clear: true`：显式清空该密钥，仍需通过配置校验；
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
- **鉴权**：静态外壳不含任何数据，因此无需密钥即可加载；控制台数据需要 `server.api_key`。需要鉴权时，浏览器首次访问会弹出密钥输入框，密钥只保存在本机 localStorage。
- **打包方式**：前端构建产物（`web/dist/` 下的 `index.html`、`app.js`、`app.css`、`favicon.svg`）通过 C++23 `#embed` 编译进二进制，服务器上只拷贝一个 `literouter` 即可。若编译器不支持 `#embed`（例如 ISO 严格模式下的 GCC），改用环境变量 `LITEROUTER_WEB_DIR` 指向包含这四个文件的构建产物目录（如 `web/dist`）。
- **能力**：实时指标磁贴（请求/成功率/Token 速率等）、中转站健康矩阵与一键探测、路由候选链排序与编辑、可视化全局配置编辑与安全回传保存、一键复制 Continue / Cursor 接入配置、增量日志流过滤（按级别/类型/关键字过滤、暂停/清空）、暗亮主题切换与中英双语国际化。
- **安全性**：与控制台同源，不向跨域请求开放管理端点；密钥仅用于浏览器到本机服务的同源请求，且字面量密钥绝不出网。
