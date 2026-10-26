# literouter

<p align="center">
  <strong>基于 C++23 与 mcpp 构建的本地高性能 AI 中转站聚合器与多协议网关</strong><br>
  单本地端点 · 故障转移 · 响应头闸门 · 熔断恢复 · 同形报文透传 · 环境变量密钥 · CLI、桌面托盘与内嵌 Web 控制台
</p>

<p align="center">
  <a href="README_en.md">English</a> | <strong>简体中文</strong>
</p>

---

## 什么是 literouter？

`literouter` 是一个面向个人开发者的本地 AI 网关。它对外提供统一的本地端点（默认 `http://127.0.0.1:8787`），对内根据你配置的策略将请求分发至各个上游模型中转站与官方 API。它是单用户工具：同一把 `server.api_key` 保护模型与管理接口，不提供账户、权限组或配额分发。

将支持自定义 API 地址、且协议匹配的 AI 客户端、编程扩展或 Python/Node.js SDK 指向 `literouter`，即可集中配置模型别名、候选上游、限流与故障转移策略。具体客户端和厂商扩展仍需按场景验证；价格来自你填写的价目表，不会自动获取官方账单。

```text
┌──────────────┐        ┌──────────────────────────────────────────────┐        ┌─────────────────────┐
│ 客户端        │  ───▶  │  literouter :8787                            │  ───▶  │ 中转站 A (优先/官方) │
│ (Cursor/     │  ◀───  │                                              │  ◀───  │                     │
│  Claude/     │        │  多协议适配 · 响应头闸门 · 熔断自愈保护        │        ├─────────────────────┤
│  OpenAI SDK) │        │  流式转发 · 模型价目表 · 请求遥测           │  ───▶  │ 中转站 B (低价/备用) │
└──────────────┘        └──────────────────────────────────────────────┘  ◀───  └─────────────────────┘
```

---

## 🚀 安装

`literouter` 有两种形态，先选一种再装：

| 形态 | 适合谁 | 长什么样 |
|---|---|---|
| **桌面应用** | 日常自己用 | 双击图标 → 自动起网关 + 浏览器打开控制台 → 驻留系统托盘 |
| **命令行网关** | 服务器 / 脚本 / 容器 | `literouter serve` 前台运行，供 systemd、Docker 或其他程序调用 |

两者共用同一份配置和同一套核心，装哪个都不影响另一个。

### 方式 1：桌面应用（推荐给个人用户）

去 [GitHub Releases](https://github.com/YangQi0408/literouter/releases) 下载对应平台的文件：

| 平台 | 下载 | 怎么用 |
|---|---|---|
| **macOS** (Apple Silicon，14+) | `literouter-<版本>-macos-arm64-app.zip` | 解压，把 `literouter.app` 拖进「应用程序」，双击；未公证版本需按系统提示允许打开 |
| **Windows** (x86_64) | `literouter-<版本>-windows-x86_64-setup.exe` | 运行安装程序，从开始菜单启动 |
| **Linux** (x86_64) | `literouter-<版本>-linux-x86_64.tar.gz` | 解压后运行其中的 `literouter-tray` |

启动后，在**同一进程内**启动网关（或接入已运行实例）、用浏览器打开实际监听地址的 `/ui/`、把图标放进托盘。默认地址为 `http://127.0.0.1:8787/ui/`；端口 `0` 与 HTTPS 使用实际端口及协议。程序自己没有窗口，**控制台就是它的界面**。

Linux 发布包的 `literouter-tray` 是启动器，使用 `lib/tray/` 下配套的加载器与共享库；请保留整个解压目录，不要只拷贝一个 tray 文件。需有 StatusNotifierWatcher（KDE，或 GNOME AppIndicator 扩展/支持托盘的面板）。CLI 仍是独立可执行文件，要求 glibc 2.38+。macOS 签名与公证是两件事，项目不把 ad-hoc/Developer ID 签名宣称为已公证。

以上打包改进描述当前源码的构建规则，**不会更新已经发布的 v0.5.0 文件**。公开发布前仍需三平台 CI 和原生桌面验证；验证步骤与平台边界见[构建和打包手册](scripts/README.md)。

托盘菜单只有两项（受底层托盘库限制）：

- **Show** — 打开控制台
- **Exit** — 停止网关并退出

> **首次运行**会在默认路径生成配置模板，无需手动创建。
>
> **如果网关已经在跑**（比如你自己开了一个 `literouter serve`），桌面应用会**接管**它：打开它的控制台、驻留托盘，退出时**不会**停掉那个不是它启动的网关。

#### 设为开机自启（可选，默认关闭）

桌面应用**不会**自己注册启动项。需要时显式执行：

```bash
literouter-tray --autostart         # 添加登录自启，然后继续运行
literouter-tray --no-autostart      # 移除
literouter-tray --autostart-status  # 只查询
```

它只写**用户级**位置，你可以手工删除、无需管理员权限：

| 平台 | 位置 |
|---|---|
| Linux | `~/.config/autostart/literouter.desktop` |
| macOS | `~/Library/LaunchAgents/io.github.yangqi0408.literouter.plist` |
| Windows | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\literouter.lnk` |

**不写系统服务、不改 PATH、不开防火墙、不动注册表 Run 键。**

### 方式 2：命令行安装脚本（Linux / macOS）

适合服务器或习惯命令行的用户。脚本安装的是 `literouter` CLI：

```bash
# 装到 /usr/local/bin（需要 sudo）
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash

# 装到当前用户目录，不需要 root
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash -s -- --prefix ~/.local

# 同时注册为 systemd 系统服务（Linux）
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --service -y

# 卸载（保留配置与遥测数据）
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --uninstall
```

**支持平台**：Linux x86_64、macOS Apple Silicon。Intel Mac 会被明确拒绝（提示用 Rosetta 或自行编译）。

> ⚠️ 脚本**不安装**桌面应用。想两者都要，用方式 1 装应用 + 方式 2 装 CLI。

### 方式 3：Windows 命令行安装（PowerShell）

```powershell
# 装到 %LOCALAPPDATA%\literouter
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-windows.ps1)))

