# 环境变量参考手册

本文档列出 `literouter` 支持的所有环境变量、默认值及在不同操作系统下的推荐配置。

---

## 环境变量速查表

| 环境变量 | 作用说明 | 默认值 / 推荐格式 | 适用组件 |
|---|---|---|:---:|
| `LITEROUTER_CONFIG` | 覆盖读取与写入的配置文件绝对路径 | `~/.config/literouter/config.json` | 核心 / CLI |
| `LITEROUTER_STATE_DIR` | 覆盖运行时状态目录（持久化遥测等） | `~/.local/state/literouter` | 核心 / CLI |
| `LITEROUTER_LANG` | 强制指定终端输出语言（`auto`, `zh`, `en`） | `auto` (跟随系统) | CLI |
| `LITEROUTER_CA_BUNDLE` | 指定用于上游与管理客户端 HTTPS 校验的自定义 CA 根证书包绝对路径 | 自动探测系统 CA 信任库 | 核心 |
| `LITEROUTER_PROXY` | 指定上游请求经过的 HTTP 代理，优先级高于 `HTTPS_PROXY` / `HTTP_PROXY` | 未设置 (直连) | 核心 |
| `HTTPS_PROXY` / `HTTP_PROXY` 等 | 标准代理环境变量，`https` 上游走 `HTTPS_PROXY`，未设时回退 `HTTP_PROXY` | 未设置 (直连) | 核心 |
| `NO_PROXY` / `no_proxy` | 逗号分隔的直连名单（域名后缀、IP、CIDR），命中者不经过代理 | 未设置 (全部走代理) | 核心 |
| `LITEROUTER_WEB_DIR` | 指定内置 Web 控制台的产物目录，改为运行时按请求读取而不是编译期 `#embed` | 未设置 (使用编译期内嵌产物) | 核心 |
| `NO_COLOR` | 遵循 no-color.org 规范，设为非空值时禁用终端彩色输出 | 未设置 (开启彩色) | CLI |

---

## 详细说明与平台差异

### 1. `LITEROUTER_CONFIG`
控制配置文件的读取与持久化位置。

- **默认行为**：
  - **Linux**：`$XDG_CONFIG_HOME/literouter/config.json`（若环境变量未设则为 `~/.config/literouter/config.json`）
  - **macOS**：与 Linux 相同（`$XDG_CONFIG_HOME` 未设时使用 `~/.config/literouter/config.json`）
  - **Windows**：`%APPDATA%\literouter\config.json`（通常为 `C:\Users\<User>\AppData\Roaming\literouter\config.json`）
- **示例**：
  ```bash
  export LITEROUTER_CONFIG="/opt/etc/literouter/prod_config.json"
  ```

### 2. `LITEROUTER_STATE_DIR`
存放遥测与实例信息的目录。

- **默认行为**：
  - **Linux / macOS**：`$XDG_STATE_HOME/literouter`（若环境变量未设则为 `~/.local/state/literouter`）
  - **Windows**：`%LOCALAPPDATA%\literouter`
- **内容**：
  - **`telemetry-<port>.json`**：开启 `server.persist_telemetry` 时，全局计数器、逐中转站及逐模型统计、最近的请求日志与最近 24 小时的小时趋势写入此处（权限 `0600`，临时文件 + 原子重命名），供下一次启动读回；关闭该开关则不会创建该文件。文件名以实例实际绑定的端口区分——每个实例一份历史，不会互相覆盖。详见[配置文件与密钥管理](configuration.md)；
  - **`literouter-<port>.pid`**：记录监听实例的 PID、端口、启动时间与配置路径，停止时删除。监听套接字禁止共享同一地址与端口；PID 文件用于在启动冲突时显示占用者。
    新记录还包含实际 `base_url`；配置端口为 0 时，管理命令只使用配置路径匹配且经管理状态验证的记录，多个匹配实例时拒绝猜测。
- **示例**：
  ```bash
  export LITEROUTER_STATE_DIR="/var/lib/literouter"
  ```

### 3. `LITEROUTER_LANG`
控制 CLI 终端输出的语言。

