# literouter 交接文档 (HANDOFF.md)

> **文档性质**：记录「档一 / 档二 / 档三」这一轮改动的完成状态、可复现的验证证据，以及**尚未完成、需要外部条件**的少量事项。
>
> **最高优先级规范仍然是 `AGENTS.md`**，本文档不覆盖它。§3 的约束与陷阱是动手前必读。

---

## 1. 完成状态

### 1.1 十二项改动的落点

| # | 改动 | 主要落点 |
|---|---|---|
| 档一1 | 客户端按金额的每日预算 `budget_usd_per_day` | `core/src/lr_clients.cpp`（准入判定 + 结算累加 `cost_today`）、`lr_json.cpp`、`lr_config.cpp` |
| 档一2 | 本地应答缓存（精确匹配、仅非流式、TTL + LRU） | `core/src/lr_cache.cpp`（新）、`lr_proxy.cpp`、`lr_json.cpp` |
| 档一3 | 中转站侧并发/QPS 保护 `max_concurrent` / `requests_per_minute` | `core/src/lr_limits.cpp`（新）、`lr_proxy.cpp`、`lr_json.cpp` |
| 档一4 | 流式 OpenAI → Anthropic 的 thinking 块合成 | `core/src/lr_protocol.cpp`（`StreamProtocolAdapter`，块惰性开闭） |
| 档二5 | 出口协议扩展：Azure OpenAI / Vertex AI / AWS Bedrock / Ollama | `core/src/lr_protocol.cpp`、`lr_auth.cpp`（新） |
| 档二6 | 容器化与运维 | `Dockerfile`、`.dockerignore`、`docker-compose.yml`、`deploy/literouter.service`、`scripts/install.sh` |
| 档二7 | 配置 schema 迁移 | `lr_json.cpp`（`migrateConfigJson`、schema 上限）、`lr_config.cpp`、CLI `config migrate` |
| 档二8 | 可观测性补完：存活/就绪拆分 + OTLP 导出 | `lr_proxy.cpp`（`/health/live`、`/health/ready`、`otlpPayload`/`exportOtlp`） |
| 档三9 | 配置导入/导出/批量操作 | CLI `config export` / `config load`；Web 控制台导出/导入按钮 |
| 档三10 | GUI / Web 功能对齐审计 | 结论已写入 `docs/{zh,en}/gui.md` 的「与 Web 控制台的功能对齐」一节 |
| 档三11 | 文档散文校验 | `scripts/check_config_docs.py`（散文单元格闭集校验 + `--validate-report`） |
| 档三12 | 趋势时间桶宽度/数量可配置 | `traffic_bucket_sec` / `traffic_bucket_count`，贯穿 core / CLI / GUI / Web |

文档侧同步完成：`docs/{zh,en}/` 的 `configuration.md`、`protocols-api.md`、`routing-failover.md`、`cli.md`、`distribution.md`、`gui.md`、`environment.md`，以及 `README.md` / `README_en.md` 的特性清单与文档索引。

### 1.2 权威验证证据（全部实测）

