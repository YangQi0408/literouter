# literouter 交接文档 (HANDOFF.md)

> **文档性质**：记录「档一 / 档二 / 档三」这一轮改动的完成状态、可复现的验证证据，以及**尚未完成、需要外部条件**的少量事项。
>
> **最高优先级规范仍然是 `AGENTS.md`**，本文档不覆盖它。§3 的约束与陷阱是动手前必读。
>
> ⚠️ **后续变更（本文档之后）**：GUI 桌面控制台（`gui/` 成员、EUI-NEO、`hide_zlib.ver`、`LITEROUTER_GUI*` 环境变量、`docs/{zh,en}/gui.md`）与 **API 分发功能**（`clients[]` 客户端账户、按账户密钥、模型/中转站组权限、配额账本 `clients-config-*.json`、每日预算 `budget_usd_per_day`、`docs/{zh,en}/distribution.md`）**已整块移除**，配置 schema 升到 3。本文档正文保留当时的历史记录；凡与上述两项相关的内容（`lr_clients.cpp`、`test_clients`、`test_distribution`、`check_client_cli.py`、GUI 截图/冒烟、配额账本实测）**均已失效**，以 `AGENTS.md` §9 为准。

---

## 1. 完成状态

### 1.1 十二项改动的落点

| # | 改动 | 主要落点 |
|---|---|---|
| 档一1 | ~~客户端按金额的每日预算 `budget_usd_per_day`~~（已随分发功能移除） | ~~`core/src/lr_clients.cpp`~~、`lr_json.cpp`、`lr_config.cpp` |
| 档一2 | 本地应答缓存（精确匹配、仅非流式、TTL + LRU） | `core/src/lr_cache.cpp`（新）、`lr_proxy.cpp`、`lr_json.cpp` |
| 档一3 | 中转站侧并发/QPS 保护 `max_concurrent` / `requests_per_minute` | `core/src/lr_limits.cpp`（新）、`lr_proxy.cpp`、`lr_json.cpp` |
| 档一4 | 流式 OpenAI → Anthropic 的 thinking 块合成 | `core/src/lr_protocol.cpp`（`StreamProtocolAdapter`，块惰性开闭） |
| 档二5 | 出口协议扩展：Azure OpenAI / Vertex AI / AWS Bedrock / Ollama | `core/src/lr_protocol.cpp`、`lr_auth.cpp`（新） |
| 档二6 | 容器化与运维 | `Dockerfile`、`.dockerignore`、`docker-compose.yml`、`deploy/literouter.service` |
| 档二7 | 配置 schema 迁移 | `lr_json.cpp`（`migrateConfigJson`、schema 上限）、`lr_config.cpp`、CLI `config migrate` |
| 档二8 | 可观测性补完：存活/就绪拆分 + OTLP 导出 | `lr_proxy.cpp`（`/health/live`、`/health/ready`、`otlpPayload`/`exportOtlp`） |
| 档三9 | 配置导入/导出/批量操作 | CLI `config export` / `config load`；Web 控制台导出/导入按钮 |
| 档三10 | ~~GUI / Web 功能对齐审计~~（GUI 与 `gui.md` 已移除） | — |
| 档三11 | 文档散文校验 | `scripts/check_config_docs.py`（散文单元格闭集校验 + `--validate-report`） |
| 档三12 | 趋势时间桶宽度/数量可配置 | `traffic_bucket_sec` / `traffic_bucket_count`，贯穿 core / CLI / Web |

文档侧同步完成：`docs/{zh,en}/` 的 `configuration.md`、`protocols-api.md`、`routing-failover.md`、`cli.md`、`environment.md`（当时还有 `distribution.md` 与 `gui.md`，现已被 `deployment.md` 取代），以及 `README.md` / `README_en.md` 的特性清单与文档索引。

### 1.2 权威验证证据（全部实测）

```text
mcpp build --workspace                       → 0 error
mcpp test -p core --timeout 600              → 当时 15 passed; 0 failed
                                               （移除分发与 GUI 后为 13 套件，见 §5.1）
npm --prefix web test                        → 73 passed (6 files)
npm --prefix web run build                   → 重建后 web/dist 哈希不变（dist 与 src 同步）
python3 scripts/check_config_docs.py --seed … --defaults … --status … --validate-report …
                                             → configuration.md matches the build and protocols-api.md matches a real reply
                                               （当时的字段计数含 client / client-key，现已随分发功能移除）
~~python3 scripts/check_client_cli.py <bin>~~  → 脚本已删除（分发功能移除）
literouter --version                         → 0.1.0
literouter config init --force && config validate → exit 0
~~LITEROUTER_GUI_SMOKE=1 xvfb-run -a mcpp run -p gui~~ → GUI 已移除，无此项
```

