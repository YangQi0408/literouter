# Configuration and Secrets Management

This document provides a comprehensive guide to the `literouter` configuration file structure, field descriptions, secure secret expansion mechanism, and rule validation engine.

---

## Table of Contents

- [Configuration File Location](#configuration-file-location)
- [Complete Configuration Example](#complete-configuration-example)
- [Top-level Fields](#top-level-fields)
  - [Server Configuration (`server`)](#server-configuration-server)
  - [Provider Configuration (`providers`)](#provider-configuration-providers)
  - [Route Configuration (`routes`)](#route-configuration-routes)
- [Secure Secret Resolution](#secure-secret-resolution)
  - [Supported Syntax Formats](#supported-syntax-formats)
  - [Zero-Leak Invariant](#zero-leak-invariant)
- [Configuration Validation Engine](#configuration-validation-engine)
- [Hot Reload](#hot-reload)

---

## Configuration File Location

`literouter` follows OS platform conventions to resolve and initialize the default configuration directory:

| OS Platform | Default Path | Note |
|---|---|---|
| **Linux** | `~/.config/literouter/config.json` | Follows XDG Base Directory specification (`$XDG_CONFIG_HOME` prioritized) |
| **macOS** | `~/.config/literouter/config.json` | Same as Linux (`$XDG_CONFIG_HOME` honored) |
| **Windows** | `%APPDATA%\literouter\config.json` | e.g. `C:\Users\<User>\AppData\Roaming\literouter\config.json` |

### Explicit Path Overrides

You can override the default configuration path at any time via:

1. **CLI Flag**:
   ```bash
   literouter serve --config /path/to/custom_config.json
   ```
2. **Environment Variable**:
   ```bash
   export LITEROUTER_CONFIG="/path/to/custom_config.json"
   ```

---

## Complete Configuration Example

```jsonc
{
  "schema": 3,

  "server": {
    "host": "127.0.0.1",          // Listen address (binding to non-loopback without api_key triggers a security warning)
    "port": 8787,                 // Port to listen on; 0 lets OS assign an ephemeral free port
    "tls_cert_file": "",         // Absolute PEM certificate chain path; both TLS paths empty keep HTTP
    "tls_key_file": "",          // Absolute unencrypted PEM private key path; restart to apply
    "api_key": "",                // Access key for the model and management endpoints; empty disables authentication
    "pass_through_unknown": true, // Automatically pass through models not listed in `routes` to providers declaring them
    "max_attempts": 0,            // Max candidates to try per request; 0 means try all available candidates
    "routing_policy": "priority", // Chain order: priority (default) / fastest (measured) / cheapest (price)
    "request_deadline_sec": 0,    // Whole-request budget in seconds; 0 disables it
    "session_affinity_sec": 0,    // How long a conversation stays on one relay; 0 disables it
    "circuit_failure_threshold": 3, // Consecutive retriable errors before tripping circuit breaker
    "circuit_cooldown_sec": 30,     // Cooldown duration (seconds) before half-open probe
    "skip_open_circuits": true,   // Deprioritize or skip open circuit providers during route selection
    "log_capacity": 200,          // Capacity of the in-memory ring buffer for request logs
    "log_bodies": false,          // Capture request/response bodies in logs (disabled by default for prompt privacy)
    "log_body_limit": 2048,       // Maximum body bytes recorded when log_bodies is true
    "persist_telemetry": true,    // Persist counters and the request log to the state dir (see "Telemetry Persistence")
    "web_ui": true,               // Serve the built-in web console at /ui (a non-loopback host without api_key warns)
    "reload_on_change": false,    // Apply the config file when it changes on disk (off by default)
    "traffic_bucket_sec": 3600,   // Width of one traffic-trend bucket in seconds; 60 watches the last minutes
    "traffic_bucket_count": 24,   // Buckets kept; 3600 x 24 is "the last day, hourly"
    "response_cache_ttl_sec": 0,  // Local response cache TTL in seconds; 0 disables it
    "response_cache_max_entries": 128, // Cache entries kept; the least recently used is evicted
    "otlp_endpoint": ""           // OpenTelemetry OTLP/HTTP metrics endpoint, e.g. http://127.0.0.1:4318; empty disables
  },

  "providers": [
    {
      "id": "openai-official",                 // Unique ID (used in routing rules and telemetry keys)
      "name": "OpenAI Official",               // Human-friendly label
      "protocol": "openai",                    // Upstream protocol: openai / anthropic / gemini / openai_responses
      "price_in_per_million": 2.5,             // Input price (USD per million tokens); 0 or absent means unpriced
      "price_out_per_million": 10.0,           // Output price (USD per million tokens)
      "base_url": "https://api.openai.com/v1", // Upstream base URL (subpaths preserved)
      "api_key": "${OPENAI_API_KEY}",          // Plaintext key or env placeholder
      "enabled": true,                         // Enable or disable this provider
      "priority": 10,                          // Dispatch priority, lower wins (example value; the field default is 100)
      "weight": 1,                             // Weight tie-breaker among providers with the same priority
      "timeout_sec": 120,                      // Request timeout in seconds
      "connect_timeout_sec": 15,               // TCP / TLS handshake timeout in seconds
      "supports_stream": true,                 // Supports Server-Sent Events (SSE) streaming
      "models": ["gpt-4o", "text-embedding-3-small"], // Models advertised by this provider
      "headers": {                             // Extra HTTP headers attached to every upstream request
        "HTTP-Referer": "https://example.com"
      },
      "chat_path": "/chat/completions",        // Custom chat endpoint path (inferred from protocol if omitted)
      "embeddings_path": "/embeddings",        // Custom embeddings endpoint path
      "note": "Primary upstream provider"
    },
    {
      "id": "anthropic-direct",
      "name": "Anthropic Official",
      "protocol": "anthropic",                 // Native Claude Messages protocol
      "base_url": "https://api.anthropic.com",
      "api_key": "${ANTHROPIC_API_KEY}",
      "enabled": true,
      "priority": 20,
      "models": ["claude-3-5-sonnet-20241022", "claude-3-5-haiku-20241022"],
      "chat_path": "/v1/messages"
    },
    {
      "id": "relay-backup",
      "name": "Third-party Relay Backup",
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
      "model": "gpt-4o",             // Logical model requested by client
      "enabled": true,
      "targets": [                   // Ordered candidate chain: index 0 is primary, followed by fallbacks
        { "provider": "openai-official", "model": "gpt-4o-2024-08-06" }, // Optional upstream physical model mapping
        { "provider": "relay-backup" } // Omitting model preserves client's requested model name
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

## Top-level Fields

### Server Configuration (`server`)

| Field | Type | Default | Description |
|---|---|---|---|
| `host` | `string` | `"127.0.0.1"` | IP address to bind to. Set to `"0.0.0.0"` for LAN access, but ensure `api_key` is configured. |
| `port` | `uint16` | `8787` | Port to bind to. Set to `0` to let OS pick an available ephemeral port. |
| `tls_cert_file` | `string` | `""` | Absolute PEM certificate chain path. Set together with `tls_key_file` for HTTPS; keep both empty for HTTP. Startup validates files, certificate dates, and key matching. Listener changes require restart. |
| `tls_key_file` | `string` | `""` | Absolute path to the matching unencrypted PEM private key. Its contents are never written into the config or returned to the web console. |
| `api_key` | `string` | `""` | Access credential for the model endpoints and the management API, supporting environment references. Authentication is disabled only while it is empty. A non-loopback `host` and the web console both warn when it is empty, because either would otherwise expose the request log. |
| `pass_through_unknown` | `bool` | `true` | If client requests an unrouted model, pass through to providers advertising that model. |
| `max_attempts` | `size_t` | `0` | Upper limit of candidate providers to try per request. `0` means try all candidates. |
| `routing_policy` | `string` | `"priority"` | How the **candidate chain is ordered before the first attempt**: `priority` (the default — the declared `priority`/`weight` order), `fastest` (relays with a measurement first, by p95, because a relay that is usually fast and occasionally terrible should not be tried first), `cheapest` (priced relays first, by input + output price per million; an unpriced relay sorts last because its cost is unknown rather than zero). Ties keep the priority order, and **session affinity still wins over both**: which relay has already seen this conversation is the more specific fact. |
| `request_deadline_sec` | `int` | `0` | Whole-request budget in seconds; `0` disables it. Unlike `timeout_sec` (one attempt) and `max_attempts` (how many), this bounds the number a client actually cares about: how long it will wait. Checked between attempts and while waiting for a streamed answer to start; once bytes are committed the answer is not cut short, because truncating it is worse than letting it finish. A value below 5s leaves no room for a second relay, and validation warns about it. |
| `session_affinity_sec` | `int` | `0` | How long a conversation stays pinned to the relay that **answered it** (seconds); `0` disables it. With it on, a follow-up turn is tried first on the relay that last served that conversation, because the provider can then reuse its **prompt cache** — on a long context that is real money and real latency, and priority order cannot know which relay is warm. Recorded only after a useful answer; a relay whose breaker is open is not held; entries expire and the table is bounded. |
| `reload_on_change` | `bool` | `false` | Watch the config file and apply it when it changes (off by default). With it on, editing and saving `config.json` takes effect within a few seconds — it rides the same 3-second tick as the telemetry flush — so no reload click and no restart. A save from the console itself does not produce a spurious "reloaded" line. It is off by default because a config that moves under a running proxy should be something the operator asked for. |
| `circuit_failure_threshold`| `uint32` | `3` | Number of consecutive network/retriable failures before tripping a provider circuit. |
| `circuit_cooldown_sec` | `uint32` | `30` | Cooldown duration in seconds before testing with a probe request. |
| `skip_open_circuits` | `bool` | `true` | Whether candidate chain building should deprioritize or skip open circuit providers. |
| `log_capacity` | `size_t` | `200` | Maximum capacity of the in-memory circular log buffer. |
| `log_bodies` | `bool` | `false` | Whether to record request and response bodies in the log buffer. **Obvious credentials are still masked** (the known `sk-`/`AIza`/`ghp_` prefixes, `Bearer <token>`, JWTs, private-key blocks, `api_key: <long value>`), because prompts are where people paste them; masking happens before truncation, so a partial key is never left behind. |
| `log_body_limit` | `size_t` | `2048` | Maximum bytes stored per body when `log_bodies` is true. |
| `persist_telemetry` | `bool` | `true` | Whether counters, per-relay stats and the request log are persisted to the state directory and read back at startup. See "Telemetry Persistence" below. |
| `web_ui` | `bool` | `true` | Whether to enable the built-in web console. Takes effect immediately; a non-loopback host without an `api_key` emits a security warning. |
| `traffic_bucket_sec` | `int` | `3600` | Width of one **traffic-trend bucket**, in seconds; values below 60 are raised to 60. The default `3600 × 24` is the historical "last day, hourly" chart; `60 × 120` is the last two hours at minute resolution, which is what debugging a relay right now wants. Each bucket carries its own `bucket_sec`, so a history restored from an older telemetry file is never relabelled at the new width. |
| `traffic_bucket_count` | `int` | `24` | How many buckets the trend keeps. Lowering it trims what is already held immediately rather than at the next bucket boundary. |
| `response_cache_ttl_sec` | `int` | `0` | TTL of the **local response cache**, in seconds; `0` disables it (the default). When on, an identical **non-streaming** request (same protocol, model and body) is answered from memory until the TTL expires instead of being sent upstream — the most direct way to stop paying twice for the same question. Streaming requests are never cached: replaying a stored body as an event stream would mean inventing chunk boundaries and timing, and a client that measures time-to-first-token would be lied to. |
| `response_cache_max_entries` | `int` | `128` | Entries kept before the **least recently used** one is dropped (expired ones first). Each entry is a whole answer, so this is the memory ceiling. |
| `otlp_endpoint` | `string` | `""` | OpenTelemetry **OTLP/HTTP** metrics endpoint, e.g. `http://127.0.0.1:4318`; empty disables export. When set, OTLP JSON is pushed to `{endpoint}/v1/metrics` every 30 seconds (counters as CUMULATIVE and monotonic), for deployments with nothing scraping Prometheus. An export failure is logged once per failure run rather than on every tick. |

### HTTPS listener

The default is `http://127.0.0.1:8787`. To serve the listener over HTTPS, set `server.tls_cert_file` to the absolute path of a PEM chain (leaf certificate first, followed by intermediates), and `server.tls_key_file` to its matching unencrypted PEM private key. The certificate SAN must cover the hostname or IP clients connect to. Missing, expired, not-yet-valid, or mismatched credentials prevent startup, even with `serve --force`.

All model, health, and `/ui/` management endpoints on the listener then use HTTPS. Host, port, TLS paths, and replacement certificates at the same paths take effect after restarting the listener; saving or hot reload keeps active requests intact. Status always advertises the actual running scheme and address.

Public CA certificates use the system trust store. For a private CA, set `LITEROUTER_CA_BUNDLE=/absolute/path/ca-bundle.pem` in the CLI environment; `status`, `logs`, and management commands verify the chain and hostname. This bundle also applies to upstream requests, so include system roots when needed. Browsers and other clients must trust that CA independently. When binding `0.0.0.0` or `::`, remote clients connect using the actual service hostname covered by the certificate; CLI management can use a local config with `server.host` set to that hostname. Certificate issuance and renewal are managed externally.

### Cost Estimation

`price_in_per_million` / `price_out_per_million` are the per-million-token prices **you** wrote down, in USD. They are multiplied by the tokens **the relay itself reported** to give per-relay and total estimates:

- only when a relay reports usage (a relay that omits `usage`, or a stream that never carries it, contributes nothing — nothing is guessed);
- only when a price is configured, so the total is a floor on what is known rather than the whole bill;
- accumulated at the moment of accounting, so correcting a price later does not rewrite history.

That makes it the right tool for "which relay is dearer, and what has today cost me", not a bill to reconcile against.

### Local Response Cache

With `server.response_cache_ttl_sec` on, an **identical** non-streaming request is answered from memory until the TTL expires instead of being sent upstream. A hit carries `X-Literouter-Cache: hit` (a store carries `miss`), the log records `cache` in the `provider` column, and the request total still goes up — but **no relay statistic moves and no tokens are charged**, which is the point.

The key is the SHA-256 of ingress protocol + logical model + the normalised request body, joined with length prefixes so that moving a character across a field boundary is a different key.

**Not** cached: streaming requests (a stored body cannot be replayed as an event stream without inventing timing the client would notice), audio and images (the request is multipart and the answer may be binary), and any non-2xx answer (caching one relay 400 for the whole TTL turns a transient problem into a permanent one). Turning the cache off drops what it held, so re-enabling it cannot serve an answer from before it was switched off.

### Relay-side Protection

`providers[].max_concurrent` and `requests_per_minute` answer "how much is literouter itself sending to this relay": **a full relay is not a broken relay**, so a candidate at its limit is skipped and the next one is tried (no breaker failure, no breaker opened) and the log says `skipping a full relay`. When the whole chain is full the client gets `429` with a real `Retry-After` — the seconds until the oldest start in the rolling window leaves it.

Every start is recorded even while no limit is set. Recording only when limited would give an operator a free first minute after turning a limit on, which is exactly when the limit was wanted. The window has a fixed ceiling, so the memory cost of counting is a constant per relay rather than a function of traffic.

### Traffic Trend Window

`traffic_bucket_sec × traffic_bucket_count` sets the console chart's window and resolution. The default `3600 × 24` is "the last day, hourly"; `60 × 120` is "the last two hours, per minute" when a relay is being debugged. Buckets are floored in UTC and each records its own `bucket_sec`, so a history restored from the telemetry file after a restart is never mislabelled at a new width.

### OpenTelemetry Export

Setting `server.otlp_endpoint` (for example `http://127.0.0.1:4318`) makes literouter push **OTLP/HTTP JSON** to `{endpoint}/v1/metrics` every 30 seconds: counters such as `literouter.requests` are reported as CUMULATIVE and monotonic, and the per-relay metrics carry a `relay=` attribute. It is for deployments with nothing scraping Prometheus. A failed export is logged once rather than on every tick.

### Telemetry Persistence

With `persist_telemetry` on (the default), the server writes the following to `telemetry-<port>.json` in the **state directory** (the port being the one the instance actually bound) (see the [Environment Variables Reference](environment.md)) and reads it back on the next start, so `literouter status` and the `/ui` console do not reset to zero across a restart:

- Global counters: `total_requests` / `total_success` / `total_failure` / `bytes_out` / `tokens_*` / average latency;
- Per-relay stats: requests, successes/failures/aborts, absorbed retries, bytes in and out, tokens and latency;
- The newest **500** request-log entries (`log.seq` keeps counting across the restart and never goes backwards).

Writes match the config file: a temp file in the same directory followed by an atomic rename, mode `0600`. Flushing happens on a background timer (at most once every 3 seconds, and only when something changed) and once more on `stop()`. A corrupt or unrecognised file is ignored and logged, never a reason to refuse startup; a disk write that fails logs one error and leaves request handling alone.

> ⚠️ **Privacy**: with `log_bodies` on as well, bodies (that is, prompts) are written to disk too. Validation warns about that combination. Set `persist_telemetry` to `false` to disable telemetry persistence.

### Provider Configuration (`providers`)

`providers` is an array specifying upstream relays. Each object contains:

| Field | Type | Default | Description |
|---|---|---|---|
| `id` | `string` | Required | Unique identifier (alphanumeric, underscores, dashes) used in routes and telemetry. |
| `name` | `string` | `id` | Human-friendly display label. |
| `protocol` | `string` | `"openai"` | Upstream protocol: `openai` (default), `anthropic`, `gemini`, `openai_responses`, `azure`, `vertex`, `bedrock`, `ollama`. The proxy decides whether a conversion is needed by **wire shape**, not by name, so `azure` shares OpenAI's shape (zero-parse passthrough) and `vertex` shares Gemini's. |
| `api_version` | `string` | `""` | `azure`: the `api-version` query value (empty uses `2024-10-21`); `vertex`: the API version segment in the path (empty uses `v1`). |
| `region` | `string` | `""` | `bedrock`: the AWS region (**required** — SigV4 cannot guess one); `vertex`: the location (empty uses `us-central1`). |
| `project` | `string` | `""` | `vertex`: the project id (**required**). |
| `credentials_file` | `string` | `""` | `vertex`: path to a service-account JSON key (**required**). Its private key signs the RS256 assertion exchanged for an access token, and the token is cached until it is nearly expired. |
| `aws_access_key` | `string` | `""` | `bedrock`: SigV4 access key id (**required**). `${VAR}` references work and are resolved only at signing time. |
| `aws_secret_key` | `string` | `""` | `bedrock`: SigV4 secret access key (**required**), `${VAR}` supported the same way. |
| `aws_session_token` | `string` | `""` | `bedrock`: STS session token for temporary credentials; it is signed along with the rest. |
| `max_concurrent` | `int` | `0` | **Relay-side** in-flight limit; `0` is unlimited. It bounds what literouter itself sends to this relay, not what any caller may ask for. A relay at its limit is skipped like an open breaker and the next candidate is tried (**without** counting a breaker failure); when the whole chain is full the client gets `429` with a `Retry-After`. |
| `requests_per_minute` | `int` | `0` | **Relay-side** rolling 60-second start limit; `0` is unlimited. Every start is recorded even when no limit is set — otherwise the first minute after an operator turns a limit on would be free, which is exactly when it was wanted. |
| `price_in_per_million` | `double` | `0` | What this relay charges for **input** tokens, in USD per million. `0` means not written down, and an unpriced relay contributes **nothing** to the cost estimate rather than being counted at zero. |
| `price_out_per_million` | `double` | `0` | What this relay charges for **output** tokens, in USD per million. |
| `base_url` | `string` | Required | Root URL of the upstream service (e.g. `https://api.openai.com/v1`). Must be a valid HTTP/HTTPS URL. |
| `api_key` | `string` | `""` | Upstream key, supporting static strings or environment variable placeholders. |
| `enabled` | `bool` | `true` | Enable or disable this provider from request dispatching. |
| `priority` | `int32` | `100` | Dispatch priority; **lower number wins** (e.g. 1 before 10, 10 before 20). |
| `weight` | `uint32` | `1` | Relative weight for breaking ties within the same priority; **higher number wins**. |
| `timeout_sec` | `uint32` | `120` | Upstream response timeout in seconds. |
| `connect_timeout_sec` | `uint32` | `15` | TCP / TLS connection establishment timeout in seconds. |
| `supports_stream` | `bool` | `true` | Whether this provider supports Server-Sent Events (SSE) streaming. |
| `models` | `string[]`| `[]` | List of model names advertised by this provider (used in pass-through mode). |
| `headers` | `object` | `{}` | Key-value pairs of extra HTTP headers attached to every request. |
| `chat_path` | `string` | By protocol (`openai`, `anthropic`, `gemini`, `openai_responses`, `azure`, `vertex`, `bedrock`, `ollama`) | Custom chat endpoint path; automatically inferred from `protocol` if empty (e.g. `anthropic` → `/v1/messages`, `vertex` → `/v1/projects/…/publishers/google/models/{model}:generateContent`, `bedrock` → `/model/{model}/converse`). |
| `embeddings_path`| `string` | `"/embeddings"`| Custom embeddings endpoint path. |
| `note` | `string` | `""` | Free-form note or description. |

### Route Configuration (`routes`)

`routes` explicitly defines how logical client model requests map to upstream provider candidate chains.

Each route contains:
- `model` (`string`, required): The logical model name requested by clients (e.g. `gpt-4o`).
- `enabled` (`bool`, default `true`): Whether this route rule is active.
- `targets` (`Target[]`, required): Ordered list of candidate providers.
  - `provider` (`string`, required): The provider `id`.
  - `model` (`string`, optional): The physical model name sent to upstream. If omitted, the client's logical model name is used.

---

## Secure Secret Resolution

### Supported Syntax Formats

In any `api_key` field, `literouter` supports 5 declaration formats:

```text
1. "${VAR_NAME}"          // Reads from env var; empty string if unset
2. "${VAR:-fallback}"     // Reads from env var; falls back to default if unset or empty
3. "$VAR_NAME"            // Simplified env var reference
4. "sk-abc123456"         // Plaintext static key
5. ""                     // Empty; sends no Authorization header (for local/LAN gateways)
```

### Zero-Leak Invariant

To guarantee maximum credential security:

1. **Resolved at invocation time**: The configuration holds raw placeholder strings in memory. Secrets are resolved into plaintext by `resolveSecret()` **strictly at the moment of issuing the network request**.
2. **Never write back plaintext**: Whenever configuration is saved to disk, through `/ui` or the CLI, `ConfigStore` **strictly preserves the original environment variable placeholders**, preventing accidental leaks into configuration files.
3. **Hardened file permissions**: If a configuration contains literal plaintext keys, the file permissions are automatically clamped to `0600` (read/write for owner only) upon save.

---

## Configuration Validation Engine

Before loading or saving, the validation engine checks the configuration for issues:

- 🔴 **Error** (Prevents server startup):
  - Duplicate or empty provider `id`;
  - Invalid `base_url` format (must start with `http://` or `https://`);
  - Route refers to a non-existent provider;
  - Multiple routes defined for the same model;
  - Port out of valid range (`> 65535`).
- 🟡 **Warning** (Server can run, but attention recommended):
  - Referenced environment variable is not defined in the current system;
  - Route candidate chain includes a disabled provider;
  - Server listens on non-loopback (`0.0.0.0`) without `server.api_key`;
  - The web console is served on a non-loopback address without `server.api_key`, so anyone
    who can reach the port can read the request log.
- 🔵 **Info** (Informational notifications):
  - Provider has an empty models list (can only be reached via explicit routes);
  - Server port is `0` (ephemeral port allocation).

Validate configuration at any time using:
```bash
mcpp run -p cli -- config validate
mcpp run -p cli -- doctor
```

---

## Hot Reload

When editing `config.json` externally, reload changes without restarting:

```bash
# Option 1: CLI reload
mcpp run -p cli -- config reload

# Option 2: Admin API endpoint
curl -X POST http://127.0.0.1:8787/__literouter/reload

# Option 3: Click "Reload from disk" in the Settings tab of the console
```

