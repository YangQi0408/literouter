# AGENTS.md — literouter 项目 Agent 操作指南

本文件是任何 AI Agent（如 Claude, Pi, Codex, Cursor 等）在进入本仓库执行任务时的**最高优先级开发规范与操作守则**。在进行任何代码修改、构建或测试前，请务必完整阅读并严格遵守。

---

## 1. 项目定位与工程结构

`literouter` 是一个基于 C++23 和 `mcpp` 构建的本地 AI 中转站聚合器（API Relay Aggregator）。它对外提供单一的本地 OpenAI 兼容端点（`http://127.0.0.1:8787/v1`），对内将请求分发至多个中转站，支持模型重命名、优先级故障转移、熔断、SSE 流式透传以及遥测统计。

工程采用 `mcpp` 虚拟工作区（Virtual Workspace），划分为三个子成员（Members）：

```text
literouter/
├── mcpp.toml                   # 虚拟工作区总清单，锁定工具链与第三方依赖
├── AGENTS.md                   # 【当前文件】Agent 操作指南；CLAUDE.md 是指向它的符号链接
├── README.md                   # 中文项目总览（默认）
├── README_en.md                # 英文项目总览
├── CHANGELOG.md                # ⚠️ 改动会触发自动发版，见规则 7
├── docs/                       # 专题技术与使用文档
│   ├── zh/                     # 中文详细文档（configuration, routing-failover, protocols-api 等）
│   ├── en/                     # 英文详细文档
│   └── release-notes/          # ⚠️ 新增文件会触发自动发版，见规则 7
├── core/                       # 核心库：literouter.core（C++23 静态库模块）
│   ├── src/literouter_core.cppm# 唯一对外公开接口（契约层）
│   ├── src/*.cpp               # 内部实现单元（未导出第三方依赖），分工见下表
│   └── tests/                  # 测试套件（test_*.cpp，每文件编译成一个独立二进制）
│       └── lr_test_check.h     # 断言宏与 EnvGuard；命名为 .h 以免被当作测试构建
├── cli/                        # 命令行前端：literouter 可执行文件 (CLI11)
│   └── src/                    # cli_main.cpp 及各类 cli_cmd_*.cpp
├── gui/                        # 桌面控制台前端：literouter-gui 可执行文件 (EUI-NEO)
│   ├── src/                    # gui_main.cpp, app_state.h, pages/, components/
│   └── hide_zlib.ver           # Linux 链接必需的版本脚本，见规则 8
└── web/                        # 内置 Web 控制台（React 19 + TypeScript + Vite + Tailwind + shadcn/ui）
    ├── src/                    # 前端源码（views/, components/, store.tsx 等）
    ├── dist/                   # 构建产物（index.html / app.css / app.js / favicon.svg）
    └── package.json            # 前端依赖配置
```

`core` 的实现单元分工：

| 单元 | 职责 |
|---|---|
| `lr_proxy.cpp` | 监听与路由注册、入站协议识别、响应头闸门、遥测环形缓冲、Admin API、内置 Web 控制台 |
| `lr_protocol.cpp` | 四种协议的请求/响应双向转换与 `StreamProtocolAdapter` |
| `lr_router.cpp` | `candidatesFor()` 候选链推导与熔断器状态机（纯决策层，不持有 socket） |
| `lr_upstream.cpp` | 单次 `upstreamPost()` / `probeProvider()`，不含路由逻辑 |
| `lr_config.cpp` | `ConfigStore` 加载与原子保存、路径解析、`validate()` |
| `lr_json.cpp` | 配置与遥测的 JSON 编解码 |
| `lr_i18n.cpp` | C++ 侧 en/zh 字典（CLI 与 GUI 共用） |
| `lr_util.cpp` | 字符串、格式化、URL 拆分等小工具 |

### 1.1 请求生命周期与多协议网关

一次请求穿过四层，读懂这条链路才能安全改动 `core`：

```text
客户端 ──▶ lr_proxy.cpp ──▶ lr_router.cpp ──▶ lr_protocol.cpp ──▶ lr_upstream.cpp ──▶ 上游
            识别入站协议      推导候选链         仅在协议不一致时转换      单次 HTTP 调用
            响应头闸门        熔断器判定
```

