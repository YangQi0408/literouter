# literouter 项目转交与交接文档 (HANDOFF.md)

> **文档性质**：本文档用于将 `literouter` 项目的当前完整上下文、代码实现状态、核心架构约束、已知避坑点及后续演进路线，无缝移交给后续接手的 AI Agent 或开发人员。

---

## 1. 项目概览与核心定位

- **项目名称**：`literouter`
- **项目定位**：基于 C++23 与 `mcpp` 构建的**本地 AI 中转站聚合与智能路由服务**。
- **核心价值**：
  - 在本地对外暴露标准的 OpenAI 兼容 HTTP 端点（`http://127.0.0.1:8787/v1`）。
  - 对内自动聚合多个上游中转站（Relay Providers），实现模型名映射、有序故障转移（Failover）、熔断器保护（Circuit Breaker）、流式 SSE 透传、连通性体检与遥测统计。
  - 具备全功能 CLI 与基于 `eui-neo` (OpenGL) 的独立桌面图形控制台，实现开箱即用的多中转站管理与监控。
- **项目名称**：`literouter`
- **构建系统**：`mcpp` 虚拟 Workspace（包含 `core`、`cli`、`gui` 三个子包），统一锁定工具链为 `llvm@22.1.8`（Clang + libc++ + C++23 Modules）。

---

## 2. 当前交付状态与验证结果

经过 Core/Tests、CLI、GUI 三个独立轨道的并发开发与验证，**当前所有模块已全部实现并通过完整验证**，无遗留编译报错或测试失败。

### 2.1 模块状态总览

| 模块 | 类型 | 当前状态 | 依赖项 | 验证结果 |
|---|---|---|---|---|
| **`core`** | 静态库 / C++23 Module (`literouter.core`) | 100% 完成 | `compat:httplib` (tls, zlib), `nlohmann:json` | 10 组测试套件（共 1060+ 项断言）**全部 PASS (0 failed)** |
| **`cli`** | 可执行程序 (`literouter`) | 100% 完成 | `literouter:core`, `compat:CLI11` | 编译正常，全部 8 组子命令及协议选择、格式化/诊断通过测试 |
| **`gui`** | 可执行程序 (`literouter-gui`) | 100% 完成 | `literouter:core`, `compat:eui-neo` | 编译正常，OpenGL EUI-NEO 控制台 5 大页面完整就绪，Smoke 测试通过 |

### 2.2 核心测试运行记录 (2026-09-12 验证)

在仓库根目录下执行 `mcpp test -p core`，结果如下：

```text
test_config ... ok (0.02s) — 206 checks, 0 failed
test_i18n ... ok (0.02s) — 42 checks, 0 failed
test_json_api ... ok (0.02s) — 103 checks, 0 failed
test_router ... ok (0.02s) — 95 checks, 0 failed
test_protocol ... ok (0.02s) — 105 checks, 0 failed (双向多协议请求/响应/流式转换及同协议原样透传)
test_tls ... ok (0.02s) — 22 checks, 0 failed
test_secrets ... ok (0.02s) — 46 checks, 0 failed
test_util ... ok (0.04s) — 156 checks, 0 failed
test_proxy ... ok (0.36s) — 214 checks, 0 failed (包含端到端多协议适配、同协议零开销直通、流式重试等)
test_malformed ... ok (0.36s) — 76 checks, 0 failed (针对网络畸变、截断流、非 JSON 响应的鲁棒性测试)

test result ok. 10 passed; 0 failed
```

---

## 3. 系统架构与关键技术决策

### 3.1 单一公开契约：`literouter_core.cppm`

- **契约隔离原则**：`core/src/literouter_core.cppm` 是引擎向外暴露的**唯一公共接口**。
- **零泄漏**：该模块不向外部导出任何第三方库类型（`httplib` 与 `nlohmann::json` 均封装在内部实现单元 `.cpp` 中）。前端（CLI 和 GUI）只需 `import literouter.core;` 即可获得所有配置模型、路由状态和管理客户端。

### 3.2 故障转移与流式“响应头闸门 (Header Gate)”

- **流式不可逆性**：一旦流式回答（SSE）的第一个字节或 HTTP 响应头向客户端发出，该连接已无法回滚或重试，否则后续重试的 Token 会拼接在错误回答之后造成污染。
- **双线程管道设计**：
  - 流式请求会在独立的后台拉取线程中向上游发起请求；
  - 服务端连接线程等待“首包响应头到达”或“传输报错”；
  - **只有在首包响应头到达之前发生错误**（如连接断开、超时、429、5xx），代理才会关闭当前连接并尝试候选链上的下一个中转站；
  - 一旦收到 200 响应头，立即向客户端开放闸门下发流式分片，此后若中途断连直接终止，不再进行 failover。

