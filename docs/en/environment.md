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
| `LITEROUTER_GUI_PAGE` | With `LITEROUTER_GUI_SMOKE`, pins the smoke run to one page so a captured frame is deterministic instead of depending on the cycling timer | Unset (cycles every page) | GUI |
| `LITEROUTER_WEB_DIR` | Directory to serve the built-in web console from, reading it per request instead of using the compile-time `#embed` | Unset (embedded bundle) | Core |
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
Directory for telemetry, listener records and client quota state.

- **Default Paths**:
  - **Linux / macOS**: `$XDG_STATE_HOME/literouter` (or `~/.local/state/literouter`)
  - **Windows**: `%LOCALAPPDATA%\literouter`
- **Contents**:
  - **`telemetry-<port>.json`**: with `server.persist_telemetry` on, the global counters, per-relay stats, the recent request log and the last 24 hourly buckets are written here (mode `0600`, temp file plus atomic rename) and read back on the next start. With the switch off, no such file is created. The port in the name is the one the instance actually bound, so each instance owns its history instead of replacing another's. See [Configuration & Secrets](configuration.md);
  - **`literouter-<port>.pid`**: records the listener PID, port, start time and config path, and is removed on stop. Listener sockets prevent sharing the same address and port; the PID file identifies the owner when startup conflicts.
  - **`clients-config-<hash>.json`**: client quota and usage ledger, keyed by the full SHA-256 of the canonical absolute configuration path. It survives listening-port changes and is independent of optional telemetry persistence. A sibling `.json.lock` file enforces exclusive ownership. Library callers without a configuration path use `clients-<port>.json`. Keep this directory on persistent storage. See [API distribution](distribution.md).
- **Example**:
  ```bash
  export LITEROUTER_STATE_DIR="/var/lib/literouter"
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

### 6. `LITEROUTER_GUI_FONT`
Custom TrueType / OpenType font for the GUI.

```bash
export LITEROUTER_GUI_FONT="/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
```

### 7. `LITEROUTER_GUI_PAGE`
Pins the smoke run to a single page.

`LITEROUTER_GUI_SMOKE=1` cycles all six pages on a fixed frame count, so "capture at frame N" lands on a different page depending on how fast the machine is. Pinning makes the captured frame deterministic:

```bash
# Settings only (page index 4)
LITEROUTER_GUI_SMOKE=1 LITEROUTER_GUI_PAGE=4 literouter-gui
```

Page indices: `0=Overview 1=Providers 2=Routes 3=Logs 4=Settings 5=Clients`. A value outside the range falls back to cycling every page. The variable only means anything in smoke mode; a normal launch ignores it.

### 8. `LITEROUTER_WEB_DIR`
Directory to serve the built-in web console from.

In a normal build the `web/dist` assets (`index.html`, `app.css`, `app.js`, `favicon.svg`) are embedded into `core` at **compile time** via `#embed`, so the running binary needs no external files. When `#embed` is unavailable — for example an ISO-strict GCC that does not implement the feature yet — the build skips the embedding, and this variable points at the directory on disk so the server reads the assets per request instead:

```bash
export LITEROUTER_WEB_DIR=/opt/literouter/web-dist
```

The directory must contain `index.html`, or the console page cannot load. Rebuilding the front end still means `npm --prefix web run build`.

### 9. Secret Environment Variables
Any environment variables referenced in `config.json` via `${VAR_NAME}` or `${VAR:-fallback}` should be exported before starting `literouter`:

```bash
export OPENAI_API_KEY="sk-..."
export ANTHROPIC_API_KEY="sk-ant-..."
export DEEPSEEK_API_KEY="sk-..."
```