**入站协议由请求路径决定**（`lr_proxy.cpp` 中的 `ingress_protocol` 判定，约 726 行起）：

| 入站路径 | `ingress_protocol` |
|---|---|
| `/v1/chat/completions`、`/v1/completions` | `openai` |
| `/v1/messages` | `anthropic` |
| `/v1beta/models/{model}:generateContent`（及 `streamGenerateContent`） | `gemini` |
| `/v1/responses` | `openai_responses` |

**出站协议**来自 `ProviderConfig::protocol`。两者一致时走 `same_protocol` 分支——**零 JSON 解析、原样直通**；不一致时才经 `adaptXToChat` / `adaptChatToX` 双向转换，流式交给有状态的 `StreamProtocolAdapter`（同协议时它同样只做透传）。

> ⚠️ Gemini 把模型名嵌在 URL path 而非请求体里。直通分支下做模型重命名时，Gemini 必须改 path，其余协议改 body。

---

## 2. 绝对不可违反的硬性规则 (Non-Negotiable Invariants)

以下规则是构建稳定性与架构纯洁性的基石，任何 Agent **均不得以任何理由破坏**：

### 规则 1：公开契约层严格受控 (`literouter_core.cppm`)
- `core/src/literouter_core.cppm` 是外界使用 `core` 的**唯一入口与契约定义**。
- **不得直接向外暴露任何第三方库类型**（包括 `httplib::*` 和 `nlohmann::json`）。所有第三方类型必须停留在 `core/src/*.cpp` 实现单元中。
- 未经明确的用户需求或架构升级指令，不要随意修改或删除公共函数签名。

### 规则 2：严禁创建 `src/main.cpp`
- 在 `cli` 和 `gui` 的 `mcpp.toml` 中，已显式声明了 `[targets.*]` 目标文件（如 `cli_main.cpp`、`gui_main.cpp`）。
- **千万不要在这些目录下创建或重命名出 `src/main.cpp`**！`mcpp` 构建系统会自动推断 `src/main.cpp` 为另一个隐式二进制目标，从而引发符号重复和构建崩溃。

### 规则 3：头文件包含与模块导入顺序
- 在所有消费 `literouter.core` 的编译单元中，必须遵循固定顺序：
  1. 先引入所有第三方 C/C++ 传统头文件（如 `#include <CLI/CLI.hpp>`、`#include <eui_neo.h>`）；
  2. 然后再导入 C++23 核心模块：`import literouter.core;`。
- `literouter.core` 已经导出了标准库（`export import std;`），因此消费代码无需重复导入 `std`。

### 规则 4：密钥持久化安全 (Secret Preservation)
- 配置文件中的 `api_key` 允许使用环境变量引用（如 `${OPENAI_API_KEY}`、`$RELAY_KEY` 或 `${VAR:-fallback}`）。
- 密钥**仅在实际发起网络请求瞬间调用 `resolveSecret()` 解析展开**。
- **绝对禁止将已解析的真实明文密钥写回配置文件**！`ConfigStore::save()` 和序列化函数必须保留原始占位符。

### 规则 5：流式故障转移的“响应头闸门 (Header Gate)”
- 流式请求（SSE）一旦向客户端下发了第一个字节或 HTTP 响应头，该请求生命周期即进入“已提交”状态，**此时绝对不能触发 failover 切换下一个中转站**（否则客户端会接收到两个混杂拼合的回答流）。
- 故障转移仅能在“首包响应头到达前”发生（如连接超时、连接拒绝、429、5xx）。

### 规则 6：多任务/并发 Agent 构建隔离
- `mcpp` 会在项目的 `target/` 目录下放置编译锁和对象缓存。
- 若有多个 Agent 正在并发修改/构建，**切勿直接在工作区根目录下并发执行构建**。应将代码复制到独立的沙盒（例如 `/tmp/lr-sandbox`）中进行编译与单测，验证全绿后再复制回本工程目录。

### 规则 7：`CHANGELOG.md` 与 `docs/release-notes/` 会触发自动发版
- `.github/workflows/release.yml` 的触发条件是 push 到 `main` 且改动 `CHANGELOG.md` 或 `docs/release-notes/**.md`，随后会**自动打 tag 并发布 GitHub Release**。
- **未经用户明确要求发版，不要修改这两处**。顺手更新一行 changelog 就足以对外发出一个版本。
- 常规的功能与修复说明写在提交信息里即可。

