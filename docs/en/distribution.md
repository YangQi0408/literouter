# Personal use and API distribution

The same routing, protocol conversion, circuit breakers and streaming pipeline serve both personal use and multiple client accounts. Existing configurations without `clients` keep their behavior. Add accounts to distribute access; no mode switch is required.

## Administration and client credentials

`server.api_key` is the administrator credential and can also call model endpoints. It must be nonempty whenever any clients are configured. Client credentials cannot read configuration, logs or metrics, reload configuration, reset statistics or shut down the service. The static console and health checks remain public; console data requires the administrator credential.

Each account can own multiple independently enabled keys. All keys share the account's permissions, concurrency and quotas. Disabling an account blocks new requests immediately; existing requests finish and settle normally. Removing an account does not erase its quota history: recreating the same ID retains its usage.

Keys support `${ENV_VAR}` references. References with a `${VAR:-fallback}` default can contain credentials and are hidden like literal keys, while saving preserves the original reference. The management API redacts literal administrator, upstream and client keys. An unchanged blank value preserves the stored key; `api_key_clear: true` explicitly clears it. Client secrets are matched by account ID and key ID, so renaming an ID requires re-entering its key. Enabled keys cannot be empty, and the administrator key cannot be cleared while accounts exist. Ambiguous credentials, including different environment references resolving to the same value, fail authentication.

## Example

This minimal distribution configuration uses existing defaults for omitted server and provider fields.

```json
{
  "server": {"host": "127.0.0.1", "port": 8787, "api_key": "${LITEROUTER_ADMIN_KEY}"},
  "providers": [{
    "id": "relay", "base_url": "https://api.example.com/v1", "api_key": "${RELAY_KEY}",
    "groups": ["team"], "models": ["gpt-4o"]
  }],
  "routes": [{"model": "assistant", "targets": [{"provider": "relay", "model": "gpt-4o"}]}],
  "clients": [{
    "id": "team-a", "name": "Team A", "enabled": true,
    "keys": [{"id": "desktop", "api_key": "${TEAM_A_KEY}", "enabled": true}],
    "models": ["assistant"], "provider_groups": ["team"],
    "requests_per_minute": 60, "max_concurrent": 4,
    "requests_per_day": 1000, "tokens_per_day": 1000000, "token_reservation": 4096
  }]
}
```

Clients call the existing model endpoints using their own `Authorization: Bearer <key>` or `x-api-key`. `models` matches the logical model requested by the caller, before upstream renaming. A provider is allowed when any of its `groups` intersects the account's `provider_groups`. An empty list leaves that dimension unrestricted. Only authorized providers participate in sorting, affinity, retries and failover; exhaustion never falls back to unauthorized providers.

Client model lists contain only models satisfying both permissions and available channel membership, without internal routing targets. Administrators retain the full list. Permission failures return `403`, missing candidates return `404`, and invalid or disabled credentials return `401`.

## Quotas

| Field | Meaning |
|---|---|
| `requests_per_minute` | Accepted requests in a rolling 60-second window; 0 is unlimited. |
| `max_concurrent` | In-flight requests, including waiting for headers and the entire stream; 0 is unlimited. |
| `requests_per_day` | Requests accepted during the UTC day; 0 is unlimited. Upstream failures count once; retries do not add requests. |
| `tokens_per_day` | UTC daily token budget; 0 is unlimited. Reconciled against reported input and output tokens. |
| `token_reservation` | Minimum tokens reserved per budgeted request, default 4096; must be positive and fit the daily budget. |

Daily quotas and token reservations must be integers from 0 through 9007199254740991 so CLI, GUI, Web and files round-trip them exactly. RPM and concurrency values are limited to 2147483647.

Admission atomically checks and reserves quota before dispatch so concurrent requests cannot spend the same balance. The reservation is the greater of the configured minimum and a conservative estimate from input bytes and explicit output limits. It is an admission budget, not a model tokenizer. Complete usage reconciles the reservation to reported tokens. Missing usage, interrupted streams and process crashes retain the reservation so unmetered responses cannot bypass the budget.

The token budget is not an upstream output cap. If reported usage exceeds the reservation, the full amount is charged and that response may take the daily total over budget; subsequent requests are refused. Interrupted streams charge the greater of their reservation and observed partial usage. Set reservations to suit the workload, particularly multimodal calls. Without reported usage, displayed reported tokens remain zero while today's charged budget includes retained reservations.

