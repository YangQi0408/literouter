# 更新日志 (Changelog)

所有重要的版本更新将在此处和 `docs/release-notes/` 专题文档中记录。
本项目遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/) 语义化版本规范。

---

## [0.5.0] - 2026-10-25

详细说明见 [docs/release-notes/v0.5.0.md](docs/release-notes/v0.5.0.md)。

### Added
- **桌面应用**：双击启动 → 网关在同一进程内启动 → 浏览器自动打开 Web 控制台 → 驻留系统托盘。程序没有自己的窗口，控制台就是界面。托盘菜单为 **Show**（打开控制台）与 **Exit**（停止网关并退出）。
- **macOS `.app` 应用包**：解压拖入「应用程序」即可双击运行，拥有独立 Dock 图标与可单独授权的 Gatekeeper 身份。
- **Windows 安装程序**：安装到 `Program Files`、生成开始菜单项、可从「应用和功能」卸载。
- **登录自启（显式 opt-in）**：`literouter-tray --autostart` / `--no-autostart` / `--autostart-status`。只写用户级位置（XDG autostart、`~/Library/LaunchAgents`、Windows 启动文件夹），可手工删除、无需提权，不写系统服务。
- 网关已在运行时，桌面应用**接管**该实例而非退出：打开它的控制台、驻留托盘，退出时不停止非自己启动的网关。

### Fixed
- **流式响应受 Nagle 算法拖慢**：服务端与上游两条 socket 均未设置 `TCP_NODELAY`（httplib 默认关闭）。本机回环实测，上游 0.1 ms 应答下缓冲式请求 42 ms → 0.7 ms，流式应答 42.15 ms → 0.90 ms。
- **TLS 证书路径以 UTF-8 字节传给 OpenSSL**：配置存 UTF-8，而 OpenSSL 的 PEM 加载在 Windows 上按 ANSI 代码页解码，导致非 ASCII 目录下的证书在绑定时失败且错误信息不含路径与原因。新增 `pathFromUtf8` / `pathForCApi` 区分两种转换。
- **Windows 上 stdout 处于 CRT 文本模式**：`--json` 输出中的 `
` 会被改写为 `\r\n`，破坏机器可读性。现两个标准流均设为二进制模式。

### Changed
- **裸 socket 测试分组在 Windows 上启用**：分组 12（悬挂连接）、23（请求超时）、20（连接复用）此前因依赖 POSIX socket 而被跳过，而这三条正是故障转移与连接池路径。新增 `core/tests/lr_test_socket.h` 提供跨平台 socket 词汇。
- `tray` 成为虚拟工作区成员（因其链接 `core`，而 `core` 使用 `workspace = true` 锁定依赖）。`mcpp build -p core -p cli` 仍只构建无界面部分。

### Notes
- 配置 schema 未变，无需迁移；已有 `config.json` 行为完全一致。
- 命令行网关与桌面应用相互独立，可分别安装，共用同一份配置。
- macOS Intel 仍未发布，`install.sh` 会明确拒绝并提示使用 Rosetta 或自行编译。
- 托盘菜单仅两项受限于 EUI-NEO 的托盘 API（三个后端均硬编码 Show/分隔线/Exit）。

## [0.4.0] - 2026-10-23

详细说明见 [docs/release-notes/v0.4.0.md](docs/release-notes/v0.4.0.md)。

### Added
- 新增中转站选项 `strip_responses_state`（默认关闭）：转发 `/v1/responses` 请求前剥离 Responses 的服务端状态——`input` 中每个 item 的 `id`、item 上任何 `encrypted*` 字段（加密思维链）以及顶层 `previous_response_id`。用于在多个 Azure OpenAI 资源间轮转且无会话粘性的中转站，避免会话落到其他资源时被拒为 400（`The requested item was created under a different Azure OpenAI resource`）。
- 剥离生效时请求日志记录剥离字段数（`… · stripped N Responses state fields`），缓冲与流式路径一致。
- 该选项在非 Responses 报文形态的中转站上会得到一条 `config validate` 告警，而不是静默无效。
- CLI 新增 `providers add --strip-responses-state`；`providers list --json` 输出该字段。
- Web 控制台在 `openai_responses` 中转站的编辑器中提供「无状态 Responses 请求」开关，中英文文案齐备。