### 规则 8：`gui/hide_zlib.ver` 不可移动或改名
- 该版本脚本隐藏 GUI 主程序中的 `deflate*` / `inflate*` 符号，避免与 `libgio-2.0.so` 动态加载的 `libz.so` 冲突。
- `gui/mcpp.toml` 的 `[target.linux.build]` 以相对路径 `../../../hide_zlib.ver` 引用它（相对于 mcpp 的构建工作目录）。移动、改名或调整 `target/` 层级都会直接破坏 Linux GUI 的链接。

### 规则 9：用户可见字符串必须补齐对应字典
- 本项目存在**两套彼此独立**的 i18n 实现，共三份字典：
  1. **CLI 与 GUI**：共用 C++ 侧的 `literouter::i18n::tr()`，中文字典是 `core/src/lr_i18n.cpp` 的 `kZhTranslations`（英文即源码里的原文，无需单独字典）；
  2. **Web 控制台**：完全独立的 `web/src/lib/i18n.tsx`，内含 `en` / `zh` 两个 map，两边都要补。
- **缺失翻译会静默回退英文原文**（`lr_i18n.cpp` 中 `tr()` 直接 `return text;`），既不报错也不构建失败，只会在界面上露出一句英文。
- 因此新增任何用户可见字符串，都必须同步补上对应字典条目；改动 Web 文案时 `en` 与 `zh` 两个 map 都要补。

---

## 3. 标准命令与操作速查

所有命令在项目根目录执行：

### 3.1 构建命令
```bash
# 全局全量构建 (core + cli + gui)
mcpp build --workspace

# 独立构建某个成员
mcpp build -p core
mcpp build -p cli
mcpp build -p gui
```

### 3.2 运行测试

`mcpp` 把 `core/tests/**/*.cpp` 中每个文件编译成一个独立二进制，以退出码判定成败，因此没有外部测试框架。

```bash
# 运行 core 全部 10 个测试套件（必须全部 PASS）
mcpp test -p core

# 只列出套件名，不构建不运行
mcpp test -p core --list

# 只跑单个套件（位置参数即模式匹配，调试时优先用这个）
mcpp test -p core test_proxy

# test_proxy 会绑定真实端口并起线程，慢机器上可放宽超时（默认 300 秒）
mcpp test -p core test_proxy --timeout 600
```

当前套件：`test_config`、`test_i18n`、`test_json_api`、`test_malformed`、`test_protocol`、`test_proxy`、`test_router`、`test_secrets`、`test_tls`、`test_util`。

### 3.3 运行 CLI
```bash
# 查看帮助
mcpp run -p cli -- --help

# 环境体检
mcpp run -p cli -- doctor

# 指定端口启动前台代理
mcpp run -p cli -- serve --port 8787

# 查看当前运行中实例的状态
mcpp run -p cli -- status

# 以 JSON 格式输出状态
mcpp run -p cli -- status --json

# 管理命令示例
mcpp run -p cli -- providers list
mcpp run -p cli -- routes list
mcpp run -p cli -- config validate
```

### 3.4 运行 GUI
```bash
# 启动桌面控制台 (需图形环境与 OpenGL)
mcpp run -p gui

# 无头冒烟：各页面渲染固定帧数后自动退出（CI 用 xvfb-run 包一层）
LITEROUTER_GUI_SMOKE=1 mcpp run -p gui

# 钉住单页做确定性验证，避免定时轮播导致抓帧抓错页面
LITEROUTER_GUI_SMOKE=1 LITEROUTER_GUI_PAGE=2 mcpp run -p gui
```

### 3.5 开发期常用环境变量

完整清单见 `docs/{zh,en}/environment.md`，以下是改代码时最常用的几个：

| 环境变量 | 用途 |
|---|---|
| `LITEROUTER_CONFIG` | 指向临时配置文件，**避免污染真实 `~/.config/literouter/config.json`**，测试套件正是靠它隔离 |
| `LITEROUTER_STATE_DIR` | 指向状态目录（`server.persist_telemetry` 开启时其中的 `telemetry.json` 保存计数器与请求日志） |
| `LITEROUTER_CA_BUNDLE` | 指定上游 HTTPS 校验用的 CA 包；上游报"证书被拒"时先查这个 |
| `LITEROUTER_GUI_SMOKE` / `LITEROUTER_GUI_PAGE` | GUI 无头冒烟与页面钉选 |
| `LITEROUTER_WEB_DIR` | `#embed` 不可用时（如 ISO 严格模式的 GCC）从该目录按请求读取 Web 产物 |
| `LITEROUTER_LANG` | 强制 CLI/GUI 语言，验证 i18n 时用（见规则 9） |

