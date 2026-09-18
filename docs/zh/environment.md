# 环境变量参考手册

本文档列出 `literouter` 支持的所有环境变量、默认值及在不同操作系统下的推荐配置。

---

## 环境变量速查表

| 环境变量 | 作用说明 | 默认值 / 推荐格式 | 适用组件 |
|---|---|---|:---:|
| `LITEROUTER_CONFIG` | 覆盖读取与写入的配置文件绝对路径 | `~/.config/literouter/config.json` | 核心 / CLI / GUI |
| `LITEROUTER_STATE_DIR` | 覆盖运行时状态目录（持久化遥测等） | `~/.local/state/literouter` | 核心 / CLI / GUI |
| `LITEROUTER_LANG` | 强制指定控制台与图形界面语言（`auto`, `zh`, `en`） | `auto` (跟随系统) | CLI / GUI |
| `LITEROUTER_UI_SCALE` | 指定桌面控制台启动时的默认缩放比例 | `1.0`（支持 `0.8` ~ `1.5`） | GUI |
| `LITEROUTER_CA_BUNDLE` | 指定用于上游 HTTPS 校验的自定义 CA 根证书包绝对路径 | 自动探测系统 CA 信任库 | 核心 |
| `LITEROUTER_GUI_FONT` | 覆盖桌面控制台渲染所使用的字体文件绝对路径 | 自动探测系统清晰中文字体 | GUI |
| `LITEROUTER_GUI_SMOKE` | 设为 `1` 时进入全自动冒烟测试模式，各页面渲染指定帧数后退出 | `0` (关闭) | GUI |
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
存放服务状态数据的目录（当前用于持久化遥测）。

- **默认行为**：
  - **Linux / macOS**：`$XDG_STATE_HOME/literouter`（若环境变量未设则为 `~/.local/state/literouter`）
  - **Windows**：`%LOCALAPPDATA%\literouter`
- **内容**：
  - **`telemetry-<port>.json`**：开启 `server.persist_telemetry` 时，全局计数器、逐中转站统计、最近的请求日志与最近 24 小时的小时趋势写入此处（权限 `0600`，临时文件 + 原子重命名），供下一次启动读回；关闭该开关则不会创建该文件。文件名以实例实际绑定的端口区分——每个实例一份历史，不会互相覆盖。详见[配置文件与密钥管理](configuration.md)；
  - **`literouter-<port>.pid`**：实例监听期间记录自身的 `pid`、端口、启动时间与配置文件路径，`stop()` 时删除。它用于让后续启动能明确指出"这个端口是谁在占着"——`literouter` 依赖的 httplib 默认开启 `SO_REUSEPORT`，两个实例可以同时绑定同一端口并由内核分流请求，仅靠 bind 失败无法察觉。已有实例在监听时，新的 `serve` 会拒绝启动并报出占用者。
- **示例**：
  ```bash
  export LITEROUTER_STATE_DIR="/var/run/literouter"
  ```

### 3. `LITEROUTER_LANG`
控制 CLI 终端输出与 GUI 控制台界面的语言。

- **可选值**：
  - `zh`：简体中文
  - `en`：英文
  - `auto`：根据当前系统的 `LANG`、`LC_ALL` 或 Windows 本地化语言配置自动推断。

### 4. `LITEROUTER_UI_SCALE`
控制 GUI 启动时的整体界面矢量缩放比例。

- **可选范围**：`0.8` ~ `1.5`（例如 `1.0`, `1.25`, `1.5`）
- **场景**：高分辨率屏幕（如 4K 显示器）推荐设置为 `1.25` 或 `1.5`。

### 5. `LITEROUTER_CA_BUNDLE`
在企业内网、透明代理或使用自建自签名证书的中转服务时，通过该变量注入根证书。

- **默认行为**：自动扫描系统标准位置（`/etc/ssl/certs/ca-certificates.crt`, Windows 证书分发目录等）。
- **示例**：
  ```bash
  export LITEROUTER_CA_BUNDLE="/etc/corp-pki/enterprise-ca.pem"
  ```

### 6. `LITEROUTER_GUI_FONT`
自定义 GUI 所使用的 TrueType / OpenType 字体。

- **示例**：
  ```bash
  export LITEROUTER_GUI_FONT="/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
  ```

### 7. 上游密钥环境变量
任何在配置文件中以 `${VAR_NAME}` 或 `${VAR:-fallback}` 声明的环境变量，均应在运行 `literouter` 之前在系统或 Shell 中导出：

```bash
export OPENAI_API_KEY="sk-..."
export ANTHROPIC_API_KEY="sk-ant-..."
export DEEPSEEK_API_KEY="sk-..."
```
