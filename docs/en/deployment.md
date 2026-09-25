# Deployment & Operations Guide

`literouter` is a lightweight single-user gateway: one `server.api_key` guards both model routing endpoints and administrative interfaces. Any user can run and maintain their own reliable aggregator instance on a workstation, home server, or cloud host.

This document covers installation methods, background service setup (systemd / launchd / Windows), Docker containerization, reverse proxying, and maintenance best practices.

---

## Table of Contents

- [1. Installation Options](#1-installation-options)
  - [Option A: One-Line Installer Script (Recommended for Linux / macOS)](#option-a-one-line-installer-script-recommended-for-linux--macos)
  - [Option B: GitHub Releases Prebuilt Binaries (Linux / macOS / Windows)](#option-b-github-releases-prebuilt-binaries-linux--macos--windows)
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

---

## 1. Installation Options

### Option A: One-Line Installer Script (Recommended for Linux / macOS)

The repository provides an automated installation script that automatically detects your OS architecture (Linux x86_64, macOS Apple Silicon / Intel), downloads the latest release from GitHub Releases, and verifies its SHA-256 checksum:

```bash
# Default install to /usr/local/bin/literouter (requires sudo)
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash

# Non-root installation to ~/.local/bin
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash -s -- --prefix ~/.local

# Install and register as a systemd service automatically (Linux):
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --service -y
```

**Common Script Flags**:
- `--prefix DIR`: Installation destination directory (default `/usr/local`);
- `--version TAG`: Install a specific release tag (e.g. `v0.2.0`, defaults to `latest`);
- `--from PATH`: Install from a local pre-downloaded binary instead of fetching from GitHub;
- `--service`: Create a dedicated unprivileged user `literouter` and enable the systemd service (Linux);
- `--uninstall`: Stop and remove the binary and systemd service (preserves configuration and telemetry);
- `-y, --yes`: Non-interactive mode without confirmation prompts.

---

### Option B: GitHub Releases Prebuilt Binaries (Linux / macOS / Windows)

For manual installation, head over to the [GitHub Releases](https://github.com/YangQi0408/literouter/releases) page and download the archive for your operating system:

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

# The executable is produced at cli/target/<triple>/<hash>/bin/literouter
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
   sudo install -d -o root -g literouter -m 0750 /etc/literouter
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
- `ProtectSystem=strict` and `ProtectHome=true`: file system is mounted read-only except for state directory `/var/lib/literouter`;
- `NoNewPrivileges=true` and `PrivateTmp=true` block privilege escalation;
- Uses graceful `SIGTERM` stop with 30s timeout to allow in-flight streaming requests to finish cleanly.

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

1. Ensure `literouter` is installed in `/usr/local/bin/` or `~/.local/bin/`.
2. Create `~/Library/LaunchAgents/com.literouter.gateway.plist`:
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
3. Load the service:
   ```bash
   launchctl load ~/Library/LaunchAgents/com.literouter.gateway.plist
   ```

---

### Setup 4: Windows Task Scheduler or Service

On Windows:
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

Use the included [docker-compose.yml](../../docker-compose.yml):

```yaml
services:
  literouter:
    image: literouter:local
    build: .
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

