# literouter

<p align="center">
  <strong>High-Performance Local AI Relay Aggregator and Multi-Protocol Gateway built with C++23 and mcpp</strong><br>
  Single Endpoint · Intelligent Failover · Header Gate · Auto-Healing Breaker · Cross-Protocol Fast Path · Zero-Leak Secrets · Native CLI & Embedded Web Console
</p>

<p align="center">
  <strong>English</strong> | <a href="README.md">简体中文</a>
</p>

---

## What is literouter?

`literouter` is a high-performance local AI gateway designed for developers and teams. It provides a single local endpoint (default `http://127.0.0.1:8787`) to client applications while dispatching requests across multiple upstream relays and official model provider APIs according to your configurable rules.

Simply point any AI tool (such as **Cursor, Claude Desktop, VS Code (Continue/Cline/Roo Code), Chatbox, NextChat, Immersive Translate, shell scripts, or standard Python/Node.js SDKs**) to `literouter`, and let it handle upstream orchestration seamlessly in milliseconds: **which provider offers the requested model, which node is down or rate-limited, which requires model aliasing, and which is cheapest—all managed automatically in the background**.

```text
┌──────────────┐        ┌──────────────────────────────────────────────┐        ┌─────────────────────┐
│ Client       │  ───▶  │  literouter :8787                            │  ───▶  │ Relay A (Official)  │
│ (Cursor/     │  ◀───  │                                              │  ◀───  │                     │
│  Claude/     │        │  Multi-Protocol Gateway · Header Gate Safety │        ├─────────────────────┤
│  OpenAI SDK) │        │  Zero-Copy SSE · Per-Model Pricing · Telemetry│  ───▶  │ Relay B (Low-Cost)  │
└──────────────┘        └──────────────────────────────────────────────┘  ◀───  └─────────────────────┘
```

---

## 🚀 1-Minute Quick Installation

Prebuilt `literouter` binaries have zero runtime dependencies (Web Console assets are embedded into the binary at compile time):

### Option 1: One-Line Installer Script (Recommended for Linux / macOS)
```bash
# Auto-detects architecture, downloads latest release, and installs to /usr/local/bin/literouter (requires sudo)
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash

# Non-root installation to ~/.local/bin:
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash -s -- --prefix ~/.local

# Install and enable as a systemd background service (Linux):
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --service -y
```

