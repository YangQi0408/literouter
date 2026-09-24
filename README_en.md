# literouter

<p align="center">
  <strong>High-Performance Local AI Relay Aggregator and Multi-Protocol Gateway built with C++23 and mcpp</strong><br>
  Single Endpoint · Intelligent Retry · Header Gate · Auto-Healing Circuit Breaker · Cross-Protocol Passthrough · Zero-Leak Secrets · Native CLI & Web Console
</p>

<p align="center">
  <strong>English</strong> | <a href="README.md">简体中文</a>
</p>

---

## What is literouter?

`literouter` is a high-performance personal AI gateway. It presents a unified endpoint (default `http://127.0.0.1:8787`) to client applications while dispatching requests across multiple upstream relays and official model provider APIs according to your configurable rules.

Simply point any AI tool (such as Chatbox, NextChat, Cursor, Immersive Translate, shell scripts, or standard SDKs) to `literouter`, and let it handle upstream orchestration seamlessly: **which provider offers the requested model, which node is down, which is rate-limited, and which requires model aliasing—all managed automatically in milliseconds**.

```text
┌──────────┐        ┌──────────────────────────────────────────┐        ┌─────────────────┐
│ Client   │  ───▶  │  literouter :8787                        │  ───▶  │ Relay A (Main)  │
│ (OpenAI/ │  ◀───  │                                          │  ◀───  │                 │
│  Claude/ │        │  Protocol Adapter · Header Gate Failover │        ├─────────────────┤
│  Gemini) │        │  Zero-Copy SSE · Model Aliasing · Metrics│  ───▶  │ Relay B (Backup)│
└──────────┘        └──────────────────────────────────────────┘  ◀───  └─────────────────┘
```

---

## Key Features

- ⚡ **Single Endpoint Aggregation**: Centralizes upstream Base URLs and API Keys into a single local port, eliminating the need to reconfigure client applications.
- 🧠 **Local Response Cache**: Exact-match caching for **non-streaming** requests (TTL + LRU eviction). A hit is served directly with `X-Literouter-Cache: hit` and never touches the upstream — the saving is real money. Streaming responses are never cached.
- 🚦 **Relay-side Protection**: Per-upstream **concurrency** and **requests-per-minute** ceilings. A relay that is at its limit is **skipped rather than failed** — it does not trip the breaker, and the request moves on to the next target in the chain, which avoids driving an upstream into its own rate limiter.
- 🔒 **HTTPS and Media Endpoints**: Listener TLS plus audio transcription, translation, speech synthesis, image generation, edits and variations through existing OpenAI-compatible providers.
- 🛡️ **Header Gate & Lossless Streaming Failover**: Novel streaming safety mechanism. Network drops, timeouts, 429 rate limits, or 5xx server errors trigger seamless failover before the first response header reaches the client. Once the first byte is emitted, the connection is locked to **strictly prevent corrupting or interweaving answers**.
- 🔌 **Circuit Breaker with Auto-Probing**: Consecutive failures trip down relays into cooldown. Once cooldown expires, a single-request probe tests recovery automatically without overwhelming services.
- 🔄 **Multi-Protocol Gateway & Zero-Overhead Fast Path**: **Inbound** OpenAI, Claude Messages, Google Gemini and OpenAI Responses requests; **outbound** `openai`, `azure`, `anthropic`, `gemini`, `vertex`, `bedrock`, `ollama` and `responses` (Azure `api-version`, Vertex OAuth2 service accounts and Bedrock SigV4 signing are all implemented natively). Matching protocols enjoy **zero JSON parsing and zero-copy streaming passthrough**, while mismatched protocols are converted bi-directionally on the fly — including Ollama NDJSON and Bedrock AWS event-stream framing.
- 📈 **Observability**: Liveness (`/health/live`) and readiness (`/health/ready`) probes are separate, Prometheus `/__literouter/metrics` exports per relay, and **OTLP metric push** to `{endpoint}/v1/metrics` is supported.
- 🔐 **Zero-Leak Secret Placeholders**: Store `${OPENAI_API_KEY}` or `${VAR:-fallback}` placeholders in your config. Secrets are resolved in memory strictly when dispatching requests and are never written back to disk.
- 🐳 **Local Deployment**: `Dockerfile`, `docker-compose.yml`, and a systemd unit for running the personal gateway on your own machine or server. The container image carries one binary and a CA bundle, and runs as a non-root user.
- 💻 **Two Frontends**:
  - **CLI**: Supports foreground server mode, `tail -f` live log streaming, status dashboards, and system diagnostics (`doctor`).
  - **Web Console**: `serve` carries a modern React + Vite + Tailwind + shadcn/ui `/ui` console on the same port, providing telemetry overview, provider/route management, probing, log filtering, and full config editing; assets are embedded in the binary with zero extra deployment (developing the web UI requires Node.js 22+).
