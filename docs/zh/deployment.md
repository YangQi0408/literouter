# 部署与运维指南

`literouter` 是一个轻量级单用户网关：同一把 `server.api_key` 同时守护模型路由端点与管理接口。每个用户可以在自己的电脑、家庭服务器或云主机上长期稳定运行属于自己的聚合实例。

本文档详细介绍 `literouter` 的各种安装方式、系统后台自启配置、Docker 容器化部署、反向代理与更新维护实践。

---

## 目录

- [1. 快速安装方式](#1-快速安装方式)
  - [方式 A：一键安装脚本（推荐，Linux / macOS）](#方式-a一键安装脚本推荐linux--macos)
  - [方式 B：GitHub Releases 预编译包（Linux / macOS / Windows）](#方式-bgithub-releases-预编译包linux--macos--windows)
    - [macOS：使用 `.app`](#macos使用-app)
    - [Windows：使用安装程序](#windows使用安装程序)
    - [手动安装裸二进制](#手动安装裸二进制)
  - [方式 C：从源码构建](#方式-c从源码构建)
- [2. 后台常驻与自启动服务](#2-后台常驻与自启动服务)
  - [方案 1：Linux Systemd 系统服务（推荐服务器使用）](#方案-1linux-systemd-系统服务推荐服务器使用)
  - [方案 2：Linux Systemd 用户服务（免 root 权限）](#方案-2linux-systemd-用户服务免-root-权限)
  - [方案 3：macOS LaunchAgent 服务（登录自启）](#方案-3macos-launchagent-服务登录自启)
  - [方案 4：Windows 计划任务或后台运行](#方案-4windows-计划任务或后台运行)
- [3. Docker 容器化部署](#3-docker-容器化部署)
  - [使用 Docker Compose 快速部署](#使用-docker-compose-快速部署)
  - [容器运维核心注意事项](#容器运维核心注意事项)
- [4. 反向代理与网络暴露 (Nginx / Caddy)](#4-反向代理与网络暴露-nginx--caddy)
  - [Nginx 配置示例（必须关闭流式缓冲）](#nginx-配置示例必须关闭流式缓冲)
  - [Caddy 配置示例](#caddy-配置示例)
- [5. 升级与卸载](#5-升级与卸载)
  - [升级至最新版本](#升级至最新版本)
  - [卸载](#卸载)
- [6. 运行时资源占用](#6-运行时资源占用)
  - [空闲与并发实测数据](#空闲与并发实测数据)
  - [线程与文件描述符](#线程与文件描述符)
  - [大报文与日志开关的影响](#大报文与日志开关的影响)

---

## 1. 快速安装方式

### 方式 A：一键安装脚本（推荐，Linux / macOS）

官方提供自动化安装脚本，支持 Linux x86_64 与 macOS Apple Silicon，从 GitHub Releases 下载最新稳定版本并校验 SHA-256：

```bash
# 默认安装至 /usr/local/bin/literouter（需 sudo 权限）
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash

# 若无 root 权限，可安装到当前用户的 ~/.local/bin
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash -s -- --prefix ~/.local

# 同时安装并注册为 systemd 系统自启服务（仅 Linux）：
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --service -y
```

**脚本常用参数**：
- `--prefix DIR`：指定可执行文件安装目录（默认 `/usr/local`）；
- `--version TAG`：安装指定版本（如 `v0.3.0`，默认安装最新发布版 `latest`）；
- `--from PATH`：从本地二进制文件安装，跳过网络下载；
- `--service`：在 Linux 系统上自动创建专属系统用户 `literouter` 并启用 systemd 服务；
- `--uninstall`：卸载已安装的二进制与系统服务（保留配置和遥测历史）；
- `-y, --yes`：安装服务时不进行交互式二次确认。

---

### 方式 B：GitHub Releases 预编译包（Linux / macOS / Windows）

如果你偏好手动管理，直接前往 [GitHub Releases](https://github.com/YangQi0408/literouter/releases) 页面下载对应系统的产物。每个平台提供两种形式，**日常使用建议选带安装器的那个**：

| 平台 | 推荐形式 | 说明 |
|---|---|---|
| **macOS** | `literouter-<版本>-macos-arm64-app.zip` | `.app` 应用包，解压后拖进「应用程序」即可。双击启动，有独立的 Dock 图标与 Gatekeeper 身份 |
| **Windows** | `literouter-<版本>-windows-x86_64-setup.exe` | 安装程序。装到 `Program Files`、生成开始菜单项、可在「应用和功能」里卸载 |
| **Linux** | `literouter-<版本>-linux-x86_64.tar.gz` | 单个可执行文件（本项目的 Linux 惯例；AppImage 不再提供） |

同时仍提供裸二进制（`.tar.gz` / `.zip` 里的 `literouter`），供安装脚本与容器镜像使用。

#### macOS：使用 `.app`

1. 解压 `literouter-...-app.zip`，把 `literouter.app` 拖进 `/Applications`；
2. 首次启动若被 Gatekeeper 拦住（提示「无法验证开发者」），右键点图标选「打开」，或执行：
   ```bash
   xattr -dr com.apple.quarantine /Applications/literouter.app
   ```
3. 双击后会在终端窗口里以 `serve` 前台运行。

> `.app` 里的可执行文件就是同一个二进制，所以 `literouter` 的全部子命令仍可在终端直接调用。若已把二进制放进 `PATH`，用命令行即可，无需 `.app`。

#### Windows：使用安装程序

1. 运行 `literouter-...-setup.exe`；
2. 安装向导里有两个**默认不勾选**的选项：
   - **加入系统 PATH** —— 勾选后可在任意终端直接输入 `literouter`。不勾选是为了不覆盖你 PATH 上已有的同名程序；
   - **登录时启动网关** —— 勾选后创建计划任务。
3. 开始菜单会生成三个快捷方式：启动网关、查看状态、打开文档；
4. 卸载走「应用和功能」，卸载前会先尝试优雅停止正在运行的网关。

> 安装程序**不会**自动启动网关（`serve` 是前台阻塞进程，从安装向导里拉起会让向导一直等），也不会改防火墙规则（默认只绑回环，等你真的绑 `0.0.0.0` 时系统会自己弹提示）。

#### 手动安装裸二进制

如果你偏好手动管理：

1. **解压产物**：每个发布包仅包含一个独立的 `literouter`（Windows 上为 `literouter.exe`）二进制文件。内置 Web 控制台所有静态资源已在编译期内嵌（`#embed`），无需 Node.js 或任何前端依赖；
2. **放置到 PATH**：
   - **Linux / macOS**：
     ```bash
     chmod +x literouter
     sudo mv literouter /usr/local/bin/
     ```
   - **Windows**：将 `literouter.exe` 移动到某个常驻目录（例如 `C:\Program Files\literouter\` 或 `C:\Tools\`），并将该目录加入系统 `PATH` 环境变量；
3. **验证安装**：
   ```bash
   literouter --version
   ```

> 💡 **Windows 路径说明**：Windows 预编译包使用 MinGW GCC 编译，默认配置文件存放于 `%APPDATA%\literouter\config.json`（如 `C:\Users\<用户名>\AppData\Roaming\literouter\config.json`），受系统 `MAX_PATH` 规则约束。保持在默认路径下即可完美工作，无须特殊前缀。

---

### 方式 C：从源码构建

开发人员或特殊架构（如 Linux arm64）用户可从源码自行编译。

**构建前置依赖**：
- 现代 C++ 构建工具 [mcpp](https://github.com/mcpp-community/mcpp)；
- 编译器：LLVM/Clang（支持 C++23 modules，mcpp 会自动引导或管理工具链）；
- （可选）若修改了内置 Web 控制台源码，需要 Node.js 22+ 预先执行 `npm --prefix web run build`。

**编译步骤**：
```bash
git clone https://github.com/YangQi0408/literouter.git
cd literouter

# 一键全量编译整个工作区（core 静态库 + cli 命令行前端）
mcpp build --workspace --release

# 编译产物位于 target/<平台三元组>/<构建哈希>/bin/cli/literouter
# 可以直接运行或复制到系统的 PATH 路径中
```

---

## 2. 后台常驻与自启动服务

长期稳定使用时，建议将 `literouter` 注册为后台常驻服务，随开机自动启动，异常自动恢复。

### 方案 1：Linux Systemd 系统服务（推荐服务器使用）

项目仓库提供了经过严格安全沙盒加固的 systemd 单元文件 [deploy/literouter.service](../../deploy/literouter.service)。

1. **一键安装**：如果使用前述 `install.sh --service`，该步骤已自动完成。
2. **手动配置流程**：
   ```bash
   # 1. 创建独立系统账户
   sudo useradd --system --no-create-home --home-dir /var/lib/literouter --shell /usr/sbin/nologin literouter 2>/dev/null || true

   # 2. 创建配置目录与状态持久化目录并设置权限
   #    配置目录对服务组可写（0770）：服务保存配置时会在同一目录写临时文件再
   #    原子改名，因此需要「目录」本身可写，只给 config.json 文件写权限是不够的。
   sudo install -d -o root -g literouter -m 0770 /etc/literouter
   sudo install -d -o literouter -g literouter -m 0750 /var/lib/literouter

   # 3. 初始化配置文件（若尚不存在）
   if [ ! -f /etc/literouter/config.json ]; then
     sudo LITEROUTER_CONFIG=/etc/literouter/config.json literouter config init --force
     sudo chown literouter:literouter /etc/literouter/config.json
     sudo chmod 0640 /etc/literouter/config.json
   fi

   # 4. 安装服务文件并启动
   sudo cp deploy/literouter.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now literouter
   ```
3. **日常服务运维**：
   ```bash
   # 查看运行状态
   sudo systemctl status literouter

   # 查看系统日志
   sudo journalctl -u literouter -f

   # 重启服务
   sudo systemctl restart literouter
   ```

**安全特性**：
- `User=literouter`：以专用的低权限账户运行；
- `ProtectSystem=strict` 与 `ProtectHome=true`：将整机文件系统挂载为只读，仅放行两条写路径——`/var/lib/literouter` 遥测状态目录，以及 `/etc/literouter` 配置目录（Web 控制台「保存配置」依赖它）。配置目录仍由 `root` 拥有，仅对服务组可写（`0770`）；
- `NoNewPrivileges=true` / `PrivateTmp=true`：彻底杜绝提权隐患；
- 采用 `SIGTERM` 优雅停机（超时 30 秒），确保正在进行的流式应答平稳输出完毕，并将最新遥测计数完整刷写至磁盘。

> **想改成「配置对服务只读」？** 这是可选的强化姿态：在单元里把 `ReadWritePaths` 中的 `/etc/literouter` 去掉、再加回 `ReadOnlyPaths=/etc/literouter`（目录权限也回到 `0750 root:literouter`）。此时 Web 控制台的「保存配置」会返回 `write_failed`，请改用 `sudo` 编辑 `/etc/literouter/config.json`，再执行 `POST /__literouter/reload`、或重启服务、或开启 `server.reload_on_change` 来生效。

> **已经在跑旧版部署？** 配置目录的可写权限与单元都不会被自动更新，需要手动补齐一次：
> ```bash
> sudo chmod g+w /etc/literouter                       # 0750 -> 0770
> sudo cp deploy/literouter.service /etc/systemd/system/
> sudo systemctl daemon-reload && sudo systemctl restart literouter
> ```
> 之后 Web 控制台的「保存配置」即可用。

---

### 方案 2：Linux Systemd 用户服务（免 root 权限）

如果你在工作电脑或个人 Linux 桌面上使用，且没有 `sudo` 权限，可以部署为 systemd 用户服务：

1. 创建用户服务目录并写入服务配置：
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
2. 启用并启动服务：
   ```bash
   systemctl --user daemon-reload
   systemctl --user enable --now literouter
   ```
3. 检查状态：
   ```bash
   systemctl --user status literouter
   journalctl --user -u literouter -f
   ```
*(如需系统重启后未登录时也保持运行，可执行 `loginctl enable-linger $USER`)*。

---

### 方案 3：macOS LaunchAgent 服务（登录自启）

在 macOS 上，推荐使用 `launchd` 用户代理配置登录自动拉起。

仓库提供了自动安装脚本。它会安装二进制、生成 LaunchAgent、立即启动，并设置为登录时自动启动：

```bash
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-macos.sh \
  | bash -s -- --service
```

本地二进制安装：

```bash
bash scripts/install-macos.sh --from ./literouter --service
```

卸载后台服务和二进制（配置与状态会保留）：

```bash
bash scripts/install-macos.sh --uninstall
```

检查服务：

```bash
launchctl print "gui/$(id -u)/com.literouter.gateway"
curl http://127.0.0.1:8787/health
```

脚本也可以手动生成同样的 `~/Library/LaunchAgents/com.literouter.gateway.plist`，内容如下：
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
手动注册并加载：
   ```bash
   launchctl load ~/Library/LaunchAgents/com.literouter.gateway.plist
   ```
4. 如需停止或卸载：
   ```bash
   launchctl unload ~/Library/LaunchAgents/com.literouter.gateway.plist
   ```

---

### 方案 4：Windows 计划任务或后台运行

在 Windows 上，有多种简单可靠的开机后台驻留方式：

仓库提供 PowerShell 安装脚本，使用当前用户的任务计划程序注册登录自启，并立即启动：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-windows.ps1))) -Service
```

更稳妥的做法是先下载脚本，再执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-windows.ps1 -Service
```

从本地二进制安装并注册：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\install-windows.ps1 `
  -From 'C:\Program Files\literouter\literouter.exe' -Service
```

检查和卸载：

```powershell
Get-ScheduledTask -TaskName literouter
powershell -ExecutionPolicy Bypass -File .\scripts\install-windows.ps1 -Uninstall
```

- **方式 1：Windows 任务计划程序 (Task Scheduler)**：
  新建任务，触发器设置为“登录时”，操作设置为“启动程序”，选择 `literouter.exe`，参数填 `serve`。
- **方式 2：使用 NSSM 注册为系统服务**：
  ```cmd
  nssm install literouter "C:\Program Files\literouter\literouter.exe" serve
  nssm set literouter AppDirectory "C:\Program Files\literouter"
  nssm start literouter
  ```

---

## 3. Docker 容器化部署

如果你运行在群晖/威联通 NAS、Unraid、Proxmox 或云服务器的 Docker 环境中，可直接使用容器部署。

### 使用 Docker Compose 快速部署

**直接拉取官方镜像（推荐）**。每次发布都会由 CI 用当次发布的 Linux 二进制打包镜像并推送到 GHCR，因此拉取只需几秒：

```bash
docker pull ghcr.io/yangqi0408/literouter:latest
```

项目根目录提供了开箱即用的 [docker-compose.yml](../../docker-compose.yml)，它默认就指向这个镜像：

```yaml
services:
  literouter:
    image: ghcr.io/yangqi0408/literouter:latest
    container_name: literouter
    restart: unless-stopped
    # 强烈建议绑定到宿主机回环地址 127.0.0.1，避免无意暴露日志与提示词
    ports:
      - "127.0.0.1:8787:8787"
    volumes:
      - ./config:/etc/literouter     # 配置持久化目录（保存 config.json）
      - ./state:/var/lib/literouter  # 遥测持久化目录（保存 telemetry-*.json）
    environment:
      - LITEROUTER_LANG=zh           # 强制中文界面（可选）
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

**启动命令**：
```bash
docker compose up -d
docker compose logs -f
```

> **关于 `docker compose build`**：仓库里的 Dockerfile 仍然支持从源码构建（`docker compose build` 或 `docker build .`），但**不建议日常使用**——它需要在镜像内下载完整的 LLVM 工具链（约 242MB），一次冷构建约 12 分钟，且当某个依赖下载超过 mcpp 的 xlings 10 分钟期限时会直接失败。官方镜像就是为了绕开这条路径而存在的。若你确实要从源码构建且网络较慢，请给构建注入代理：
>
> ```bash
> docker build --network host \
>   --build-arg HTTP_PROXY=http://127.0.0.1:7890 \
>   --build-arg HTTPS_PROXY=http://127.0.0.1:7890 .
> ```

**首次启动的初始化**：

容器不会替你创建 `config.json`。第一次启动时配置卷是空的，`literouter serve` 会用内置的 seed 配置跑起来（监听 `0.0.0.0:8787`、只带一个指向 `api.openai.com` 的示例中转站），并打印一条 `config … does not exist; running with the built-in seed` 提示。接着任选一种方式把它写成真正的配置：

```bash
# 方式一：在浏览器里打开 http://127.0.0.1:8787/ui 添加中转站与路由。
# 控制台保存时会自动把配置写入卷，不需要任何命令行操作。

# 方式二：用命令行写入 seed 配置，再用 providers/routes 子命令补齐
docker compose exec literouter literouter config init
docker compose exec literouter literouter providers add --help
```

> `config init` 在 `config.json` 已存在时会拒绝覆盖（提示加 `--force`），所以重复执行不会冲掉已经编辑好的配置。

---

### 容器运维核心注意事项

1. **配置与状态卷必须分开持久化**：
   - `/etc/literouter`：存放 `config.json`（内含 API Key 等敏感配置），是需要手动维护和备份的文件；
   - `/var/lib/literouter`：存放运行时生成的遥测统计与环形请求日志文件。
   *若将其置于匿名卷，每次 `docker compose down -v` 都会意外抹除全部历史遥测数据。*
2. **端口映射建议绑定回环（`127.0.0.1`）**：
   容器内部服务默认监听 `0.0.0.0:8787`。内置的 Web 控制台与管理接口默认能读取请求日志（包含提示词）。若映射为 `0.0.0.0:8787`，请务必在 `config.json` 中配置强密码 `server.api_key`！
3. **改端口请修改配置文件**：
   容器 `CMD` 默认指定了 `--host 0.0.0.0`，实际监听端口跟随 `config.json` 中的 `server.port`。修改端口时只需更改配置并相应更改宿主机端口映射（例如 `-p 127.0.0.1:9000:9000`）。
4. **健康检查机制**：
   容器内置健康检查使用 `literouter status --json --quiet`，直接调用内部管理链路检测网关就绪状态，准确反映网关可用性。

---

## 4. 反向代理与网络暴露 (Nginx / Caddy)

当需要将本机的 `literouter` 暴露给局域网其他设备或通过公网域名访问时，推荐在本地 `127.0.0.1:8787` 前置配置 Nginx 或 Caddy 进行 TLS 终结与鉴权。

> ⚠️ **极为关键的一点**：LLM 对话补全极度依赖 SSE（Server-Sent Events）流式下发。**反向代理必须禁用响应缓冲（Response Buffering）**，否则 Nginx 会等待缓冲满后再批量下发，导致流式打字机效果失效、界面卡顿！

### Nginx 配置示例（必须关闭流式缓冲）

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

        # ⚡ 禁用代理缓存与缓冲，确保 SSE 流式秒级直通
        proxy_buffering off;
        proxy_cache off;
        proxy_read_timeout 300s;
        proxy_send_timeout 300s;

        # 支持 HTTP/1.1 长连接
        proxy_http_version 1.1;
        proxy_set_header Connection "";
    }
}
```

### Caddy 配置示例

Caddy 默认对流式响应具有良好的自动识别能力：

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

## 5. 升级与卸载

### 升级至最新版本

- **使用安装脚本更新**：
  直接重新执行安装脚本即可安全覆盖旧二进制，现有配置文件与历史遥测数据完全保留：
  ```bash
  curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash
  ```
  如果运行了 systemd 服务，随后执行一次重启：
  ```bash
  sudo systemctl restart literouter
  ```

- **手动更新二进制**：
  从 GitHub Releases 下载新二进制，覆盖旧的 `literouter` 路径即可。

### 卸载

若不再需要运行 `literouter`，可通过脚本安全移除：
```bash
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --uninstall
```
该命令会自动停止并移除 systemd 服务单元以及 `/usr/local/bin/literouter` 二进制文件，保留您的配置文件（`/etc/literouter`）与状态记录（`/var/lib/literouter`）以防误删。若需彻底删除配置与数据，可手动清理上述两个目录。

---

## 6. 运行时资源占用

`literouter` 的资源占用很小，一台常年在线的低配机器或小型云主机就足够承载。它没有数据库、没有独立的 worker 进程，常驻内存只有个位数的 MB，因此可以和编辑器、浏览器、本地模型客户端并排跑在同一台机器上。

以下数据为**实测值**，采集自 12 逻辑核 / 13.8 GB 内存的 Linux x86-64 机器，被测二进制为 `0.4.0` 发行构建。压测方式是：一个本地 mock 上游（不含真实模型推理时间，延迟约 0.1 ms），客户端与服务端保持 keep-alive 长连接，`/proc/<pid>/status` 每 200 ms 采样一次。

### 空闲与并发实测数据

| 场景 | 吞吐 (req/s) | p50 延迟 | 常驻内存 (RSS) | 峰值内存 |
|---|---|---|---|---|
| **空载（无流量）** | — | — | **8.9 MB** | 8.9 MB |
| 缓冲式，1 并发 | 1 286 | 0.72 ms | 10.2 MB | 10.3 MB |
| 缓冲式，8 并发 | **5 012** | 1.5 ms | 10.3 MB | 10.3 MB |
| 缓冲式，32 并发 | 4 759 | 6.0 ms | 12.1 MB | 12.1 MB |
| 缓冲式，128 并发 | 4 955 | 5.9 ms | 12.2 MB | 12.2 MB |
| 流式 (SSE)，8 并发 | 3 415 | 2.2 ms | 11.8 MB | 11.8 MB |
| 流式 (SSE)，32 并发 | 3 059 | 9.5 ms | 13.0 MB | 13.0 MB |
| 流式 (SSE)，128 并发 | 3 093 | 9.7 ms | 13.3 MB | 13.3 MB |

**要点**：

- **内存几乎不随并发增长**：从 1 并发到 128 并发，RSS 只从 10.2 MB 涨到 12.2 MB。代理本身不缓存报文，每条请求的正文是流式搬运的，因此并发数不直接换算成内存。
- **吞吐在 8 并发附近饱和**（约 5 000 req/s），继续加压不会更快，只会让排队变长。这是**本机 loopback + mock 上游**的上限，真实上游的延迟会远早于此成为瓶颈。
- **长时间运行不增长**：以 16 并发混合跑 60 秒（约 8 万请求），RSS 稳定在 161.5 MB 平线（该轮之前刚做过 8 MB 大报文测试，基线上浮），线性回归趋势约 **+0.3 MB/分钟**，属于内存分配器的正常抖动而非泄漏。遥测环形缓冲严格封顶在 `server.log_capacity`（实测设为 200 / 400 时条目数精确等于该值）。
- 真实场景中，一次 LLM 调用的耗时以秒计，上述吞吐数字留有几个数量级的余量。

### 线程与文件描述符

| 项目 | 实测值 | 说明 |
|---|---|---|
| 空闲线程数 | **11** | `max(8, CPU 核数 - 1)` 的基础工作线程池 |
| 高并发线程峰值 | **35** | 满载时动态扩展到基础池的 4 倍 |
| 空闲线程回落 | 3 秒 | 高峰过后自动回收，回到 11 |
| 空闲 FD 数 | **5** | 监听 socket + 少量内部句柄 |
| 并发 500 时的 FD 数 | **536** | 约等于「并发数 + 12」 |

> ⚠️ **并发超过约 1000 时需要调高 FD 上限**。每个在途请求占用一个客户端 FD 加一个上游 FD，Linux 默认 `ulimit -n` 是 1024。若要支撑上千并发，请在 systemd 单元里设置 `LimitNOFILE=65535`，或用 `ulimit -n 65535` 启动。
>
> 线程数**不会**随并发线性增长：500 并发时线程峰值仍是 35，请求在队列里排队。这也是 p99 延迟在高并发下显著变高的原因（500 并发时 p99 达 7.4 秒）——单用户网关的正常使用模式不会触及这个区间。

### 大报文与日志开关的影响

**大请求体**会按倍数放大内存占用，因为一条请求在转发期间需要在内存里持有完整正文：

| 请求体大小 | 并发 | RSS 增量 |
|---|---|---|
| 4 MB | 1 | +37.8 MB |
| 8 MB | 4 | 峰值 +228 MB |

这部分内存在请求结束后会被回收，多轮重复压测不会累积（实测 4 轮 8 MB 压测后 RSS 回到 161–196 MB 区间波动，无单调上升）。单条请求体的硬上限是 **64 MB**（`kMaxRequestBody`）。

> ⚠️ **`server.log_bodies` 是性能开关，不只是隐私开关**。开启后每条请求的请求体与响应体都会截断保存进环形缓冲。实测在 32 KB 请求体、8 并发下：关闭时 1 514 req/s、p50 5.5 ms；**开启后降到 17 req/s、p50 426 ms，慢约 89 倍**。日常请保持关闭（这也是默认值），只在排查具体请求时临时打开。

**响应缓存**（`server.response_cache_ttl_sec`）能显著提升重复请求的吞吐且几乎不占内存：实测 5 000+ req/s、p50 0.7 ms，缓存条目数受 `response_cache_max_entries` 严格限制。

**遥测持久化**（`server.persist_telemetry`）的文件大小由 `log_capacity` 决定，不随请求总量增长：`log_capacity = 400` 时状态文件约 408 KB。
