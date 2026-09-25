# 命令行工具 (CLI) 手册

`literouter` 命令行工具提供服务前台运行、实时健康状态查看、日志跟踪跟踪、上游中转站与路由规则管理，以及环境体检等完整功能。

---

## 目录

- [全局选项 (Global Options)](#全局选项-global-options)
- [子命令概览](#子命令概览)
- [服务运行 (`serve`)](#服务运行-serve)
- [状态监控 (`status`)](#状态监控-status)
- [日志跟踪 (`logs`)](#日志跟踪-logs)
- [环境体检 (`doctor`)](#环境体检-doctor)
- [模型列表 (`models`)](#模型列表-models)
- [中转站管理 (`providers`)](#中转站管理-providers)
- [路由管理 (`routes`)](#路由管理-routes)
- [配置工具 (`config`)](#配置工具-config)

---

## 全局选项 (Global Options)

全局选项位于子命令之前或之后均可解析：

```bash
literouter [OPTIONS] <SUBCOMMAND>
```

| 选项 | 简写 | 说明 |
|---|:---:|---|
| `--config <path>` | | 指定配置文件路径（默认读取 `$LITEROUTER_CONFIG` 或系统默认配置路径） |
| `--lang <auto\|zh\|en>` | `-l` | 指定输出语言（默认跟随系统） |
| `--json` | | 以标准机器可读的 JSON 格式输出结果（适合自动化脚本与监控采集） |
| `--no-color` | | 强制禁用控制台 ANSI 彩色输出 |
| `--quiet` | `-q` | 静默模式，仅输出关键信息与错误 |
| `--version` | `-V` | 输出版本号并退出 |
| `--help` | `-h` | 打印命令帮助信息 |

---

## 子命令概览

| 子命令 | 说明 |
|---|---|
| [`serve`](#服务运行-serve) | 在前台启动代理服务 |
| [`status`](#状态监控-status) | 查看当前正在运行的 literouter 实例状态看板 |
| [`logs`](#日志跟踪-logs) | 跟踪查看代理请求历史与实时日志流 |
| [`doctor`](#环境体检-doctor) | 对配置文件、环境变量和上游连通性进行全面体检 |
| [`models`](#模型列表-models) | 查看当前支持的逻辑模型及各自的候选上游链路 |
| [`providers`](#中转站管理-providers) | 列出、添加、测试、启用或禁用上游中转站 |
| [`routes`](#路由管理-routes) | 列出、添加、删除、启用或禁用模型路由规则 |
| [`config`](#配置工具-config) | 查看路径、打印内容、生成模板、校验、迁移或整体导入导出配置文件 |
| [`bench`](#基准对比-bench) | 同一个问题发给所有可服务该模型的中转站，对比延迟与花费 |
| [`replay`](#请求重放-replay) | 把保存的请求体发给指定中转站或策略首选站重放 |

---

## 服务运行 (`serve`)

在前台启动 HTTP 代理服务：

```bash
literouter serve [OPTIONS]
```

### 选项
- `--host <ip>`：临时覆盖配置文件中的监听地址（例如 `0.0.0.0`）；
- `--port <port>`：临时覆盖监听端口（例如 `9000`）；
- `--tls-cert <path>` / `--tls-key <path>`：临时覆盖 HTTPS 证书链与未加密私钥绝对路径；两项必须同时有效。
- `--web-ui` / `--no-web-ui`：本次运行是否在 `/ui` 提供内置 Web 控制台（不指定则沿用配置文件）；
- `--persist` / `--no-persist`：本次运行是否将计数器与请求日志持久化到状态目录（不指定则沿用配置文件）；
- `--force`：即使配置校验存在错误也强制启动；
- `--print-config`：启动前在控制台打印解析后的配置结构；
- `--check`：仅加载并校验配置，不绑定端口直接退出。

> 若目标端口上已有实例在监听，`serve` 会拒绝启动并报出占用者与 pid（原因见[环境变量参考](environment.md)）：两个实例共享同一端口时，请求会被内核随机分流，统计数据与路由配置都将变得不可解释。

**示例**：
```bash
# 启动在默认 8787 端口
literouter serve

# 启动并绑定指定端口
literouter serve --port 8080 --host 0.0.0.0

# 一次性运行，不在磁盘留下任何遥测
literouter serve --no-persist
```

---

## 状态监控 (`status`)

查询并展示当前正在运行的代理实例的运行快照：

```bash
literouter status [OPTIONS]
```

### 选项
- `--json`：输出完整的原始 JSON 快照；
- `--live`：动态实时刷新看板（类似 top / htop）；
- `--interval <ms>`：动态刷新的间隔时间（毫秒，默认 1000）。

**控制台展示信息**：
- 运行时间 (Uptime)、活动请求数 (Active Requests)；
- 累计请求数、成功率、平均响应延迟；
- 中转站健康状态矩阵（熔断状态、连续失败数、累计请求数）；
- 路由表拓扑概览。

---

## 日志跟踪 (`logs`)

从运行中的服务内存环形缓冲区拉取最近的请求日志：

```bash
literouter logs [OPTIONS]
```

### 选项
- `-f, --follow`：持续轮询并实时输出新产生的日志（类似 `tail -f`）；
- `--limit <n>`：最多拉取的日志条数（默认 50 条）；
- `--since <id>`：拉取指定序号之后的增量日志；
- `--level <info|warn|error>`：按日志级别过滤输出。

**示例**：
```bash
# 实时跟踪警告和错误日志
literouter logs --follow --level warn
```

---

## 环境体检 (`doctor`)

对整个系统的运行环境与配置进行深度体检，输出诊断报告：

```bash
literouter doctor
```

**体检项包括**：
1. **配置文件语法**：格式是否合法、各节点 ID 是否唯一、URL 是否有效；
2. **环境变量展开**：检查配置中引用的 `${VAR}` 在当前环境中是否存在对应值；
3. **上游连通性与延迟**：主动向各个已启用的中转站发送网络探测，测量 TLS 握手与响应时间；
4. **模型支持扫描**：自动探测上游中转站声明的模型是否可用；
5. **安全合规检查**：公网监听时是否开启了 API Key 鉴权保护。

---

## 模型列表 (`models`)

列出当前聚合器对外提供的所有逻辑模型：

```bash
literouter models [OPTIONS]
```

### 选项
- `-a, --all`：当开启自动透传时，额外输出各中转站声明的所有物理模型。

输出表格展示：
- 模型逻辑名称 (Model ID)；
- 路由来源（显式路由 / 自动透传）；
- 优先尝试的候选站与兜底故障转移链路。

---

## 中转站管理 (`providers`)

管理配置文件中的上游中转站：

### 1. 列出所有中转站
```bash
literouter providers list
```

### 2. 测试中转站连通性
```bash
literouter providers test <provider-id>
```

### 3. 添加新中转站
```bash
literouter providers add <id> \
  --base-url https://api.deepseek.com/v1 \
  --name "DeepSeek 官方" \
  --key-env DEEPSEEK_API_KEY \
  --priority 10 \
  --model deepseek-chat \
  --model deepseek-reasoner
```
**关键参数**：
- `--key <literal>`：直接存入明文静态密钥；
- `--key-env <VAR>`：以安全占位符 `${VAR}` 格式写入配置文件；
- `--key-stdin`：从终端标准输入安全交互式读取密钥；
- `--protocol <openai|anthropic|gemini|openai_responses|azure|vertex|bedrock|ollama>`：指定上游协议规范；
- `--api-version <v>`：Azure 的 `api-version` 查询值，或 Vertex 的 API 版本段；
- `--region <r>`：Bedrock 的 AWS 区域（**必填**），或 Vertex 的位置；
- `--project <p>` / `--credentials-file <path>`：Vertex 的项目 ID 与 service-account JSON 路径（都**必填**）；
- `--aws-access-key <k>` / `--aws-secret-key <k>` / `--aws-session-token <t>`：Bedrock 的 SigV4 凭据（前两项**必填**），支持 `${VAR}` 引用且只在签名瞬间解析；
- `--max-concurrent <n>` / `--rpm <n>`：**中转站侧**保护上限，约束的是 literouter 自己往这个站发多少（0 表示不限）。满站会被跳过并尝试下一个候选，而不是判定为故障；
- `--price-in <n>` / `--price-out <n>`：该中转站默认的输入 / 输出每百万 token 单价（美元，0 表示不填/不计入）；
- `--model-price <model=in:out|model=price>`：为特定模型单独设置差异化单价（可重复指定），例如 `--model-price gpt-4o=2.5:10.0 --model-price gpt-4o-mini=0.15:0.60`。未单独配置的模型回退到默认单价。

> 协议专属字段不是"填了也无所谓"：校验器会检查 Vertex 的 `project`/`credentials_file` 与 Bedrock 的 `region`/AWS 凭据，缺失时直接报错——一个签不出名的请求到上游只会变成 401，而那时的报错信息指向错误的方向。

### 4. 启用 / 禁用中转站
```bash
literouter providers enable <provider-id>
literouter providers disable <provider-id>
```

### 5. 删除中转站
```bash
literouter providers remove <provider-id>
```

---

## 路由管理 (`routes`)

管理模型名称到中转站候选链的显式映射规则：

### 1. 列出所有路由
```bash
literouter routes list
```

### 2. 添加或更新路由
```bash
# 将 gpt-4o 路由到 openai-official，并以 backup-relay 兜底
literouter routes add gpt-4o \
  --target openai-official:gpt-4o-2024-08-06 \
  --target backup-relay
```

### 3. 启用 / 禁用路由
```bash
literouter routes enable gpt-4o
literouter routes disable gpt-4o
```

### 4. 删除路由
```bash
literouter routes remove gpt-4o
```

---

## 配置工具 (`config`)

管理与验证配置文件本身：

```bash
# 输出当前解析出的配置文件绝对路径
literouter config path

# 在控制台打印当前磁盘上的配置文件原始内容
literouter config show

# 在默认路径生成包含示例的初始配置文件
literouter config init
# 若文件已存在，使用 --force 强制覆盖：
literouter config init --force

# 校验当前配置文件合法性并输出诊断清单
literouter config validate

# 把旧版 schema 的配置改写到当前 schema（加载时已在内存中迁移，这条命令是把它写回磁盘）
literouter config migrate

# 导出整份配置（含引用形式的密钥，绝不解引用），省略文件或写 `-` 输出到标准输出
literouter config export backup.json
literouter config export - > backup.json

# 加载整份配置，或只把文件里点名的中转站/路由合并进当前配置
literouter config load backup.json
literouter config load new-relays.json --merge
# 只校验并报告，不写入
literouter config load backup.json --dry-run
```

**关于 schema 迁移**：`schema` 字段记录配置结构版本。加载时会把旧版文档迁移到当前版本，并在 `config migrate` 里逐条报告改了什么：schema 1→2 把 `openai_compatible` / `openai_chat` 归一为 `openai`；schema 2→3 移除分发时代的字段（`clients`、`server.language`、`server.ui_scale`、`providers[].groups`），并逐条说明丢弃了什么。反过来，**来自更新版本 literouter 的配置会被拒绝加载**，而不是"读进来、丢掉不认识的字段、下次保存时写没"——后者才是真正的静默损坏。

**关于 `--merge`**：按 id 合并。文件里点名的中转站和路由会替换同名的既有条目，其余保持不动。这正是"把我手上这份配置里再加三个中转站"这种操作，整文件替换表达不了。

---

## 基准对比 (`bench`)

把**同一个问题**发给所有可服务该模型的中转站，用来回答"哪家更快、哪家更贵"：

```bash
literouter bench --model gpt-4o
literouter bench --model gpt-4o --runs 3 --json
```

| 选项 | 说明 |
|---|---|
| `--model` | 要对比的模型（必填）；候选按路由策略顺序展开 |
| `--prompt` | 发送的内容（默认一句话短问） |
| `--runs` | 每个中转站发几次（默认 1） |
| `--timeout` | 单次超时（秒，默认 30） |
| `--max-tokens` | 请求的 `max_tokens`（默认 16） |
| `--json` | 以 JSON 输出（全局标志） |

> ⚠️ **这会真的发请求并真的花钱**：每个可用中转站各发 `--runs` 次。表格给出 状态 / 平均延迟 / 最快一次 / token 数 / 估算花费，并在末尾给出"最快"与（已填价目表时）"最便宜"。未填价格的中转站花费列显示 `—`。

## 请求重放 (`replay`)

把一份保存好的请求体重新发出去，用来回答"**这一个**请求会发生什么"——默认走**路由策略的首选站**，也可以指定站：

```bash
# 看策略会把它发给谁、结果如何
literouter replay --file request.json

# 指定中转站，并打印回答正文
literouter replay --file request.json --provider openai-official --show
```

请求体从文件读取是刻意的：日志里的报文已被截断并脱敏，拿它重放会发出**与原请求不同**的请求，比不重放更糟。

| 选项 | 说明 |
|---|---|
| `--file` | 请求体文件（OpenAI Chat 形状，必填） |
| `--provider` | 指定中转站；省略则用路由策略的首选 |
| `--model` | 覆盖请求体里的模型名 |
| `--timeout` | 单次超时（秒，默认用中转站自己的配置） |
| `--show` | 打印回答正文（若上游是别的协议，会先转回 Chat 形状） |
| `--json` | 以 JSON 输出（全局标志） |