### 3.3 状态码与熔断器的互斥完备性

- **非上游故障不熔断**：
  - 400/401/403/404/422 等客户端请求错误直接原样返回客户端，**不触发重试，也不计入中转站连续失败次数**；
  - 客户端主动断开连接计为 `aborted`，不归咎于中转站；
  - 只有真正的传输故障（TLS、超时、连接重置）以及可重试状态码（408, 409, 425, 429, 5xx）才计入 `failures` 并触发 failover。
- **指标守恒公式**：
  `requests == successes + failures + aborted`
- **软性熔断（冷却）**：
  - 达到连续失败阈值 `circuit_failure_threshold`（默认 3）后进入冷却期（默认 30s）；
  - 熔断期间该中转站被移至候选链末尾作为降级备选（避免全部中转站熔断时完全不可用）；
  - 冷却时间结束后首个请求作为探测流量（探针）。

### 3.4 密钥无损引用存储 (Secret Reference Preservation)

- 配置中的 `api_key` 支持环境变量占位符：`${VAR}`、`$VAR`、`${VAR:-fallback}` 或明文字面量。
- **只在请求发送瞬间由 `resolveSecret()` 解析展开**。
- `ConfigStore::save()` 保存时**永远保持原始占位符写法**，绝不会将展开的真实密钥写回磁盘，确保配置文件可安全提交或截图。

### 3.5 无锁线程独享连接池与 Keep-Alive 探针

- `httplib::Client` 是阻塞式且单线程绑定的，无法跨并发线程安全共享。
- `core/src/lr_upstream.cpp` 采用 `thread_local g_connection_pool`，每个工作线程为每个中转站维护独立的热连接，实现全生命周期无锁复用。
- 池的缓存键包含 `(base_url, timeout_sec, connect_timeout_sec, ca_bundle)`。
- Keep-Alive 连接在空闲超时后可能发生半开关闭。若发送时捕获 `Error::Connection`，判定为无副作用断开，丢弃失效连接并静默重试一次。

### 3.6 TLS CA 证书自适应探测

- 由于 `mcpp` 隔离编译的 OpenSSL 并不内置宿主机操作系统的根证书路径，`resolveCaBundle()` 会按优先级依次探测：
  1. `LITEROUTER_CA_BUNDLE`
  2. `SSL_CERT_FILE` / `CURL_CA_BUNDLE`
  3. 宿主机标准证书文件（如 `/etc/ssl/certs/ca-certificates.crt` 等）。
  确保 HTTPS 请求在无额外配置下直接可用。

---

## 4. 工程目录与源码映射

