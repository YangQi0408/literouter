# literouter — the desktop app

Double-click it and it opens your browser on the literouter console. It has no
window of its own: the console *is* the interface, and the program lives in the
system tray.

```
double-click  →  gateway starts in-process  →  browser opens at /ui/
             →  tray icon stays                →  Exit stops the gateway
```

---

## The tray menu is two entries, and that is not a choice

`literouter-tray` is built on [EUI-NEO](https://github.com/sudoevolve/EUI-NEO),
which provides tray support without requiring a window — that is what makes a
tray-only program possible at all.

Its tray menu, however, is **hard-coded inside the library**: every one of its
three backends (AppKit on macOS, Win32 on Windows, StatusNotifierItem on Linux)
presents exactly `Show` / separator / `Exit`, and the consumer side can only read
two booleans. There is no API for adding items.

| Entry | What it does |
|---|---|
| **Show** | Opens the console in your default browser |
| **Exit** | Stops the gateway and quits |

Starting is implicit: the gateway is up for as long as the app runs. Anything
that would otherwise be a menu entry is a command-line flag — see *Autostart*.

---

## The gateway runs inside this process

`literouter.core` is a library, so the app links it and calls `start()` / `stop()`
directly instead of spawning `literouter serve` as a child.

That decision is the reason there is one implementation instead of three. A
supervised child would need, on every platform:

| Job | POSIX | Windows |
|---|---|---|
| start | `posix_spawn` | `CreateProcessW` |
| stop | `kill(SIGTERM)` | `GenerateConsoleCtrlEvent` |
| reap | `waitpid` | `WaitForSingleObject` |
| read status | `pipe` | `CreatePipe` + handle inheritance |
| find the binary | `access(X_OK)` | `.exe` probing |

Linking the library deletes that whole table. What remains is C++ and EUI-NEO's
tray API, both of which already work on all three platforms. The cost is that
this binary carries the proxy engine instead of shelling out to a smaller one —
the right trade for an application you install.

---

## Build

The app is a member of the repository's virtual workspace, so it builds with the
rest:

```bash
mcpp build            # from the repository root
```

> ⚠️ A workspace build pulls EUI-NEO's graphics closure (GLFW, OpenGL, freetype,
> libpng, glib/gio), which is several minutes on a cold cache. To build only the
> headless front ends, use `mcpp build -p core -p cli`.

The binary lands in `target/<triple>/<hash>/bin/tray/literouter-tray`.

Linux release archives use a launcher with a matching private GNU loader/libc
and GLib/X11 libraries under `lib/tray/`. Keep that directory beside the launcher;
autostart records the launcher path, not the private loader. Corresponding runtime
source and license texts are published alongside the binary archive.
macOS bundles require macOS 14+; code signing alone does not imply notarization.

### Runtime requirements (Linux)

The tray uses freedesktop **StatusNotifierItem** over D-Bus, which needs a
`StatusNotifierWatcher` on the session bus:

- KDE Plasma: built in.
- GNOME: install the *AppIndicator and KStatusNotifierItem Support* extension.
- Other compositors (sway, niri, hyprland…): a panel or applet that implements
  the watcher — e.g. `noctalia`, `waybar` with the tray module, or `snixembed`.

Without one, the app says so and exits rather than leaving a gateway running with
no way to reach it.

---

## Usage

```bash
literouter-tray                    # run the app
literouter-tray --autostart        # add a sign-in entry, then run
literouter-tray --no-autostart     # remove it, then run
literouter-tray --autostart-status # report it and exit
```

The app inherits the environment, so `LITEROUTER_CONFIG` and
`LITEROUTER_STATE_DIR` work as they do everywhere else. On first run it writes
the seed config if there is none — "double-click and it works" has to mean that.
A config that exists but does not parse is reported rather than replaced.

### When a gateway is already running

The app **adopts it** rather than exiting. It opens the running instance's
console, keeps the tray icon, and leaves that gateway alone on exit — it did not
start it, so stopping it is not its call. Show then works against a live page.

This is not a nicety. A windowless program has exactly one channel to tell the
user anything, and it is the browser: stdout is discarded when a GUI program is
launched from a file manager, EUI-NEO's tray tooltip is dropped inside the
library (`(void)options.tooltip`), and there is no notification API. So a
"the port is taken, go open it yourself" message would reach nobody, and the app
would look like it did nothing at all.

The detection asks the running instance rather than probing the port: `GET
/__literouter/status` on the configured address, and the instance's own
`base_url` is what the console URL is built from — which is the only way
`port: 0` (kernel-assigned) can work. Port-zero discovery verifies a PID record
for the exact config path in the state directory; multiple matching live
instances are refused. HTTPS and IPv6 use scheme-aware addresses.

---

## Autostart

**Nothing is registered unless you ask for it.** The app never adds itself to
your login items on first launch — silently installing a startup entry is
exactly the kind of thing an installer should not do on someone's behalf.

When you do ask (`--autostart`), the entry goes in a **user-scoped** location
that you can delete by hand without elevation:

| Platform | Path |
|---|---|
| Linux | `$XDG_CONFIG_HOME/autostart/literouter.desktop` |
| macOS | `~/Library/LaunchAgents/io.github.yangqi0408.literouter.plist` |
| Windows | `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\literouter.lnk` |

Deliberately *not* used, and why:

- **No system service** (`systemd`, `LaunchDaemons`, Windows service). Those live
  outside your home, run before anyone logs in, and need elevation to add or
  remove. They suit a headless server; a tray app has to attach to a session.
- **No scheduled task on Windows.** The Startup folder is visible in Explorer and
  appears in Task Manager's Startup tab, so turning it off is one click rather
  than a `schtasks` invocation.
- **No PATH edits, no firewall rules, no registry Run keys.**

If you *do* want a headless gateway at boot, that is the CLI's own explicit
opt-in: `install.sh --service` (Linux) or `install-windows.ps1 -Service`
(Windows). The app and that path are independent on purpose.

---

## Limitations

- **Two tray entries.** See above — this is an EUI-NEO API limit, not a design
  choice here.
- **One gateway per port.** The app runs the gateway on the port your config
  names, or adopts an instance already there. It never runs two.
- **Show cannot report failure.** `openUrl` in EUI-NEO runs `xdg-open … &`
  through `std::system()`, so it returns success as soon as the fork happens,
  whether or not a browser appears. The app calls it and does not pretend the
  result means anything.
