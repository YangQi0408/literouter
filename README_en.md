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

## 🚀 Installation

`literouter` ships in two shapes. Pick one first:

| Shape | For | What it looks like |
|---|---|---|
| **Desktop app** | Everyday personal use | Double-click → gateway starts + browser opens the console → icon in the system tray |
| **Command-line gateway** | Servers, scripts, containers | `literouter serve` runs in the foreground, for systemd, Docker, or another program to drive |

Both share the same config and the same core; installing one does not affect the other.

### Option 1: Desktop App (recommended for individuals)

Download the file for your platform from [GitHub Releases](https://github.com/YangQi0408/literouter/releases):

| Platform | Download | How to run |
|---|---|---|
| **macOS** (Apple Silicon) | `literouter-<version>-macos-arm64-app.zip` | Unzip, drag `literouter.app` into Applications, double-click |
| **Windows** (x86_64) | `literouter-<version>-windows-x86_64-setup.exe` | Run the installer, launch from the Start menu |
| **Linux** (x86_64) | `literouter-<version>-linux-x86_64.tar.gz` | Unzip and run the `literouter-tray` inside |

On launch it does three things: starts the gateway **in its own process**, opens `http://127.0.0.1:8787/ui/` in your browser, and puts an icon in the tray. It has no window of its own — **the console is the interface**.

The tray menu has two entries (a limit of the underlying tray library):

- **Show** — open the console
- **Exit** — stop the gateway and quit

> **First run** writes a config template to the default location; there is nothing to create by hand.
>
> **If a gateway is already running** (say you started `literouter serve` yourself), the app **adopts** it: it opens that instance's console, keeps the icon, and on exit leaves alone a gateway it did not start.

#### Start at login (optional, off by default)

The app **never registers itself**. Ask explicitly:

```bash
literouter-tray --autostart         # add the login entry, then keep running
literouter-tray --no-autostart      # remove it
literouter-tray --autostart-status  # report it
```

The entry goes in a **user-scoped** location you can delete by hand, no elevation needed:

| Platform | Path |
|---|---|
| Linux | `~/.config/autostart/literouter.desktop` |
| macOS | `~/Library/LaunchAgents/io.github.yangqi0408.literouter.plist` |
| Windows | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\literouter.lnk` |

**No system services, no PATH edits, no firewall rules, no registry Run keys.**

### Option 2: Installer Script (Linux / macOS)

For servers and command-line users. This installs the `literouter` CLI:

```bash
# Install to /usr/local/bin (needs sudo)
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash

# Install into your home directory, no root needed
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash -s -- --prefix ~/.local

# Also register as a systemd service (Linux)
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --service -y

# Uninstall (keeps config and telemetry)
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --uninstall
```

**Supported platforms**: Linux x86_64 and macOS Apple Silicon. Intel Macs are refused with a clear message (use Rosetta or build from source).

> ⚠️ The script does **not** install the desktop app. To have both, use Option 1 for the app and Option 2 for the CLI.

### Option 3: Windows CLI Install (PowerShell)

```powershell
# Install to %LOCALAPPDATA%\literouter
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-windows.ps1)))

# Also register a scheduled task (start at login)
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-windows.ps1))) -Service
```

If you already installed the desktop app, the `-Service` path still works on its own — it is the "no tray, just run at boot" case.

### Option 4: Docker / Docker Compose

```bash
# The published image, packaged by CI at release time (takes seconds)
docker pull ghcr.io/yangqi0408/literouter:latest

# Bring it up (loopback binding, persistent config and state)
docker compose up -d
```

> The image is built from the release's Linux binary, so nothing compiles locally. Building from source with `docker compose build` downloads the full LLVM toolchain and takes about 12 minutes cold.

### Option 5: Build from Source

```bash
# Everything: core library + CLI + desktop app
mcpp build --workspace --release

# Only the headless parts (much faster, no graphics dependencies)
mcpp build -p core -p cli --release
```

> The desktop app depends on EUI-NEO, which pulls in GLFW, OpenGL, freetype, libpng and glib — several minutes on a cold cache. Use the second command if you only need the gateway.

Where the build lands:

```text
target/<triple>/<build-hash>/bin/cli/literouter          # command line
target/<triple>/<build-hash>/bin/tray/literouter-tray    # desktop app
```

---

## ⚡ Quickstart in 3 Steps

### Step 1: Start the Gateway

**With the desktop app**: just double-click it. The gateway starts in the same process, the browser opens the console, and the icon appears in the tray — skip to step 2.

**With the command-line build**:
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
- **Balanced Round-Robin (`routing_policy: "round_robin"`)**: Rotates the first relay between requests so equal relays share traffic over time instead of staying in a fixed primary/backup split; a relay's model fallback chain stays together.
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

This project is built with C++23 and [mcpp](https://github.com/mcpp-community/mcpp). The virtual workspace has three members: `core` (library), `cli` (command line), `tray` (desktop app).

```bash
# Full build (all three)
mcpp build --workspace

# Headless only — skips the graphics dependencies
mcpp build -p core -p cli

# Run the core test suites (all 13 pass)
mcpp test -p core
mcpp test -p cli
```

> `tray` depends on EUI-NEO, which brings in GLFW / OpenGL / freetype / libpng / glib. Use `-p core -p cli` when you are only touching core logic; it saves several minutes.

## License

Licensed under the [Apache-2.0 License](LICENSE).
Third-party notices and licenses are documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
