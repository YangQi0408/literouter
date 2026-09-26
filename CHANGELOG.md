# 更新日志 (Changelog)

所有重要的版本更新将在此处和 `docs/release-notes/` 专题文档中记录。
本项目遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/) 语义化版本规范。

---

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
