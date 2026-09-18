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

> **Upstream connection reuse**: both legs draw their connections from a per-worker-thread pool (one per relay, keyed by the root, the two timeouts and the CA bundle), so consecutive requests do not pay for a TCP and TLS handshake over and over — the streamed leg used to build a fresh connection per attempt, which put the handshake cost on the leg carrying nearly all the traffic. A transfer that ends cleanly leaves its connection in the pool; an aborted or transport-failed transfer retires it, so the next request is only ever handed a clean socket. httplib probes a socket before reusing it (including a TLS peer-closed check), so a relay dropping an idle connection costs one reconnect rather than a failed request.

> **Token counts in streams**: to account for usage, every chunk that passes is substring-matched — and a chunk that ends on an event boundary without mentioning `usage` is not copied and not parsed at all (the fast path). Only a chunk that mentions `usage` / `usageMetadata` is parsed as JSON (for OpenAI that is usually the final chunk alone), and only an event split across two reads enters the bounded carry buffer. So both the "zero JSON overhead" and the "zero intermediate allocation" above still hold in substance. Counts merge by taking the **largest** seen: Anthropic reports input and output separately in `message_start` and `message_delta`, Gemini reports cumulatively on every chunk, and OpenAI reports once at the tail — but only when the client asked for `stream_options.include_usage`, which `literouter` deliberately does not inject on the client's behalf. A stream that never reports usage counts as zero; nothing is estimated.

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

> **Reasoning / thinking content** is preserved across a conversion instead of dropped. An Anthropic upstream's `thinking` blocks become `reasoning_content` on the OpenAI side (`redacted_thinking` is left alone — its payload is encrypted), a Gemini upstream's `thought: true` parts go to `reasoning_content` rather than into the answer (which is where they used to end up), and a Responses upstream's `reasoning` items do too (read from `summary[].text`, with the `content[].text` shape that unencrypted relays use also accepted). Going the other way, `reasoning_content` / `reasoning` becomes a leading Anthropic `thinking` block, a leading Responses `reasoning` item, or a Gemini part with `thought: true`. In a stream, both an Anthropic `thinking_delta` and a Responses `response.reasoning_summary_text.delta` are relayed as `reasoning_content` deltas.
>
> **Two deliberate asymmetries**, neither of them an oversight:
> 1. **The request direction does not synthesize a `thinking` block.** Anthropic only accepts a thinking block back with the signature the provider issued for it, and Gemini only accepts a thinking part back with its `thoughtSignature`; a fabricated one turns a request that would have succeeded into a 400 — so a client's `reasoning_content` is dropped on those two paths. Going the other way (Anthropic / Gemini / Responses → Chat) the client's reasoning is carried over as `reasoning_content`.
> 2. **The reverse stream (OpenAI → Anthropic) does not synthesize a thinking block yet.** That needs a second content block with its own index and start/stop frames, and a block sequence that is wrong is worse for a strict client than thinking content that is simply absent.

---

## Admin API (`/__literouter`)

Administrative endpoints are prefixed with `/__literouter`. If `server.api_key` is set, Bearer authorization is required.

Neither the admin API nor the web console sends CORS headers; only the client-facing endpoints keep `Access-Control-Allow-Origin: *`. The request log can hold prompts, and letting any page read it cross-origin would hand those prompts out. Every admin response carries `Cache-Control: no-store`.

### 1. Snapshot Status

- **Request**: `GET /__literouter/status`
- **Response**:
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

  `providers` holds cumulative stats (since the last `POST /__literouter/reset-stats` or the restored telemetry file) — `latency_ms_p95` being the nearest-rank p95 over the last 64 attempts, always a sample the relay really served, and 0 when the window is empty — and `health` the breaker state, whose `state` is one of `unknown` / `healthy` / `degraded` / `open`. `uptime_sec` is computed per request, which is what lets the web console and the GUI tick the uptime once a second; it is formatted as `1h 2m 5s` and always keeps the seconds (`humanUptime`), while plain durations — a breaker's remaining cooldown, for instance — still use the minute-rounding `humanDuration`.

### 2. Incremental Request Logs

- **Request**: `GET /__literouter/logs?since=<id>&limit=<n>`
- **Parameters**:
  - `since`: Retrieve logs strictly newer than sequence ID `id`;
  - `limit`: Maximum number of entries (default 100).

  Each entry splits its latency into **three phases** (`wait_ms` / `ttfb_ms` / `stream_ms`), in the order they are spent:

  | Field | Meaning |
  |---|---|
  | `wait_ms` | From the request arriving to *this relay* being tried — queueing plus whatever earlier candidates cost, which is what makes the price of a failover chain visible |
  | `ttfb_ms` | From the attempt starting to the first response byte: the relay's own thinking time. A buffered answer cannot be split further (httplib reports one number for connect-plus-answer), so there it is the whole call |
  | `stream_ms` | From the first byte to the last: the streaming phase, always 0 for a buffered answer |

  A streamed request is therefore the one case where "how long it took to start answering" and "how long the answer took" are separable — the two numbers that matter when comparing relays.

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

