# literouter

<p align="center">
  <strong>High-Performance Local AI Relay Aggregator and Multi-Protocol Gateway built with C++23 and mcpp</strong><br>
  Single Endpoint · Failover · Header Gate · Circuit Recovery · Wire Passthrough · Secret References · CLI, Desktop Tray & Embedded Web Console
</p>

<p align="center">
  <strong>English</strong> | <a href="README.md">简体中文</a>
</p>

---

## What is literouter?

`literouter` is a local AI gateway for individual developers. It provides a single local endpoint (default `http://127.0.0.1:8787`) while dispatching requests across upstream relays and official APIs. It is a single-user tool: one `server.api_key` protects both model and management endpoints, with no accounts, permission groups or quota distribution.

Point clients, editor extensions or SDKs that support a custom API address and a matching protocol to `literouter` to centralize model aliases, upstream candidates, limits and failover. Client versions and vendor extensions still need scenario-specific testing. Prices come from your configured rates, not an automatically synchronized bill.

```text
┌──────────────┐        ┌──────────────────────────────────────────────┐        ┌─────────────────────┐
│ Client       │  ───▶  │  literouter :8787                            │  ───▶  │ Relay A (Official)  │
│ (Cursor/     │  ◀───  │                                              │  ◀───  │                     │
│  Claude/     │        │  Multi-Protocol Gateway · Header Gate Safety │        ├─────────────────────┤
│  OpenAI SDK) │        │  SSE Forwarding · Model Rates · Telemetry   │  ───▶  │ Relay B (Low-Cost)  │
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
| **macOS** (Apple Silicon, 14+) | `literouter-<version>-macos-arm64-app.zip` | Unzip, drag into Applications, double-click; allow an unnotarized build through the OS prompt |
| **Windows** (x86_64) | `literouter-<version>-windows-x86_64-setup.exe` | Run the installer, launch from the Start menu |
| **Linux** (x86_64) | `literouter-<version>-linux-x86_64.tar.gz` | Unzip and run the `literouter-tray` inside |

On launch it starts the gateway **in the same process** (or adopts an existing instance), opens `/ui/` at the actual listener address, and puts an icon in the tray. The default is `http://127.0.0.1:8787/ui/`; port `0` and HTTPS use the actual port and scheme. It has no window of its own — **the console is the interface**.

On Linux, `literouter-tray` is a launcher using the matching loader and shared libraries under `lib/tray/`; keep the entire extracted directory. A StatusNotifierWatcher is required (KDE, GNOME AppIndicator, or a tray-enabled panel). The standalone CLI requires glibc 2.38+. macOS code signing is not notarization; ad-hoc or Developer ID signing alone is not advertised as notarized.

`v0.5.1` repairs desktop packaging and runtime boundaries; the [pre-release platform CI matrix passed](https://github.com/YangQi0408/literouter/actions/runs/40286684160). It does not replace historical v0.5.0 assets or auto-upgrade a running service. Native desktop behavior still depends on the target environment. See [v0.5.1 upgrade notes](docs/release-notes/v0.5.1.md) and the [packaging guide](scripts/README.md).

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

These are integration references for clients that support custom endpoints, not a guarantee for every client version. A configured `server.api_key` must be supplied; arbitrary placeholder strings work only while that local key is empty.

| Client / Tool | Endpoint Dialect | Base URL / Server Address | API Key Notes |
|---|---|---|---|
| **Cursor** | OpenAI Compatible | `http://127.0.0.1:8787/v1` | Any string or `server.api_key` |
| **VS Code (Continue)** | OpenAI / Anthropic | `http://127.0.0.1:8787/v1` | Any string or `server.api_key` |
| **VS Code (Cline / Roo Code)** | OpenAI Compatible | `http://127.0.0.1:8787/v1` | Any string or `server.api_key` |
| **Anthropic SDK / custom-endpoint tools** | Native Anthropic | `http://127.0.0.1:8787` | `/v1/messages`, subject to client support |
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
- **Cheapest Routing Policy (`routing_policy: "cheapest"`)**: Orders candidates by their configured rates for this model, not a synchronized bill; session affinity still takes precedence;
- **Balanced Round-Robin (`routing_policy: "round_robin"`)**: Rotates the first relay; explicitly authored same-provider model fallbacks stay together. `providers[].models` is a directory and never implicitly replaces the requested model.
- **Visual Management**: Configure model rates in `/ui` and estimate spend from reported usage; unreported or unpriced portions are not included.

---

## 🛡️ Key Architectural Capabilities

- 🛡️ **Header Gate Streaming Safety**: Connection failures, timeouts, 429s and retryable 5xx errors can try the next candidate before a response is committed. After commitment, the upstream is never switched, avoiding interleaved answers.
- 🔄 **Multi-Protocol Gateway & Wire Passthrough**: Four inbound protocols and eight outbound protocols, including `openai_responses`. Matching wire shapes avoid protocol translation, but request validation/routing still parses JSON and streaming uses bounded queues; this is not end-to-end zero parsing or zero copying.
- 🧠 **Local Response Cache**: Exact-match caching for **non-streaming** requests (TTL + LRU eviction). Cache hits are served directly from memory with `X-Literouter-Cache: hit` without upstream roundtrips, saving real API spend.
- 🚦 **Relay-Side Rate Limiting & Auto-Breaker**: Set per-relay concurrency and rolling RPM limits. Saturated relays are skipped without penalizing health; failing relays trip into cooldown and auto-recover via single-request probes.
- 🔐 **Secret References & Redaction**: Store `${OPENAI_API_KEY}` references; saves preserve them and management responses hide dedicated stored credentials. Literal keys remain a supported user choice, so this is not an absolute zero-leak guarantee. Upstream redirects are refused; configure the final Base URL.
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

# Run the core and CLI test suites
mcpp test -p core
mcpp test -p cli
```

> `tray` depends on EUI-NEO, which brings in GLFW / OpenGL / freetype / libpng / glib. Use `-p core -p cli` when you are only touching core logic; it saves several minutes.

## License

Licensed under the [Apache-2.0 License](LICENSE).
Third-party notices and licenses are documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
