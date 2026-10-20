# Environment Variables Reference

This document lists all environment variables supported by `literouter`, their default values, and platform recommendations.

---

## Quick Reference Table

| Variable | Description | Default / Recommended Format | Scope |
|---|---|---|:---:|
| `LITEROUTER_CONFIG` | Absolute path override for reading and writing config | `~/.config/literouter/config.json` | Core / CLI |
| `LITEROUTER_STATE_DIR` | Absolute path override for runtime state (persisted telemetry, etc.) | `~/.local/state/literouter` | Core / CLI |
| `LITEROUTER_LANG` | Terminal language override (`auto`, `zh`, `en`) | `auto` (system locale) | CLI |
| `LITEROUTER_CA_BUNDLE` | Custom CA bundle for upstream and management-client HTTPS verification | Auto-detected system CA trust store | Core |
| `LITEROUTER_PROXY` | HTTP proxy for upstream requests; takes precedence over `HTTPS_PROXY` / `HTTP_PROXY` | Unset (direct) | Core |
| `HTTPS_PROXY` / `HTTP_PROXY` etc. | Standard proxy variables: an `https` upstream uses `HTTPS_PROXY`, falling back to `HTTP_PROXY` | Unset (direct) | Core |
| `NO_PROXY` / `no_proxy` | Comma-separated bypass list (domain suffixes, IPs, CIDR); matches connect directly | Unset (everything proxied) | Core |
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
Directory for telemetry and listener records.

- **Default Paths**:
  - **Linux / macOS**: `$XDG_STATE_HOME/literouter` (or `~/.local/state/literouter`)
  - **Windows**: `%LOCALAPPDATA%\literouter`
- **Contents**:
  - **`telemetry-<port>.json`**: with `server.persist_telemetry` on, the global counters, per-relay and per-model stats, the recent request log and the last 24 hourly buckets are written here (mode `0600`, temp file plus atomic rename) and read back on the next start. With the switch off, no such file is created. The port in the name is the one the instance actually bound, so each instance owns its history instead of replacing another's. See [Configuration & Secrets](configuration.md);
  - **`literouter-<port>.pid`**: records the listener PID, port, start time and config path, and is removed on stop. Listener sockets prevent sharing the same address and port; the PID file identifies the owner when startup conflicts.
- **Example**:
  ```bash
  export LITEROUTER_STATE_DIR="/var/lib/literouter"
  ```

### 3. `LITEROUTER_LANG`
Forces the terminal interface language.

- **Values**:
  - `zh`: Simplified Chinese
  - `en`: English
  - `auto`: Reads `LC_ALL`, `LC_MESSAGES`, `LANG` in turn; if none names a language this build has, Windows falls back to the account's display language (`GetUserDefaultUILanguage`). English is the last resort.

### 4. `LITEROUTER_CA_BUNDLE`
Points to a custom certificate authority bundle for corporate proxies or private PKI.

- **Example**:
  ```bash
  export LITEROUTER_CA_BUNDLE="/etc/ssl/certs/corp-ca.pem"
  ```

### 5. Proxy Variables (`LITEROUTER_PROXY` / `HTTPS_PROXY` / `NO_PROXY`)

Inside a corporate network a relay is usually reachable only through a proxy, so literouter reads the standard proxy environment variables the way curl does. The proxy is a property of **the machine and the shell that started the process**, not of an individual relay, which is why it is read from the environment rather than stored in the config file — a config file copied between machines would be wrong about it on every one of them.

**Resolution order, first hit wins:**

1. `LITEROUTER_PROXY`
2. `HTTPS_PROXY`, then `https_proxy`
3. `HTTP_PROXY`, then `http_proxy` (curl's fallback for https targets)

The value may be a full URL or a bare `host:port`. Only `http` and `https` schemes are supported (no SOCKS5). Credentials go in the URL as `http://user:pass@proxy:3128`; percent-escape any character that is special in a URL (`p%40ss` for `p@ss`), and literouter sends them as Basic `Proxy-Authorization`. When no port is given, `http` defaults to `8080` and `https` to `443`.

```bash
# A corporate proxy with credentials
export HTTPS_PROXY="http://user:p%40ss@proxy.corp:3128"

# Or override for literouter alone, leaving other tools in this shell untouched
export LITEROUTER_PROXY="proxy.corp:3128"

# Keep a local relay and an OTLP collector off the proxy
export NO_PROXY="localhost,127.0.0.1,.corp.example"
```

**Behaviour worth knowing:**

- **Loopback is always direct.** `serve`'s pre-bind instance check and `literouter status` both talk to a local instance, so they never go through the proxy and both keep working behind one.
- **`NO_PROXY` understands domain suffixes, IP literals and CIDR** (`10.0.0.0/8`, `[::1]`); `*` bypasses the proxy for everything.
- **Changing the proxy needs a restart.** The connection pool is keyed by the proxy configuration, but it is read when a connection is dialled; restart `literouter` after editing the environment.
- **An unusable value means direct, plus a warning.** A proxy string that cannot be parsed (`socks5://...`, a bad port) is never guessed at: literouter falls back to a direct connection and prints one line to stderr, rather than quietly sending requests to a proxy nobody intended.

### 6. `LITEROUTER_WEB_DIR`
Directory to serve the built-in web console from.

In a normal build the `web/dist` assets (`index.html`, `app.css`, `app.js`, `favicon.svg`) are embedded into `core` at **compile time** via `#embed`, so the running binary needs no external files. When `#embed` is unavailable — for example an ISO-strict GCC that does not implement the feature yet — the build skips the embedding, and this variable points at the directory on disk so the server reads the assets per request instead:

```bash
export LITEROUTER_WEB_DIR=/opt/literouter/web-dist
```

The directory must contain `index.html`, or the console page cannot load. Rebuilding the front end still means `npm --prefix web run build`.

### 7. Secret Environment Variables
Any environment variables referenced in `config.json` via `${VAR_NAME}` or `${VAR:-fallback}` should be exported before starting `literouter`:

```bash
export OPENAI_API_KEY="sk-..."
export ANTHROPIC_API_KEY="sk-ant-..."
export DEEPSEEK_API_KEY="sk-..."
```
