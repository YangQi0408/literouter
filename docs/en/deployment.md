# Deployment

`literouter` is a single-user gateway: one `server.api_key` guards both the model endpoints and the management API. To reach it from beyond this machine over TLS or through a trusted reverse proxy, see the TLS section of [Configuration & Secrets](configuration.md) and set that key.

## Methods

Four ways to land this on a machine, depending on what the machine has:

| Method | File | For |
|---|---|---|
| Run the binary | `scripts/install.sh` | A long-lived **Linux** machine; the script verifies the release's SHA-256 and can install the systemd unit |
| systemd service | `deploy/literouter.service` | Managing it with the distribution's service manager: dedicated account, `ProtectSystem=strict`, graceful `SIGTERM` stop |
| Docker | `Dockerfile` | Container hosts. Multi-stage: the runtime image carries `ca-certificates` and one binary, running as uid 8787, not root |
| Docker Compose | `docker-compose.yml` | When the config and the state need separate persistent volumes |

**`scripts/install.sh` is Linux-only.** It drives `systemctl` and `/etc/systemd/system`, and uses the GNU `sha256sum` and `readlink -f` (macOS has only `shasum`); no macOS or Windows artifact is published either. On those platforms take the binary from the releases page directly. Docker and Compose are unaffected.

**On Windows the config path is an ordinary, unprefixed path.** The build uses MinGW GCC and never spells a path with the `\\?\` long-path prefix, and no `longPathAware` manifest is shipped, so a path is subject to the `MAX_PATH` rules Windows applies to plain paths. Keeping the config at the default `%APPDATA%\literouter\` — where the path is short by construction — means this never comes up. Where the OS does refuse a path, the failure is loud and harmless rather than silent: the save reports `cannot write config …`, exits non-zero, and leaves the previous file untouched, because the new content goes to a temporary name and is only renamed over the old one once it is on disk.

Two operational notes that matter:

- **Mount the config and the state separately.** The config (`/etc/literouter`) holds keys and is what you edit and back up; the state directory (`/var/lib/literouter`) holds the telemetry file. State on an anonymous volume means every `docker compose down -v` clears the recorded history.
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