### 3.6 编辑器：clangd 必须用 mcpp 自带的那一份

C++ 模块的 `std.pcm` 与编译器构建**严格绑定**，系统 clangd 会以 `ast_file_different_branch` 拒绝加载。必须指向 mcpp 工具链内的 clangd 并开启模块支持：

```jsonc
{
  "clangd.path": "<mcpp registry>/data/xpkgs/xim-x-llvm-tools/22.1.8/bin/clangd",
  "clangd.arguments": ["--experimental-modules-support"]
}
```

`compile_commands.json` 由 `mcpp build` 生成（也可用 `mcpp build --configure-only` 只生成不编译），它与 `.vscode/` 均已被 gitignore，需各自在本地配置。

---

## 4. 编码规范与实现指引

### 4.1 C++23 特性与代码风格
- 编译器标准：Clang `llvm@22.1.8`，开启 `-std=c++23` 与 libc++。
- 命名规范：
  - 类与结构体：`PascalCase`（如 `ProxyServer`, `AppConfig`）
  - 函数与方法：`camelCase`（如 `resolveSecret()`, `boundPort()`）
  - 常量：`kCamelCase`（如 `kVersion`, `kAdminPrefix`）
  - 成员变量：带下划线后缀 `name_`, `config_`
- 错误处理：
  - 优先使用 C++23 的 `std::expected<T, std::string>` 表示可能失败的操作，不要使用大范围 C++ 异常穿透。
  - 对于网络通信和状态抓取，返回带有 `bool reachable` 和 `std::string error` 的轻量结构体（如 `AdminStatus`）。
- 内存与生命周期：
  - 坚持 RAII 原则，严禁裸 `new`/`delete`。使用 `std::unique_ptr` 管理 Pimpl 实现（如 `ProxyServer::Impl`）。

### 4.2 结构化绑定避坑
- 遍历 `nlohmann::json` 对象时，不要使用跨模块的结构化绑定（`for (auto [k, v] : obj.items())` 在 clang modules 下会缺少 `std::tuple_size` 特化）。
- 请始终使用显式迭代器：
  ```cpp
  for (auto it = obj.begin(); it != obj.end(); ++it) {
      const std::string& key = it.key();
      const auto& val = it.value();
  }
  ```

### 4.3 单元测试开发准则 (`core/tests/`)
- 核心测试位于 `core/tests/test_*.cpp`。
- 每个测试文件必须包含头文件 `#include "lr_test_check.h"`。
- 使用统一的断言宏：
  - `LR_GROUP("分组描述")`：输出测试子分组；
  - `LR_CHECK(expr)`：通用真值断言；
  - `LR_CHECK_EQ(actual, expected)`：相等断言，在失败时会自动打印期望值与实际值；
  - `LR_SUMMARY("测试名")`：返回退出码（0 表示成功，1 表示有失败）。
- 涉及环境变量修改的测试，必须使用 `lr_test::EnvGuard guard("VAR_NAME");`，确保无论测试成功或失败，环境变量均能被自动还原，不影响其他测试。

---

## 5. 常见任务实战指南

### 任务 A：在 Core 中新增配置字段

一个配置字段要真正可用，扇面横跨 core 与三个前端。只改 core 的话字段能存能读，但 CLI、GUI、Web 三处界面都看不到它——这是最容易漏的一类改动。以最近新增的 `server.web_ui` 为参照，完整清单如下：

**Core（必做）**
1. `core/src/literouter_core.cppm`：在 `ServerConfig` / `ProviderConfig` / `RouteConfig` 中添加**带默认值**的字段，并补一行注释说明它的取值含义；
2. `core/src/lr_json.cpp`：同步序列化与反序列化（缺失时必须回落到默认值，旧配置文件不能因此加载失败）；
3. `core/src/lr_config.cpp`：按需补 `validate()` 规则，路径写成 `providers[2].base_url` 这种可定位形式；
4. `core/tests/test_config.cpp`：补默认值断言与 round-trip 单测。

