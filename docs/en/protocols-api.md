# Protocols and API Reference

Every endpoint also supports HTTPS on the same listener: configure `server.tls_cert_file` and `server.tls_key_file`, restart, and use `https://` in the URLs below. Management clients verify the chain and hostname; private CAs use `LITEROUTER_CA_BUNDLE`. See [configuration](configuration.md#https-listener).

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
  - [Audio and Images](#audio-and-images)
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

Model endpoints accept `server.api_key`. Authentication is disabled only while that key is empty:
```http
Authorization: Bearer <your-api-key>
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

### Audio and Images

| Endpoint (`POST`) | Request | Response |
|---|---|---|
| `/v1/audio/transcriptions` | `multipart/form-data` with `file` and `model` | JSON, text/subtitle formats, or upstream SSE with `stream=true` |
| `/v1/audio/translations` | `multipart/form-data` with `file` and `model` | Upstream JSON or text/subtitle format |
| `/v1/audio/speech` | JSON with `model`, `input`, `voice`, and optional `response_format` | Audio bytes or upstream SSE; delivered as they arrive |
| `/v1/images/generations` | JSON with `prompt` and model-specific options | JSON or upstream SSE with `stream=true` |
| `/v1/images/edits` | Multipart images/mask, or upstream-compatible JSON | JSON or upstream SSE with `stream=true` |
| `/v1/images/variations` | Multipart image and options | Upstream JSON |

These endpoints require an **`openai` provider** that implements the requested endpoint. Other provider protocols are excluded before the attempt limit; an otherwise routed model with no compatible provider returns `400` with code `unsupported_media_protocol`. There is no audio/image conversion into Chat, Anthropic, Gemini, or Responses requests. Provider-specific field validation and model support remain with that provider.

`base_url` is the API prefix: `https://api.openai.com/v1` plus `/audio/speech`, for example. `chat_path` and `embeddings_path` do not change media paths. The `model` field uses the existing route/model mapping. Image requests that omit it route the OpenAI default `dall-e-2`; audio requests require a model. Multipart fields, repeated fields (`image[]`, timestamp options), filenames, MIME part headers, and binary contents are preserved in order; the MIME boundary is regenerated and only the model field is renamed. Uploads are buffered for replay, bounded by the gateway's **64 MiB request-body limit**; upstream limits may be lower.

Media requests share authentication, routing policy, failover, circuit breakers, request logs, and usage counters with text requests. Non-streaming responses are buffered before commitment. Speech and explicit streaming requests use the response-header gate: connection failures, `429`, and retryable `5xx` may switch providers before a response is committed. After successful headers are committed, a truncated upstream audio/SSE response terminates the client connection, records a failure, and never switches providers. Binary response bytes and the upstream `Content-Type` are preserved; gateway-generated errors remain OpenAI-style JSON. A retry after an ambiguous transport failure may repeat an upstream operation or charge, so use `max_attempts: 1` when replay is unacceptable.

Usage reflects token counts actually reported in JSON/SSE. Audio duration charges and image-per-item charges are not estimated by the token price fields. Binary uploads and media responses are omitted from body logs; multipart logs retain only model/stream and part metadata (names, filenames, MIME types, byte counts). The web console's log filters include audio and image.

```bash
curl http://127.0.0.1:8787/v1/audio/transcriptions \
  -H "Authorization: Bearer $LITEROUTER_KEY" \
  -F model=whisper-1 -F file=@recording.wav

curl http://127.0.0.1:8787/v1/audio/speech \
  -H "Authorization: Bearer $LITEROUTER_KEY" -H "Content-Type: application/json" \
  -d '{"model":"tts-1","input":"Hello","voice":"alloy","response_format":"mp3"}' \
  --output speech.mp3

curl http://127.0.0.1:8787/v1/images/generations \
  -H "Authorization: Bearer $LITEROUTER_KEY" -H "Content-Type: application/json" \
  -d '{"model":"gpt-image-1","prompt":"A watercolor hill","size":"1024x1024"}'
```

### Model Discovery & Health Check

| Method | Endpoint | Description |
|---|---|---|
| `GET` | `/v1/models`, `/models`, `/v1beta/models` | Lists all logical models aggregated by literouter, with routing topology metadata |
| `GET` | `/v1/models/{id}`, `/models/{id}` | Inspect a single model's candidate providers and health status |
| `GET` | `/health`, `/health/live` | Unauthenticated **liveness** probe, returns `{"status":"ok"}`. It answers "is this process alive", so it only looks at the listener. |
| `GET` | `/health/ready` | Unauthenticated **readiness** probe. It answers "should traffic be sent here", so a config with no enabled relay returns `503` with `{"status":"not_ready","reason":"..."}` — an instance that can accept a connection and still cannot answer a single request is not ready for a load balancer. |

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
   - Default header: `Authorization: Bearer <api_key>`; path `/v1/responses`.
5. **`azure`** (Azure OpenAI):
   - The body is **exactly OpenAI's**, so it takes the same-protocol fast path with zero parsing. Only the path and the credential differ: the model name is a **deployment** in the path, an `api-version` query is mandatory, and the key goes in an `api-key` header (Azure rejects a bearer).
   - Path: `/openai/deployments/{model}/chat/completions?api-version={api_version}`; an empty `api_version` uses `2024-10-21`.
   - `base_url` is the resource root, e.g. `https://my-resource.openai.azure.com`.
6. **`vertex`** (Vertex AI):
   - The body is **Gemini's** generateContent JSON, so it shares that request/response adaptation.
   - Path: `/{api_version}/projects/{project}/locations/{region}/publishers/google/models/{model}:generateContent` (or `:streamGenerateContent?alt=sse`).
   - Authentication mints an OAuth2 access token: the private key in the file `credentials_file` names signs an RS256 assertion, which is exchanged at `token_uri` for a token sent as `Authorization: Bearer`. The token is cached until it is nearly expired — exchanging one per request would add a round trip to every call.
   - Required: `project` and `credentials_file`; `region` defaults to `us-central1`.
7. **`bedrock`** (AWS Bedrock Converse API):
   - **Its own body**, not OpenAI's: `system` is a separate top-level list rather than a message, content is a list of typed blocks (`text` / `image` / `toolUse` / `toolResult`), and the sampling knobs live under `inferenceConfig`.
   - Path: `/model/{model}/converse` (streaming: `/model/{model}/converse-stream`).
   - Authentication is **SigV4**, covering the method, the path, the signed header set and the SHA-256 of the body. Required: `region`, `aws_access_key`, `aws_secret_key`; temporary credentials also need `aws_session_token`. All three accept `${VAR}` references, resolved only at signing time.
   - Its stream is not SSE: Bedrock frames a binary event stream (length prefix plus CRC32s at both ends). literouter verifies the checksums and converts to standard OpenAI SSE; a frame that fails its checksum is refused rather than resynchronised on whatever bytes look like a length.
   - The model catalogue lives on a different host behind a different signing service (the `bedrock` control plane, not `bedrock-runtime`), so a probe can report reachability but cannot list models.
8. **`ollama`** (Ollama's native `/api/chat`):
   - Its own body: sampling knobs sit under `options` with different names (`num_predict`, not `max_tokens`), there is no `developer` role, and images are bare base64 strings rather than data URLs.
   - Its stream is **newline-delimited JSON**, not SSE: one complete object per line, with no `data:` prefix and no blank-line terminator.
   - Unauthenticated by default (local `http://127.0.0.1:11434`); the model list is at `/api/tags`.

### Automatic Path Deduction

When `chat_path` is left empty in provider configuration, `literouter` resolves it based on `protocol`:

| Protocol (`protocol`) | Inferred Chat Path (`chat_path`) |
|---|---|
| `openai` | `/chat/completions` |
| `anthropic` | `/v1/messages` |
| `gemini` | `/v1beta/models/{model}:generateContent` (or `:streamGenerateContent?alt=sse`) |
| `openai_responses` | `/v1/responses` |
| `azure` | `/openai/deployments/{model}/chat/completions?api-version={api_version}` |
| `vertex` | `/{api_version}/projects/{project}/locations/{region}/publishers/google/models/{model}:generateContent` |
| `bedrock` | `/model/{model}/converse` (streaming: `/model/{model}/converse-stream`) |
| `ollama` | `/api/chat` (model list at `/api/tags`) |

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
> 2. **The reverse stream (OpenAI → Anthropic) does synthesize a `thinking` block**, in the response direction: an upstream reasoning delta opens a `content_block_start` (`type: thinking`, index 0), and when the text starts that block is closed and a text block opens at index 1. Blocks are opened **lazily** — reasoning first means a thinking block first, text with no reasoning means a single text block — and neither leaves a block unclosed. When a stream is cut short without a `finish_reason`, `finish()` closes the index it actually opened rather than a hard-coded 0. Synthesizing is acceptable here and not in the request direction because these blocks are literouter's own: there is no upstream signature to reproduce.

---

## Admin API (`/__literouter`)

Administrative endpoints are prefixed with `/__literouter`. If `server.api_key` is set, Bearer authorization is required.

Neither the admin API nor the web console sends CORS headers; only the client-facing endpoints keep `Access-Control-Allow-Origin: *`. The request log can hold prompts, and letting any page read it cross-origin would hand those prompts out. Every admin response carries `Cache-Control: no-store`.

### 1. Snapshot Status

- **Request**: `GET /__literouter/status`
- **Response**:
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
  "version": "0.2.0"
}
```

  `hourly` holds the last 24 **hour buckets** (`hour_unix` is the start of the hour, on the UTC hour), oldest first: it is what the console's trend chart draws, and it is restored from the telemetry file on restart so the shape of the day does not vanish with the process. The array is empty until there has been traffic.

  `providers` holds cumulative stats (since the last `POST /__literouter/reset-stats` or the restored telemetry file) — `latency_ms_p95` being the nearest-rank p95 over the last 64 attempts, always a sample the relay really served, and 0 when the window is empty — and `health` the breaker state, whose `state` is one of `unknown` / `healthy` / `degraded` / `open`. `uptime_sec` is computed per request, which is what lets the web console tick the uptime once a second; it is formatted as `1h 2m 5s` and always keeps the seconds (`humanUptime`), while plain durations — a breaker's remaining cooldown, for instance — still use the minute-rounding `humanDuration`.

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
- **Metrics**: `literouter_build_info`, `literouter_running`, `literouter_uptime_seconds`, `literouter_requests_total` / `successes_total` / `failures_total`, `literouter_active_requests`, `literouter_breakers_open`, `literouter_log_entries_total`, `literouter_bytes_out_total`, `literouter_tokens_*_total`, `literouter_latency_ms_avg`, `literouter_cost_usd_total`;
- **Per relay**: `literouter_relay_*{relay="<id>"}` (requests / successes / failures / client-aborted / absorbed retries / bytes in and out / tokens / `latency_ms_last|avg|p95` / `last_used_unixtime` / `healthy` / `cooldown_seconds` / `cost_usd_total`);
- **Response cache**: `literouter_cache_enabled`, `literouter_cache_hits_total`, `literouter_cache_misses_total`, `literouter_cache_entries`. "Enabled with no hits" and "disabled" look identical in the config; these four are how they are told apart.
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
- **Redaction**: an `api_key` that is a `${VAR}` reference is returned as written — it names a variable, not a key. References containing fallback credentials, such as `${VAR:-fallback}`, are hidden too. A literal key is replaced by an empty string and marked `"api_key_source": "literal"`. Config GET never returns stored plaintext keys.

### 9. Update & Persist Running Config

- **Request**: `PUT /__literouter/config`
- **Request Body**: `{ "config": <AppConfig> }` or bare `<AppConfig>` object
- **Validation & Atomicity**: Deserializes JSON, then runs semantic validation (`validate()`). If any errors occur, returns **422 Unprocessable Entity** and touches neither memory nor disk config.
- **Secret Preservation Rules**:
  - An empty server or provider `api_key` preserves its stored value (safe round-trip from the redacted GET response). Providers match by ID;
  - `api_key_clear: true`: explicitly clears that secret, subject to validation;
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
- **Auth**: The shell holds no data, so it loads without a key. Console data requires `server.api_key`. The first authenticated visit opens a key prompt and the key stays in that browser's localStorage.
- **Packaging**: Frontend build artifacts (under `web/dist/`) are compiled into the binary with C++23 `#embed`, so a server needs nothing but `literouter`. Where `#embed` is unavailable (an ISO-strict GCC, for instance), set `LITEROUTER_WEB_DIR` to a directory holding the same four files (e.g. `web/dist`).
- **Capabilities**: Live metric tiles (requests, success rate, token rates), relay health matrix with one-click probing, route candidate ordering and editing, full visual configuration editing and safe round-trip persistence, copy-ready Continue / Cursor snippets, incremental request log streaming (filter by level, kind, or keyword, pause/clear), dark/light theme toggle, and English/Chinese i18n.
- **Security**: Same-origin only, admin endpoints never advertise CORS headers; secrets are used solely for browser-to-localhost requests and literal keys never leave the server.