```text
mcpp build --workspace                       → 0 error
mcpp test -p core --timeout 600              → 15 passed; 0 failed（合计 2735 项断言，test_proxy 682 项）
npm --prefix web test                        → 73 passed (6 files)
npm --prefix web run build                   → 重建后 web/dist 哈希不变（dist 与 src 同步）
python3 scripts/check_config_docs.py --seed … --defaults … --status … --validate-report …
                                             → configuration.md matches the build and protocols-api.md matches a real reply
                                               checked: 26 server / 14 provider / 3 route / 1 target / 12 client / 3 client-key fields, zh and en
                                               prose default cells: 18 (12 verified against the build, 6 recognised)
python3 scripts/check_client_cli.py <bin>    → persistence, validation, secret redaction and live usage PASS
literouter --version                         → 0.1.0
literouter config init --force && config validate → exit 0
LITEROUTER_GUI_SMOKE=1 xvfb-run -a mcpp run -p gui → 900 frames, 6 pages, exit 0
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
3. **32 条 GUI 用户可见字符串没有中文字典条目**（违反规则 9）。中文控制台里整块「性能与指标」卡片和协议窗格全部回退显示英文；`tr()` 对缺失条目静默回退英文，不报错、不构建失败。已全部补齐，并新增防回归测试 `test_i18n.cpp::testGuiSurfaceTranslations`。
4. **字典 11 组重复键**，其中 4 组语义冲突。最严重的是 `{"open"}`：熔断状态需要「熔断」、指标磁贴需要「开启」，`unordered_map` 初始化列表里后者静默覆盖前者，导致 CLI `status` 把三个已熔断中继显示成 `3 开启`。现已 0 重复、0 冲突，并新增语义断言。

字典现状：**GUI 328 条 + CLI 81 条 `tr()` 字面量，缺失 0；`kZhTranslations` 558 键，重复 0，冲突 0**。

---

## 2. 尚未完成的事项

### 2.1 `docker compose up` 的端到端验证（需要真实 Docker 主机）

**已验证**：`docker compose config` 合法；`docker compose build` 成功（`Image literouter:local Built`）；`docker build` + `docker run` 端到端全部通过（§1.3）。

**未验证**：`docker compose up -d`。本沙盒无法创建 bridge 网络：

```text
Error response from daemon: failed to set up container networking: failed to create endpoint
literouter on network literouter_default: failed to add the host (veth…) <=> sandbox (veth…)
pair interfaces: operation not supported
```

**需要在正常 Docker 主机上做**：

1. `docker compose up -d`，确认容器 healthy；
2. 验证命名卷拆分确实生效：`down`（不加 `-v`）后 `up`，确认**客户端配额账本没有重置**——这是 compose 文件注释里承诺的行为，必须实测；再 `down -v` 确认此时才会重置；
3. 确认 `security_opt: no-new-privileges` + `cap_drop: ALL` 下服务仍能正常发起上游 HTTPS 请求。

**验收**：上述三步的实测输出；结论写回 `docs/{zh,en}/distribution.md`。

### 2.2 （可选）把 `docker compose` 的 build 网络写进 CI 或文档

本沙盒的 `docker compose build` 需要临时 override `build.network: host`（否则撞上同一个 veth 限制）。这是**沙盒特性而非项目缺陷**，但如果 CI 里要构建镜像，需要注意同一件事。

---

## 3. 约束与陷阱（动手前必读）

### 3.1 来自 `AGENTS.md` 的硬性规则（与本轮改动最相关的部分）

- **规则 1**：`core/src/literouter_core.cppm` 是唯一契约层，**不得导出任何第三方类型**（`httplib::*`、`nlohmann::json` 必须留在 `.cpp` 内）。
- **规则 2**：**绝不要**在 `cli/`、`gui/` 下创建 `src/main.cpp`（mcpp 会推断出第二个二进制目标，符号重复）。
- **规则 4**：`api_key` 的 `${VAR}` / `${VAR:-fallback}` 引用**只能在发请求的瞬间**用 `resolveSecret()` 解析，**绝不写回配置文件**。
- **规则 5**：流式请求一旦下发响应头即进入「已提交」状态，**此后不得故障转移**。
- **规则 7**：`CHANGELOG.md` 与 `docs/release-notes/**.md` 会触发自动发版。**未经用户明确要求，不要改这两处**；常规说明写在提交信息里。
- **规则 9**：新增用户可见字符串必须补字典。CLI/GUI 用 `core/src/lr_i18n.cpp` 的 `kZhTranslations`；Web 用 `web/src/lib/i18n.tsx` 的 `en`/`zh` **两个 map**。

### 3.2 字典维护

- `tr()` 对缺失条目**静默回退英文**，不报错、不构建失败 —— 「能编译、能跑」不代表翻译齐全。
- `kZhTranslations` 是 `unordered_map` 初始化列表：**重复键后者静默覆盖前者**。改字典后请复跑去重扫描。
- 同一英文词在不同语境需要不同中文时，**必须拆成两个键**（本轮 `Models` → 日志筛选用「模型」，中转站编辑器字段改用 `Models list` →「模型列表」）。
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

`CONDITIONALLY_REQUIRED` 里列了两个**有条件必填**的字段（`server.api_key` 仅在配置了客户端时必填、`clients[0].keys[0].api_key` 仅在密钥启用时必填），它们的表格单元格写 `""` 是**正确**的。往这个集合里加东西时，必须是验证器真的有条件，而不是为了消掉一条报告。

### 3.9 Web 控制台的两条硬约定

- 服务端写入器**对处于默认值的字段不写**（`headers` / `protocol` / `price_*` / `note` 等），前端会拿到 `undefined`。约定是 `web/src/lib/api.ts` 的类型**刻意不加可选标记**，由 `web/src/lib/normalize.ts` 在 API 边界补默认值。**新增任何可能被省略的字段，必须同时更新 `normalize.ts` 的 `*_DEFAULTS` 与 `web/src/lib/api.test.ts`。**
- 控制台**不得引入任何外部资源或 CDN**，必须断网可用。

---

## 4. 收尾自查清单（DoD）

- [ ] `mcpp test -p core --timeout 600` → 15 套件全部通过、0 failures；
- [ ] `npm --prefix web test` 通过；改动 `web/src/lib` 或 `store.tsx` 后必须跑；
- [ ] `mcpp build --workspace` → 无 error；
- [ ] 若动过 `web/src`：`npm --prefix web run build` 已跑，`web/dist` 与 `web/src` **在同一提交**里（可用「重建后 `md5sum` 不变」自证）；
- [ ] 若动过用户可见字符串：CLI/GUI 字典与 Web 的 `en`/`zh` 两个 map 均已补齐，**并复跑了字典去重扫描**（0 重复、0 冲突）；
- [ ] 若动过配置字段：`docs/{zh,en}/configuration.md` 已同步，`check_config_docs.py` 带 `--validate-report` 通过（CI 会卡这一项）；
- [ ] 若动过 GUI：`LITEROUTER_GUI_SMOKE=1` 冒烟通过，且**关键页面已用真实截图核对**（§5.2 的方法）；
- [ ] 若动过 Docker：`docker build` 与 `docker run` 端到端通过（探针、`/ui`、非 root 用户、SIGTERM 优雅退出）；
- [ ] **未**改动 `CHANGELOG.md` 与 `docs/release-notes/`（规则 7），除非用户明确要求发版；
- [ ] `cli/`、`gui/` 下**没有** `src/main.cpp`；
- [ ] 提交信息符合 Conventional Commits（`AGENTS.md` §7）。

---

## 5. 关键环境与事实速查

### 5.1 构建、测试、运行

- 仓库：`/home/yangqi/literouter`；workspace 成员 `core` / `cli` / `gui` + 内嵌 `web`；版本 `0.1.0`。
- 构建：`mcpp build -p <member>` / `mcpp build --workspace`；测试：`mcpp test -p core [<suite>] [--timeout 600]`；运行：`mcpp run -p cli -- …`。
- 新增源文件（mcpp 自动 glob `src/**/*.{cppm,cpp}`）：`core/src/lr_limits.cpp`、`lr_cache.cpp`、`lr_auth.cpp`。
- 新增测试文件（各自编译成独立二进制）：`core/tests/test_auth.cpp`、`test_gates.cpp`。
- 测试套件（15）：`test_auth`、`test_clients`、`test_config`、`test_distribution`、`test_gates`、`test_i18n`、`test_json_api`、`test_malformed`、`test_media`、`test_protocol`、`test_proxy`、`test_router`、`test_secrets`、`test_tls`、`test_util`。
- 字典规模：`kZhTranslations` 558 键；GUI 328 条 + CLI 81 条 `tr()` 字面量全覆盖。

### 5.2 GUI 真实截图验证方法

```bash
Xvfb :97 -screen 0 1400x950x24 &
DISPLAY=:97 LITEROUTER_CONFIG=<cfg> LITEROUTER_SMOKE=1 LITEROUTER_GUI_SMOKE=1 \
  LITEROUTER_GUI_PAGE=4 LIBGL_ALWAYS_SOFTWARE=1 <gui-bin> &