```
literouter
├── mcpp.toml                   # 虚拟 Workspace 清单，锁定依赖版本与 llvm@22.1.8
├── README.md                   # 详细的用户使用说明书与架构图
├── HANDOFF.md                  # 【本文档】项目移交与交接说明
├── AGENTS.md                   # 针对 AI Agent 的开发规范与硬性工作规则
│
├── core/                       # literouter.core 核心引擎模块
│   ├── mcpp.toml               # 核心库配置，启用 httplib 的 tls 与 zlib 特性
│   ├── src/
│   │   ├── literouter_core.cppm # 唯一公开 C++23 模块契约层（数据结构、类声明）
│   │   ├── lr_util.cpp         # 基础工具函数（脱敏、字符串、单位转换、CA 解析）
│   │   ├── lr_json.cpp         # nlohmann::json 序列化/反序列化（配置与遥测）
│   │   ├── lr_config.cpp       # ConfigStore 实现（原子写磁盘、配置校验）
│   │   ├── lr_router.cpp       # 纯决策逻辑（候选链排序、健康评估、熔断状态机）
│   │   ├── lr_upstream.cpp     # 单跳 HTTP/HTTPS 调用、连接池、连通性探针
│   │   └── lr_proxy.cpp        # ProxyServer 核心：监听循环、路由分发、SSE 桥接、管理端点
│   └── tests/                  # 测试套件（一个 .cpp 对应一个独立测试可执行文件）
│       ├── lr_test_check.h     # 简易断言宏与测试环境隔离守卫 (EnvGuard)
│       ├── test_util.cpp       # 字符串、格式化、路径工具测试
│       ├── test_secrets.cpp    # 密钥占位符解析与脱敏测试
│       ├── test_config.cpp     # 配置加载、原子写、校验规则测试
│       ├── test_router.cpp     # 候选链选择、熔断冷却、路由优先级测试
│       ├── test_tls.cpp        # 证书探测与环境变量覆盖测试
│       ├── test_proxy.cpp      # 端到端真实 Socket 代理、故障转移、SSE 流式测试
│       ├── test_json_api.cpp   # 快照、日志、探针 JSON 格式与 Usage 累计测试
│       └── test_malformed.cpp  # 异常上游、畸变报文与连接中断鲁棒性测试
│
├── cli/                        # literouter 命令行前端
│   ├── mcpp.toml               # CLI 包清单，依赖 compat:CLI11 与 literouter:core
│   └── src/
│       ├── cli_main.cpp        # 程序入口、全局选项注册、子命令路由
│       ├── cli_context.hpp     # CLI 运行上下文状态
│       ├── cli_ui.hpp/cpp      # 终端格式化、Box-drawing 边框表格、ANSI 颜色渲染
│       ├── cli_cmd_serve.cpp   # `serve` 前台运行代理服务
│       ├── cli_cmd_status.cpp  # `status` 查询正在运行的实例快照
│       ├── cli_cmd_models.cpp  # `models` 列出逻辑模型与故障转移链路
│       ├── cli_cmd_providers.cpp # `providers` 中转站管理 (list/add/remove/enable/disable/test)
│       ├── cli_cmd_routes.cpp  # `routes` 路由规则管理 (list/add/remove/enable/disable)
│       ├── cli_cmd_config.cpp  # `config` 配置查看、初始化、校验 (path/show/init/validate)
│       ├── cli_cmd_doctor.cpp  # `doctor` 6 步深度体检诊断
│       └── cli_cmd_logs.cpp    # `logs` 增量日志抓取与 `--follow` 实时滚动
│
└── gui/                        # literouter-gui 桌面控制台前端
    ├── mcpp.toml               # GUI 包清单，依赖 compat:eui-neo 与 literouter:core
    └── src/
        ├── gui_main.cpp        # EUI-NEO 应用入口、窗口配置、全局主循环驱动
        ├── app_state.h         # 进程内 ProxyServer 生命周期管理、异步任务线程、UI 响应式状态
        ├── components/
        │   ├── theme.h         # 配色盘（暗色现代控制台调色板）
        │   ├── shell.h         # 导航栏、侧边栏、状态指示器与标题布局
        │   └── widgets.h       # 统计卡片、开关、操作按钮、输入框组件
        └── pages/
            ├── overview.h      # 概览页：全局指标卡、中转站健康卡、最近活动流
            ├── providers.h     # 中转站管理页：列表、状态开关、模型关联
            ├── routes.h        # 路由管理页：模型路由表、优先级调整、备选链路可视化
            ├── logs.h          # 实时日志页：级别过滤、按字段检索、冻结/滚动控制
            ├── settings.h      # 服务设置页：端口/监听地址配置、配置热重载、cURL 示例生成
            └── overlays.h      # 弹窗层：中转站添加/编辑、一键探测、错误提示
```

---

## 5. 核心开发与验证指令速查

所有指令均在仓库根目录下执行：

### 5.1 编译与构建

```bash
# 构建整个 Workspace（包括 core, cli, gui）
mcpp build --workspace

# 单独构建各子包
mcpp build -p core
mcpp build -p cli
mcpp build -p gui
```

### 5.2 运行核心测试套件

```bash
# 运行 core 的所有单测与端到端集成测试
mcpp test -p core
```

### 5.3 运行 CLI 命令

```bash
# 查看帮助
mcpp run -p cli -- --help

# 环境诊断体检
mcpp run -p cli -- doctor

# 在指定端口前台运行
mcpp run -p cli -- serve --port 8787

# 查询运行状态 (需另开终端且有服务在运行)
mcpp run -p cli -- status

# 格式化导出配置
mcpp run -p cli -- config show
```

### 5.4 运行 GUI 控制台

```bash
# 启动 GUI（需要 X11/Wayland 桌面环境与 OpenGL 支持）
mcpp run -p gui
```

---

## 6. 关键踩坑点与绝对硬性约束 (Gotchas & Invariants)

接手本项目的 Agent 或开发者**必须时刻牢记以下限制**：

