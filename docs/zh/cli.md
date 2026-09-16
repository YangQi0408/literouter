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
| [`config`](#配置工具-config) | 查看路径、打印内容、生成模板或校验配置文件 |

---

## 服务运行 (`serve`)

在前台启动 HTTP 代理服务：

```bash
literouter serve [OPTIONS]
```

### 选项
- `--host <ip>`：临时覆盖配置文件中的监听地址（例如 `0.0.0.0`）；
- `--port <port>`：临时覆盖监听端口（例如 `9000`）；
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
- `--protocol <openai|anthropic|gemini|openai_responses>`：指定上游协议规范。

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
```
