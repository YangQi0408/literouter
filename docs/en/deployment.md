# Deployment & Operations Guide

`literouter` is a lightweight single-user gateway: one `server.api_key` guards both model routing endpoints and administrative interfaces. Any user can run and maintain their own reliable aggregator instance on a workstation, home server, or cloud host.

This document covers installation methods, background service setup (systemd / launchd / Windows), Docker containerization, reverse proxying, and maintenance best practices.

---

## Table of Contents

- [1. Installation Options](#1-installation-options)
  - [Option A: One-Line Installer Script (Recommended for Linux / macOS)](#option-a-one-line-installer-script-recommended-for-linux--macos)
  - [Option B: GitHub Releases Prebuilt Binaries (Linux / macOS / Windows)](#option-b-github-releases-prebuilt-binaries-linux--macos--windows)
    - [macOS: using the `.app`](#macos-using-the-app)
    - [Windows: using the installer](#windows-using-the-installer)
    - [Installing the bare binary by hand](#installing-the-bare-binary-by-hand)
  - [Option C: Build from Source](#option-c-build-from-source)
- [2. Running as a Background Service](#2-running-as-a-background-service)
  - [Setup 1: Linux Systemd System Service (Recommended for Servers)](#setup-1-linux-systemd-system-service-recommended-for-servers)
  - [Setup 2: Linux Systemd User Service (Rootless / Desktop)](#setup-2-linux-systemd-user-service-rootless--desktop)
  - [Setup 3: macOS LaunchAgent (Start on Login)](#setup-3-macos-launchagent-start-on-login)
  - [Setup 4: Windows Task Scheduler or Service](#setup-4-windows-task-scheduler-or-service)
- [3. Docker Container Deployment](#3-docker-container-deployment)
  - [Quickstart with Docker Compose](#quickstart-with-docker-compose)
  - [Container Operational Notes](#container-operational-notes)
- [4. Reverse Proxy & Network Exposure (Nginx / Caddy)](#4-reverse-proxy--network-exposure-nginx--caddy)
  - [Nginx Configuration (Must Disable Buffering)](#nginx-configuration-must-disable-buffering)
  - [Caddy Configuration](#caddy-configuration)
- [5. Upgrades & Uninstallation](#5-upgrades--uninstallation)
  - [Upgrades](#upgrades)
  - [Uninstallation](#uninstallation)
- [6. Runtime Footprint](#6-runtime-footprint)
  - [Idle and Concurrent Measurements](#idle-and-concurrent-measurements)
  - [Threads and File Descriptors](#threads-and-file-descriptors)
  - [Large Bodies and the Body-Logging Switch](#large-bodies-and-the-body-logging-switch)

---

## 1. Installation Options

### Option A: One-Line Installer Script (Recommended for Linux / macOS)

The repository provides an automated installation script for Linux x86_64 and macOS Apple Silicon. It detects the platform, downloads the latest release from GitHub Releases, and verifies its SHA-256 checksum:

```bash
# Default install to /usr/local/bin/literouter (requires sudo)
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash

# Non-root installation to ~/.local/bin
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash -s -- --prefix ~/.local

# Install and register as a systemd service automatically (Linux only):
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --service -y
```

**Common Script Flags**:
- `--prefix DIR`: Installation destination directory (default `/usr/local`);
- `--version TAG`: Install a specific release tag (e.g. `v0.3.0`, defaults to `latest`);
- `--from PATH`: Install from a local pre-downloaded binary instead of fetching from GitHub;
- `--service`: Create a dedicated unprivileged user `literouter` and enable the systemd service (Linux);
- `--uninstall`: Stop and remove the binary and systemd service (preserves configuration and telemetry);
- `-y, --yes`: Non-interactive mode without confirmation prompts.

---

### Option B: GitHub Releases Prebuilt Binaries (Linux / macOS / Windows)

For manual installation, head over to the [GitHub Releases](https://github.com/YangQi0408/literouter/releases) page and download the artifact for your operating system. Each platform ships two forms — **the installer is the one to pick for everyday use**:

| Platform | Recommended | Notes |
|---|---|---|
| **macOS** | `literouter-<version>-macos-arm64-app.zip` | A `.app` bundle: unzip, drag to Applications, double-click. Gets its own Dock icon and Gatekeeper identity |
| **Windows** | `literouter-<version>-windows-x86_64-setup.exe` | An installer: installs to `Program Files`, adds Start-menu entries, uninstalls from Apps & features |
| **Linux** | `literouter-<version>-linux-x86_64.tar.gz` | Contains both `literouter` (command line) and `literouter-tray` (desktop app); no AppImage |

The macOS and Windows install forms install the **desktop app** (tray icon with an embedded gateway). The bare `literouter` binaries inside the archives are what the install scripts and the container image consume.

#### macOS: using the `.app`

1. Unzip `literouter-...-app.zip` and drag `literouter.app` into `/Applications`;
2. If Gatekeeper refuses the first launch ("cannot be verified"), right-click the icon and choose **Open**, or run:
   ```bash
   xattr -dr com.apple.quarantine /Applications/literouter.app
   ```
3. Double-clicking starts the gateway inside the app's own process, opens `http://127.0.0.1:8787/ui/` in your browser, and puts an icon in the system tray. The app has **no window of its own** — the console is the interface;
4. The tray menu has two entries: **Show** opens the console, **Exit** stops the gateway and quits.

> If a gateway is already running (say you started `literouter serve` yourself), the app **adopts** it — it opens that instance's console, keeps the icon, and does not stop a gateway it did not start.

#### Windows: using the installer

1. Run `literouter-...-setup.exe`;
2. One option in the wizard is **off by default**:
   - **Add to system PATH** — lets you type `literouter` in any terminal. Off by default so it cannot shadow an existing `literouter` already on your PATH;
3. The Start menu gets: `literouter` (desktop app), `literouter (headless gateway)`, `literouter (status)`, the documentation, and uninstall;
4. Uninstall through Apps & features; it tries to stop a running gateway gracefully first.

> The installer does **not** start the gateway, does **not** register a login item (that is the app's `--autostart` or the CLI script's `-Service`, both of which you trigger explicitly), and does **not** touch firewall rules (the default bind is loopback; Windows will prompt on the day you bind `0.0.0.0`).

#### Installing the bare binary by hand

If you would rather manage it yourself:

1. **Standalone Binaries**: Each release package contains a self-contained `literouter` executable (`literouter.exe` on Windows). All Web Console frontend assets are embedded into the binary at compile time (`#embed`) — zero Node.js, Python, or external runtime dependencies needed;
2. **Move to PATH**:
   - **Linux / macOS**:
     ```bash
     chmod +x literouter
     sudo mv literouter /usr/local/bin/
     ```
   - **Windows**: Place `literouter.exe` in a permanent directory (such as `C:\Program Files\literouter\` or `C:\Tools\`), and add it to your user or system `PATH`;
3. **Verify Installation**:
   ```bash
   literouter --version
   ```

> 💡 **Windows Paths**: Windows builds use MinGW GCC. The default configuration file resides in `%APPDATA%\literouter\config.json` (e.g. `C:\Users\<User>\AppData\Roaming\literouter\config.json`). Keep it in the default location to avoid Windows `MAX_PATH` limitations.

---

### Option C: Build from Source

Developers or users on specialized architectures (like Linux arm64) can build from source.

**Prerequisites**:
- Modern C++ build system [mcpp](https://github.com/mcpp-community/mcpp);
- Compiler: LLVM/Clang with C++23 modules support (mcpp automatically manages this);
- (Optional) If editing the Web Console frontend, Node.js 22+ is required to run `npm --prefix web run build`.

**Build Steps**:
```bash
git clone https://github.com/YangQi0408/literouter.git
cd literouter

# Build entire workspace in release mode (core library + cli executable)
mcpp build --workspace --release

# The executable is produced at target/<triple>/<hash>/bin/cli/literouter
```

---

## 2. Running as a Background Service

For reliable personal or server use, running `literouter` as a background daemon ensures automatic restart on failures and auto-launch on boot.

### Setup 1: Linux Systemd System Service (Recommended for Servers)

The repository provides a hardened systemd unit file [deploy/literouter.service](../../deploy/literouter.service).

1. **Automated Setup**: Running `scripts/install.sh --service` handles this automatically.
2. **Manual Setup Steps**:
   ```bash
   # 1. Create a dedicated unprivileged system account
   sudo useradd --system --no-create-home --home-dir /var/lib/literouter --shell /usr/sbin/nologin literouter 2>/dev/null || true

   # 2. Prepare directories with secure permissions
   #    The config directory is group-writable by the service (0770): saving from
   #    the Web console writes a temp file next to the config and renames it, so
   #    the directory itself must be writable, not just the file.
   sudo install -d -o root -g literouter -m 0770 /etc/literouter
   sudo install -d -o literouter -g literouter -m 0750 /var/lib/literouter

   # 3. Initialize default config if not present
   if [ ! -f /etc/literouter/config.json ]; then
     sudo LITEROUTER_CONFIG=/etc/literouter/config.json literouter config init --force
     sudo chown literouter:literouter /etc/literouter/config.json
     sudo chmod 0640 /etc/literouter/config.json
   fi

   # 4. Install unit file and start service
   sudo cp deploy/literouter.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now literouter
   ```
3. **Daily Operations**:
   ```bash
   sudo systemctl status literouter
   sudo journalctl -u literouter -f
   sudo systemctl restart literouter
   ```

**Security Hardening Highlights**:
- Runs under dedicated unprivileged user `User=literouter`;
- `ProtectSystem=strict` and `ProtectHome=true`: the file system is mounted read-only except for two paths — the state directory `/var/lib/literouter` and the config directory `/etc/literouter` (the Web console's "save configuration" needs it). The config directory stays owned by `root` and is only group-writable by the service (`0770`);
- `NoNewPrivileges=true` and `PrivateTmp=true` block privilege escalation;
- Uses graceful `SIGTERM` stop with 30s timeout to allow in-flight streaming requests to finish cleanly.

> **Prefer the config read-only to the service?** That is an optional hardened posture: drop `/etc/literouter` from `ReadWritePaths` in the unit, add back `ReadOnlyPaths=/etc/literouter` (and set the directory back to `0750 root:literouter`). The Web console's "save configuration" then returns `write_failed`; edit `/etc/literouter/config.json` with `sudo` and apply it with `POST /__literouter/reload`, a service restart, or `server.reload_on_change`.

> **Already running an older deployment?** Neither the directory mode nor the unit updates itself, so apply the change once by hand:
> ```bash
> sudo chmod g+w /etc/literouter                       # 0750 -> 0770
> sudo cp deploy/literouter.service /etc/systemd/system/
> sudo systemctl daemon-reload && sudo systemctl restart literouter
> ```
> The Web console's "save configuration" then works.

---

### Setup 2: Linux Systemd User Service (Rootless / Desktop)

If running on a desktop or in an environment without `sudo` privileges:

1. Create user service directory and unit:
   ```bash
   mkdir -p ~/.config/systemd/user
   cat <<'EOF' > ~/.config/systemd/user/literouter.service
   [Unit]
   Description=literouter — personal AI gateway
   After=network.target

   [Service]
   Type=simple
   ExecStart=%h/.local/bin/literouter serve --port 8787
   Restart=on-failure
   RestartSec=5
   KillSignal=SIGTERM
   TimeoutStopSec=30

   [Install]
   WantedBy=default.target
   EOF
   ```
2. Enable and start:
   ```bash
   systemctl --user daemon-reload
   systemctl --user enable --now literouter
   ```
*(To keep the user service running after logout, execute `loginctl enable-linger $USER`)*.

---

### Setup 3: macOS LaunchAgent (Start on Login)

On macOS, configure a `launchd` user agent for automatic background startup on login.

The repository includes an installer that installs the binary, generates the LaunchAgent, starts it immediately, and enables it for future logins:

```bash
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-macos.sh \
  | bash -s -- --service
```

For a local binary:

```bash
bash scripts/install-macos.sh --from ./literouter --service
```

Uninstall the background service and binary while keeping configuration and state:

```bash
bash scripts/install-macos.sh --uninstall
```

Check it with:

```bash
launchctl print "gui/$(id -u)/com.literouter.gateway"
curl http://127.0.0.1:8787/health
```

The generated `~/Library/LaunchAgents/com.literouter.gateway.plist` is equivalent to:
   ```xml
   <?xml version="1.0" encoding="UTF-8"?>
   <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
   <plist version="1.0">
   <dict>
       <key>Label</key>
       <string>com.literouter.gateway</string>
       <key>ProgramArguments</key>
       <array>
           <string>/usr/local/bin/literouter</string>
           <string>serve</string>
           <string>--port</string>
           <string>8787</string>
       </array>
       <key>RunAtLoad</key>
       <true/>
       <key>KeepAlive</key>
       <true/>
       <key>StandardOutPath</key>
       <string>/tmp/literouter.stdout.log</string>
       <key>StandardErrorPath</key>
       <string>/tmp/literouter.stderr.log</string>
   </dict>
   </plist>
   ```
Load the service manually with:
   ```bash
   launchctl load ~/Library/LaunchAgents/com.literouter.gateway.plist
   ```

---

### Setup 4: Windows Task Scheduler or Service

On Windows:

The repository includes a PowerShell installer that registers a per-user Task Scheduler entry, starts it immediately, and runs it at every login:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-windows.ps1))) -Service
```

For a checked-out repository or local binary:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-windows.ps1 -Service
powershell -ExecutionPolicy Bypass -File .\scripts\install-windows.ps1 `
  -From 'C:\Program Files\literouter\literouter.exe' -Service
```

Check or remove it with:

```powershell
Get-ScheduledTask -TaskName literouter
powershell -ExecutionPolicy Bypass -File .\scripts\install-windows.ps1 -Uninstall
```

- **Option 1: Windows Task Scheduler**:
  Create a new task triggered "At log on", action "Start a program", pointing to `literouter.exe` with arguments `serve`.
- **Option 2: NSSM (Non-Sucking Service Manager)**:
  ```cmd
  nssm install literouter "C:\Program Files\literouter\literouter.exe" serve
  nssm set literouter AppDirectory "C:\Program Files\literouter"
  nssm start literouter
  ```

---

## 3. Docker Container Deployment

For containerized hosts (Synology/QNAP NAS, Unraid, Proxmox, or cloud servers):

### Quickstart with Docker Compose

**Pull the published image (recommended).** Every release has CI package the Linux binary it just published into an image on GHCR, so pulling takes seconds:

```bash
docker pull ghcr.io/yangqi0408/literouter:latest
```

The included [docker-compose.yml](../../docker-compose.yml) already points at it:

```yaml
services:
  literouter:
    image: ghcr.io/yangqi0408/literouter:latest
    container_name: literouter
    restart: unless-stopped
    # Strongly recommended: bind to host loopback 127.0.0.1
    ports:
      - "127.0.0.1:8787:8787"
    volumes:
      - ./config:/etc/literouter     # Config persistence (holds config.json)
      - ./state:/var/lib/literouter  # Telemetry persistence (holds telemetry-*.json)
    environment:
      - LITEROUTER_LANG=en
    security_opt:
      - no-new-privileges:true
    cap_drop:
      - ALL
    stop_grace_period: 30s
    healthcheck:
      test: ["CMD", "literouter", "status", "--json", "--quiet"]
      interval: 30s
      timeout: 5s
      retries: 3
```

**Commands**:
```bash
docker compose up -d
docker compose logs -f
```

> **On `docker compose build`**: the Dockerfile still supports building from source (`docker compose build` or `docker build .`), but it is not the recommended path — it downloads the full LLVM toolchain (~242 MB) inside the image, which takes about twelve minutes cold and fails outright when a fetch crosses mcpp's ten-minute xlings deadline. The published image exists to avoid that path. If you do build from source behind a slow network, give the build a proxy:
>
> ```bash
> docker build --network host \
>   --build-arg HTTP_PROXY=http://127.0.0.1:7890 \
>   --build-arg HTTPS_PROXY=http://127.0.0.1:7890 .
> ```

**First-run initialisation**:

The container does not create `config.json` for you. On the first start the config volume is empty, so `literouter serve` comes up on the built-in seed config (listening on `0.0.0.0:8787`, with a single example relay pointing at `api.openai.com`) and prints `config … does not exist; running with the built-in seed`. Turn that into a real config with either of these:

```bash
# Option 1: open http://127.0.0.1:8787/ui and add relays and routes there.
# Saving from the console writes the config into the volume, no CLI needed.

# Option 2: write the seed config from the CLI, then fill it in
docker compose exec literouter literouter config init
docker compose exec literouter literouter providers add --help
```

> `config init` refuses to overwrite an existing `config.json` (it asks for `--force`), so re-running it never clobbers a config you have already edited.

---

### Container Operational Notes

1. **Mount Config and State Separately**:
   - `/etc/literouter` contains your `config.json` with sensitive API keys;
   - `/var/lib/literouter` contains rotating metrics and request logs.
   *Avoid putting state on an anonymous volume, otherwise `docker compose down -v` wipes all historical metrics.*
2. **Bind Port to Loopback (`127.0.0.1`)**:
   The Web Console and management APIs expose request logs (which contain user prompts). When mapping port 8787, bind to `127.0.0.1:8787:8787` unless you configured a strong `server.api_key`.
3. **Changing Ports**:
   Change `server.port` in `config.json` and adjust the host port mapping (e.g. `-p 127.0.0.1:9000:9000`). The container command defaults to `--host 0.0.0.0`.
4. **Health Check**:
   The built-in health check executes `literouter status --json --quiet`, verifying both network readiness and internal routing state.

---

## 4. Reverse Proxy & Network Exposure (Nginx / Caddy)

When exposing `literouter` across a LAN or through a domain, use Nginx or Caddy for TLS termination and access control.

> ⚠️ **Critical Requirement**: LLM inference relies heavily on Server-Sent Events (SSE). **Your reverse proxy must disable response buffering**, otherwise Nginx holds streamed chunks until buffer thresholds are met, breaking smooth typewriter effects.

### Nginx Configuration (Must Disable Buffering)

```nginx
server {
    listen 443 ssl http2;
    server_name ai.example.com;

    ssl_certificate /path/to/fullchain.pem;
    ssl_certificate_key /path/to/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8787;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;

        # ⚡ Disable proxy buffering for instant SSE streaming
        proxy_buffering off;
        proxy_cache off;
        proxy_read_timeout 300s;
        proxy_send_timeout 300s;

        # HTTP/1.1 persistent connections
        proxy_http_version 1.1;
        proxy_set_header Connection "";
    }
}
```

### Caddy Configuration

Caddy natively recognizes and streams chunked responses:

```caddyfile
ai.example.com {
    reverse_proxy 127.0.0.1:8787 {
        transport http {
            keepalive 120s
        }
    }
}
```

---

## 5. Upgrades & Uninstallation

### Upgrades

- **Via Installer Script**:
  Re-running the installation script safely replaces the binary while preserving your configuration and telemetry data:
  ```bash
  curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash
  ```
  If running systemd, restart the service:
  ```bash
  sudo systemctl restart literouter
  ```

- **Manual Binary Replacement**:
  Download the latest binary from GitHub Releases and overwrite your existing executable.

### Uninstallation

```bash
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --uninstall
```
This safely disables and removes the systemd service and binary while preserving `/etc/literouter` and `/var/lib/literouter` against accidental data loss.

---

## 6. Runtime Footprint

`literouter`'s footprint is small. A cheap always-on box or a small cloud instance carries it comfortably: there is no database and no separate worker process, and resident memory sits in the single-digit megabytes, so it can run side by side with an editor, a browser and a local model client on the same machine.

The numbers below are **measured**, on a 12-logical-core / 13.8 GB Linux x86-64 machine running the `0.4.0` release build. The harness was a local mock upstream (no real inference time; ~0.1 ms per answer), a keep-alive client, and `/proc/<pid>/status` sampled every 200 ms.

### Idle and Concurrent Measurements

| Scenario | Throughput (req/s) | p50 latency | Resident (RSS) | Peak |
|---|---|---|---|---|
| **Idle (no traffic)** | — | — | **8.9 MB** | 8.9 MB |
| Buffered, 1 concurrent | 1,286 | 0.72 ms | 10.2 MB | 10.3 MB |
| Buffered, 8 concurrent | **5,012** | 1.5 ms | 10.3 MB | 10.3 MB |
| Buffered, 32 concurrent | 4,759 | 6.0 ms | 12.1 MB | 12.1 MB |
| Buffered, 128 concurrent | 4,955 | 5.9 ms | 12.2 MB | 12.2 MB |
| Streamed (SSE), 8 concurrent | 3,415 | 2.2 ms | 11.8 MB | 11.8 MB |
| Streamed (SSE), 32 concurrent | 3,059 | 9.5 ms | 13.0 MB | 13.0 MB |
| Streamed (SSE), 128 concurrent | 3,093 | 9.7 ms | 13.3 MB | 13.3 MB |

**What to take from this:**

- **Memory barely tracks concurrency.** Going from 1 to 128 concurrent clients moved RSS from 10.2 MB to 12.2 MB. The proxy does not buffer payloads: each body is pumped through as it arrives, so concurrency does not translate into memory.
- **Throughput saturates around 8 concurrent** (~5,000 req/s); more pressure only lengthens the queue. This is the ceiling of loopback against a mock upstream — a real relay's own latency becomes the bottleneck long before this.
- **It does not grow while it runs.** 60 seconds at 16 concurrent (about 80,000 requests) held RSS flat at 161.5 MB (the baseline was elevated because an 8 MB-body run preceded it), with a least-squares trend of about **+0.3 MB/minute** — allocator noise, not a leak. The telemetry ring is capped hard at `server.log_capacity` (measured at 200 and 400: the entry count matched exactly).
- A real LLM call takes seconds, so the numbers above leave several orders of magnitude of headroom.

### Threads and File Descriptors

| Item | Measured | Notes |
|---|---|---|
| Idle threads | **11** | The `max(8, cores - 1)` base worker pool |
| Peak threads under load | **35** | Grows to 4× the base pool when saturated |
| Idle shrink-back | 3 seconds | Returns to 11 once the burst ends |
| Idle FDs | **5** | Listening socket plus a few internal handles |
| FDs at 500 concurrent | **536** | Roughly "concurrency + 12" |

> ⚠️ **Raise the FD limit past about 1,000 concurrent requests.** Each in-flight request holds one client FD and one upstream FD, and Linux defaults `ulimit -n` to 1024. Set `LimitNOFILE=65535` in the systemd unit, or start under `ulimit -n 65535`, to go beyond that.
>
> Threads do **not** grow with concurrency: 500 concurrent requests still peak at 35 threads, with the rest queued. That queueing is why p99 latency climbs sharply under heavy load (7.4 s at 500 concurrent). A single-user gateway does not normally reach that range.

### Large Bodies and the Body-Logging Switch

**Large request bodies** multiply memory use, because a request's full body has to be held in memory while it is forwarded:

| Body size | Concurrency | RSS delta |
|---|---|---|
| 4 MB | 1 | +37.8 MB |
| 8 MB | 4 | +228 MB at peak |

That memory is released once the request finishes, and repeated runs do not accumulate it (four consecutive 8 MB runs left RSS oscillating in the 161–196 MB range with no upward trend). A single body is capped at **64 MB** (`kMaxRequestBody`).

> ⚠️ **`server.log_bodies` is a performance switch, not only a privacy one.** When it is on, every request and response body is truncated and stored in the ring buffer. Measured at 32 KB bodies and 8 concurrent: 1,514 req/s and p50 5.5 ms with it off, **17 req/s and p50 426 ms with it on — roughly 89× slower**. Leave it off (the default) and turn it on only to diagnose a specific request.

**Response caching** (`server.response_cache_ttl_sec`) lifts throughput on repeated requests at almost no memory cost: measured 5,000+ req/s at p50 0.7 ms, with entries bounded by `response_cache_max_entries`.

**Telemetry persistence** (`server.persist_telemetry`) produces a file whose size is set by `log_capacity`, not by total request count: about 408 KB at `log_capacity = 400`.
