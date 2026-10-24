# literouter-tray

A tray-only companion to `literouter serve`. It puts an icon in the system tray,
starts the gateway when it starts, stops the gateway when it exits, and opens the
built-in web console when you pick **Show**.

There is no window. The icon *is* the program.

---

## What the menu can do, and why it is only two entries

`literouter-tray` is built on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO),
which provides tray support without requiring a window. That is what makes a
tray-only program possible.

EUI-NEO's tray menu, however, is **hard-coded inside the library**: every one of
its three backends (AppKit on macOS, Win32 on Windows, StatusNotifierItem on
Linux) presents exactly `Show` / separator / `Exit`, and the consumer side can
only read two booleans — `consumeTrayShowRequested()` and
`consumeTrayExitRequested()`. There is no API for adding items.

So this program maps the two entries it can observe:

| Entry | What it does |
|---|---|
| **Show** | Opens the built-in web console in your default browser, at whatever address the gateway is actually listening on |
| **Exit** | Stops the gateway (if this program started it) and quits |

Starting and stopping the gateway therefore follows the process lifetime rather
than a menu entry: the gateway is spawned when the tray starts and stopped when
the tray exits. If you want an explicit start/stop control in the tray menu
itself, that needs a change inside EUI-NEO's tray backends, not here.

---

## Build

This is a **standalone project**, not a member of the repository's root virtual
workspace. It is the only thing here that pulls EUI-NEO, which in turn brings
GLFW, OpenGL, freetype, libpng and glib/gio. Making it a workspace member would
put that whole closure on every `mcpp build --workspace` in a repository whose
own product is a single dependency-light binary. CI and release both build
`-p core` / `-p cli` only, so nothing here can break a release.

```bash
cd tray
mcpp build
```

The binary lands in `target/<triple>/<hash>/bin/<name>/literouter-tray`.

### Runtime requirements (Linux)

The tray uses freedesktop **StatusNotifierItem** over D-Bus, which needs a
`StatusNotifierWatcher` on the session bus:

- KDE Plasma: built in.
- GNOME: install the *AppIndicator and KStatusNotifierItem Support* extension.
- Other compositors (sway, niri, hyprland…): a panel or applet that implements
  the watcher — e.g. `noctalia`, `waybar` with the tray module, or `snixembed`.

If nothing implements it, `initializeTray()` fails and the program says so
instead of leaving a gateway running with no way to stop it.

---

## Usage

```bash
# Supervise the literouter found on PATH
./literouter-tray

# Supervise a specific build
LITEROUTER_BIN=/path/to/literouter ./literouter-tray
```

The gateway inherits the tray's environment, so `LITEROUTER_CONFIG`,
`LITEROUTER_STATE_DIR` and friends work exactly as they do for `literouter
serve`.

If a gateway is **already running** when the tray starts, the tray adopts it
rather than spawning a second one: the CLI refuses to share a port anyway, and a
second spawn would only print an error banner about a gateway that is already
fine. In that case the tray does not stop it on exit — it did not start it.

### Binary resolution

1. `$LITEROUTER_BIN`, if set.
2. `literouter` on `$PATH`.
3. The newest `target/<triple>/<hash>/bin/cli/literouter` in the checkout, so it
   works straight out of a build tree.

---

## How the gateway is stopped

`SIGTERM`, not `SIGKILL`. `literouter serve` installs a handler for it and shuts
the listener down cleanly, which is what flushes telemetry and removes the pid
file. If the process has not exited after five seconds (a request still in
flight, for instance), the tray escalates to `SIGKILL` so a wedged gateway
cannot outlive its tray.

---

## Limitations worth knowing

- **Two entries only.** See above — this is an EUI-NEO API limit.
- **Linux-only build for now.** The supervision code uses `posix_spawn` /
  `waitpid`. Windows and macOS would need their own spawn path.
- **One tray per gateway.** There is no multiplexing; the tray manages the one
  gateway its configuration points at.
