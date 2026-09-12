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

---

## 内部管理端点 (Admin API)

管理端点统一以 `/__literouter` 为路径前缀，供 CLI、GUI 或自动化运维脚本调用。当配置了 `server.api_key` 时，管理接口同样要求携带 Bearer 鉴权头。

### 1. 获取系统状态快照

- **请求**：`GET /__literouter/status`
- **响应**：
  ```json
  {
    "version": "1.0.0",
    "uptime_seconds": 3600,
    "active_requests": 2,
    "total_requests": 1420,
    "success_requests": 1410,
    "circuit_tripped_count": 1,
    "providers": [
      {
        "id": "openai-official",
        "healthy": true,
        "circuit_state": "Healthy",
        "consecutive_failures": 0,
        "total_requests": 1200,
        "success_requests": 1198,
        "avg_latency_ms": 345.2
      }
    ]
  }
  ```

### 2. 获取增量请求日志

- **请求**：`GET /__literouter/logs?since=<id>&limit=<n>`
- **参数**：
  - `since`：拉取日志 ID 大于此值的增量记录；
  - `limit`：最多返回条数（默认 100）。
- **响应**：包含请求摘要、方法、目标模型、上游站点、HTTP 状态码、耗时（毫秒）、Token 统计等。

### 3. 配置热重载

- **请求**：`POST /__literouter/reload`
- **响应**：`{"ok": true, "message": "reloaded successfully"}`

### 4. 重置遥测统计与熔断器

- **请求**：`POST /__literouter/reset-stats`
- **作用**：清空所有中转站的成功/失败计数、延迟统计，并将所有熔断器强制恢复为 Healthy。

### 5. 存活探测与模型扫描

- **请求**：`POST /__literouter/probe`
- **请求体**：中转站配置 JSON（包含 `base_url`, `api_key`, `protocol`）
- **作用**：即时向该中转站发起探测，测试网络连通性，并自动拉取上游所支持的模型列表。

### 6. 安全关闭服务

- **请求**：`POST /__literouter/shutdown`
- **作用**：通知正在运行的后台服务安全释放资源并优雅退出进程。