### 1.3 容器镜像：端到端实测

```text
DOCKER_BUILDKIT=1 docker build --network=host -t literouter:full .   → exit 0
docker run -d --network=host -v <etc>:/etc/literouter -v <state>:/var/lib/literouter literouter:full
  docker exec <c> id                       → uid=8787(literouter) gid=8787        （非 root）
  /health /health/live /health/ready       → 三者均免鉴权 200
  /ui/                                     → 200 text/html
  /v1/models                               → 正常返回模型列表
  docker inspect .State.Health.Status      → healthy
  docker stop                              → 136 ms 内优雅退出，exit=0
```

**应答缓存（档一2）在镜像内实测**：同一非流式请求发两次，第一次 `X-Literouter-Cache: miss`、第二次 `hit`，上游桩计数为 **2 而非 3**；`literouter_cache_hits_total 1 / misses_total 2 / entries 2`。

**中转站限流（档一3）在镜像内实测**：`requests_per_minute: 1` 时第 1 次 200，第 2、3 次 **429 + `Retry-After: 60`**，错误体 `code=relay_capacity_exceeded`。

**端口语义实测**：配置写 `server.port = 9000`，容器监听 `http://0.0.0.0:9000`，8787 无响应，健康检查报 `healthy`。

### 1.4 本轮修掉的四个真实缺陷

都只有**真跑**（真构建镜像、真截图、真扫描）才会暴露：

1. **Docker 缓存挂载遮蔽了 mcpp 自带的 xlings。** `--mount=type=cache,target=/root/.mcpp/registry` 覆盖整个 registry，而 bundled xlings 就在 `registry/bin/xlings`；首次构建缓存卷为空，该二进制被遮蔽，构建在编译前死在 `xlings binary not found`。现只挂 `registry/data`（真正会膨胀的 4.5G 工具链目录）。
2. **缺 `.dockerignore` 导致镜像里装的是陈旧二进制。** `COPY . .` 把宿主机 `cli/target/`（本机 5 个不同哈希的旧产物）带进镜像，而 Dockerfile 用 `find … | head -1` 取二进制——`find` 按目录序返回，于是装进镜像的是**任意一个旧二进制**。症状极具迷惑性：构建成功但镜像缺功能（实测 `/health/live` 返回 404）。已补 `.dockerignore` 排除 `target/`，**并且**把取二进制改成按 mtime 取最新，双保险。
3. **32 条 GUI 用户可见字符串没有中文字典条目**（违反当时的规则 9）。中文控制台里整块「性能与指标」卡片和协议窗格全部回退显示英文；`tr()` 对缺失条目静默回退英文，不报错、不构建失败。当时已全部补齐（防回归测试为 `test_i18n.cpp::testGuiSurfaceTranslations`）。**GUI 现已删除**，该测试随之改为 `testCliTranslations`。
4. **字典 11 组重复键**，其中 4 组语义冲突。最严重的是 `{"open"}`：熔断状态需要「熔断」、指标磁贴需要「开启」，`unordered_map` 初始化列表里后者静默覆盖前者，导致 CLI `status` 把三个已熔断中继显示成 `3 开启`。现已 0 重复、0 冲突，并新增语义断言。

字典现状（当时）：**GUI 328 条 + CLI 81 条 `tr()` 字面量，缺失 0；`kZhTranslations` 558 键，重复 0，冲突 0**。移除 GUI 与分发后，字典按「字面量仍出现在 `cli/src` 或 `core/src` 中」的可达性规则收敛。

### 1.5 `docker compose`：端到端实测

沙盒无法创建 veth 对（`failed to add the host (veth…) <=> sandbox (veth…) pair interfaces: operation not supported`），所以默认 bridge 网络连不上。这挡住的只是**网络接入**，不是 compose 文件里真正需要验证的东西。用 `network_mode: host` + `ports: !reset []` 临时覆盖后，其余承诺全部实测：

