# Build and packaging helpers

These helpers create **local staged artifacts**, not a release or an installation.
Do not run an install script against a live gateway as part of validation. Use an
isolated checkout, temporary `LITEROUTER_CONFIG` / `LITEROUTER_STATE_DIR`, and port
`0`; never send shutdown or reload requests to an unrelated listener.

## Validation

```bash
npm ci --prefix web
npm test --prefix web
npm run build --prefix web
mcpp test -p core
mcpp test -p cli
mcpp build --workspace
mcpp test -p core --profile release
mcpp test -p cli --profile release
mcpp build --workspace --release
python3 -m unittest discover -s scripts/tests -v
python3 scripts/check_cli_smoke.py --binary "$(python3 scripts/find_program.py)"
```

`find_program.py` selects the newest **exact member target** (`bin/cli/literouter`
or `literouter.exe`), never a similarly named test or tray image. Use a fresh
checkout or remove stale build profiles when validating a particular profile.
`check_cli_smoke.py` owns its temporary configuration and child processes; it
checks first-run creation, read-only check/print, corrupt-config preservation,
dynamic-port management, stdin parsing, and secret-reference round trips.

`check_config_docs.py` compares both language versions against the binary's
effective configuration and can also check a real status reply. Its module
docstring contains a complete isolated invocation. `web/dist/` must remain in
sync with the frontend build even on hosts that have no Node.js at compile time.

## Linux archive

```bash
python3 scripts/stage_linux_release.py \
  --cli "$(python3 scripts/find_program.py)" \
  --tray "$(python3 scripts/find_program.py --member tray --name literouter-tray)" \
  --output staging/package
python3 scripts/stage_runtime_sources.py --output staging/linux-runtime-sources.tar.xz
python3 scripts/stage_licenses.py --output staging/package/licenses \
  --runtime-sources staging/linux-runtime-sources.tar.xz
```

- Requires `ldd` and `patchelf` on the Linux build host.
- Only **staged copies** are relocated; build and installed programs are untouched.
- The CLI uses system glibc. The x86-64 CLI currently requires glibc **2.38+**;
  do not advertise compatibility with older distributions without building and
  testing against their ABI baseline.
- The tray uses a launcher plus a matching private loader/libc and the shared
  dependency closure in `lib/tray/`. Move the **whole package**, not just the
  launcher. Do not mix the private loader with a different libc.
- Missing dependencies, absolute library dependencies and remaining private
  build paths fail staging. `RUNTIME-LIBRARIES.json` records source payloads and
  pre-relocation hashes.
- Corresponding GNU/GLib/util-linux/libselinux source, pinned source digests,
  payload recipes and build/relocation scripts accompany Linux distribution.
  `THIRD_PARTY_NOTICES.md` describes the applicable licensing boundaries.
- `stage_licenses.py` collects installed notices and vendored header notices;
  `LICENSE-MANIFEST.json` records every copied license/header with its SHA-256.
- A desktop session with a StatusNotifierWatcher is required. A headless
  container can verify `--help`, but that does **not** validate visible tray UI.

## macOS and Windows

`make_macos_app.sh` wraps the tray executable in an agent-style `.app`, copies
assets and license notices, strips the tag's `v` prefix for numeric bundle
versions, and requires a deployment minimum of **14.0+**. On macOS it ad-hoc
signs by default; `--sign` chooses a signing identity and `--no-sign` is for
fixtures or explicit later signing. Signing alone is **not notarization**.

`make_windows_installer.sh --binary PATH --app PATH --version vX.Y.Z` passes
both executables to Inno Setup and stages icons and notices. The release
workflow requires the tray build rather than silently dropping it. The
installer scopes PATH edits to the chosen per-user/system installation and
does not terminate an arbitrary `literouter` process or guess a shutdown port.
Uninstall removes its PATH entry only if this installation added it, and leaves
untracked files in the application directory alone.
Installing/upgrading while one's own gateway is running remains an explicit
operator action.

Pure helper tests run on Linux, macOS and Windows. They validate package
contracts, not native desktop behavior. Before public distribution, the
platform CI matrix must pass and native qualification must cover:

- Windows Unicode config/certificate paths, shortcut COM initialization,
  per-user/system install/uninstall, PATH preservation, and tray visibility.
- macOS signed bundle launch, minimum-OS claims, autostart paths containing XML
  special characters, and Gatekeeper/notarization requirements.
- Linux package relocation without an mcpp registry, a real tray watcher,
  ownership of newly started gateways, and safe adoption of existing ones.

Do not create version tags or edit `CHANGELOG.md` / `docs/release-notes/` merely
to record routine fixes: those paths can trigger the actual release workflow.

## Packaging-only repair of an existing release

`repair-linux-release.yml` is an explicit operator action. It refuses core,
CLI, tray, frontend or manifest differences from the existing tag, then rebuilds,
tests and executes staged programs before replacing only Linux assets/checksums.
It takes the private loader from ELF `PT_INTERP`, not the system `ldd` trace.
Do not move an existing version tag. Record replacement time and reason in the
GitHub release body so previous downloads remain distinguishable by checksum.

If rebuilding also changes the CLI file hash, `refresh-release-image.yml` can
repackage the current latest release's checksum-verified CLI into GHCR. Verify
the image's binary hash against the published file after it completes.