### Option 2: Prebuilt Binary Releases (Linux / macOS / Windows)
Download the standalone binary for your platform from [GitHub Releases](https://github.com/YangQi0408/literouter/releases):
- **Linux (x86_64)**: Extract, `chmod +x literouter`, and move to `/usr/local/bin/`;
- **macOS (Apple Silicon / Intel)**: Extract and move to `/usr/local/bin/` or `~/.local/bin/`;
- **Windows (x86_64)**: Extract `literouter.exe` to a permanent folder (such as `C:\Program Files\literouter\`), and add it to your system `PATH`.

### Option 3: Docker / Docker Compose
```bash
# Run with docker compose (binds to loopback, persistent config & state)
docker compose up -d
```

### Option 4: Build from Source (Developers)
```bash
# Compile entire workspace using modern mcpp build toolchain:
mcpp build --workspace --release
```

---

## ⚡ Quickstart in 3 Steps

### Step 1: Start the Gateway
```bash
literouter serve
```
> The gateway will listen on `http://127.0.0.1:8787`. On its first run, it automatically creates a default seed config file (`~/.config/literouter/config.json` on Linux/macOS, `%APPDATA%\literouter\config.json` on Windows).

### Step 2: Configure Providers in the Web Console
Open in your browser: **`http://127.0.0.1:8787/ui/`**

In the modern, embedded Web Console, you can:
- ➕ **Add Providers visually**: Configure provider name, Base URL, API Key (supports `${VAR}` placeholders), and protocol (`openai`, `anthropic`, `gemini`, `azure`, `vertex`, `bedrock`, `ollama`);
- 💰 **Set Default & Per-Model Pricing**: Configure general input/output prices and **override prices for specific models**;
- 🔄 **Manage Routes & Aliasing**: Define primary and fallback chains for client models (e.g. `gpt-4o`);
- 🩺 **One-Click Connectivity Probing**: Test upstream latency and check advertised model availability.

> 💡 *Command-line enthusiasts can also configure everything via CLI: `literouter providers add <id> --base-url <url> --key-env <VAR>`.*

### Step 3: Send a Test Request
```bash
curl http://127.0.0.1:8787/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o","messages":[{"role":"user","content":"Hello!"}]}'
```
Receiving a response confirms that `literouter` successfully routed and dispatched your request!

---

## 🔌 Client & AI Tool Integration Guide

Point your client application's API base to `http://127.0.0.1:8787` to enjoy transparent load balancing and failover.

| Client / Tool | Endpoint Dialect | Base URL / Server Address | API Key Notes |
|---|---|---|---|
| **Cursor** | OpenAI Compatible | `http://127.0.0.1:8787/v1` | Any string or `server.api_key` |
| **VS Code (Continue)** | OpenAI / Anthropic | `http://127.0.0.1:8787/v1` | Any string or `server.api_key` |
| **VS Code (Cline / Roo Code)** | OpenAI Compatible | `http://127.0.0.1:8787/v1` | Any string or `server.api_key` |
| **Claude Desktop** | Native Anthropic | `http://127.0.0.1:8787` | Passthrough `/v1/messages` |
| **NextChat / Chatbox** | OpenAI Compatible | `http://127.0.0.1:8787` | Any string or `server.api_key` |
| **Immersive Translate / Cherry Studio** | OpenAI Compatible | `http://127.0.0.1:8787/v1` | Any string or `server.api_key` |
| **Python / Node.js SDK** | OpenAI Official SDK | `http://127.0.0.1:8787/v1` | See code snippet below |

### Python OpenAI SDK Example
```python
from openai import OpenAI

client = OpenAI(
    base_url="http://127.0.0.1:8787/v1",
    api_key="none",  # Any non-empty string if server.api_key is unconfigured
)

response = client.chat.completions.create(
    model="gpt-4o",
    messages=[{"role": "user", "content": "Hello!"}],
    stream=True,
)
for chunk in response:
    print(chunk.choices[0].delta.content or "", end="")
```

### Continue (VS Code Extension) `config.yaml` Example
```yaml
models:
  - name: GPT-4o
    provider: openai
    model: gpt-4o
    apiBase: http://127.0.0.1:8787/v1
    apiKey: none
  - name: Claude 3.5 Sonnet
    provider: anthropic
    model: claude-3-5-sonnet
    apiBase: http://127.0.0.1:8787
    apiKey: none
```

---

## 💰 Flexible Cost Estimation & Per-Model Pricing

Pricing often differs significantly across providers and among different models on the same provider (e.g. `gpt-4o` vs `gpt-4o-mini`, or `claude-3-5-sonnet` vs `claude-3-5-haiku`).

`literouter` provides fine-grained **provider default rates** alongside **per-model price overrides**:
- **Provider Default Pricing**: Set `price_in_per_million` and `price_out_per_million` on a provider as baseline prices;
- **Per-Model Overrides (`model_prices`)**: Override input/output rates for specific models; unlisted models fall back to the provider default;
- **Cheapest Routing Policy (`routing_policy: "cheapest"`)**: Dynamically evaluates the actual price of the requested model across all candidate relays and routes to the cheapest healthy relay first;
- **Visual Management**: Easily configure model rates in the `/ui` Web Console and monitor real-time spend across relays and models.

---

## 🛡️ Key Architectural Capabilities

- 🛡️ **Header Gate Streaming Safety**: Proprietary streaming failover mechanism. Connection failures, timeouts, 429s, or 5xx errors trigger transparent failover before the first response header reaches the client. Once the first byte is emitted, the connection is locked to **strictly prevent corrupting or interweaving answers**.
- 🔄 **Multi-Protocol Gateway & Zero-Copy Fast Path**: Supports inbound OpenAI, Anthropic Claude, Google Gemini, and OpenAI Responses requests, and 8 outbound provider protocols. Matching protocols enjoy **zero JSON parsing and zero-copy streaming passthrough**, while mismatched protocols are converted bi-directionally on the fly.
- 🧠 **Local Response Cache**: Exact-match caching for **non-streaming** requests (TTL + LRU eviction). Cache hits are served directly from memory with `X-Literouter-Cache: hit` without upstream roundtrips, saving real API spend.
- 🚦 **Relay-Side Rate Limiting & Auto-Breaker**: Set per-relay concurrency and rolling RPM limits. Saturated relays are skipped without penalizing health; failing relays trip into cooldown and auto-recover via single-request probes.
- 🔐 **Zero-Leak Secret Placeholders**: Store `${OPENAI_API_KEY}` placeholders in your config. Secrets are resolved in memory strictly when dispatching requests and are never written back to disk.
- 💻 **Two Frontends in Harmony**:
  - **CLI**: Foreground server, `tail -f` live log streaming, status dashboards, and health diagnostics (`doctor`);
  - **Web Console**: An embedded modern React + Vite + Tailwind + shadcn/ui `/ui` console served directly on the same port with zero extra deployment.

---

## 🛠️ Common CLI Commands

```bash
literouter serve                 # Start foreground proxy gateway (default :8787)
literouter status                # View current status and health dashboard
literouter logs -f               # Stream live request logs in real time
literouter doctor                # Run end-to-end environment, network, and config health checks
literouter models                # Inspect all exposed models and routing topology
literouter providers list        # List all configured upstream providers
literouter providers add <id>    # Add a new provider (supports --model-price)
literouter routes add <model>    # Configure explicit model routing and fallback chains
literouter bench --model gpt-4o  # Benchmark latency and cost across relays
literouter config validate       # Deep validation of configuration syntax and security
```

---

## 📚 Documentation Directory

| Topic | Description |
|---|---|
| 📖 [**Configuration & Secrets**](docs/en/configuration.md) | Complete JSON schema, per-model pricing, secret placeholders, and hot reload |
| 📦 [**Deployment & Operations**](docs/en/deployment.md) | Installer script flags, systemd system/user services, macOS launchd, Docker & Nginx |
| 🛡️ [**Routing & Failover**](docs/en/routing-failover.md) | Candidate resolution, Header Gate streaming logic, session affinity, circuit breaker |
| 🔄 [**Protocols & API Reference**](docs/en/protocols-api.md) | Inbound endpoints, upstream protocol adapters, fast-path streaming, and Admin APIs |
| 💻 [**CLI Manual**](docs/en/cli.md) | Full CLI subcommand reference, live dashboard, and automation scripts |
| ⚙️ [**Environment Variables**](docs/en/environment.md) | Full environment variable reference and cross-platform path resolution |

---

## Building and Contributing

Built with C++23 and [mcpp](https://github.com/mcpp-community/mcpp):

```bash
# Build workspace
mcpp build --workspace

# Run core test suites (all 13 suites pass)
mcpp test -p core
mcpp test -p cli
```

## License

Licensed under the [Apache-2.0 License](LICENSE).
Third-party notices and licenses are documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
