# Desktop GUI Console Guide

> Running on a headless server or in a container? Use the built-in [Web Console](protocols-api.md#built-in-web-console) — open `/ui` in a browser, no graphics stack needed.

`literouter-gui` is a native OpenGL desktop dashboard frontend designed for developers. Built with a lightweight declarative UI framework, it offers minimal CPU/memory footprint while providing real-time telemetry, provider health matrices, visual route adjustments, and log exploration.

---

## Table of Contents

- [Launch & Execution](#launch--execution)
- [Six Core Panels](#six-core-panels)
  - [1. Overview](#1-overview)
  - [2. Providers](#2-providers)
  - [3. Routes](#3-routes)
  - [4. Logs](#4-logs)
  - [5. Settings](#5-settings)
  - [6. Clients](#6-clients)
- [Multi-language Localization (i18n)](#multi-language-localization-i18n)
- [Dynamic UI Scaling (UI Scale)](#dynamic-ui-scaling-ui-scale)
- [High-Definition CJK Font Fallback](#high-definition-cjk-font-fallback)
- [Automated Headless Smoke Testing](#automated-headless-smoke-testing)

---

## Launch & Execution

Launch the GUI console from the project root:

```bash
mcpp run -p gui
```

> [!NOTE]
> In GUI mode, the underlying proxy engine (`literouter.core`) and the UI render loop execute within the same process. After opening the console, click **Start proxy** in the top-right corner to listen on the configured port (default `8787`); no separate CLI process is needed.

---

## Six Core Panels

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│ LR  literouter  │ Overview Providers Routes Logs Settings │ [-] 100% [+] [EN/中] ● Active │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  [ Requests ]     [ Success Rate ]   [ In-flight ]   [ Avg Latency ]   [ Tokens ]   │
│     1,248              99.2%               0             412 ms          3.8M       │
│                                                                             │
│  Provider Health Matrix                                                     │
│  ┌─────────────────────────┐  ┌─────────────────────────┐                   │
│  │ OpenAI Official  ● OK   │  │ DeepSeek Backup ● Degr  │                   │
│  │ priority 10 · 1.1k ok   │  │ priority 20 · 128 ok    │                   │
│  └─────────────────────────┘  └─────────────────────────┘                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1. Overview
- **Global Metric Tiles**: Real-time totals for requests, success rate, in-flight concurrency, average round-trip latency (RTT), and accumulated tokens;
- **Hourly Traffic Trend**: Request counts for the last 24 hourly buckets, including the current partial hour, scaled to the busiest hour. Idle hours retain empty slots and hours containing failures are amber. Hover for the local hour, requests, successes/failures, tokens, outgoing bytes and estimated cost. An empty state appears when the window has no traffic; persisted telemetry carries the history across restarts;
- **Provider Health Matrix**: Status badges for each provider (Healthy green, Degraded yellow, Open red), consecutive failure counters, and request volume;
- **Alert Banner**: Alerts trigger when a provider trips its circuit breaker, with quick actions to view causes or reset telemetry.

### 2. Providers
- **Lifecycle Control**: Toggle providers on/off, adjust priority, weight, and timeouts;
- **Connectivity Testing**: Test TLS handshakes and upstream latency on demand;
- **Model Auto-Discovery & Sync**: Click "Probe Models" to scan available physical models from upstream and merge them into your configuration.

### 3. Routes
- **Visual Route Graph**: Visual display of fallback chains (Primary -> Backup A -> Backup B);
- **Model Aliasing**: Easily map logical model names to upstream physical models.

### 4. Logs
- **Virtual Scrolling List**: Handles hundreds of request records smoothly with minimal memory;
- **Filtering & Search**: Filter by level (All / Info / Warn / Error) or stream mode, and full-text search paths and models;
- **Log Inspector Drawer**: Inspect detailed metadata, timing breakdowns, and request/response body previews.

### 5. Settings
- **Server Parameters**: Configure listen host, port, and client authentication key;
- **Circuit Breaker Tuning**: Adjust failure thresholds and cooldown intervals graphically;
- **Telemetry Persistence**: Choose whether counters, relay stats and the request log are written to the state directory and resumed after a restart;
- **Hot Reload & Safe Save**: Save settings to disk while strictly preserving secret placeholders, or reload from disk;
- **Code Snippet Generator**: Copy ready-to-use curl commands and SDK connection snippets.

---

### 6. Clients
- **Optional distribution**: Personal access requires no accounts. Set the administrator key in Settings before creating the first client; account keys only call model endpoints.
- **Account editor**: Configure the display name, enabled state, allowed models and provider groups, requests per minute, concurrency, daily requests/tokens and token reservation. Empty allowlists mean unrestricted and a zero limit means unlimited.
- **Key management**: Add, replace, enable, disable or delete multiple keys in one account. Existing values remain hidden; a blank replacement preserves the stored secret or environment reference. Key edits stay in the draft until **Save client**; Cancel discards the draft. Validation or disk errors preserve the live config and keep the draft available for correction.
- **Live usage**: Request outcomes, active requests, token quotas and reservations, and estimated cost use the same ledger as the API. Daily limits reset at UTC midnight. Reservations remain charged when an upstream omits usage; clearing dashboard counters or disabling telemetry does not reset quotas.
- **Channel groups**: Assign comma-separated group names in the provider editor, then select those names in a client account. Log detail shows client and key IDs; both can be searched in Logs.

---

## Multi-language Localization (i18n)

The console features full English and Simplified Chinese support:

- **One-Click Switcher**: Click the `[EN/中]` button in the top navigation bar to switch instantly;
- **Language Modes**:
  - `English`
  - `Simplified Chinese` (`简体中文`)
  - `Follow System (Auto)`: Infers language from `LANG`, `LC_ALL`, or Windows locale;
- **Environment Variable Override**:
  ```bash
  LITEROUTER_LANG=en mcpp run -p gui
  ```

---

## Dynamic UI Scaling (UI Scale)

Scale the entire user interface smoothly across different monitor resolutions (from 1080p to 4K):

- **Presets**: `80%`, `90%`, `100%`, `110%`, `125%`, `150%`;
- **Top Bar Controls**: Click `[-]` and `[+]` buttons to scale incrementally;
- **Global Shortcuts**:
  - `Ctrl` + `+` (or keypad `+`): Zoom in
  - `Ctrl` + `-` (or keypad `-`): Zoom out
  - `Ctrl` + `0` (or keypad `0`): Reset to 100%
- **Environment Variable**:
  ```bash
  LITEROUTER_UI_SCALE=1.25 mcpp run -p gui
  ```

---

## High-Definition CJK Font Fallback

To ensure sharp typography across Linux distributions without CJK rendering issues, `literouter-gui` scans system fonts and falls back gracefully:

1. **Sarasa Gothic** (`sarasa-regular-nerd-font.ttc`): Blends Source Han Sans with Iosevka for crisp clarity;
2. **WenQuanYi Zen Hei** (`wqy-zenhei.ttc`): Standard Linux CJK typeface;
3. **Noto Sans CJK** family;
4. **Microsoft YaHei** (`msyh.ttc`, Windows) / **PingFang** (`PingFang.ttc`, macOS).

Override with a custom font file:
```bash
LITEROUTER_GUI_FONT="/path/to/custom-font.ttf" mcpp run -p gui
```

---

## Automated Headless Smoke Testing

Verify GUI rendering integrity in CI pipelines without an attached physical display:

```bash
LITEROUTER_GUI_SMOKE=1 mcpp run -p gui
```

This renders 150 frames across all 6 panels sequentially and exits cleanly with exit code 0 on success.
