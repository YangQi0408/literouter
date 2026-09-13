# 更新日志 (Changelog)

所有重要的版本更新将在此处和 `docs/release-notes/` 专题文档中记录。
本项目遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/) 语义化版本规范。

---

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