### Changed
- 无字段可剥离的请求仍按客户端原始字节转发，同协议直通的零解析、零拷贝性质不变。

### Notes
- 该实现移植自 [codex-strip-proxy](https://github.com/AnnaofArendelle/codex-strip-proxy)（作者 AnnaofArendelle），署名见 `NOTICE`。
- 开启后模型不再跨轮接续隐藏思维链，每轮重新推理；对话内容与工具调用不受影响。

## [0.3.1] - 2026-09-27

详细说明见 [docs/release-notes/v0.3.1.md](docs/release-notes/v0.3.1.md)。

### Added
- Web 控制台的请求活动页新增请求路径、入站/出站协议、客户端 IP 与 User-Agent、请求与响应大小、输入/输出 Token、预估费用及本地缓存命中状态。
- 活动列表支持按缓存命中、故障转移和错误筛选，并汇总当前筛选结果中的 Token、预估费用与缓存命中数。
- 日志详情按请求、路由和用量分区展示，模型重命名、上游协议、三阶段耗时、故障转移和请求正文仍可完整追踪。

### Changed
- 核心日志条目新增 `method`、`path`、`ingress_protocol`、`client_ip`、`user_agent`、`upstream_protocol`、`request_bytes`、`prompt_tokens`、`completion_tokens`、`cost_usd` 与 `cache_status` 字段；管理 API 与 CLI 的 JSON 输出同步扩展，旧日志仍可读取。

## [0.3.0] - 2026-09-26

详细说明见 [docs/release-notes/v0.3.0.md](docs/release-notes/v0.3.0.md)。

### Added
- 新增 `routing_policy: "round_robin"` 平均分流策略，按请求在候选链的中转站之间轮换首选站，长期流量可自然接近均分。
- 同一中转站的模型回退链保持连续；熔断站仍排在健康站之后，会话亲和继续优先于轮换。
- Web 控制台增加“平均分流”路由策略选项和中英文说明。

### Fixed
- 修复同一中转站的多个模型回退候选在代理层可能被拆散的问题，并覆盖单站多模型回退的回归测试。
- 修复流式转换、请求转发和控制台工作流中的若干边界问题。

## [0.2.0] - 2026-09-25

详细说明见 [docs/release-notes/v0.2.0.md](docs/release-notes/v0.2.0.md)。

### Added
- 完整的个人本地聚合网关：Provider 路由、模型别名、故障转移、熔断、限流与本地响应缓存。
- 内置 Web 控制台继续随服务提供，支持遥测、Provider/路由管理、探测、日志和配置编辑。
- 支持 OpenAI、Anthropic、Gemini、Responses、Azure、Vertex、Bedrock 与 Ollama 协议组合。
- 提供跨平台 GitHub Release、Linux 安装脚本、Docker、Compose 与 systemd 部署方式。

### Changed
- 项目定位为每个人独立运行的单用户本地聚合器，仅使用一个 `server.api_key`。
- 移除桌面 GUI、客户端账户、按账户密钥、权限组、配额账本和每日预算等 API 分发功能。

## [0.1.0] - 2026-09-13

详细说明见 [docs/release-notes/v0.1.0.md](docs/release-notes/v0.1.0.md)。

### Added
- 初版发布：支持 OpenAI、Anthropic Claude、Google Gemini 多协议双向转换网关。
- 响应头闸门 (Header Gate) 流式无感故障转移机制与熔断自愈保护。
- 基于 React 19 + TypeScript + Vite + Tailwind CSS v4 + shadcn/ui 的内置 Web 控制台，支持概览看板、中转站健康探测、路由候选链调序、实时日志流与配置在线安全保存。
- 原生跨平台 CLI 命令行工具（支持前台 `serve`、实时看板 `status`、日志流跟踪 `logs`、配置工具 `config` 与环境体检 `doctor`）。
- 原生跨平台桌面 GUI 控制台（基于 EUI-NEO 与 OpenGL 硬件加速）。
- 环境变量占位符解析与密钥防泄漏持久化保障。
- 跨平台二进制分发：支持 Linux (x86_64)、macOS (Apple Silicon)、Windows (x86_64)，Linux 产物自动适配系统标准动态链接器与 GUI 运行时动态依赖库。
