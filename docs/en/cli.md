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
- [Client Distribution (`clients`)](#client-distribution-clients)
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
| [`clients`](#client-distribution-clients) | Manage client accounts, keys, access rules, request/token quotas and usage |
| [`routes`](#route-management-routes) | List, add, remove, enable, or disable model routing rules |
| [`config`](#configuration-utility-config) | Show path, display file contents, initialize seed config, validate, migrate, or export/import the whole config |
| [`bench`](#benchmark-bench) | The same question to every relay that serves a model, compared |
| [`replay`](#replay-replay) | Send a saved request body to one relay, or to the policy's first choice |

---

## Foreground Server (`serve`)

Starts the HTTP proxy service:

```bash
literouter serve [OPTIONS]
```

### Options
- `--host <ip>`: Temporarily override `server.host` for this run (e.g. `0.0.0.0`);
- `--port <port>`: Temporarily override `server.port` for this run (e.g. `9000`);
- `--tls-cert <path>` / `--tls-key <path>`: Override the absolute HTTPS certificate chain and unencrypted private key paths; both must be valid.
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
- `--protocol <openai|anthropic|gemini|openai_responses|azure|vertex|bedrock|ollama>`: Upstream protocol dialect;
- `--api-version <v>`: Azure's `api-version` query value, or Vertex's API version segment;
- `--region <r>`: Bedrock's AWS region (**required**), or Vertex's location;
- `--project <p>` / `--credentials-file <path>`: Vertex's project id and service-account JSON path (**both required**);
- `--aws-access-key <k>` / `--aws-secret-key <k>` / `--aws-session-token <t>`: Bedrock's SigV4 credentials (the first two are **required**). `${VAR}` references work and are resolved only at signing time;
- `--max-concurrent <n>` / `--rpm <n>`: **relay-side** protection, bounding what literouter itself sends to this relay (0 is unlimited). A full relay is skipped for this request and the next candidate tried, rather than being judged as failing.

> The protocol-specific fields are not optional decoration: the validator checks Vertex's `project`/`credentials_file` and Bedrock's `region`/AWS credentials and refuses the config without them. A request that cannot be signed reaches the relay as a 401, and that message points the operator in the wrong direction.

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

# Rewrite an older config onto this build's schema. Loading already migrates in
# memory; this is what writes the migrated form back to disk.
literouter config migrate

# Export the whole config (secrets stay as the references they are, never
# expanded). Omit the file, or use `-`, to write to standard output.
literouter config export backup.json
literouter config export - > backup.json

# Load a whole config, or merge only the relays, routes and accounts it names
literouter config load backup.json
literouter config load new-relays.json --merge
# Validate and report without writing anything
literouter config load backup.json --dry-run
```

---

## Benchmark (`bench`)

Asks every relay that can serve a model the *same* question, which is how "which
one is faster, and which one is dearer" gets an answer:

```bash
literouter bench --model gpt-4o
literouter bench --model gpt-4o --runs 3 --json
```

| Option | Meaning |
|---|---|
| `--model` | Model to compare (required); candidates follow the routing policy |
| `--prompt` | What to send (default: a one-line question) |
| `--runs` | Requests per relay (default 1) |
| `--timeout` | Per-request timeout in seconds (default 30) |
| `--max-tokens` | `max_tokens` to ask for (default 16) |
| `--json` | Machine-readable output (global flag) |

> **These are real requests and they cost real money**: `--runs` of them to every
> usable relay. The table gives status, average and best latency, tokens and the
> estimated cost, and ends with the fastest and — when prices are written down —
> the cheapest. A relay with no price shows `—` for cost.

## Replay (`replay`)

Sends a saved request body again, which answers "what happens to *this* request":
by default to whichever relay the routing policy would try first, or to one you
name.

```bash
# See who the policy would pick, and what happens
literouter replay --file request.json

# A specific relay, printing the answer
literouter replay --file request.json --provider openai-official --show
```

Reading the body from a file is deliberate: a logged body has been truncated and
had credentials masked, so replaying it would send a *different* request than the
one that was logged — worse than not replaying it at all.

| Option | Meaning |
|---|---|
| `--file` | Request body (OpenAI chat shape, required) |
| `--provider` | Send it to this relay instead of the policy's first choice |
| `--model` | Override the model in the body |
| `--timeout` | Per-request timeout (default: the relay's own) |
| `--show` | Print the answer body (converted back to chat shape if the relay speaks another protocol) |
| `--json` | Machine-readable output (global flag) |


## Client Distribution (`clients`)

Personal use needs no client accounts. For distribution, first set `server.api_key` to an administrator key (a literal or `${ENV_VAR}` reference). It retains management access; keys under `clients` only access model endpoints. Each account can own several keys sharing one quota.

```bash
# Tag an existing relay, then restrict an account to that channel and model.
literouter providers groups relay-primary --group team
literouter clients add team-a --name "Team A" --model gpt-4o --group team \
  --rpm 60 --concurrent 4 --requests-per-day 2000 --tokens-per-day 1000000 \
  --budget-usd-per-day 5 --token-reservation 4096

# Store the placeholder without expanding it, or read a literal key from stdin.
literouter clients keys add team-a desktop --key-env TEAM_A_DESKTOP_KEY
literouter clients keys add team-a server --key-stdin < /secure/client-key.txt
literouter clients keys replace team-a desktop --key-env TEAM_A_ROTATED_KEY

literouter clients list
literouter clients show team-a --json
literouter clients usage team-a
literouter clients usage --json

# Update only named fields. Lists replace the previous list.
literouter clients update team-a --rpm 120 --model gpt-4o --model gpt-4o-mini
literouter clients update team-a --all-models --all-groups
literouter clients keys disable team-a desktop
literouter clients keys enable team-a desktop
literouter clients keys remove team-a desktop
literouter clients disable team-a
literouter clients enable team-a
literouter clients remove team-a
literouter providers groups relay-primary --clear
```

`providers add` also accepts repeatable `--group`. Keys have stable IDs within their account; lists and JSON output never reveal stored key values. `clients keys add` and `replace` require either `--key-env` or `--key-stdin`; secret values are not accepted as command-line arguments.

Empty model/group lists allow all models/groups. `--rpm`, `--concurrent`, `--requests-per-day` and `--tokens-per-day` default to `0` (unlimited). Daily quotas reset at **00:00 UTC**. `--token-reservation` defaults to `4096`; with a daily token quota it must be positive and fit within that quota. Requests reserve tokens before dispatch, reconcile against reported usage, and retain the reservation if upstream omits usage. `tokens_today` includes reservations; `reserved_tokens` reports the outstanding amount. The quota ledger persists independently of telemetry logging and is not reset by resetting dashboard counters.

`--budget-usd-per-day` is a daily ceiling **in money** (USD, `0` — the default — is unlimited), alongside the token quota. A token quota is not equivalent across models whose prices differ by orders of magnitude — a million tokens of `gpt-4o` and of a cheap model are not the same cost — so "five dollars a day for this person" is the natural dimension for distribution. It **cannot be reserved**: the price depends on which relay answers and how many tokens it reports, so the check happens at admission and the actual cost is added when the request settles; a request already in flight when the ceiling is reached still completes and is still charged. Only relays that **report usage and have a price** contribute to the day's spend, and `config validate` warns when nothing is priced, because the ceiling would then be permanently unreachable and therefore inert. `literouter clients usage` shows the day's spend next to the ceiling.

Commands editing accounts, keys or groups write the config file; reload the running proxy or enable `server.reload_on_change`. `clients usage` queries the running proxy with the configured administrator key. Missing accounts and invalid edits leave the file unchanged and return a nonzero exit code.