```text
0. 全新命名卷 compose up     → healthy；空配置卷时给出
                                “config … does not exist; running with the built-in seed” 警告
                                并使用内置 seed，不崩溃
1. 带密钥发请求               → HTTP 200，requests_today = 1
2. 状态卷内容（当时）         → clients-config-<sha256>.json + .lock（配额账本）、
                                 telemetry-*.json、literouter-<port>.pid
                                 （配额账本已随分发功能移除，现在只有后两者）
3. down（不加 -v）→ up        → requests_today 仍为 1   ← compose 注释里的承诺
4. down -v → up               → 遥测归 0；当时的 clients-*.json 消失
5. 安全上下文                 → uid=8787、CapDrop=[ALL]、no-new-privileges:true、
                                 Restart=unless-stopped
6. 健康检查                   → healthy
```

**尚未覆盖的只剩端口发布本身**（`ports: 127.0.0.1:8787:8787` 需要 bridge 网络）。那是标准 Docker 行为，不属于 compose 注释里的任何承诺。

---

## 2. 尚未完成的事项

### 2.1 在真实 Docker 主机上补测端口发布（唯一未覆盖项）

**已覆盖**：`docker compose config` 合法；`docker compose build` 成功；`docker build` + `docker run` 端到端通过（§1.3）；compose 的命名卷拆分、状态持久性（当时还含配额账本）、健康检查、重启策略与安全上下文全部实测（§1.5）。

**未覆盖**：`ports: 127.0.0.1:8787:8787` 的**端口发布**本身。它需要 bridge 网络，而本沙盒创建 veth 对会失败：

```text
Error response from daemon: failed to set up container networking: failed to create endpoint
literouter on network literouter_default: failed to add the host (veth…) <=> sandbox (veth…)
pair interfaces: operation not supported
```

这不是项目缺陷（用 `network_mode: host` 覆盖后同一套 compose 能 healthy 运行），但**要在真机上补一次**：

```bash
docker compose up -d
curl -sf http://127.0.0.1:8787/health/ready     # 经端口映射可达
docker compose exec literouter literouter status --json --quiet
docker compose down -v
```

顺带值得确认的一项：在 `cap_drop: ALL` + `no-new-privileges` 下，容器向**真实 HTTPS 上游**发请求仍能成功（本沙盒的上游是本地 HTTP 桩，因此 TLS 路径未经容器验证）。

### 2.2 （可选）镜像构建的网络要求写进 CI

`docker compose build` / `docker build` 在本沙盒需要 `--network=host`（或 compose 的 `build.network: host`），否则撞上同一个 veth 限制。若以后把镜像构建加进 CI，注意同一件事。

### 2.3 （可选）`routes.*` 的 6 个 `[empty]` 散文单元格

`check_config_docs.py` 现在把 12/18 个散文单元格核对为 verified，剩下 6 个是中英各 3 个 `routes.model` / `routes.provider` / `routes.targets` 的**空单元格**。空单元格声称「没有默认值」，而这一点构建产物本身枚举不出来，因此保留为 recognised 并在输出里说明了原因。若要继续收紧，需要给验证器一个「该字段确实无默认值」的机器可读来源。

---

## 3. 约束与陷阱（动手前必读）

### 3.1 来自 `AGENTS.md` 的硬性规则（与本轮改动最相关的部分）

- **规则 1**：`core/src/literouter_core.cppm` 是唯一契约层，**不得导出任何第三方类型**（`httplib::*`、`nlohmann::json` 必须留在 `.cpp` 内）。
- **规则 2**：**绝不要**在 `cli/` 下创建 `src/main.cpp`（mcpp 会推断出第二个二进制目标，符号重复）。
- **规则 4**：`api_key` 的 `${VAR}` / `${VAR:-fallback}` 引用**只能在发请求的瞬间**用 `resolveSecret()` 解析，**绝不写回配置文件**。
- **规则 5**：流式请求一旦下发响应头即进入「已提交」状态，**此后不得故障转移**。
- **规则 7**：仓库不提供自动发版工作流、GitHub Release 或预编译下载包。`CHANGELOG.md` 与 `docs/release-notes/` 仅保留历史记录，未经用户明确要求不要改动；常规说明写在提交信息里。
- **规则 8**（原规则 9）：新增用户可见字符串必须补字典。CLI 用 `core/src/lr_i18n.cpp` 的 `kZhTranslations`；Web 用 `web/src/lib/i18n.tsx` 的 `en`/`zh` **两个 map**。

### 3.2 字典维护