1. **绝对禁止创建 `src/main.cpp`**：
   - 在已显式声明 `[targets.*]`（如 `cli/mcpp.toml` 或 `gui/mcpp.toml`）的子包中，若存在 `src/main.cpp`，`mcpp` 构建工具会自动推断生成第二个 target，导致符号冲突并破坏构建。
   - 所有主入口必须命名为 `cli_main.cpp` 或 `gui_main.cpp`。
2. **头文件包含与 C++23 Module 引入顺序**：
   - 必须严格保持：**第三方头文件（如 `<CLI/CLI.hpp>`, `<eui_neo.h>`）在最前面 `#include`，然后才能 `import literouter.core;`**。
   - `literouter.core` 内部执行了 `export import std;`，因此消费端无需也不应重复 `import std;`。
3. **nlohmann 迭代器结构化绑定限制**：
   - `nlohmann::json` 的 `iteration_proxy_value` 的 `std::tuple_size` 特化位于 `nlohmann.json` 内部模块，在跨模块消费时直接使用结构化绑定（`for (auto [key, val] : ...)`）会触发 Clang 编译报错。
   - 必须使用标准迭代器遍历：
     ```cpp
     for (auto it = obj.begin(); it != obj.end(); ++it) {
         const auto& key = it.key();
         const auto& val = it.value();
     }
     ```
4. **多 Agent / 多任务并发构建隔离**：
   - `mcpp` 在每个 member 目录下生成专用的 `target/` 目录，多进程并发在该目录下执行 `mcpp build` 会产生文件锁冲突或破坏 build 缓存。
   - 如需多 Agent 并发开发，应各自在独立的 sandbox 临时目录（如 `/tmp/lr-worker`）中构建验证，验收无误后再同步回主工程。

---

## 7. 接手 Agent 后续工作建议 (Next Steps & Backlog)

如果继续对 `literouter` 进行功能扩展或二次开发，推荐按以下优先级进行：

### 7.1 优先级 P1：日志持久化与历史查询

- **现状**：当前 `LogEntry` 保存在内存环形队列（默认容量 400 条），服务进程重启后日志清空。
- **待办任务**：
  - 支持将日志追加写入本地文件（如 `~/.local/state/literouter/access.log`，格式为 JSONL）。
  - 或者集成 SQLite / DuckDB 实现多维度的历史请求查询与耗时排查。

### 7.2 优先级 P2：Token 计费与速率限制 (Cost & Rate Limiting)

- **现状**：目前已从上游响应中统计 `prompt_tokens` 与 `completion_tokens`，但未维护单价表。
- **待办任务**：
  - 在配置中允许为不同模型配置按千 Token 单价（Prompt/Completion）。
  - 增加按分钟请求数（RPM）和按分钟 Token 数（TPM）的主动限流防穿透策略。

### 7.3 多协议双向适配与零开销直通 (已完成 100%)

- **实现状态**：已完整支持客户端与服务端全链路多协议支持（OpenAI, Anthropic Claude Messages, Google Gemini, OpenAI Responses API）。
- **零开销直通原则 (Passthrough Invariant)**：
  - 当对外暴露的 Ingress 协议与上游 Provider 的 Egress 协议相同时（例如 Anthropic 客户端调用 Anthropic 上游，或 Gemini 客户端调用 Gemini 上游，或 OpenAI 调用 OpenAI），请求体、响应体以及 SSE 流式分片均**100% 零转换原样透传**，保证最高保真度、零延迟增加和全部专有字段完整保留。
  - 当 Ingress 与 Egress 协议不一致时，通过 Canonical Hub (OpenAI Chat 格式) 自动进行双向无缝转换，包括流式 SSE 事件的状态机映射。

### 7.4 优先级 P4：打包分发与系统服务集成

- **待办任务**：
  - 编写 Systemd User Service 配置文件模板（`literouter.service`），支持开机自启和后台常驻。
  - 编写一键 Release 构建脚本，生成剥离调试信息的干净发布包。

---

## 8. 接手者快速启动 Checklist

接手该项目的 Agent 可按如下顺序快速验证并确认接管：

1. [ ] **检视环境**：确认 `clang++ -v` 与 `mcpp --version` 正常可用。
2. [ ] **执行全量核心单测**：运行 `mcpp test -p core`，确保 8 组测试全绿。
3. [ ] **编译验证 CLI 与 GUI**：运行 `mcpp build --workspace` 确保零错误。
4. [ ] **阅读规范**：通读 [`AGENTS.md`](AGENTS.md) 了解代码与提交规范。
5. [ ] **开始开发**：从上述 Backlog 或用户下发的新需求中选取任务开展工作。