# 同时注册为计划任务（登录自启）
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-windows.ps1))) -Service
```

如果你已经装了桌面应用，`--service` 这条路径仍然独立可用——它适合「不要托盘、开机就跑」的场景。

### 方式 4：Docker / Docker Compose

```bash
# 官方镜像，发布时由 CI 打包，秒级完成
docker pull ghcr.io/yangqi0408/literouter:latest

# 一行命令拉起（端口绑回环，配置与状态持久化）
docker compose up -d
```

> 镜像用当次发布的 Linux 二进制构建，不需要本地编译。若改用 `docker compose build` 从源码构建，需要下载完整 LLVM 工具链，冷构建约 12 分钟。

### 方式 5：从源码编译

```bash
# 全部构建：core 库 + CLI + 桌面应用
mcpp build --workspace --release

# 只要无界面的部分（快得多，不拉图形依赖）
mcpp build -p core -p cli --release
```

> 桌面应用依赖 EUI-NEO，会连带编译 GLFW、OpenGL、freetype、libpng、glib 一整套图形库，冷缓存下需要几分钟。只跑网关的话用第二条命令。

构建产物位置：

```text
target/<平台三元组>/<构建哈希>/bin/cli/literouter          # 命令行
target/<平台三元组>/<构建哈希>/bin/tray/literouter-tray    # 桌面应用
```

---

## ⚡ 3 步极速上手

### 第 1 步：启动网关

**装了桌面应用**：双击图标即可。网关会在同一进程内启动，浏览器自动打开控制台，图标进入托盘——直接跳到第 2 步。

**装的命令行版本**：
```bash
literouter serve
```
> 服务将在 `http://127.0.0.1:8787` 启动监听。如果是首次运行，它会自动在默认路径创建配置模板（Linux/macOS 为 `~/.config/literouter/config.json`，Windows 为 `%APPDATA%\literouter\config.json`）。

### 第 2 步：打开内置 Web 控制台配置中转站
浏览器打开：**`http://127.0.0.1:8787/ui/`**

在直观现代的 Web 控制台中，你可以：
- ➕ **界面化增删中转站**：输入上游名称、Base URL、API Key（支持环境变量占位符 `${KEY}`）、协议类型（OpenAI / Anthropic / Gemini / Azure / Vertex / Bedrock / Ollama）；
- 💰 **配置计费与模型定价**：可设定全局默认输入/输出单价，更支持**为每个模型单独指定单价**；
- 🔄 **管理路由与映射**：指定请求模型（如 `gpt-4o`）优先使用哪个站，并在失败时自动切到备用站；
- 🩺 **一键连通性探测**：即时测试各个上游中转站的连通性与可用模型。

> 💡 *偏好命令行的用户也可以完全通过 CLI 完成配置：`literouter providers add <id> --base-url <url> --key-env <VAR>`。*

### 第 3 步：发送测试请求
```bash
curl http://127.0.0.1:8787/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o","messages":[{"role":"user","content":"Hello!"}]}'
```
看到模型应答，说明网关已成功调度并完成了中转！

