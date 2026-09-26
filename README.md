# literouter

<p align="center">
  <strong>基于 C++23 与 mcpp 构建的本地高性能 AI 中转站聚合器与多协议网关</strong><br>
  单本地端点 · 智能故障转移 · 响应头闸门 · 熔断自愈 · 跨协议零开销直通 · 密钥零泄露 · 原生 CLI 与嵌入式 Web 控制台
</p>

<p align="center">
  <a href="README_en.md">English</a> | <strong>简体中文</strong>
</p>

---

## 什么是 literouter？

`literouter` 是一个专为个人开发者与团队打造的高性能本地 AI 网关。它对外提供统一的本地端点（默认 `http://127.0.0.1:8787`），对内根据你配置的策略将请求分发至各个上游模型中转站与官方 API。

你只需将日常使用的各类 AI 客户端（如 **Cursor、Claude Desktop、VS Code (Continue/Cline/Roo Code)、Chatbox、NextChat、沉浸式翻译、Shell 脚本或 Python/Node.js SDK**）统一指向 `literouter`，它就会在毫秒级自动接管调度：**哪个中转站有对应模型、哪个站挂了/限流了、哪个站需要模型重命名映射、哪个模型最便宜，全部自动静默处理**。

```text
┌──────────────┐        ┌──────────────────────────────────────────────┐        ┌─────────────────────┐
│ 客户端        │  ───▶  │  literouter :8787                            │  ───▶  │ 中转站 A (优先/官方) │
│ (Cursor/     │  ◀───  │                                              │  ◀───  │                     │
│  Claude/     │        │  多协议适配 · 响应头闸门 · 熔断自愈保护        │        ├─────────────────────┤
│  OpenAI SDK) │        │  流式零拷贝 · 动态模型定价 · 端到端遥测大盘    │  ───▶  │ 中转站 B (低价/备用) │
└──────────────┘        └──────────────────────────────────────────────┘  ◀───  └─────────────────────┘
```

---

## 🚀 1 分钟快速安装

`literouter` 预编译二进制没有任何运行时依赖（内置 Web 控制台所有资源已内嵌打包），开箱即用：

### 方式 1：一键安装脚本（推荐，Linux / macOS）
```bash
# 自动探测系统架构，下载最新版本并安装至 /usr/local/bin/literouter（需 sudo 权限）
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash

# 若无 root 权限，可安装到当前用户的 ~/.local/bin：
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | bash -s -- --prefix ~/.local

# 同时安装并注册为 systemd 系统自启服务（Linux）：
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install.sh | sudo bash -s -- --service -y

# macOS 登录后自动驻留（Apple Silicon）：
curl -fsSL https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-macos.sh | bash -s -- --service

# Windows 登录后自动驻留（PowerShell）：
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/YangQi0408/literouter/main/scripts/install-windows.ps1))) -Service
```

### 方式 2：预编译二进制下载（Linux / macOS / Windows）
直接前往 [GitHub Releases](https://github.com/YangQi0408/literouter/releases) 页面下载适合你系统的发布包：
- **Linux (x86_64)**：解压后 `chmod +x literouter` 并移动到 `/usr/local/bin/`；
- **macOS (Apple Silicon / Intel)**：解压后移动至 `/usr/local/bin/` 或 `~/.local/bin/`；
- **Windows (x86_64)**：解压 `literouter.exe` 到任意常驻目录（如 `C:\Program Files\literouter\`），并将该目录添加到系统环境变量 `PATH`。

### 方式 3：Docker / Docker Compose 容器化运行
```bash
# 一行命令拉起（端口绑定回环，持久化配置与状态）
docker compose up -d
```

### 方式 4：从源码编译（开发者 / 特殊架构）
```bash
# 基于现代化构建系统 mcpp 一键全量构建
mcpp build --workspace --release
```

---

## ⚡ 3 步极速上手

### 第 1 步：启动网关
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

只需将客户端的 API 根地址指向 `http://127.0.0.1:8787` 即可无缝享受统一调度与故障转移。

| 客户端 / 工具 | 端点类型 | Base URL / 接口地址 | API Key 说明 |
|---|---|---|---|
| **Cursor** | OpenAI 兼容 | `http://127.0.0.1:8787/v1` | 填任意字符或 `server.api_key` |
| **VS Code (Continue)** | OpenAI / Anthropic | `http://127.0.0.1:8787/v1` | 填任意字符或 `server.api_key` |
| **VS Code (Cline / Roo Code)** | OpenAI Compatible | `http://127.0.0.1:8787/v1` | 填任意字符或 `server.api_key` |
| **Claude Desktop** | 原生 Anthropic | `http://127.0.0.1:8787` | 直通 `/v1/messages` |
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
- **最便宜路由策略 (`routing_policy: "cheapest"`)**：当开启此策略时，网关会根据当前请求的模型计算出各可用中转站的真实单价，并自动将单价最低的中转站排在最前面！
- **平均分流策略 (`routing_policy: "round_robin"`)**：平级中转站在请求之间轮流作为首选站，适合多家能力相近、希望长期均分流量而不是固定主备的场景；同一站的模型回退链保持连续。
- **可视化设置**：在 `/ui` Web 控制台中，可直观地为每个模型增删单价，并在花费大盘中实时监控每个站、每个模型的实际开销。

---

## 🛡️ 核心特性深析

- 🛡️ **响应头闸门 (Header Gate)**：独创流式安全机制。首包响应头发出前发生超时、网络故障、429 或 5xx 时无感切换下一个候选；一旦首字节开始下发，立即锁定该连接，**彻底杜绝回答流交叉拼接串扰**。
- 🔄 **多协议网关与零开销直通**：**入站**支持 OpenAI、Claude Messages、Google Gemini 与 OpenAI Responses 协议；**出口**支持 `openai`、`azure`、`anthropic`、`gemini`、`vertex`、`bedrock`、`ollama`、`responses` 八种协议。同协议请求**零 JSON 解析、零拷贝直通极速转发**，异构协议全自动双向转码。
- 🧠 **本地应答缓存**：对**非流式**请求提供本地精准匹配缓存（TTL + LRU 淘汰），命中直接从内存秒返，不消耗上游 token，真金白银省钱。
- 🚦 **中转站侧频控与熔断保护**：支持每中转站设置在途并发上限与 RPM 限制。达到上限的节点自动跳过尝试下一个，避免打崩上游；连续失败自动熔断并在冷却后探针探测自愈。
- 🔐 **密钥零泄露保障**：支持 `${OPENAI_API_KEY}` 环境变量引用，仅在发包瞬间在内存展开，无论控制台保存还是 CLI 修改，磁盘配置文件绝不回写明文。
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

本项目采用 C++23 与 [mcpp](https://github.com/mcpp-community/mcpp) 构建：

```bash
# 全量构建
mcpp build --workspace

# 运行全套核心单元测试（13 个套件全部通过）
mcpp test -p core
mcpp test -p cli
```

## 开源许可

本项目采用 [Apache-2.0 License](LICENSE) 协议开源。
第三方依赖及其许可证详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