- `tr()` 对缺失条目**静默回退英文**，不报错、不构建失败 —— 「能编译、能跑」不代表翻译齐全。
- `kZhTranslations` 是 `unordered_map` 初始化列表：**重复键后者静默覆盖前者**。改字典后请复跑去重扫描。
- 同一英文词在不同语境需要不同中文时，**必须拆成两个键**（当时 `Models` → 日志筛选用「模型」，改为 `Models list` →「模型列表」）。
- 覆盖扫描脚本要点：正则抓 `tr("…")`，**C++ 相邻字符串字面量会拼接**，必须把连续的 `"a" "b"` 合并成 `"ab"` 再和字典比对，否则会漏报。

### 3.3 工具链特有禁忌

- **禁用 `std::jthread` / `<stop_token>`**：这份 libc++ 的 `stop_token` 辅助符号导出在链接器匹配不到的 ABI 标签下，会以 `undefined hidden symbol … atomic_unique_lock::__set_locked_bit` 链接失败。统一用 `std::thread` + `std::atomic<bool>` + `std::condition_variable`。
- **`std::regex` 是 ECMAScript 语法**，内联 `(?i)` 不生效；忽略大小写要传构造参数 `std::regex_constants::icase`。
- 遍历 `nlohmann::json` 对象**不要用跨模块结构化绑定**（clang modules 下缺 `std::tuple_size` 特化），用显式迭代器。

### 3.4 构建陷阱

改动 `literouter_core.cppm`（结构体字段、函数签名）后，mcpp 的**增量构建偶尔会给出陈旧产物**。遇到莫名其妙的测试失败时**先怀疑构建，再怀疑代码**：删掉该 member 的 `target/` 重编，或用 `mcpp build -p <member> --cache=off`。clangd 必须用 mcpp 自带那份（`--experimental-modules-support`），否则 `std.pcm` 会以 `ast_file_different_branch` 被拒绝。

### 3.5 前端构建顺序（经典陷阱）

`web/dist` 是在**编译期**通过 `#embed` 嵌进 `core` 的：

```bash
npm --prefix web run build && mcpp build -p cli    # 顺序不能反
```

只重建前端不重建 C++，跑起来的二进制仍是旧控制台。**验证 Web 前先确认二进制比 `web/dist` 新。**

### 3.6 Docker 相关的坑（已修，别改回去）

- `--mount=type=cache` **不能挂在 `/root/.mcpp/registry` 整体**上，会遮蔽 `registry/bin/xlings`。只能挂 `registry/data`。
- **`.dockerignore` 必须排除 `target/`**，否则宿主机旧二进制会进镜像。取二进制已改为按 mtime 取最新，这是第二道防线。
- `--mount=type=cache` 需要 **BuildKit + buildx**；缺 buildx 时报 `BuildKit is enabled but the buildx component is missing`。
- `failed to add the host (veth…) <=> sandbox (veth…) pair interfaces: operation not supported` 是**沙盒不支持创建 veth**，加 `--network=host` 即可绕过，不是项目缺陷。
- 容器的 `CMD` 只钉 `--host 0.0.0.0`，**端口由配置文件的 `server.port` 决定**。

### 3.7 测试与记账

- 断言宏：`LR_GROUP` / `LR_CHECK` / `LR_CHECK_EQ` / `LR_CHECK_MSG` / `LR_SUMMARY`（`core/tests/lr_test_check.h`）。`LR_CHECK_EQ` 只重载 `string_view` 与 `long long`，**枚举、vector、double 要用 `LR_CHECK_MSG`**。
- 改环境变量的测试必须用 `lr_test::EnvGuard`。
- **流式请求的记账是异步的**：服务端 `finish()` 可能晚于客户端读完 body，断言前必须轮询（参考 `test_proxy.cpp` 的 `waitForRequests`）。
- `test_config.cpp` 有「逐字段比对 JSON 读取器默认值 vs 契约默认值」的守护测试，新增带默认值的字段后必须仍然通过。

### 3.8 文档校验器的两个输入

`scripts/check_config_docs.py` 的散文单元格校验依赖两个可选输入：

- `--status`：一份真实的 `GET /__literouter/status` 响应，用于核对 `protocols-api.md` 的样例；
- `--validate-report`：`literouter config validate --json` 对一份**故意缺字段**的配置的输出。有了它，`required` 散文单元格才能从「recognised」升级为「verified」，并且是**双向**校验——文档说必填但验证器不报错会被抓，文档给必填字段写了字面量默认值也会被抓。

`CONDITIONALLY_REQUIRED` 里现在只列一个**有条件必填**的字段（`server.api_key` 在监听非回环地址时才被验证器报告），它的表格单元格写 `""` 是**正确**的。往这个集合里加东西时，必须是验证器真的有条件，而不是为了消掉一条报告。

### 3.9 Web 控制台的两条硬约定