- **可选值**：
  - `zh`：简体中文
  - `en`：英文
  - `auto`：依次读取 `LC_ALL`、`LC_MESSAGES`、`LANG`；都不含 `zh`/`en` 时，Windows 上再看账户的显示语言（`GetUserDefaultUILanguage`）。仍然无法判定时回退英文。

### 4. `LITEROUTER_CA_BUNDLE`
在企业内网、透明代理或使用自建自签名证书的中转服务时，通过该变量注入根证书。

- **默认行为**：自动扫描系统标准位置（`/etc/ssl/certs/ca-certificates.crt`, Windows 证书分发目录等）。
- **示例**：
  ```bash
  export LITEROUTER_CA_BUNDLE="/etc/corp-pki/enterprise-ca.pem"
  ```

### 5. 代理环境变量（`LITEROUTER_PROXY` / `HTTPS_PROXY` / `NO_PROXY`）

内网部署下中转站往往只能经企业代理访问，literouter 因此按 curl 的习惯读取标准代理环境变量。代理是**机器与启动它的 shell 的属性**，不是某个中转站的属性，所以它只从环境变量读取，不放进配置文件——一份在多台机器之间拷贝的配置文件，在每台机器上都会是错的。

**解析顺序（命中即止）：**

1. `LITEROUTER_PROXY`
2. `HTTPS_PROXY`，其次 `https_proxy`
3. `HTTP_PROXY`，其次 `http_proxy`（curl 对 https 目标的回退行为）

值可以是 `http://` URL 或裸的 `host:port`，默认端口 `8080`。HTTPS **目标站点**仍通过 CONNECT 隧道进行 TLS 校验，但不支持到**代理自身**的 TLS 连接（`https://proxy...`）：此配置下请求明确失败，不发送凭据，也不回退直连。不支持 SOCKS5。凭据按 `http://user:pass@proxy:3128` 书写，特殊字符需百分号转义（`p%40ss` 表示 `p@ss`）；Basic `Proxy-Authorization` 在 HTTP 代理连接上发送，仅在可信网络使用。

```bash
# 企业代理，带凭据
export HTTPS_PROXY="http://user:p%40ss@proxy.corp:3128"

# 或只覆盖 literouter，不影响同一 shell 里的其他工具
export LITEROUTER_PROXY="proxy.corp:3128"

# 本地中转站与 OTLP collector 不要经过代理
export NO_PROXY="localhost,127.0.0.1,.corp.example"
```

**行为要点：**

- **本地回环永远直连**：`serve` 启动时的实例探测与 `literouter status` 面向本机实例，不会走代理，因此这两个命令在代理环境下仍可用。
- **`NO_PROXY` 支持域名后缀、IP 字面量与 CIDR**（如 `10.0.0.0/8`、`[::1]`），`*` 表示全部直连。
- **代理配置变更需要重启**：连接池按代理配置分桶，但只在建立连接时读取；改完环境变量请重启 `literouter`。
- **值非法时直连并告警**：无法解析的代理值（例如 `socks5://...`、端口非法）不会被猜测使用，而是回退直连并在 stderr 打印一条说明，避免把请求悄悄送到非预期的代理上。

### 6. `LITEROUTER_WEB_DIR`
指定内置 Web 控制台的产物目录。

正常构建下 `web/dist`（`index.html` / `app.css` / `app.js` / `favicon.svg`）会在**编译期**通过 `#embed` 嵌进 `core`，运行时不需要任何外部文件。当 `#embed` 不可用时（例如 ISO 严格模式的 GCC 尚未实现该特性），构建会跳过内嵌，此时用这个变量指向磁盘上的产物目录，服务端改为按请求读取：

```bash
export LITEROUTER_WEB_DIR=/opt/literouter/web-dist
```

目录必须包含 `index.html`，否则控制台页面无法加载。改前端后仍需 `npm --prefix web run build` 重新生成产物。

### 7. 上游密钥环境变量
任何在配置文件中以 `${VAR_NAME}` 或 `${VAR:-fallback}` 声明的环境变量，均应在运行 `literouter` 之前在系统或 Shell 中导出：

```bash
export OPENAI_API_KEY="sk-..."
export ANTHROPIC_API_KEY="sk-ant-..."
export DEEPSEEK_API_KEY="sk-..."
```
