# literouter

<p align="center">
  <strong>High-Performance Local AI Relay Aggregator and Multi-Protocol Gateway built with C++23 and mcpp</strong><br>
  Single Endpoint · Intelligent Retry · Header Gate · Auto-Healing Circuit Breaker · Cross-Protocol Passthrough · Zero-Leak Secrets · Native CLI & GUI
</p>

<p align="center">
  <strong>English</strong> | <a href="README.md">简体中文</a>
</p>

---

## What is literouter?

`literouter` is a high-performance local AI proxy aggregator. It presents a unified local endpoint (default `http://127.0.0.1:8787`) to client applications while dispatching requests across multiple upstream relays and official model provider APIs according to your configurable rules.

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
- 🛡️ **Header Gate & Lossless Streaming Failover**: Novel streaming safety mechanism. Network drops, timeouts, 429 rate limits, or 5xx server errors trigger seamless failover before the first response header reaches the client. Once the first byte is emitted, the connection is locked to **strictly prevent corrupting or interweaving answers**.
- 🔌 **Circuit Breaker with Auto-Probing**: Consecutive failures trip down relays into cooldown. Once cooldown expires, a single-request probe tests recovery automatically without overwhelming services.
- 🔄 **Multi-Protocol Gateway & Zero-Overhead Fast Path**: Supports inbound OpenAI, Claude Messages, Google Gemini, and OpenAI Responses requests. Matching protocols enjoy **zero JSON parsing and zero-copy streaming passthrough**, while mismatched protocols are converted bi-directionally on the fly.
- 🔐 **Zero-Leak Secret Placeholders**: Store `${OPENAI_API_KEY}` or `${VAR:-fallback}` placeholders in your config. Secrets are resolved in memory strictly when dispatching requests and are never written back to disk.
- 💻 **Dual Frontends**:
  - **CLI**: Supports foreground server mode, `tail -f` live log streaming, status dashboards, and system diagnostics (`doctor`).
  - **GUI Console**: Native hardware-accelerated OpenGL desktop dashboard featuring real-time telemetry tiles, provider health matrices, visual route ordering, and one-click model auto-discovery.
- 🌐 **Cross-Platform & Internationalization**: Native support for Linux, macOS, and Windows. Includes English and Simplified Chinese localization, alongside dynamic vector UI scaling (80% ~ 150%).

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

# Option B: Run native OpenGL desktop console (GUI)
mcpp run -p gui
```

### 4. Verify with a Request
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
| 💻 [**CLI Manual**](docs/en/cli.md) | Comprehensive reference for all 8 subcommands, live dashboard, and automation scripts |
| 🖥️ [**Desktop GUI Guide**](docs/en/gui.md) | Walkthrough of 5 core panels, model scanning, zoom shortcuts, and CJK font fallback |
| ⚙️ [**Environment Variables**](docs/en/environment.md) | Full environment variable reference and cross-platform path resolution |

---

## Building and Testing

### Prerequisites
- OS: Linux / macOS / Windows
- Build tool: [mcpp](https://github.com/mcpp-community/mcpp)
- Compiler: Clang `llvm@22.1.8` with `-std=c++23` and libc++ (automatically managed by mcpp)

### Common Commands
```bash
# Run core unit test suite (all 10 test suites pass)
mcpp test -p core

# Build individual members
mcpp build -p core
mcpp build -p cli
mcpp build -p gui

# Run automated headless GUI smoke tests
LITEROUTER_GUI_SMOKE=1 mcpp run -p gui
```

---

## License

This project is licensed under the [Apache-2.0 License](LICENSE).
