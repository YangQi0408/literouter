# Protocols and API Reference

This document covers `literouter`'s inbound client endpoints, supported upstream provider protocols, automatic cross-protocol translation, zero-overhead passthrough mechanism, and administrative management APIs.

---

## Table of Contents

- [Overview](#overview)
- [Client Inbound Endpoints](#client-inbound-endpoints)
  - [OpenAI Chat Completions](#openai-chat-completions)
  - [Anthropic Claude Messages API](#anthropic-claude-messages-api)
  - [Google Gemini GenerateContent](#google-gemini-generatecontent)
  - [OpenAI Responses API](#openai-responses-api)
  - [Embeddings & Legacy Completions](#embeddings--legacy-completions)
  - [Model Discovery & Health Check](#model-discovery--health-check)
- [Upstream Provider Protocols](#upstream-provider-protocols)
  - [Supported Protocols](#supported-protocols)
  - [Automatic Path Deduction](#automatic-path-deduction)
- [Cross-Protocol Adapters & Zero-Overhead Passthrough](#cross-protocol-adapters--zero-overhead-passthrough)
  - [Same-Protocol Fast Path](#same-protocol-fast-path)
  - [Cross-Protocol Bidirectional Translation](#cross-protocol-bidirectional-translation)
- [Admin API (`/__literouter`)](#admin-api-__literouter)
- [Built-in Web Console](#built-in-web-console)

---

## Overview

`literouter` acts as a multi-protocol aggregation gateway. Whether your client uses the OpenAI Python SDK, Claude Code / Cline, or Google GenAI SDK, you can point them directly to `http://127.0.0.1:8787`. `literouter` handles authentication, protocol bridging, model mapping, and failover seamlessly.

---

## Client Inbound Endpoints

All endpoints support client Bearer token authentication if `server.api_key` is set:
```http
Authorization: Bearer <your-server-api-key>
```

### OpenAI Chat Completions

- **Endpoints**:
  - `POST /v1/chat/completions`
  - `POST /chat/completions` (supports lightweight clients that omit `/v1`)
- **Streaming**: Supports standard `text/event-stream` SSE when `"stream": true`.

**Example**:
```bash
curl http://127.0.0.1:8787/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gpt-4o",
    "messages": [
      {"role": "user", "content": "Hello!"}
    ],
    "stream": true
  }'
```

### Anthropic Claude Messages API

- **Endpoints**:
  - `POST /v1/messages`
  - `POST /messages`
- **Notes**: Accepts `x-api-key` or `Authorization: Bearer` headers, and `anthropic-version`.

**Example**:
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

### Google Gemini GenerateContent

- **Endpoints**:
  - Non-streaming: `POST /v1beta/models/{model}:generateContent`, `POST /v1/models/{model}:generateContent`
  - Streaming: `POST /v1beta/models/{model}:streamGenerateContent`, `POST /v1/models/{model}:streamGenerateContent`

### OpenAI Responses API

- **Endpoints**:
  - `POST /v1/responses`
  - `POST /responses`

### Embeddings & Legacy Completions

- **Embeddings**: `POST /v1/embeddings`
- **Legacy Completions**: `POST /v1/completions`

### Model Discovery & Health Check

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/v1/models`, `/models`, `/v1beta/models` | Lists all logical models aggregated by literouter, with routing topology metadata |
| `GET` | `/v1/models/{id}`, `/models/{id}` | Inspect a single model's candidate providers and health status |
| `GET` | `/health` | Unauthenticated liveness probe, returns `{"status":"ok"}` |

---

## Upstream Provider Protocols

Configure `providers[].protocol` to specify how to talk to each upstream service:

### Supported Protocols

1. **`openai` (Default)**:
   - For: OpenAI official, DeepSeek, Moonshot (Kimi), SiliconFlow, Groq, Ollama, vLLM, and standard third-party relays.
   - Default header: `Authorization: Bearer <api_key>`.
2. **`anthropic`**:
   - For: Anthropic official (`api.anthropic.com`) and native Claude Messages API providers.
   - Automatically injects: `x-api-key: <api_key>` and `anthropic-version: 2023-06-01`.
3. **`gemini`**:
   - For: Google AI Studio (`generativelanguage.googleapis.com`).
   - Automatically injects: `x-goog-api-key: <api_key>`.
4. **`openai_responses`**:
   - For: Upstream endpoints implementing the OpenAI Responses protocol.

### Automatic Path Deduction

When `chat_path` is left empty in provider configuration, `literouter` resolves it based on `protocol`:

| Protocol (`protocol`) | Inferred Chat Path (`chat_path`) |
|---|---|
| `openai` | `/chat/completions` |
| `anthropic` | `/v1/messages` |
| `gemini` | `/v1beta/models/{model}:generateContent` (or `:streamGenerateContent?alt=sse`) |
| `openai_responses` | `/v1/responses` |

---

## Cross-Protocol Adapters & Zero-Overhead Passthrough

### Same-Protocol Fast Path

When the **client request protocol** matches the **upstream provider protocol** (e.g. OpenAI Chat Completions in and `openai` provider out):

- **Zero JSON DOM Overhead**: The payload streams directly without constructing an internal JSON tree (unless model renaming is required);
- **Zero Copy SSE Forwarding**: Upstream SSE data blocks are streamed directly into the client socket with zero intermediate allocation, reaching theoretical network limits.

### Cross-Protocol Bidirectional Translation

When the incoming protocol differs from the upstream target, `literouter`'s translation layer (`core/src/lr_protocol.cpp`) adapts requests and streams on the fly:

```text
Client (e.g. OpenAI SDK)             literouter                Upstream (e.g. Anthropic)
    /v1/chat/completions                 │                           /v1/messages
             │                           │                                │
             │─── OpenAI Request JSON ──▶│                                │
             │                           │── Adapt messages/system/tools ▶│
             │                           │── Inject anthropic-version ───▶│
             │                           │                                │
             │                           │◀── Anthropic SSE Stream Chunk ─│
             │                           │── Transform to OpenAI SSE ────│
             │◀── data: {delta:...} ─────│                                │
```

Key features supported across transformations:
- System & developer prompts, user/assistant role mapping, and tool calls / tool responses;
- Parameter harmonization (`max_tokens`, `temperature`, `top_p`, `top_k`, `stop`);
- SSE streaming chunk adaptation with aligned stop events;
- Token `usage` statistics normalization.

---

## Admin API (`/__literouter`)

Administrative endpoints are prefixed with `/__literouter`. If `server.api_key` is set, Bearer authorization is required.

Neither the admin API nor the web console sends CORS headers; only the client-facing endpoints keep `Access-Control-Allow-Origin: *`. The request log can hold prompts, and letting any page read it cross-origin would hand those prompts out. Every admin response carries `Cache-Control: no-store`.

### 1. Snapshot Status

- **Request**: `GET /__literouter/status`
- **Response**: JSON snapshot containing uptime, total/success/active requests, circuit tripped counts, and per-provider telemetry.

### 2. Incremental Request Logs

- **Request**: `GET /__literouter/logs?since=<id>&limit=<n>`
- **Parameters**:
  - `since`: Retrieve logs strictly newer than sequence ID `id`;
  - `limit`: Maximum number of entries (default 100).

### 3. Hot Reload Configuration

- **Request**: `POST /__literouter/reload`
- **Response**: `{"ok": true, "message": "reloaded successfully"}`

### 4. Reset Telemetry & Circuit Breakers

- **Request**: `POST /__literouter/reset-stats`
- **Action**: Clears request counters, resets latencies, and forces all circuit breakers back to `Healthy`.

### 5. Probe Provider & Scan Models

- **Request**: `POST /__literouter/probe`
- **Body** (either form):
  - `{"provider": "<id>"}` — probes a **configured** relay; the secret is resolved server-side (`resolveSecret`) and never travels over the wire;
  - a provider object (`base_url`, `api_key`, `protocol`, `headers`, `timeout_sec`) — probes an unsaved entry, which is what a form wants.
- **Action**: Runs a live connectivity probe and fetches available model names from upstream.

### 6. Graceful Shutdown

- **Request**: `POST /__literouter/shutdown`
- **Action**: Gracefully drains connections and terminates the proxy process.

### 7. Read the running config (redacted)

- **Request**: `GET /__literouter/config`
- **Response**:
  ```json
  {
    "path": "/home/me/.config/literouter/config.json",
    "exists": true,
    "config": { "server": { "...": "..." }, "providers": [ "..." ], "routes": [ "..." ] },
    "validation": { "ok": false, "summary": "0 errors, 1 warning", "issues": [ "..." ] }
  }
  ```
- **Redaction**: an `api_key` that is a `${VAR}` reference is returned as written — it names a variable, not a key. A literal key is replaced by an empty string and marked `"api_key_source": "literal"`. **A real secret never crosses the network.**

---

## Built-in Web Console

While `literouter serve` runs, the same port carries a web console that needs **no external assets**, for headless servers and containers.

| Path | Purpose |
|---|---|
| `GET /` | 302 to `/ui/` |
| `GET /ui/` | the console (overview / logs / config) |
| `GET /ui/app.js`, `/ui/app.css`, `/ui/favicon.svg` | static assets |
| `GET /favicon.ico` | 302 to `/ui/favicon.svg` |

- **Auth**: the shell holds no data, so it loads without a key; every `/__literouter/*` call the page then makes is protected by `server.api_key`, exactly like `/v1/*`. The first visit opens a key prompt and the key stays in that browser's localStorage.
- **Packaging**: the four assets are compiled into the binary with C++23 `#embed`, so a server needs nothing but `literouter`. Where `#embed` is unavailable (an ISO-strict GCC, for instance), set `LITEROUTER_WEB_DIR` to a directory holding the same four files.
- **What it does**: live metric tiles, a per-relay health matrix with one-click probing, an incremental request log (filter by level, kind or text), a read-only config view with validation results, config reload, stats reset, shutdown.
- **Security**: same-origin only, no cross-origin access, and the key is used solely for browser-to-localhost calls.
