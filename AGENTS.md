# AGENTS.md — literouter 项目 Agent 操作指南

本文件是任何 AI Agent（如 Claude, Pi, Codex, Cursor 等）在进入本仓库执行任务时的**最高优先级开发规范与操作守则**。在进行任何代码修改、构建或测试前，请务必完整阅读并严格遵守。

---

## 1. 项目定位与工程结构

`literouter` 是一个基于 C++23 和 `mcpp` 构建的本地 AI 中转站聚合器（API Relay Aggregator）。它对外提供单一的本地 OpenAI 兼容端点（`http://127.0.0.1:8787/v1`），对内将请求分发至多个中转站，支持模型重命名、优先级故障转移、熔断、SSE 流式透传以及遥测统计。

工程采用 `mcpp` 虚拟工作区（Virtual Workspace），划分为三个子成员（Members）：

```text
literouter/
├── mcpp.toml                   # 虚拟工作区总清单，锁定工具链与第三方依赖
├── AGENTS.md                   # 【当前文件】Agent 操作指南与开发规范
├── README.md                   # 中文项目总览（默认）
├── README_en.md                # 英文项目总览
├── docs/                       # 专题技术与使用文档
│   ├── zh/                     # 中文详细文档（configuration, routing-failover, protocols-api 等）
│   └── en/                     # 英文详细文档
├── core/                       # 核心库：literouter.core（C++23 静态库模块）
│   ├── src/literouter_core.cppm# 唯一对外公开接口（契约层）
│   ├── src/*.cpp               # 内部实现单元（未导出第三方依赖）
│   └── tests/                  # 测试套件（test_*.cpp）
├── cli/                        # 命令行前端：literouter 可执行文件 (CLI11)
│   └── src/                    # cli_main.cpp 及各类 cli_cmd_*.cpp
├── gui/                        # 桌面控制台前端：literouter-gui 可执行文件 (EUI-NEO)
│   └── src/                    # gui_main.cpp, app_state.h, pages/, components/
└── web/                        # 内置 Web 控制台资源（index.html / app.css / app.js / favicon.svg）
                                # 由 core 用 C++23 #embed 编译进二进制，经 `serve` 的同端口 /ui 提供
```

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
```bash
# 运行 core 测试套件（必须全部 PASS）
mcpp test -p core
```

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
```

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
1. 在 `core/src/literouter_core.cppm` 的相应结构体（如 `ServerConfig` 或 `ProviderConfig`）中添加带默认值的字段；
2. 在 `core/src/lr_json.cpp` 中同步修改 JSON 序列化与反序列化逻辑；
3. 在 `core/src/lr_config.cpp` 中检查是否需要补充 `validate()` 规则；
4. 在 `core/tests/test_config.cpp` 中增加新字段的默认值验证与 round-trip 单测；
5. 执行 `mcpp test -p core` 确认所有测试通过。

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
1. 前端资源只有四个文件，全部位于 `web/`：`index.html`、`app.css`、`app.js`、`favicon.svg`；
2. 它们通过 `#embed` 在**编译期**嵌进 `core`（见 `core/src/lr_proxy.cpp` 顶部），因此改完必须重新构建（`mcpp build -p cli`）并强制刷新浏览器；不支持 `#embed` 的编译器会改从 `$LITEROUTER_WEB_DIR` 读取同名文件，新增/改名文件时要同步更新 `kWebAssets` 白名单；
3. **禁止引入任何外部资源或 CDN**（无外链字体/脚本/图标），控制台要在断网的内网服务器上可用；保持零构建工具链，纯原生 HTML/CSS/JS；
4. 数据只来自同源的管理 API（`/__literouter/*`）与 `/v1/models`：**不要把密钥或明文 api_key 送进浏览器**，配置视图依赖 `GET /__literouter/config` 的脱敏规则；新增管理端点时同步更新 `docs/{zh,en}/protocols-api.md`；
5. 控制台外壳可免密钥加载，但所有数据接口仍受 `server.api_key` 保护；不要给 `/__literouter/*` 或 `/ui/*` 添加 CORS 头（请求日志含提示词）；
6. 在 `core/tests/test_proxy.cpp` 的 15 号分组补充断言（页面可取、未知资源 404、脱敏、CORS 边界、按 id 探测），并运行 `mcpp test -p core`。

---

## 6. 完成定义 (Definition of Done - DoD)

任何 Agent 在声称任务完成或提交代码前，必须对照以下清单进行自查：

- [ ] `mcpp test -p core` 执行无误，8 组测试套件全部通过（0 failures）；
- [ ] `mcpp build --workspace` 执行无误，全工作区无 warning、无 error；
- [ ] 涉及 CLI 修改的，手动运行一次对应子命令确认控制台输出无乱码、对齐正常；
- [ ] 涉及配置变动的，确认环境变量密钥引用未被意外展开成明文；
- [ ] 未在任何 member 中遗留 `src/main.cpp` 或临时测试垃圾文件；
- [ ] 保持代码风格整洁，保留所有既有注释和文档。