**前端（凡是用户需要看见或修改的字段，都要做）**

5. `gui/src/pages/settings.h`（或对应页面）：加控件，注意规则 9 的字典；
6. `web/src/lib/api.ts`：补 TypeScript 类型定义；
7. `web/src/views/Settings.tsx`（或对应视图）：加控件；
8. `web/src/store.tsx`：若涉及前端状态流转；
9. `web/src/lib/i18n.tsx`：`en` 与 `zh` **两个 map 都要补**文案；
10. `cli/src/cli_cmd_serve.cpp`：若该字段需要命令行开关覆盖。

**若字段影响服务端行为**

11. `core/src/lr_proxy.cpp`：实现实际行为；
12. `core/tests/test_proxy.cpp`：补行为断言。

最后执行 `mcpp test -p core` 与 `mcpp build --workspace` 确认全绿。

### 任务 B：在 CLI 中增加一个新的子命令
1. 在 `cli/src/` 下创建 `cli_cmd_<name>.cpp`；
2. 包含头文件与上下文声明：
   ```cpp
   #include <CLI/CLI.hpp>
   #include "cli_context.hpp"
   #include "cli_ui.hpp"
   import literouter.core;
   ```
3. 实现子命令注册函数 `void register_<name>(CLI::App &app, Context &ctx);`；
4. 在 `cli_context.hpp` 中声明该函数；
5. 在 `cli/src/cli_main.cpp` 中调用注册函数；
6. 运行 `mcpp run -p cli -- <name> --help` 验证命令注册与选项解析。

### 任务 C：在 GUI 中修改或新增页面
1. GUI 组件与页面均位于 `gui/src/pages/` 与 `gui/src/components/`；
2. 页面采用 EUI-NEO 的 DSL 声明式 UI，通过 `app_state.h` 中的 `AppState` 访问运行中数据；
3. **不要在 UI 渲染主循环帧（`compose`）中执行任何耗时阻塞操作或网络 I/O**。网络请求或配置保存应放入 `AppState` 的后台线程池或异步队列中执行。
4. 运行 `mcpp build -p gui` 确认编译成功。

### 任务 D：修改内置 Web 控制台
1. 前端基于 **React 19 + TypeScript + Vite + Tailwind CSS v4 + shadcn/ui** 开发，源码位于 `web/src/`，固定产物输出至 `web/dist/`（`index.html`、`app.css`、`app.js`、`favicon.svg`）；
2. 构建产物通过 `#embed` 在**编译期**嵌进 `core`（见 `core/src/lr_proxy.cpp` 顶部），因此修改前端代码后必须先运行 `npm --prefix web run build` 重新生成 `web/dist/`，再构建 C++ 模块（`mcpp build -p cli`）。**`web/dist/` 构建产物必须与 `web/src/` 源码同步提交**，以便无 Node.js 环境的用户直接编译；
3. **禁止引入任何外部资源或 CDN**（无外链字体/外部脚本/远程图标），控制台需在断网内网环境下可用；组件库直接导入本地源码（如 `@radix-ui/react-*` 单包、`lucide-react` 本地图标）；
4. 数据只来自同源的管理 API（`/__literouter/*`）与 `/v1/models`：**不要把明文密钥送进浏览器**。配置视图依赖 `GET /__literouter/config` 的脱敏规则，全量回写配置通过 `PUT /__literouter/config`（空密钥保留服务端原值，`api_key_clear: true` 清空）；新增管理端点时同步更新 `docs/{zh,en}/protocols-api.md`；
5. 控制台外壳可免密钥加载，但所有数据接口仍受 `server.api_key` 保护；不要给 `/__literouter/*` 或 `/ui/*` 添加 CORS 头（请求日志含提示词）；
6. 在 `core/tests/test_proxy.cpp` 的 15 号分组补充断言（页面可取、未知资源 404、脱敏、CORS 边界、按 id 探测、PUT 配置回写、`web_ui` 开关），并运行 `mcpp test -p core`。

### 任务 E：新增或修改一种上游协议

先读 §1.1 弄清入站/出站协议的判定与直通分支，再动手。改动集中在 `core/src/lr_protocol.cpp`，需要成对补齐：