---

## 🔌 常用客户端与 AI 工具接入指南

下表是支持自定义地址时的接入参考，不是所有客户端版本均通过实测的保证。设置了 `server.api_key` 时必须填写它；只有本地访问密钥为空时才可填任意占位符。

| 客户端 / 工具 | 端点类型 | Base URL / 接口地址 | API Key 说明 |
|---|---|---|---|
| **Cursor** | OpenAI 兼容 | `http://127.0.0.1:8787/v1` | 填任意字符或 `server.api_key` |
| **VS Code (Continue)** | OpenAI / Anthropic | `http://127.0.0.1:8787/v1` | 填任意字符或 `server.api_key` |
| **VS Code (Cline / Roo Code)** | OpenAI Compatible | `http://127.0.0.1:8787/v1` | 填任意字符或 `server.api_key` |
| **支持自定义地址的 Anthropic SDK / 工具** | 原生 Anthropic | `http://127.0.0.1:8787` | `/v1/messages`；依客户端支持配置 |
| **NextChat / Chatbox** | OpenAI 兼容 | `http://127.0.0.1:8787` | 填任意字符或 `server.api_key` |
| **沉浸式翻译 / Cherry Studio** | OpenAI 兼容 | `http://127.0.0.1:8787/v1` | 填任意字符或 `server.api_key` |
| **Python / Node.js SDK** | OpenAI 官方库 | `http://127.0.0.1:8787/v1` | 参见下文代码示例 |

### Python OpenAI SDK 示例
```python
from openai import OpenAI

client = OpenAI(
    base_url="http://127.0.0.1:8787/v1",
    api_key="none",  # 若 server.api_key 为空则可填任意非空字符串
)

response = client.chat.completions.create(
    model="gpt-4o",
    messages=[{"role": "user", "content": "你好！"}],
    stream=True,
)
for chunk in response:
    print(chunk.choices[0].delta.content or "", end="")
```

### Continue (VS Code 扩展) `config.yaml` 示例
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

## 💰 灵活计费与按模型价格管理

在大模型调用中，不同中转站、同一中转站下的不同模型（如 `gpt-4o` 与 `gpt-4o-mini`，或 `claude-3-5-sonnet` 与 `claude-3-5-haiku`）价格往往差异极大。

`literouter` 提供了细粒度的**全局兜底定价**与**特定模型独立定价**机制：
- **全局默认单价**：在中转站上配置 `price_in_per_million` 与 `price_out_per_million`，作为该中转站所有模型的基准单价；
- **按模型单独定价 (`model_prices`)**：可为该站下的特定模型单独设置输入/输出单价；未单独配置的模型自动回退到全局默认单价；
- **最便宜路由策略 (`routing_policy: "cheapest"`)**：根据当前模型在各候选中转站的已配置单价排序，不自动获取账单；会话亲和优先。
- **平均分流策略 (`routing_policy: "round_robin"`)**：平级中转站轮换首选；显式配置的同站模型回退链保持连续。`providers[].models` 仅是目录，不会隐式更换模型。
- **可视化设置**：在 `/ui` 为模型填写单价，结合上游报告的用量查看费用估算；未上报/未填价的部分不计入。

---

## 🛡️ 核心特性深析

- 🛡️ **响应头闸门 (Header Gate)**：向客户端提交响应前发生超时、网络故障、429 或可重试 5xx 时尝试后续候选；一旦响应开始下发，不再切换上游，避免拼接两个回答。
- 🔄 **多协议网关与同形报文透传**：**入站**支持 OpenAI、Claude Messages、Google Gemini 与 OpenAI Responses；**出口**支持 `openai`、`azure`、`anthropic`、`gemini`、`vertex`、`bedrock`、`ollama`、`openai_responses`。同报文形态不做协议转换，但路由/校验仍解析请求、SSE 使用有界队列；不是全链路零解析或零拷贝。
- 🧠 **本地应答缓存**：对**非流式**请求提供本地精准匹配缓存（TTL + LRU 淘汰），命中直接从内存秒返，不消耗上游 token，真金白银省钱。
- 🚦 **中转站侧频控与熔断保护**：支持每中转站设置在途并发上限与 RPM 限制。达到上限的节点自动跳过尝试下一个，避免打崩上游；连续失败自动熔断并在冷却后探针探测自愈。
- 🔐 **密钥引用与脱敏**：支持 `${OPENAI_API_KEY}` 引用，保存保留原始引用，管理接口隐藏已保存的专用凭据。允许用户直接保存明文密钥，因此不承诺所有配置/日志“零泄露”。上游重定向默认拒绝，请填写最终 Base URL。
- 💻 **双前端协同**：
  - **CLI 命令行**：支持前台运行、`tail -f` 风格实时日志跟踪、状态看板与环境体检 (`doctor`)；
  - **Web 控制台**：`serve` 启动后同一端口自带 `/ui` 现代 Web 控制台，基于 React + Vite + Tailwind + shadcn/ui 构建，资产编译期内嵌，零额外部署。