### 6. Prometheus Metrics

- **Request**: `GET /__literouter/metrics`
- **Response**: Prometheus text format (`text/plain; version=0.0.4`), carrying the *same* numbers as `/__literouter/status` — the console is for a person, this is for a graph, and both read one snapshot.
- **Metrics**: `literouter_running`, `literouter_uptime_seconds`, `literouter_requests_total` / `successes_total` / `failures_total`, `literouter_active_requests`, `literouter_breakers_open`, `literouter_bytes_out_total`, `literouter_tokens_*_total`, `literouter_latency_ms_avg`, `literouter_cost_usd_total`, plus per-relay `literouter_relay_*{relay="<id>"}` (requests, successes, failures, client aborts, absorbed retries, bytes in and out, tokens, `latency_ms_last|avg|p95`, `last_used_unixtime`, `healthy`, `cooldown_seconds`).
- **Auth**: the same `server.api_key` gate as every other management endpoint, which a scraper satisfies with `bearer_token` / `authorization`.
- **Example** (`prometheus.yml`):
  ```yaml
  scrape_configs:
    - job_name: literouter
      authorization:
        credentials_file: /etc/literouter/key
      static_configs:
        - targets: ["127.0.0.1:8787"]
      metrics_path: /__literouter/metrics
  ```

### 7. Graceful Shutdown

- **Request**: `POST /__literouter/shutdown`
- **Action**: Gracefully drains connections and terminates the proxy process.

### 8. Read the running config (redacted)

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

### 9. Update & Persist Running Config

- **Request**: `PUT /__literouter/config`
- **Request Body**: `{ "config": <AppConfig> }` or bare `<AppConfig>` object
- **Validation & Atomicity**: Deserializes JSON, then runs semantic validation (`validate()`). If any errors occur, returns **422 Unprocessable Entity** and touches neither memory nor disk config.
- **Secret Preservation Rules**:
  - Provider `api_key` is empty string: preserves the provider's existing secret in memory (safe round-trip from the redacted GET response);
  - `api_key_clear: true`: explicitly clears that provider's secret;
  - Non-empty string: updates to the new value (supports `${VAR}` placeholders).
- **Persistence**: When started with a config path, saves atomically using temporary-file-and-rename and calls `updateConfig()`; when running without a config path, updates memory only and returns `"saved": false`.
- **Response**: `{"ok": true, "saved": true, "path": "...", "summary": "...", "issues": [...]}`

---

## Built-in Web Console

While `literouter serve` runs, the same port carries a modern web console built with **React + Vite + Tailwind + shadcn/ui**, with no external CDN dependencies, ideal for headless servers and container deployments.

| Path | Purpose |
|---|---|
| `GET /` | 302 to `/ui/` (when console is enabled) |
| `GET /ui/` | the console single-page application entry (`index.html`) |
| `GET /ui/app.js`, `/ui/app.css`, `/ui/favicon.svg` | frontend build artifacts |
| `GET /favicon.ico` | 302 to `/ui/favicon.svg` |

- **Toggle Control**: Controlled by `server.web_ui` (boolean, defaults to `true`). Can also be overridden at launch via CLI flags `serve --web-ui` or `serve --no-web-ui`. When disabled, navigating to `/ui/` returns 404 (error code `console_disabled`). Changes via reload or PUT take effect immediately without restarting the process.
- **Auth**: The shell holds no data, so it loads without a key; every `/__literouter/*` call the page then makes is protected by `server.api_key`, exactly like `/v1/*`. The first visit opens a key prompt and the key stays in that browser's localStorage.
- **Packaging**: Frontend build artifacts (under `web/dist/`) are compiled into the binary with C++23 `#embed`, so a server needs nothing but `literouter`. Where `#embed` is unavailable (an ISO-strict GCC, for instance), set `LITEROUTER_WEB_DIR` to a directory holding the same four files (e.g. `web/dist`).
- **Capabilities**: Live metric tiles (requests, success rate, token rates), relay health matrix with one-click probing, route candidate ordering and editing, full visual configuration editing and safe round-trip persistence, incremental request log streaming (filter by level, kind, or keyword, pause/clear), dark/light theme toggle, and English/Chinese i18n.
- **Security**: Same-origin only, admin endpoints never advertise CORS headers; secrets are used solely for browser-to-localhost requests and literal keys never leave the server.