Limits return `429` with `Retry-After`. Quota state lives in `clients-config-<hash>.json` under `LITEROUTER_STATE_DIR`, where `<hash>` is the full SHA-256 of the canonical absolute configuration path. It is atomically persisted on admission and settlement independently of `server.persist_telemetry`. Restarting, changing the listening port (including random ports), clearing logs or resetting telemetry cannot reset quotas. Keep the configuration path and state directory stable; use persistent storage. Separate configurations have separate ledgers. Library callers without a configuration path use `clients-<port>.json` instead.

A sibling `.json.lock` file provides exclusive operating-system ownership of the ledger. An instance with configured clients refuses to start if another process owns its ledger or its saved state is corrupt. The lock releases when the instance stops, including after a crash; the lock file may remain. Personal instances without clients or existing quota state create neither quota nor lock files. Enabling clients later acquires ownership before admitting their requests. Failure to acquire ownership or persist a reservation returns `503`. Restore valid state or storage and restart; deleting state is not a quota-reset workflow.

Days roll over at midnight UTC. Requests crossing midnight remain charged to their admission day and continue holding a concurrency slot. Administrator status includes a `clients` array containing requests, outcomes, input/output tokens, estimated costs and quota usage. Cost uses existing provider prices; unpriced providers contribute no estimate.

## Management and deployment

CLI, GUI and Web support accounts, keys, model/group permissions and limits. Request logs include `client_id` and `client_key_id`, never authentication credentials. Prometheus adds request, token, cost, active-request and daily-budget metrics labeled by `client`, accessible only to administrators.

Deploy HTTPS using listener TLS or a trusted reverse proxy. Shared deployments use an administrator key and separate client keys; personal use does not require accounts. Payments, public registration and new upstream protocols are outside this feature.

### Deployment

Four ways to land this on a machine, depending on what the machine has:

| Method | File | For |
|---|---|---|
| Run the binary | `scripts/install.sh` | A machine that is up long-term; the script verifies the release's SHA-256 and can install the systemd unit |
| systemd service | `deploy/literouter.service` | Managing it with the distribution's service manager: dedicated account, `ProtectSystem=strict`, graceful `SIGTERM` stop |
| Docker | `Dockerfile` | Container hosts. Multi-stage: the runtime image carries `ca-certificates` and one binary, running as uid 8787, not root |
| Docker Compose | `docker-compose.yml` | When the config and the state need separate persistent volumes |

Two operational notes that matter:

- **Mount the config and the state separately.** The config (`/etc/literouter`) holds keys and is what you edit and back up; the state directory (`/var/lib/literouter`) holds the telemetry file and the **client quota ledger**. A ledger on an anonymous volume means every `docker compose down -v` hands every client a fresh day's quota.
- **The container listens on `0.0.0.0`; publish the port to the host's loopback** (`-p 127.0.0.1:8787:8787`). The `/ui` console can read the request log, which carries prompts; the validator warns when that is exposed on `0.0.0.0` without a `server.api_key`, but that should be a deliberate decision rather than a default inherited from a compose file.

**To change the port, change the config file, not the `docker run` command.** The container's `CMD` pins only `--host 0.0.0.0` (without it the container would listen on its own loopback and the published port would answer nothing) and leaves the port to `server.port`, so the field means the same thing inside the container as it does outside:

```bash
# config says server.port = 9000, so publish 9000
docker run -p 127.0.0.1:9000:9000 …
```

`EXPOSE 8787` is documentation only and does not decide the listening port. The healthcheck runs `literouter status`, which reads the same config, so it follows a changed `server.port` without needing to be updated.

**The config volume is empty on the first start.** The container then logs a warning (`config … does not exist; running with the built-in seed`) and starts from the built-in seed instead of crashing. That seed points at example relays, so do not publish the port before writing a real config:

```bash
docker compose up -d
docker compose exec literouter literouter config init --force   # write the seed file
docker compose restart
```

The healthcheck is `literouter status --json --quiet`: it goes through the same admin API a client would, so it checks the whole path — listener, config, admin surface — rather than only whether a process exists. The difference between liveness and readiness is in [Protocols & API](protocols-api.md).
