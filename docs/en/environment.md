# Environment Variables Reference

This document lists all environment variables supported by `literouter`, their default values, and platform recommendations.

---

## Quick Reference Table

| Variable | Description | Default / Recommended Format | Scope |
|---|---|---|:---:|
| `LITEROUTER_CONFIG` | Absolute path override for reading and writing config | `~/.config/literouter/config.json` | Core / CLI / GUI |
| `LITEROUTER_STATE_DIR` | Absolute path override for runtime state (persisted telemetry, etc.) | `~/.local/state/literouter` | Core / CLI / GUI |
| `LITEROUTER_LANG` | UI and terminal language override (`auto`, `zh`, `en`) | `auto` (system locale) | CLI / GUI |
| `LITEROUTER_UI_SCALE` | Initial GUI vector scaling factor | `1.0` (supports `0.8` ~ `1.5`) | GUI |
| `LITEROUTER_CA_BUNDLE` | Custom CA bundle for upstream and management-client HTTPS verification | Auto-detected system CA trust store | Core |
| `LITEROUTER_GUI_FONT` | Custom font file path override for GUI rendering | Auto-detected crisp CJK font | GUI |
| `LITEROUTER_GUI_SMOKE` | Set to `1` to run automated headless smoke tests and exit | `0` (disabled) | GUI |
| `NO_COLOR` | Adheres to no-color.org; disables ANSI terminal colors when set | Unset (colors enabled) | CLI |

---

## Detailed Explanations & Platform Conventions

### 1. `LITEROUTER_CONFIG`
Controls where the configuration file is loaded and saved.

- **Default Paths**:
  - **Linux**: `$XDG_CONFIG_HOME/literouter/config.json` (or `~/.config/literouter/config.json`)
  - **macOS**: same as Linux (`~/.config/literouter/config.json` unless `$XDG_CONFIG_HOME` is set)
  - **Windows**: `%APPDATA%\literouter\config.json` (typically `C:\Users\<User>\AppData\Roaming\literouter\config.json`)
- **Example**:
  ```bash
  export LITEROUTER_CONFIG="/etc/literouter/config.json"
  ```

### 2. `LITEROUTER_STATE_DIR`
Directory for state the server keeps (currently persisted telemetry).

- **Default Paths**:
  - **Linux / macOS**: `$XDG_STATE_HOME/literouter` (or `~/.local/state/literouter`)
  - **Windows**: `%LOCALAPPDATA%\literouter`
- **Contents**:
  - **`telemetry-<port>.json`**: with `server.persist_telemetry` on, the global counters, per-relay stats, the recent request log and the last 24 hourly buckets are written here (mode `0600`, temp file plus atomic rename) and read back on the next start. With the switch off, no such file is created. The port in the name is the one the instance actually bound, so each instance owns its history instead of replacing another's. See [Configuration & Secrets](configuration.md);
  - **`literouter-<port>.pid`**: while an instance is listening it records its `pid`, port, start time and config path here, and `stop()` removes it. It exists so a later start can say *which* process is holding the port — httplib, which this depends on, sets `SO_REUSEPORT` by default, so two instances can bind the same port and let the kernel split requests between them, which a failed bind would never reveal. A `serve` aimed at a port an instance already answers on refuses to start and names the holder.
- **Example**:
  ```bash
  export LITEROUTER_STATE_DIR="/var/run/literouter"
  ```

### 3. `LITEROUTER_LANG`
Forces the UI and terminal interface language.

- **Values**:
  - `zh`: Simplified Chinese
  - `en`: English
  - `auto`: Inferred from system locale (`LANG`, `LC_ALL`, or Windows locale).

### 4. `LITEROUTER_UI_SCALE`
Sets the GUI zoom factor on startup.

- **Range**: `0.8` to `1.5`
- **Use case**: Recommended set to `1.25` or `1.5` on 4K / HiDPI monitors.

### 5. `LITEROUTER_CA_BUNDLE`
Points to a custom certificate authority bundle for corporate proxies or private PKI.

- **Example**:
  ```bash
  export LITEROUTER_CA_BUNDLE="/etc/ssl/certs/corp-ca.pem"
  ```

### 6. Secret Environment Variables
Any environment variables referenced in `config.json` via `${VAR_NAME}` or `${VAR:-fallback}` should be exported before starting `literouter`:

```bash
export OPENAI_API_KEY="sk-..."
export ANTHROPIC_API_KEY="sk-ant-..."
export DEEPSEEK_API_KEY="sk-..."
```