sleep 8 && DISPLAY=:97 import -window root /tmp/page4.png
```

页面索引：`0=概览 1=中转站 2=路由 3=日志 4=设置 5=客户端`。中转站编辑器第二窗格（`协议设置与限流`）的分段控件绝对坐标约 `x≈955, y≈125`（面板居中于 (270,85)）。用 `xdotool mousemove X Y click 1` 触发点击。

### 5.3 接口与协议

- Admin API：`GET/PUT /__literouter/config`、`GET /status|/metrics|/logs`、`POST /reload|/reset-stats|/shutdown|/probe`；探针 `/health`、`/health/live`、`/health/ready`（三者**免鉴权**）。
- 入站端点：`/v1/chat/completions`、`/v1/completions`、`/v1/embeddings`、`/v1/responses`、`/v1/messages`、`/v1beta/models/*`、`/v1/models`、`/v1/audio/*`、`/v1/images/*`、`/ui/*`。
- 出口协议：`openai`、`azure`、`anthropic`、`gemini`、`vertex`、`bedrock`、`ollama`、`responses`。**报文形态（`wireShapeOf()`）而非协议名决定要不要转换**：`azure` = OpenAI 报文，`vertex` = Gemini 报文。
- 容器：运行镜像基于 `debian:trixie-slim`（二进制需要 glibc ≥ 2.38，bookworm 是 2.36 起不来），uid/gid 8787，`LITEROUTER_CONFIG=/etc/literouter/config.json`、`LITEROUTER_STATE_DIR=/var/lib/literouter`。