1. `resolveChatPath()` / `resolveModelsPath()`：该协议的上游路径推导；
2. **请求方向** `adaptXToChat()`：把入站请求归一成 OpenAI Chat Completion；
3. **响应方向** `adaptChatToX()`：把 OpenAI 响应转回该协议的形态。两个方向必须同时存在，否则非流式路径会一头通一头断；
4. `StreamProtocolAdapter::Impl`：流式增量转换（同协议时保持零转换透传）；
5. 若新增的是**入站**协议，还要在 `lr_proxy.cpp` 注册路由并扩展 `ingress_protocol` 判定；
6. `core/tests/test_protocol.cpp` 补 round-trip 断言，`core/tests/test_proxy.cpp` 的 14 号分组补入站与直通断言。

> ⚠️ 两处易错：Gemini 的模型名在 URL path 里（见 §1.1 的警示）；流式转换是有状态的，不能假设一个 chunk 恰好是一个完整 SSE 事件。

---

## 6. CI 会卡住的地方

`.github/workflows/ci.yml` 在 Linux / macOS / Windows 三平台跑同一套流程。本地自查时优先覆盖以下几处——它们是最常见的 CI 失败原因：

1. **`web/dist` 同步检查**：Linux 任务会执行 `git diff --exit-code -- web/dist`。改了 `web/src/` 却没重新构建并提交 `web/dist/`，CI 直接失败（对应任务 D 第 2 条）。
2. **GUI 构建的平台差异**：macOS 上 GUI 构建标记为 `continue-on-error`（zlib 共享链接缺 C++ 运行时符号），**Linux 与 Windows 上不容许失败**。
3. **CLI 冒烟**：`--version` → `config init --force` → `config validate` 三连。注意 `--force` 会覆写配置文件，所以本地跑之前先设好 `LITEROUTER_CONFIG`。
4. **GUI 无头冒烟**：仅 Linux，`xvfb-run` + `LITEROUTER_GUI_SMOKE=1`，并以 `LIBGL_ALWAYS_SOFTWARE=1` 强制软件光栅化。

`release.yml` 是独立的发版流水线，触发条件见规则 7。

---

## 7. 提交信息规范

严格遵循 Conventional Commits，scope 可多值逗号分隔，与仓库现有历史保持一致：

```text
feat(core,cli): add dynamic web_ui toggle and PUT /__literouter/config endpoint
fix(gui,build): eliminate zlib duplicate symbol warning and optimize build concurrency
test(core): add coverage for web console bundle, config PUT, and web_ui toggle
docs: modularize documentation into docs/ with Chinese and English versions
ci: unify multi-platform CI into single workflow
```

常用 type：`feat` / `fix` / `docs` / `test` / `ci` / `chore` / `refactor`。常用 scope：`core` / `cli` / `gui` / `web` / `build` / `release` / `test`。描述用英文小写祈使句，不加句号。

---

## 8. 完成定义 (Definition of Done - DoD)

任何 Agent 在声称任务完成或提交代码前，必须对照以下清单进行自查：

- [ ] `mcpp test -p core` 执行无误，10 组测试套件全部通过（0 failures）；
- [ ] `mcpp build --workspace` 执行无误，全工作区无 warning、无 error；
- [ ] 涉及 CLI 修改的，手动运行一次对应子命令确认控制台输出无乱码、对齐正常；
- [ ] 涉及配置变动的，确认环境变量密钥引用未被意外展开成明文；
- [ ] 涉及新增配置字段的，四个前端（CLI / GUI / Web / 文档）均已同步，见任务 A 的完整清单；
- [ ] 涉及新增用户可见字符串的，C++ 与 Web 两侧字典均已补齐（规则 9）；
- [ ] 涉及前端改动的，`web/dist/` 已重新构建并与 `web/src/` 一同提交（CI 会 diff 校验）；
- [ ] 未擅自改动 `CHANGELOG.md` 或 `docs/release-notes/`（规则 7）；
- [ ] 未在任何 member 中遗留 `src/main.cpp` 或临时测试垃圾文件；
- [ ] 提交信息符合第 7 节的 Conventional Commits 规范；
- [ ] 保持代码风格整洁，保留所有既有注释和文档。
