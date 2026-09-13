# literouter

<p align="center">
  <strong>基于 C++23 与 mcpp 构建的本地高性能 AI 中转站聚合器与多协议网关</strong><br>
  单本地端点 · 智能重试 · 响应头闸门 · 熔断自愈 · 跨协议直通 · 密钥零泄露 · 原生 CLI 与 GUI
</p>

<p align="center">
  <a href="README_en.md">English</a> | <strong>简体中文</strong>
</p>

---

## 什么是 literouter？

`literouter` 是一个运行在本地的高性能 AI 代理聚合器。它对外暴露统一的本地端口（默认 `http://127.0.0.1:8787`），对内将请求按你设定的策略分发至多个上游模型中转站与官方 API。

你只需将各类常用客户端（Chatbox、NextChat、Cursor、沉浸式翻译、Shell 脚本或 Python SDK）统一指向 `literouter`，由它在毫秒级自动接管调度：**哪个中转站有对应模型、哪个挂了、哪个被限流了、哪个需要重命名映射，全部自动静默处理**。

```text
┌──────────┐        ┌──────────────────────────────────────────┐        ┌─────────────────┐
│ 客户端    │  ───▶  │  literouter :8787                        │  ───▶  │ 中转站 A (优先)  │
│ (OpenAI/ │  ◀───  │                                          │  ◀───  │                 │
│  Claude/ │        │  多协议适配 · 响应头闸门 · 熔断自愈保护    │        ├─────────────────┤
│  Gemini) │        │  流式零拷贝 · 动态重命名 · 端到端遥测      │  ───▶  │ 中转站 B (备用)  │
└──────────┘        └──────────────────────────────────────────┘  ◀───  └─────────────────┘
```

---

## 核心特性

- ⚡ **单端点聚合**：统一管理多个中转站的 Base URL 与 API Key，客户端无需频繁切换配置。
- 🛡️ **响应头闸门 (Header Gate)**：独创流式安全机制。首包响应头发出前发生网络错误、超时、429 或 5xx 无感切换下一个候选；首包下发后立即锁定连接，**彻底杜绝回答流交叉拼接串扰**。
- 🔌 **熔断与探针自动恢复**：连续失败触发中转站熔断降级；冷却期后自动放行单请求探针，验证成功即刻满血复活。
- 🔄 **多协议网关与零开销直通**：入站支持 OpenAI、Claude Messages、Google Gemini 与 OpenAI Responses 协议；同协议请求享受**零 JSON 解析、零拷贝直通极速转发**，异构协议自动双向无缝转换。
- 🔐 **密钥零泄露安全占位符**：配置中支持 `${OPENAI_API_KEY}` 或 `${VAR:-fallback}` 环境变量引用；仅在实际发起网络请求瞬间内存解析，保存配置时绝对不回写明文。
- 💻 **双前端协同**：
  - **CLI 命令行**：支持前台运行、`tail -f` 风格实时日志跟踪、状态看板与环境体检 (`doctor`)。
  - **GUI 桌面控制台**：基于 OpenGL 的原生桌面应用，提供实时指标磁贴、中转站健康矩阵、可视化拖拽调序、一键探测并同步模型。
- 🌐 **原生跨平台与国际化**：支持 Linux、macOS、Windows；界面支持中英双语无缝切换与 80%~150% 动态矢量缩放。

---

## 快速开始

### 1. 编译构建
本项目基于现代化构建系统 [mcpp](https://github.com/mcpp-community/mcpp)，一行命令完成全工作区构建：

```bash
mcpp build --workspace
```

### 2. 初始化配置与体检
```bash
# 生成默认配置文件模板 (~/.config/literouter/config.json)
mcpp run -p cli -- config init

# 配置环境变量（示例配置默认引用）
export OPENAI_API_KEY="sk-..."

# 运行 doctor 进行环境与网络连通性体检
mcpp run -p cli -- doctor
```

### 3. 启动代理服务
```bash
# 方式 A：启动前台 CLI 代理服务
mcpp run -p cli -- serve

# 方式 B：启动带硬件加速的原生桌面控制台 (GUI)
mcpp run -p gui
```

### 4. 发起调用验证
```bash
curl http://127.0.0.1:8787/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"model":"gpt-4o","messages":[{"role":"user","content":"Hello!"}]}'
```

---

## 简明配置预览

配置文件默认存放于 `~/.config/literouter/config.json`（Windows 为 `%APPDATA%\literouter\config.json`）：

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
      "name": "OpenAI 官方直连",
      "base_url": "https://api.openai.com/v1",
      "api_key": "${OPENAI_API_KEY}",
      "priority": 10,
      "models": ["gpt-4o", "text-embedding-3-small"]
    },
    {
      "id": "relay-backup",
      "name": "备用中转站",
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

## 详细专题文档

为了保持架构与查阅的清晰性，核心技术细节已拆分为独立专题文档：

| 专题文档 | 描述 |
|---|---|
| 📖 [**配置文件与密钥管理**](docs/zh/configuration.md) | 完整 JSON 配置字段规范、安全占位符语法、规则校验与热重载 |
| 🛡️ [**路由与故障转移机制**](docs/zh/routing-failover.md) | 候选链推导算法、响应头闸门 (Header Gate) 原理、熔断器状态机 |
| 🔄 [**接口与协议规范**](docs/zh/protocols-api.md) | 客户端入口端点、上游协议适配、同协议零开销直通与 Admin API |
| 💻 [**命令行工具 (CLI) 手册**](docs/zh/cli.md) | 全量 8 个子命令参数用法、状态看板监控与自动化脚本示例 |
| 🖥️ [**桌面控制台 (GUI) 使用指南**](docs/zh/gui.md) | 五大功能面板说明、模型自动探测、界面缩放快捷键与字体回退 |
| ⚙️ [**环境变量参考手册**](docs/zh/environment.md) | 系统环境变量完整列表及 Linux / macOS / Windows 跨平台路径 |

---

## 构建与测试

### 环境依赖
- 操作系统：Linux / macOS / Windows
- 构建工具：[mcpp](https://github.com/mcpp-community/mcpp)
- 编译器：Clang `llvm@22.1.8`（启用 `-std=c++23` 与 libc++，mcpp 自动纳管）

### 常用命令
```bash
# 运行核心单测套件（10 组套件全部通过）
mcpp test -p core

# 独立构建各个组件
mcpp build -p core
mcpp build -p cli
mcpp build -p gui

# GUI 自动化无头冒烟测试
LITEROUTER_GUI_SMOKE=1 mcpp run -p gui
```

---

## 开源许可

本项目采用 [Apache-2.0 License](LICENSE) 协议开源。
第三方依赖及其许可证详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

