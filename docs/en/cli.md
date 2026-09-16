# Command-Line Interface (CLI) Manual

The `literouter` command-line executable provides complete service lifecycle management, real-time health monitoring, log streaming, relay/route administration, and diagnostic checks.

---

## Table of Contents

- [Global Options](#global-options)
- [Subcommands Overview](#subcommands-overview)
- [Foreground Server (`serve`)](#foreground-server-serve)
- [Status Dashboard (`status`)](#status-dashboard-status)
- [Log Streaming (`logs`)](#log-streaming-logs)
- [System Diagnostics (`doctor`)](#system-diagnostics-doctor)
- [Model Discovery (`models`)](#model-discovery-models)
- [Provider Management (`providers`)](#provider-management-providers)
- [Route Management (`routes`)](#route-management-routes)
- [Configuration Utility (`config`)](#configuration-utility-config)

---

## Global Options

Global options may be placed either before or after subcommands:

```bash
literouter [OPTIONS] <SUBCOMMAND>
```

| Option | Short | Description |
|---|:---:|---|
| `--config <path>` | | Path to configuration file (defaults to `$LITEROUTER_CONFIG` or OS default) |
| `--lang <auto\|zh\|en>` | `-l` | Language for terminal output (defaults to system locale) |
| `--json` | | Emit machine-readable JSON (ideal for scripts and CI/CD pipelines) |
| `--no-color` | | Suppress ANSI color escape sequences |
| `--quiet` | `-q` | Quiet mode; suppress non-essential output |
| `--version` | `-V` | Print version and exit |
| `--help` | `-h` | Print help message |

---

## Subcommands Overview

| Subcommand | Description |
|---|---|
| [`serve`](#foreground-server-serve) | Run the proxy server in the foreground |
| [`status`](#status-dashboard-status) | Display live status dashboard or JSON snapshot of the running instance |
| [`logs`](#log-streaming-logs) | Stream or inspect recent request logs from the in-memory ring buffer |
| [`doctor`](#system-diagnostics-doctor) | Run deep diagnostics on config, environment variables, and upstream networks |
| [`models`](#model-discovery-models) | Display available models and their candidate upstream chains |
| [`providers`](#provider-management-providers) | List, add, remove, test, enable, or disable upstream providers |
| [`routes`](#route-management-routes) | List, add, remove, enable, or disable model routing rules |
| [`config`](#configuration-utility-config) | Show path, display file contents, initialize seed config, or validate |

---

## Foreground Server (`serve`)

Starts the HTTP proxy service:

```bash
literouter serve [OPTIONS]
```

### Options
- `--host <ip>`: Temporarily override `server.host` for this run (e.g. `0.0.0.0`);
- `--port <port>`: Temporarily override `server.port` for this run (e.g. `9000`);
- `--web-ui` / `--no-web-ui`: Whether this run serves the built-in console at `/ui` (untouched from the file when neither is given);
- `--persist` / `--no-persist`: Whether this run persists counters and the request log to the state directory (untouched from the file when neither is given);
- `--force`: Start even if `validate()` reports configuration errors;
- `--print-config`: Print the parsed configuration structure before listening;
- `--check`: Load and validate configuration, then exit without binding.

> If an instance already answers on the target port, `serve` refuses to start and names it together with its pid (why: see the [Environment Variables Reference](environment.md)) — two instances sharing one port have their requests split by the kernel, which makes both the counters and the routing unexplainable.

**Examples**:
```bash
# Start on default port 8787
literouter serve

# Start on custom port and host
literouter serve --port 8080 --host 0.0.0.0

# One-off run that leaves no telemetry behind
literouter serve --no-persist
```

---

## Status Dashboard (`status`)

Queries the running instance's administrative endpoint and renders a health dashboard:

```bash
literouter status [OPTIONS]
```

### Options
- `--json`: Output the raw JSON status snapshot;
- `--live`: Continuously refresh dashboard in place (top/htop style);
- `--interval <ms>`: Refresh interval in milliseconds (default 1000).

**Information displayed**:
- Uptime, Active Requests, Success Rate, Average Latency;
- Per-provider health matrix (Circuit state, consecutive failures, total requests);
- Route topology summary.

---

## Log Streaming (`logs`)

Fetches recent request records from the in-memory ring buffer:

```bash
literouter logs [OPTIONS]
```

### Options
- `-f, --follow`: Stream new logs continuously until Ctrl-C;
- `--limit <n>`: Maximum number of entries to retrieve (default 50);
- `--since <id>`: Fetch entries strictly newer than sequence ID `id`;
- `--level <info|warn|error>`: Filter output by minimum severity level.

**Example**:
```bash
# Follow warning and error logs in real-time
literouter logs --follow --level warn
```

---

## System Diagnostics (`doctor`)

Runs comprehensive diagnostics on your setup and prints an actionable report:

```bash
literouter doctor
```

**Checks performed**:
1. **Config Syntax**: Validates JSON format, ensures provider IDs are unique and URLs valid;
2. **Environment Variables**: Verifies that `${VAR}` references exist in the environment;
3. **Upstream Connectivity**: Sends network probes to enabled providers to measure TLS latency;
4. **Model Verification**: Probes upstreams to ensure advertised models respond correctly;
5. **Security**: Warns if binding to non-loopback addresses without `server.api_key`.

---

## Model Discovery (`models`)

Lists all logical models aggregated by `literouter`:

```bash
literouter models [OPTIONS]
```

### Options
- `-a, --all`: Include unrouted physical models advertised by providers in pass-through mode.

---

## Provider Management (`providers`)

Manage upstream providers defined in the configuration file:

### 1. List Providers
```bash
literouter providers list
```

### 2. Test Provider Connectivity
```bash
literouter providers test <provider-id>
```

### 3. Add a New Provider
```bash
literouter providers add <id> \
  --base-url https://api.deepseek.com/v1 \
  --name "DeepSeek Official" \
  --key-env DEEPSEEK_API_KEY \
  --priority 10 \
  --model deepseek-chat \
  --model deepseek-reasoner
```
**Key options**:
- `--key <literal>`: Store literal plaintext key;
- `--key-env <VAR>`: Store secure placeholder `${VAR}`;
- `--key-stdin`: Interactively prompt for the key from terminal stdin;
- `--protocol <openai|anthropic|gemini|openai_responses>`: Upstream protocol dialect.

### 4. Enable / Disable a Provider
```bash
literouter providers enable <provider-id>
literouter providers disable <provider-id>
```

### 5. Remove a Provider
```bash
literouter providers remove <provider-id>
```

---

## Route Management (`routes`)

Manage explicit model-to-provider mappings:

### 1. List Routes
```bash
literouter routes list
```

### 2. Add or Update a Route
```bash
# Route gpt-4o to openai-official, falling back to backup-relay
literouter routes add gpt-4o \
  --target openai-official:gpt-4o-2024-08-06 \
  --target backup-relay
```

### 3. Enable / Disable a Route
```bash
literouter routes enable gpt-4o
literouter routes disable gpt-4o
```

### 4. Remove a Route
```bash
literouter routes remove gpt-4o
```

---

## Configuration Utility (`config`)

Inspect and validate the configuration file:

```bash
# Print resolved path to configuration file
literouter config path

# Print raw configuration file contents
literouter config show

# Generate seed template configuration
literouter config init
# Overwrite existing file:
literouter config init --force

# Validate configuration and print all errors/warnings
literouter config validate
```