- 服务端写入器**对处于默认值的字段不写**（`headers` / `protocol` / `price_*` / `note` 等），前端会拿到 `undefined`。约定是 `web/src/lib/api.ts` 的类型**刻意不加可选标记**，由 `web/src/lib/normalize.ts` 在 API 边界补默认值。**新增任何可能被省略的字段，必须同时更新 `normalize.ts` 的 `*_DEFAULTS` 与 `web/src/lib/api.test.ts`。**
- 控制台**不得引入任何外部资源或 CDN**，必须断网可用。

---

## 4. 收尾自查清单（DoD）

- [ ] `mcpp test -p core --timeout 600` → 13 套件全部通过、0 failures；
- [ ] `npm --prefix web test` 通过；改动 `web/src/lib` 或 `store.tsx` 后必须跑；
- [ ] `mcpp build --workspace` → 无 error；
- [ ] 若动过 `web/src`：`npm --prefix web run build` 已跑，`web/dist` 与 `web/src` **在同一提交**里（可用「重建后 `md5sum` 不变」自证）；
- [ ] 若动过用户可见字符串：CLI 字典与 Web 的 `en`/`zh` 两个 map 均已补齐，**并复跑了字典去重扫描**（0 重复、0 冲突）；
- [ ] 若动过配置字段：`docs/{zh,en}/configuration.md` 已同步，`check_config_docs.py` 带 `--validate-report` 通过（CI 会卡这一项）；
- [ ] 若动过 Docker：`docker build` 与 `docker run` 端到端通过（探针、`/ui`、非 root 用户、SIGTERM 优雅退出）；
- [ ] **未**改动 `CHANGELOG.md` 与 `docs/release-notes/`（规则 7），除非用户明确要求发版；
- [ ] `cli/` 下**没有** `src/main.cpp`；
- [ ] 提交信息符合 Conventional Commits（`AGENTS.md` §7）。

---

## 5. 关键环境与事实速查

### 5.1 构建、测试、运行

- 仓库：`/home/yangqi/literouter`；workspace 成员 `core` / `cli` + 内嵌 `web`；版本 `0.1.0`。
- 构建：`mcpp build -p <member>` / `mcpp build --workspace`；测试：`mcpp test -p core [<suite>] [--timeout 600]`；运行：`mcpp run -p cli -- …`。
- 新增源文件（mcpp 自动 glob `src/**/*.{cppm,cpp}`）：`core/src/lr_limits.cpp`、`lr_cache.cpp`、`lr_auth.cpp`。
- 新增测试文件（各自编译成独立二进制）：`core/tests/test_auth.cpp`、`test_gates.cpp`。
- 测试套件（13）：`test_auth`、`test_config`、`test_gates`、`test_i18n`、`test_json_api`、`test_malformed`、`test_media`、`test_protocol`、`test_proxy`、`test_router`、`test_secrets`、`test_tls`、`test_util`。（`test_clients` 与 `test_distribution` 已随分发功能删除。）
- 字典规模：`kZhTranslations` 只保留 CLI 侧仍可达的条目。

### 5.2 ~~GUI 真实截图验证方法~~（已移除）

GUI 成员已删除，本节记录的方法不再适用。替代做法是用 `browser-use` skill 驱动无头 Chrome 验证 `/ui/` 控制台（见 `AGENTS.md` 任务 C）。

### 5.3 接口与协议

- Admin API：`GET/PUT /__literouter/config`、`GET /status|/metrics|/logs`、`POST /reload|/reset-stats|/shutdown|/probe`；探针 `/health`、`/health/live`、`/health/ready`（三者**免鉴权**）。
- 入站端点：`/v1/chat/completions`、`/v1/completions`、`/v1/embeddings`、`/v1/responses`、`/v1/messages`、`/v1beta/models/*`、`/v1/models`、`/v1/audio/*`、`/v1/images/*`、`/ui/*`。
- 出口协议：`openai`、`azure`、`anthropic`、`gemini`、`vertex`、`bedrock`、`ollama`、`responses`。**报文形态（`wireShapeOf()`）而非协议名决定要不要转换**：`azure` = OpenAI 报文，`vertex` = Gemini 报文。
- 容器：运行镜像基于 `debian:trixie-slim`（二进制需要 glibc ≥ 2.38，bookworm 是 2.36 起不来），uid/gid 8787，`LITEROUTER_CONFIG=/etc/literouter/config.json`、`LITEROUTER_STATE_DIR=/var/lib/literouter`。