- 🌐 **Cross-Platform**: Native support for Linux, macOS, and Windows.

---

## Quick Start

### 1. Build from Source
Built using modern [mcpp](https://github.com/mcpp-community/mcpp) build toolchain. Compile all components with one command:

```bash
mcpp build --workspace
```

### 2. Initialize Config & Diagnostics
```bash
# Generate seed configuration template (~/.config/literouter/config.json)
mcpp run -p cli -- config init

# Export required environment variables
export OPENAI_API_KEY="sk-..."

# Run doctor to verify syntax, environment variables, and network connectivity
mcpp run -p cli -- doctor
```

### 3. Start the Service
```bash
# Option A: Run CLI server in foreground
mcpp run -p cli -- serve

# (There is no second front end to start; the console is served by `serve`.)
```

### 4. Open the built-in Web Console
Browse to `http://127.0.0.1:8787/ui/` — live metrics, per-relay health and probing, an incremental request log and a config view, with nothing extra to deploy (see [Protocols & API](docs/en/protocols-api.md#built-in-web-console)).

### 5. Verify with a Request
```bash
curl http://127.0.0.1:8787/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o","messages":[{"role":"user","content":"Hello!"}]}'
```

---

## Configuration Preview

Configuration is located by default at `~/.config/literouter/config.json` (`%APPDATA%\literouter\config.json` on Windows):

```jsonc
{
  "server": {
    "host": "127.0.0.1",
    "port": 8787,
    "pass_through_unknown": true,
    "circuit_failure_threshold": 3,
    "circuit_cooldown_sec": 30
  },
  "providers": [
    {
      "id": "openai-main",
      "name": "OpenAI Official",
      "base_url": "https://api.openai.com/v1",
      "api_key": "${OPENAI_API_KEY}",
      "priority": 10,
      "models": ["gpt-4o", "text-embedding-3-small"]
    },
    {
      "id": "relay-backup",
      "name": "Backup Relay",
      "base_url": "https://api.relay-provider.com/v1",
      "api_key": "${BACKUP_KEY}",
      "priority": 20,
      "models": ["gpt-4o"]
    }
  ],
  "routes": [
    {
      "model": "gpt-4o",
      "targets": [
        { "provider": "openai-main", "model": "gpt-4o-2024-08-06" },
        { "provider": "relay-backup" }
      ]
    }
  ]
}
```

---

## Documentation Directory

Technical details are organized into topic-specific documentation:

| Topic | Description |
|---|---|
| 📖 [**Configuration & Secrets**](docs/en/configuration.md) | Complete JSON schema, placeholder syntax, rule validation, and hot reload |
| 🛡️ [**Routing & Failover**](docs/en/routing-failover.md) | Candidate chain resolution, Header Gate streaming logic, circuit breaker state machine |
| 🔄 [**Protocols & API Reference**](docs/en/protocols-api.md) | Inbound endpoints, upstream protocol adapters, fast-path streaming, and Admin APIs |
| 💻 [**CLI Manual**](docs/en/cli.md) | Reference for every subcommand, plus the live dashboard and automation scripts |
| 📦 [**Deployment**](docs/en/deployment.md) | Docker / systemd: run the personal gateway on your own machine or server |
| ⚙️ [**Environment Variables**](docs/en/environment.md) | Full environment variable reference and cross-platform path resolution |

---

## Building and Testing

### Prerequisites
- OS: Linux / macOS / Windows
- Build tool: [mcpp](https://github.com/mcpp-community/mcpp)
- Compiler: Clang `llvm@22.1.8` with `-std=c++23` and libc++ (automatically managed by mcpp)

### Common Commands
```bash
# Run core unit test suite (all 13 test suites pass)
mcpp test -p core

# Build individual members
mcpp build -p core
mcpp build -p cli
```

---

## License

This project is licensed under the [Apache-2.0 License](LICENSE).
Third-party components and the license terms that apply to them are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