---

## 🛠️ 常用 CLI 命令速查

```bash
literouter serve                 # 启动前台网关代理服务（默认端口 8787）
literouter status                # 查看当前网关状态与健康看板
literouter logs -f               # 实时跟踪请求日志（类似 tail -f）
literouter doctor                # 运行环境、网络连通性与配置深度体检
literouter models                # 查看聚合后对外提供的所有模型与路由拓扑
literouter providers list        # 列出所有已配置的上游中转站
literouter providers add <id>    # 添加新中转站（支持 --model-price 等参数）
literouter routes add <model>    # 配置显式模型路由与候选链路
literouter bench --model gpt-4o  # 对比各中转站在同一模型上的真实延迟与费用
literouter config validate       # 深度校验配置文件的正确性与安全性
```

### 桌面应用（托盘版）

`tray/` 是 **literouter 的桌面形态**：双击打开 → 网关在**同一进程内**启动 →
浏览器自动打开内置 Web 控制台 → 程序驻留系统托盘。它自己没有窗口，控制台就是它的界面。

```bash
mcpp build -p tray
./target/*/*/bin/tray/literouter-tray
```

托盘菜单只有 **Show**（打开控制台）与 **Exit**（停止网关并退出）两项，这是
EUI-NEO 托盘 API 的硬限制，不是本项目的取舍。网关随程序生命周期启停。

**自启动是可选的，且默认不写入。** 需要时显式执行 `literouter-tray --autostart`，
它只写**用户级**位置（Linux `~/.config/autostart`、macOS `~/Library/LaunchAgents`、
Windows 启动文件夹），可手工删除、无需提权，不碰系统服务：

```bash
literouter-tray --autostart         # 添加登录自启，然后继续运行
literouter-tray --no-autostart      # 移除，然后继续运行
literouter-tray --autostart-status  # 只查询状态
```

> 想要**无界面常驻**（开机即起、不依赖登录会话），那是 CLI 的显式选项：
> `install.sh --service`（Linux）或 `install-windows.ps1 -Service`（Windows）。
> 两条路径互相独立。

细节、跨平台实现与限制见 [`tray/README.md`](tray/README.md)。

---

## 📚 详细专题文档导航

| 专题文档 | 内容概述 |
|---|---|
| 📖 [**配置文件与密钥管理**](docs/zh/configuration.md) | 完整 JSON 字段表、多模型定价、安全占位符语法与热重载 |
| 📦 [**部署与运维指南**](docs/zh/deployment.md) | 一键脚本参数、systemd 系统/用户服务、macOS launchd、Docker 与 Nginx 反代 |
| 🛡️ [**路由与故障转移机制**](docs/zh/routing-failover.md) | 候选链推导、响应头闸门原理、会话粘性、熔断器状态机 |
| 🔄 [**接口与协议规范**](docs/zh/protocols-api.md) | 各种客户端端点、上游协议适配、同协议直通与 Admin API |
| 💻 [**命令行工具 (CLI) 手册**](docs/zh/cli.md) | 所有 CLI 子命令参数、实时看板监控与脚本自动化 |
| ⚙️ [**环境变量参考手册**](docs/zh/environment.md) | 系统环境变量完整列表及跨平台路径解析 |

---

## 贡献与源码构建

本项目采用 C++23 与 [mcpp](https://github.com/mcpp-community/mcpp) 构建。虚拟工作区包含三个成员：`core`（库）、`cli`（命令行）、`tray`（桌面应用）。

```bash
# 全量构建（三者都构建）
mcpp build --workspace

# 只构建无界面部分，跳过图形依赖
mcpp build -p core -p cli

# 运行全套核心与 CLI 单元测试
mcpp test -p core
mcpp test -p cli
```

> `tray` 依赖 EUI-NEO，会连带编译 GLFW / OpenGL / freetype / libpng / glib。只改核心逻辑时用 `-p core -p cli` 能省下几分钟。

## 开源许可

本项目采用 [Apache-2.0 License](LICENSE) 协议开源。
第三方依赖及其许可证详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
