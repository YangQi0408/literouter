#pragma once

#include <eui_neo.h>

import literouter.core;

// The console's whole mutable world. compose() runs every frame and must never
// own persistent state, so everything that has to survive a frame — the config
// store, the proxy, the telemetry snapshot, the editor drafts and the
// background work — lives in the one static AppState returned here.
namespace lr_gui {

enum class Page { Overview = 0, Providers, Routes, Logs, Settings };
inline constexpr int kPageCount = 5;

inline const char* pageTitle(Page page) {
    using literouter::i18n::tr;
    switch (page) {
        case Page::Overview: return tr("Overview");
        case Page::Providers: return tr("Providers");
        case Page::Routes: return tr("Routes");
        case Page::Logs: return tr("Logs");
        case Page::Settings: return tr("Settings");
    }
    return tr("Overview");
}

inline const char* pageSubtitle(Page page) {
    using literouter::i18n::tr;
    switch (page) {
        case Page::Overview: return tr("Live traffic, relay health and recent activity");
        case Page::Providers: return tr("Upstream relays, credentials and reachability");
        case Page::Routes: return tr("Model mapping and the ordered failover chain");
        case Page::Logs: return tr("Every request this proxy has handled");
        case Page::Settings: return tr("Listener, circuit breaker and logging");
    }
    return "";
}

// How often the UI re-reads ProxyServer telemetry, in frames. Six at 60fps is a
// 10Hz refresh — live enough to watch, cheap enough to leave running.
inline constexpr int kPollIntervalFrames = 30;

// Result of one provider probe, kept per relay id.
struct ProbeView {
    bool running = false;
    bool done = false;
    bool reachable = false;
    int status = 0;
    double latencyMs = 0.0;
    std::string detail;
    std::vector<std::string> models;
};

struct ToastState {
    bool visible = false;
    bool error = false;
    std::string title;
    std::string message;
};

enum class ConfirmKind { None, DeleteProvider, DeleteRoute, ResetCounters };

struct ConfirmState {
    bool open = false;
    ConfirmKind kind = ConfirmKind::None;
    int index = -1;
    std::string title;
    std::string message;
    std::string primary;
};

// Draft for the Providers editor dialog. Fields are edited as text/ints so the
// config model is only touched on Apply (or on the use-discovered-models action).
struct ProviderEditor {
    bool open = false;
    bool isNew = true;
    int index = -1;
    std::string id;
    std::string name;
    std::string baseUrl;
    std::string apiKey;
    std::string modelsText;
    std::string headersText;
    std::string note;
    int priority = 100;
    int weight = 1;
    int timeoutSec = 120;
    int connectTimeoutSec = 15;
    std::string protocol = "openai";
    int protocolChoice = 0;
    // Prices are free text in dollars: a per-million price is often below a
    // dollar, and a stepper of whole units could not express the cheap ones.
    std::string priceInText = "0";
    std::string priceOutText = "0";
    bool enabled = true;
    bool supportsStream = true;
    std::string statusLine;
    bool statusError = false;
};

// Draft for the Route editor (create / rename / toggle route).
struct RouteEditor {
    bool open = false;
    bool isNew = true;
    int index = -1; // -1 for new route, >= 0 for editing existing route
    std::string model;
    bool enabled = true;
    std::string statusLine;
    bool statusError = false;
};

// Draft for the Add-hop / Edit-hop dialog on the Routes page.
struct HopEditor {
    bool open = false;
    int routeIndex = -1;
    int hopIndex = -1; // -1 for adding a new hop, >= 0 for editing existing hop
    int providerIndex = 0;
    bool providerOpen = false;
    std::string model;
    std::string statusLine;
    bool statusError = false;
};

struct LogsView {
    int levelFilter = 0; // 0 all, 1 info, 2 warn, 3 error
    int kindFilter = 0;  // 0 all, 1 chat, 2 embeddings, 3 models, 4 admin, 5 system
    bool kindOpen = false;
    std::string search;
    bool paused = false;
    bool follow = true;
    int capacityChoice = 1; // 0 = 100, 1 = 250, 2 = 500
    float scroll = 0.0f;
    bool detailOpen = false;
    literouter::LogEntry detail;

    int cachedLevelFilter = -1;
    int cachedKindFilter = -1;
    std::string cachedSearch;
    std::uint64_t cachedLogSeq = static_cast<std::uint64_t>(-1);
    std::size_t cachedLogCount = 0;
    std::vector<std::size_t> filteredIndices;
};

// Serial background executor. Server start/stop and provider probes block, so
// they never run on the frame thread; a single worker keeps them ordered and
// each result is posted back to the UI queue.
class TaskQueue {
public:
    TaskQueue() = default;
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;
    ~TaskQueue() { stop(); }

    void start() {
        if (thread_.joinable()) {
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) {
                    if (stopping_) {
                        return;
                    }
                    continue;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::thread thread_;
    bool stopping_ = false;
};

struct AppState {
    // ── configuration ───────────────────────────────────────────────────
    std::uint64_t configRevision_ = 0;
    std::uint64_t baselineRevision_ = 0;
    bool configDirty_ = false;
    literouter::ConfigStore store;
    bool storeReady = false;
    bool configFileExisted = false;
    std::string loadError;
    std::string baselineJson; // last persisted bytes, for the unsaved-changes flag
    bool diskChanged = false;

    // ── the running proxy ───────────────────────────────────────────────
    literouter::ProxyServer server;
    literouter::Snapshot snapshot;
    std::vector<literouter::LogEntry> logs;
    std::uint64_t lastLogSeq = 0;
    bool serverStarting = false;
    bool serverStopping = false;
    std::string serverError;

    // ── probes ──────────────────────────────────────────────────────────
    std::map<std::string, ProbeView> probes;
    bool testingAll = false;

    // ── ui ──────────────────────────────────────────────────────────────
    Page page = Page::Overview;
    ToastState toast;
    ConfirmState confirm;
    ProviderEditor editor;
    RouteEditor routeEditor;
    HopEditor hop;
    LogsView logsView;
    std::string actionStatus;
    bool actionStatusError = false;
    float overviewScroll = 0.0f;
    float providersScroll = 0.0f;
    float routesScroll = 0.0f;
    float settingsScroll = 0.0f;

    // ── background jobs ─────────────────────────────────────────────────
    TaskQueue worker;
    std::mutex uiMutex;
    std::deque<std::function<void()>> uiTasks;

    // ── smoke mode (LITEROUTER_GUI_SMOKE=1) ─────────────────────────────
    bool smokeMode = false;
    int pinnedPage = -1; // LITEROUTER_GUI_PAGE: pin one page instead of cycling
    long long smokeFrame = 0;
    int smokeFramesPerPage = 150;
    long long smokeRect[kPageCount] = {};
    long long smokeText[kPageCount] = {};
    long long drivenFrames = 0;

    int pollCountdown = kPollIntervalFrames;
    double lastTrendHour = -1.0;
    double lastSecondTick = 0.0;

    // ════════════════════════════════════════════════════════════════════
    void init() {
        worker.start();
        auto loaded = literouter::ConfigStore::loadDefault();
        if (loaded) {
            store = std::move(*loaded);
        } else {
            auto seeded = literouter::ConfigStore::loadOrSeed(literouter::defaultConfigPath());
            if (seeded) {
                store = std::move(*seeded);
            }
            loadError = loaded.error();
        }
        if (!store.loadError().empty()) {
            loadError = store.loadError();
        }
        storeReady = true;
        configFileExisted = store.existsOnDisk();
        server.setConfigPath(store.path().string());
        baselineJson = store.toJson();
        baselineRevision_ = configRevision_;

        // Initialize display language from server config, env, or system
        if (const char* envLang = std::getenv("LITEROUTER_LANG"); envLang != nullptr && *envLang != '\0') {
            literouter::i18n::setLang(literouter::i18n::parseLang(envLang));
        } else if (!store.config().server.language.empty()) {
            literouter::i18n::setLang(literouter::i18n::parseLang(store.config().server.language));
        } else {
            literouter::i18n::setLang(literouter::i18n::detectSystemLang());
        }

        // Initialize display scale from server config or env
        if (const char* envScale = std::getenv("LITEROUTER_UI_SCALE"); envScale != nullptr && *envScale != '\0') {
            try {
                store.config().server.ui_scale = std::stod(envScale);
                ++configRevision_;
            } catch (...) {}
        }

        smokeMode = std::getenv("LITEROUTER_GUI_SMOKE") != nullptr;
        if (const char* pin = std::getenv("LITEROUTER_GUI_PAGE"); pin != nullptr && *pin != '\0') {
            const int wanted = std::atoi(pin);
            if (wanted >= 0 && wanted < kPageCount) {
                pinnedPage = wanted;
                page = static_cast<Page>(wanted);
            }
        }
        pollTelemetry();
    }

    void setLanguage(literouter::i18n::Lang lang) {
        store.config().server.language = std::string(literouter::i18n::langCode(lang));
        ++configRevision_;
        literouter::i18n::setLang(lang);
    }

    literouter::i18n::Lang currentLanguage() const {
        return literouter::i18n::getLang();
    }

    void toggleLanguage() {
        auto effective = literouter::i18n::resolveLang(literouter::i18n::getLang());
        if (effective == literouter::i18n::Lang::Zh) {
            setLanguage(literouter::i18n::Lang::En);
        } else {
            setLanguage(literouter::i18n::Lang::Zh);
        }
    }

    // ── zoom / page scale ───────────────────────────────────────────────
    static constexpr float kMinScale = 0.5f;
    static constexpr float kMaxScale = 2.5f;
    static constexpr float kScalePresets[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f};

    float currentScale() const {
        const double s = store.config().server.ui_scale;
        if (s <= 0.1) {
            return 1.0f;
        }
        return static_cast<float>(s);
    }

    void setScale(float scale, bool notify = false) {
        float clamped = std::clamp(scale, kMinScale, kMaxScale);
        clamped = std::round(clamped * 100.0f) / 100.0f;
        store.config().server.ui_scale = clamped;
        ++configRevision_;
        if (notify) {
            const int pct = static_cast<int>(std::round(clamped * 100.0f));
            showToast(std::string(literouter::i18n::tr("Page scale")),
                      std::string(literouter::i18n::tr("Zoom: ")) + std::to_string(pct) + "%", false);
        }
    }

    void zoomIn() {
        float next = currentScale() + 0.1f;
        if (next > kMaxScale) next = kMaxScale;
        setScale(next, true);
    }

    void zoomOut() {
        float next = currentScale() - 0.1f;
        if (next < kMinScale) next = kMinScale;
        setScale(next, true);
    }

    void resetZoom() {
        setScale(1.0f, true);
    }

    // ── background plumbing ─────────────────────────────────────────────
    void postToUi(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(uiMutex);
            uiTasks.push_back(std::move(task));
        }
    }

    bool drainUiTasks() {
        std::deque<std::function<void()>> local;
        {
            std::lock_guard<std::mutex> lock(uiMutex);
            local.swap(uiTasks);
        }
        for (auto& task : local) {
            task();
        }
        return !local.empty();
    }

    // ── the per-frame tick, driven by the hidden .onFrame() element ─────
    void onTick(float deltaSeconds) {
        if (deltaSeconds <= 0.0f) {
            return;
        }
        ++drivenFrames;
        bool dirty = drainUiTasks();
        if (smokeMode) {
            ++smokeFrame;
            // LITEROUTER_GUI_PAGE pins one page instead of cycling. Verification
            // needs a deterministic frame, and a timer-driven cycle can only be
            // sampled by guessing at the clock — which is exactly how a capture
            // ends up labelled "Providers" while showing Routes.
            const int wanted = pinnedPage >= 0 ? pinnedPage
                                               : static_cast<int>((smokeFrame / smokeFramesPerPage) % kPageCount);
            if (wanted != static_cast<int>(page)) {
                page = static_cast<Page>(wanted);
                dirty = true;
            }
        }
        if (--pollCountdown <= 0) {
            pollCountdown = kPollIntervalFrames;
            dirty = pollTelemetry() || dirty;
        }
        const double now = literouter::nowUnix();
        const double trendHour = std::floor(now / 3600.0);
        if (trendHour != lastTrendHour) {
            lastTrendHour = trendHour;
            dirty = true; // expire old chart slots even while the proxy is stopped
        }
        if (snapshot.running && now - lastSecondTick >= 1.0) {
            lastSecondTick = now;
            dirty = true; // keep the uptime / live indicators moving
        }
        if (dirty) {
            app::requestUpdate();
        }
    }

    void recordFrame(long long rects, long long texts) {
        const int index = static_cast<int>(page);
        if (index >= 0 && index < kPageCount) {
            smokeRect[index] += rects;
            smokeText[index] += texts;
        }
    }

    // ── telemetry ───────────────────────────────────────────────────────
    std::size_t logLimit() const {
        switch (logsView.capacityChoice) {
            case 0: return 100;
            case 2: return 500;
            default: return 250;
        }
    }

    static bool snapshotDiffers(const literouter::Snapshot& a, const literouter::Snapshot& b) {
        if (a.running != b.running || a.total_requests != b.total_requests ||
            a.total_failure != b.total_failure || a.active_requests != b.active_requests ||
            a.breakers_open != b.breakers_open || a.log_seq != b.log_seq ||
            a.bytes_out != b.bytes_out || a.providers.size() != b.providers.size() ||
            a.health.size() != b.health.size()) {
            return true;
        }
        for (std::size_t i = 0; i < a.health.size() && i < b.health.size(); ++i) {
            if (a.health[i].state != b.health[i].state ||
                a.health[i].consecutive_failures != b.health[i].consecutive_failures) {
                return true;
            }
        }
        return false;
    }

    // Drops the oldest entries past the capacity the operator picked. Returns
    // whether anything went, so the caller can repaint.
    bool trimLogs() {
        const std::size_t limit = logLimit();
        if (logs.size() <= limit) {
            return false;
        }
        logs.erase(logs.begin(), logs.begin() + static_cast<long>(logs.size() - limit));
        return true;
    }

    bool pollTelemetry() {
        bool changed = false;
        auto fresh = server.snapshot();
        if (snapshotDiffers(snapshot, fresh)) {
            changed = true;
        }
        snapshot = std::move(fresh);

        if (!logsView.paused) {
            auto entries = server.logsSince(lastLogSeq, 500);
            if (!entries.empty()) {
                changed = true;
                for (auto& entry : entries) {
                    lastLogSeq = entry.seq > lastLogSeq ? entry.seq : lastLogSeq;
                    logs.push_back(std::move(entry));
                }
                if (logsView.follow) {
                    logsView.scroll = 1000000.0f; // clamped by virtualList at the bottom
                }
            }
        }
        // Trimmed outside both guards above: the capacity segmented control only
        // records the choice, so lowering 500 → 100 has to take effect on the
        // next poll rather than waiting for traffic that may never come — and a
        // paused log must honour a smaller buffer too.
        if (trimLogs()) {
            changed = true;
        }
        if (checkDiskChange()) {
            changed = true;
        }
        return changed;
    }

    bool checkDiskChange() {
        std::error_code ec;
        const auto& path = store.path();
        if (!std::filesystem::exists(path, ec) || ec) {
            const bool was = diskChanged;
            diskChanged = false;
            return was;
        }
        const auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return false;
        }
        const bool changed = (stamp != store.mtime);
        const bool flipped = changed != diskChanged;
        diskChanged = changed;
        return flipped;
    }

    // ── server lifecycle (always off-thread) ────────────────────────────
    void startServer() {
        if (serverStarting) {
            return;
        }
        serverStarting = true;
        serverError.clear();
        const literouter::AppConfig config = store.config();
        worker.post([this, config] {
            auto result = server.start(config);
            const std::string error = result ? std::string{} : result.error();
            postToUi([this, error] {
                serverStarting = false;
                if (error.empty()) {
                    showToast(std::string(literouter::i18n::tr("Server started")),
                              std::string(literouter::i18n::tr("Listening on ")) + snapshot.base_url, false);
                } else {
                    serverError = error;
                    showToast(std::string(literouter::i18n::tr("Could not start server")), error, true);
                }
                pollTelemetry();
            });
        });
    }

    void stopServer() {
        if (serverStopping) {
            return;
        }
        serverStopping = true;
        worker.post([this] {
            server.stop();
            postToUi([this] {
                serverStopping = false;
                showToast(std::string(literouter::i18n::tr("Server stopped")),
                          std::string(literouter::i18n::tr("The listener is closed; in-flight replies drained.")), false);
                pollTelemetry();
            });
        });
    }

    void toggleServer() {
        if (server.running()) {
            stopServer();
        } else {
            startServer();
        }
    }

    // ── probes (always off-thread) ──────────────────────────────────────
    void testProvider(int index) {
        const auto& providers = store.config().providers;
        if (index < 0 || index >= static_cast<int>(providers.size())) {
            return;
        }
        const literouter::ProviderConfig provider = providers[static_cast<std::size_t>(index)];
        ProbeView& view = probes[provider.id];
        view.running = true;
        view.done = false;
        worker.post([this, provider] {
            auto result = literouter::probeProvider(provider, 15);
            ProbeView next;
            next.running = false;
            next.done = true;
            next.reachable = result.reachable;
            next.status = result.status;
            next.latencyMs = result.latency_ms;
            next.detail = result.detail;
            next.models = result.models;
            postToUi([this, id = provider.id, next]() mutable {
                probes[id] = std::move(next);
                const ProbeView& shown = probes[id];
                showToast(shown.reachable ? std::string(literouter::i18n::tr("Relay reachable"))
                                          : std::string(literouter::i18n::tr("Relay unreachable")),
                          id + " — " + (shown.detail.empty()
                                            ? (shown.reachable ? std::string(literouter::i18n::tr("answered /models"))
                                                               : std::string(literouter::i18n::tr("no response")))
                                            : shown.detail),
                          !shown.reachable);
            });
        });
    }

    void testAllProviders() {
        testingAll = true;
        const std::vector<literouter::ProviderConfig> providers = store.config().providers;
        for (const auto& provider : providers) {
            ProbeView& view = probes[provider.id];
            view.running = true;
            view.done = false;
        }
        worker.post([this, providers] {
            for (const auto& provider : providers) {
                auto result = literouter::probeProvider(provider, 15);
                ProbeView next;
                next.running = false;
                next.done = true;
                next.reachable = result.reachable;
                next.status = result.status;
                next.latencyMs = result.latency_ms;
                next.detail = result.detail;
                next.models = result.models;
                postToUi([this, id = provider.id, next]() mutable { probes[id] = std::move(next); });
            }
            postToUi([this] {
                testingAll = false;
                showToast(std::string(literouter::i18n::tr("Probe finished")),
                          std::string(literouter::i18n::tr("Reachability refreshed for every enabled relay.")), false);
            });
        });
    }

    // Probe the draft in the editor, so a relay can be tested before it is
    // saved. (testProvider probes a stored entry by index.)
    void testEditorProvider() {
        literouter::ProviderConfig provider;
        provider.id = literouter::trim(editor.id);
        if (provider.id.empty()) {
            editor.statusLine = std::string(literouter::i18n::tr("Give the relay an id before testing it."));
            editor.statusError = true;
            return;
        }
        provider.base_url = literouter::trim(editor.baseUrl);
        provider.api_key = literouter::trim(editor.apiKey);
        provider.protocol = editor.protocol;
        provider.timeout_sec = editor.timeoutSec;
        provider.connect_timeout_sec = editor.connectTimeoutSec;
        provider.models = splitList(editor.modelsText);
        probes[provider.id].running = true;
        probes[provider.id].done = false;
        worker.post([this, provider] {
            auto result = literouter::probeProvider(provider, 15);
            ProbeView next;
            next.running = false;
            next.done = true;
            next.reachable = result.reachable;
            next.status = result.status;
            next.latencyMs = result.latency_ms;
            next.detail = result.detail;
            next.models = result.models;
            postToUi([this, id = provider.id, next]() mutable { probes[id] = std::move(next); });
        });
    }

    const ProbeView* editorProbe() const {
        const std::string id = literouter::trim(editor.id);
        const auto it = probes.find(id);
        return it == probes.end() ? nullptr : &it->second;
    }

    // ── persistence ─────────────────────────────────────────────────────
    bool saveConfig() {
        auto result = store.save();
        if (!result) {
            actionStatus = result.error();
            actionStatusError = true;
            showToast(std::string(literouter::i18n::tr("Save failed")), result.error(), true);
            return false;
        }
        std::error_code ec;
        store.mtime = std::filesystem::last_write_time(store.path(), ec);
        store.setExistsOnDisk(true);
        configFileExisted = true;
        diskChanged = false;
        baselineJson = store.toJson();
        baselineRevision_ = configRevision_;
        if (server.running()) {
            server.updateConfig(store.config());
        }
        actionStatus = std::string(literouter::i18n::tr("Saved to ")) + store.path().string();
        actionStatusError = false;
        showToast(std::string(literouter::i18n::tr("Configuration saved")), store.path().string(), false);
        return true;
    }

    void reloadFromDisk() {
        auto loaded = literouter::ConfigStore::load(store.path());
        if (!loaded) {
            actionStatus = loaded.error();
            actionStatusError = true;
            showToast(std::string(literouter::i18n::tr("Reload failed")), loaded.error(), true);
            return;
        }
        store.config() = loaded->config();
        ++configRevision_;
        store.mtime = loaded->mtime;
        store.setExistsOnDisk(loaded->existsOnDisk());
        configFileExisted = loaded->existsOnDisk();
        diskChanged = false;
        baselineJson = store.toJson();
        baselineRevision_ = configRevision_;
        loadError.clear();
        if (server.running()) {
            server.updateConfig(store.config());
        }
        actionStatus = std::string(literouter::i18n::tr("Reloaded from disk"));
        actionStatusError = false;
        showToast(std::string(literouter::i18n::tr("Configuration reloaded")), store.path().string(), false);
    }

    bool hasUnsavedChanges() const { return configRevision_ != baselineRevision_; }

    void createConfigFile() {
        saveConfig();
    }

    void resetCounters() {
        server.resetStats();
        showToast(std::string(literouter::i18n::tr("Counters reset")),
                  std::string(literouter::i18n::tr("Traffic totals and breaker state cleared.")), false);
    }

    void clearLogs() {
        server.clearLogs();
        logs.clear();
        logsView.detailOpen = false;
        lastLogSeq = server.snapshot().log_seq;
        showToast(std::string(literouter::i18n::tr("Log cleared")),
                  std::string(literouter::i18n::tr("The on-disk ring buffer is empty.")), false);
    }

    void showToast(std::string title, std::string message, bool error) {
        toast.visible = true;
        toast.error = error;
        toast.title = std::move(title);
        toast.message = std::move(message);
    }

    // ── editors ─────────────────────────────────────────────────────────
    void openProviderEditor(int index, bool isNew) {
        const auto& providers = store.config().providers;
        editor = ProviderEditor{};
        editor.open = true;
        editor.isNew = isNew;
        editor.index = index;
        editor.modelsText.clear();
        editor.headersText.clear();
        if (isNew) {
            editor.protocol = "openai";
            editor.protocolChoice = 0;
        } else if (index >= 0 && index < static_cast<int>(providers.size())) {
            const auto& provider = providers[static_cast<std::size_t>(index)];
            editor.id = provider.id;
            editor.name = provider.name;
            editor.baseUrl = provider.base_url;
            editor.apiKey = provider.api_key;
            editor.protocol = provider.protocol;
            if (provider.protocol == "anthropic") editor.protocolChoice = 1;
            else if (provider.protocol == "gemini") editor.protocolChoice = 2;
            else if (provider.protocol == "openai_responses") editor.protocolChoice = 3;
            else editor.protocolChoice = 0;
            editor.priority = provider.priority;
            editor.weight = provider.weight;
            editor.timeoutSec = provider.timeout_sec;
            editor.connectTimeoutSec = provider.connect_timeout_sec;
            editor.enabled = provider.enabled;
            editor.supportsStream = provider.supports_stream;
            editor.priceInText = priceText(provider.price_in_per_million);
            editor.priceOutText = priceText(provider.price_out_per_million);
            editor.note = provider.note;
            editor.modelsText = joinLines(provider.models);
            editor.headersText = headersToText(provider.headers);
        }
    }

    // "$3.00" in and out of the config's double. Trailing zeroes are trimmed so
    // the field reads the way an operator typed it, not the way a formatter
    // would print it.
    static std::string priceText(double value) {
        if (value <= 0.0) {
            return "0";
        }
        std::string text = std::format("{:.4f}", value);
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
        return text;
    }

    static double parsePrice(const std::string& text) {
        const std::string trimmed = literouter::trim(text);
        if (trimmed.empty()) {
            return 0.0;
        }
        char* end = nullptr;
        const double value = std::strtod(trimmed.c_str(), &end);
        if (end == trimmed.c_str() || value < 0.0) {
            return 0.0;
        }
        return value;
    }

    void duplicateProvider(int index) {
        const auto& providers = store.config().providers;
        if (index < 0 || index >= static_cast<int>(providers.size())) {
            return;
        }
        literouter::ProviderConfig copy = providers[static_cast<std::size_t>(index)];
        copy.id = uniqueId(copy.id + "-copy");
        copy.name = copy.name.empty() ? copy.id : copy.name + " (copy)";
        store.config().providers.push_back(std::move(copy));
        ++configRevision_;
        saveConfig();
    }

    void applyProviderEditor() {
        const std::string id = literouter::trim(editor.id);
        if (id.empty()) {
            editor.statusLine = std::string(literouter::i18n::tr("An id is required — it is what routes and logs reference."));
            editor.statusError = true;
            return;
        }
        const auto& providers = store.config().providers;
        for (std::size_t i = 0; i < providers.size(); ++i) {
            if (providers[i].id == id && static_cast<int>(i) != editor.index) {
                const bool isZh = literouter::i18n::resolveLang(currentLanguage()) == literouter::i18n::Lang::Zh;
                editor.statusLine = isZh
                    ? ("已有其他中转站使用了 ID “" + id + "”。")
                    : ("Another relay already uses the id \"" + id + "\".");
                editor.statusError = true;
                return;
            }
        }

        literouter::ProviderConfig provider;
        provider.id = id;
        provider.name = literouter::trim(editor.name);
        provider.base_url = literouter::trim(editor.baseUrl);
        provider.api_key = literouter::trim(editor.apiKey);
        provider.protocol = editor.protocol;
        provider.priority = editor.priority;
        provider.weight = editor.weight;
        provider.timeout_sec = editor.timeoutSec;
        provider.connect_timeout_sec = editor.connectTimeoutSec;
        provider.enabled = editor.enabled;
        provider.supports_stream = editor.supportsStream;
        // Free text in, a number out: anything that does not parse is 0, which
        // is the same as leaving the price unset rather than refusing to save.
        provider.price_in_per_million = parsePrice(editor.priceInText);
        provider.price_out_per_million = parsePrice(editor.priceOutText);
        provider.note = editor.note;
        provider.models = splitList(editor.modelsText);
        provider.headers = parseHeaders(editor.headersText);

        if (editor.isNew || editor.index < 0 ||
            editor.index >= static_cast<int>(providers.size())) {
            store.config().providers.push_back(std::move(provider));
            ++configRevision_;
            editor.index = static_cast<int>(store.config().providers.size()) - 1;
            editor.isNew = false;
        } else {
            store.config().providers[static_cast<std::size_t>(editor.index)] = std::move(provider);
            ++configRevision_;
        }

        if (saveConfig()) {
            editor.statusLine.clear();
            editor.statusError = false;
            editor.open = false;
        } else {
            editor.statusLine = actionStatus;
            editor.statusError = true;
        }
    }

    void requestConfirm(ConfirmKind kind, int index, std::string title, std::string message,
                        std::string primary) {
        confirm.open = true;
        confirm.kind = kind;
        confirm.index = index;
        confirm.title = std::move(title);
        confirm.message = std::move(message);
        confirm.primary = std::move(primary);
    }

    void confirmAccepted() {
        const int index = confirm.index;
        switch (confirm.kind) {
            case ConfirmKind::DeleteProvider:
                deleteProvider(index);
                break;
            case ConfirmKind::DeleteRoute:
                deleteRoute(index);
                break;
            case ConfirmKind::ResetCounters:
                resetCounters();
                break;
            case ConfirmKind::None:
                break;
        }
        confirm.open = false;
        confirm.kind = ConfirmKind::None;
    }

    void deleteProvider(int index) {
        auto& providers = store.config().providers;
        if (index < 0 || index >= static_cast<int>(providers.size())) {
            return;
        }
        const std::string id = providers[static_cast<std::size_t>(index)].id;
        providers.erase(providers.begin() + index);
        ++configRevision_;
        if (editor.index == index) {
            editor.open = false;
        }
        saveConfig();
        showToast(std::string(literouter::i18n::tr("Relay removed")),
                  id + std::string(literouter::i18n::tr(" is no longer part of the routing model.")), false);
    }

    void deleteRoute(int index) {
        auto& routes = store.config().routes;
        if (index < 0 || index >= static_cast<int>(routes.size())) {
            return;
        }
        const std::string model = routes[static_cast<std::size_t>(index)].model;
        routes.erase(routes.begin() + index);
        ++configRevision_;
        saveConfig();
        showToast(std::string(literouter::i18n::tr("Route removed")),
                  model + std::string(literouter::i18n::tr(" will no longer be advertised.")), false);
    }

    void openRouteEditor(int routeIndex = -1) {
        const auto& routes = store.config().routes;
        if (routeIndex >= 0 && routeIndex < static_cast<int>(routes.size())) {
            routeEditor.isNew = false;
            routeEditor.index = routeIndex;
            routeEditor.model = routes[static_cast<std::size_t>(routeIndex)].model;
            routeEditor.enabled = routes[static_cast<std::size_t>(routeIndex)].enabled;
        } else {
            routeEditor.isNew = true;
            routeEditor.index = -1;
            routeEditor.model = uniqueModelName();
            routeEditor.enabled = true;
        }
        routeEditor.open = true;
        routeEditor.statusLine.clear();
        routeEditor.statusError = false;
    }

    void applyRouteEditor() {
        const std::string trimmed = literouter::trim(routeEditor.model);
        if (trimmed.empty()) {
            routeEditor.statusLine = std::string(literouter::i18n::tr("Model name cannot be empty"));
            routeEditor.statusError = true;
            return;
        }
        auto& routes = store.config().routes;
        for (std::size_t i = 0; i < routes.size(); ++i) {
            if (static_cast<int>(i) != routeEditor.index && routes[i].model == trimmed) {
                routeEditor.statusLine =
                    std::string(literouter::i18n::tr("A route with this model name already exists"));
                routeEditor.statusError = true;
                return;
            }
        }
        if (routeEditor.isNew) {
            literouter::RouteConfig route;
            route.model = trimmed;
            route.enabled = routeEditor.enabled;
            routes.push_back(std::move(route));
        } else if (routeEditor.index >= 0 && routeEditor.index < static_cast<int>(routes.size())) {
            routes[static_cast<std::size_t>(routeEditor.index)].model = trimmed;
            routes[static_cast<std::size_t>(routeEditor.index)].enabled = routeEditor.enabled;
        }
        ++configRevision_;
        if (saveConfig()) {
            routeEditor.open = false;
            routeEditor.statusLine.clear();
        } else {
            routeEditor.statusLine = actionStatus;
            routeEditor.statusError = true;
        }
        page = Page::Routes;
    }

    void openHopEditor(int routeIndex, int hopIndex = -1) {
        const auto& routes = store.config().routes;
        if (routeIndex < 0 || routeIndex >= static_cast<int>(routes.size())) {
            return;
        }
        hop = HopEditor{};
        hop.open = true;
        hop.routeIndex = routeIndex;
        hop.hopIndex = hopIndex;
        const auto& providers = store.config().providers;
        if (hopIndex >= 0 &&
            hopIndex < static_cast<int>(routes[static_cast<std::size_t>(routeIndex)].targets.size())) {
            const auto& target =
                routes[static_cast<std::size_t>(routeIndex)].targets[static_cast<std::size_t>(hopIndex)];
            hop.providerIndex = 0;
            for (std::size_t i = 0; i < providers.size(); ++i) {
                if (providers[i].id == target.provider) {
                    hop.providerIndex = static_cast<int>(i);
                    break;
                }
            }
            hop.model = target.model;
        } else {
            hop.providerIndex = 0;
            hop.model.clear();
        }
    }

    void addHopFromEditor() {
        const auto& providers = store.config().providers;
        auto& routes = store.config().routes;
        if (hop.routeIndex < 0 || hop.routeIndex >= static_cast<int>(routes.size())) {
            hop.open = false;
            return;
        }
        if (providers.empty()) {
            hop.statusLine = std::string(literouter::i18n::tr("Add a relay on the Providers page first."));
            hop.statusError = true;
            return;
        }
        const int providerIndex =
            std::clamp(hop.providerIndex, 0, static_cast<int>(providers.size()) - 1);
        literouter::RouteTarget target;
        target.provider = providers[static_cast<std::size_t>(providerIndex)].id;
        target.model = literouter::trim(hop.model);

        if (hop.hopIndex >= 0 &&
            hop.hopIndex < static_cast<int>(routes[static_cast<std::size_t>(hop.routeIndex)].targets.size())) {
            routes[static_cast<std::size_t>(hop.routeIndex)].targets[static_cast<std::size_t>(hop.hopIndex)] =
                std::move(target);
        } else {
            routes[static_cast<std::size_t>(hop.routeIndex)].targets.push_back(std::move(target));
        }
        ++configRevision_;
        if (saveConfig()) {
            hop.open = false;
            hop.statusLine.clear();
        } else {
            hop.statusLine = actionStatus;
            hop.statusError = true;
        }
    }

    void moveHop(int routeIndex, int hopIndex, int delta) {
        auto& routes = store.config().routes;
        if (routeIndex < 0 || routeIndex >= static_cast<int>(routes.size())) {
            return;
        }
        auto& targets = routes[static_cast<std::size_t>(routeIndex)].targets;
        const int next = hopIndex + delta;
        if (hopIndex < 0 || hopIndex >= static_cast<int>(targets.size()) || next < 0 ||
            next >= static_cast<int>(targets.size())) {
            return;
        }
        std::swap(targets[static_cast<std::size_t>(hopIndex)],
                  targets[static_cast<std::size_t>(next)]);
        ++configRevision_;
        saveConfig();
    }

    void removeHop(int routeIndex, int hopIndex) {
        auto& routes = store.config().routes;
        if (routeIndex < 0 || routeIndex >= static_cast<int>(routes.size())) {
            return;
        }
        auto& targets = routes[static_cast<std::size_t>(routeIndex)].targets;
        if (hopIndex < 0 || hopIndex >= static_cast<int>(targets.size())) {
            return;
        }
        targets.erase(targets.begin() + hopIndex);
        ++configRevision_;
        saveConfig();
    }

    void addRoute() {
        openRouteEditor(-1);
    }

    // ── helpers ─────────────────────────────────────────────────────────
    static std::vector<std::string> splitList(const std::string& text) {
        std::vector<std::string> out;
        std::string current;
        for (char ch : text) {
            if (ch == ',' || ch == '\n' || ch == '\r') {
                const std::string item = literouter::trim(current);
                if (!item.empty()) {
                    out.push_back(item);
                }
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        const std::string item = literouter::trim(current);
        if (!item.empty()) {
            out.push_back(item);
        }
        return out;
    }

    static std::string joinLines(const std::vector<std::string>& values) {
        std::string out;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                out += "\n";
            }
            out += values[i];
        }
        return out;
    }

    static std::map<std::string, std::string> parseHeaders(const std::string& text) {
        std::map<std::string, std::string> headers;
        std::string line;
        for (char ch : text) {
            if (ch == '\n' || ch == '\r') {
                addHeaderLine(headers, line);
                line.clear();
            } else {
                line.push_back(ch);
            }
        }
        addHeaderLine(headers, line);
        return headers;
    }

    static void addHeaderLine(std::map<std::string, std::string>& headers, const std::string& raw) {
        const std::size_t colon = raw.find(':');
        if (colon == std::string::npos) {
            return;
        }
        const std::string key = literouter::trim(raw.substr(0, colon));
        const std::string value = literouter::trim(raw.substr(colon + 1));
        if (!key.empty()) {
            headers[key] = value;
        }
    }

    static std::string headersToText(const std::map<std::string, std::string>& headers) {
        std::string out;
        bool first = true;
        for (auto it = headers.begin(); it != headers.end(); ++it) {
            if (!first) {
                out += "\n";
            }
            first = false;
            out += it->first + ": " + it->second;
        }
        return out;
    }

    std::string uniqueId(const std::string& base) const {
        const auto& providers = store.config().providers;
        const auto taken = [&](const std::string& candidate) {
            for (const auto& provider : providers) {
                if (provider.id == candidate) {
                    return true;
                }
            }
            return false;
        };
        if (!taken(base)) {
            return base;
        }
        for (int i = 2; i < 1000; ++i) {
            const std::string candidate = base + "-" + std::to_string(i);
            if (!taken(candidate)) {
                return candidate;
            }
        }
        return base + "-" + literouter::hexId(3);
    }

    std::string uniqueModelName() const {
        const auto& routes = store.config().routes;
        const auto taken = [&](const std::string& candidate) {
            for (const auto& route : routes) {
                if (route.model == candidate) {
                    return true;
                }
            }
            return false;
        };
        if (!taken("new-model")) {
            return "new-model";
        }
        for (int i = 2; i < 1000; ++i) {
            const std::string candidate = "new-model-" + std::to_string(i);
            if (!taken(candidate)) {
                return candidate;
            }
        }
        return "new-model-" + literouter::hexId(3);
    }
};

inline AppState& appState() {
    static AppState state;
    return state;
}

// Shared read-only views over the config and telemetry, used by every page.
inline std::string providerDisplayName(const literouter::ProviderConfig& provider) {
    return provider.name.empty() ? provider.id : provider.name;
}

// "no key" / "$OPENAI_API_KEY (set)" / "sk-abc…xyz" — the console never prints a
// literal secret, and an environment reference is shown as the variable it names.
inline std::string providerKeySource(const literouter::ProviderConfig& provider) {
    const bool isZh = literouter::i18n::resolveLang(literouter::i18n::getLang()) == literouter::i18n::Lang::Zh;
    if (provider.api_key.empty()) {
        return isZh ? "无密钥" : "no key";
    }
    if (literouter::isSecretReference(provider.api_key)) {
        const std::string name = literouter::secretReferenceName(provider.api_key);
        const bool set = !literouter::resolveSecret(provider.api_key).empty();
        return "$" + name + (isZh ? (set ? " (已配置)" : " (缺失)") : (set ? " (set)" : " (missing)"));
    }
    return literouter::maskSecret(provider.api_key);
}

inline const literouter::ProviderStat* statFor(const literouter::Snapshot& snapshot,
                                               const std::string& id) {
    for (const auto& stat : snapshot.providers) {
        if (stat.provider == id) {
            return &stat;
        }
    }
    return nullptr;
}

inline const literouter::ProviderHealth* healthFor(const literouter::Snapshot& snapshot,
                                                   const std::string& id) {
    for (const auto& health : snapshot.health) {
        if (health.provider == id) {
            return &health;
        }
    }
    return nullptr;
}

// Fonts: the bundled EUI fonts carry no CJK glyphs and this machine's system
// fallback list does not reach Noto CJK, so a Chinese relay name or upstream
// error would render as blanks. Probe once for a CJK-capable face and hand it to
// DslAppConfig::textFont(); fall through to the default font when none exists.
inline const std::string& uiFontPath() {
    static const std::string path = [] {
        const char* candidates[] = {
            "/usr/share/fonts/sarasa-gothic-nerd-fonts/sarasa-regular-nerd-font.ttc",
            "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc",
            "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
            "C:/Windows/Fonts/msyh.ttc",
            "/System/Library/Fonts/PingFang.ttc",
        };
        if (const char* override = std::getenv("LITEROUTER_GUI_FONT");
            override != nullptr && *override != '\0') {
            std::error_code ec;
            if (std::filesystem::exists(override, ec) && !ec) {
                return std::string(override);
            }
            std::println("LITEROUTER_GUI_FONT points at a missing file: {} — using the default font",
                         override);
        }
        for (const char* candidate : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec) && !ec) {
                return std::string(candidate);
            }
        }
        std::println("no CJK-capable font found; upstream text may render as blanks");
        return std::string{};
    }();
    return path;
}

// Smoke budget: five pages at 150 frames each.
inline int smokeFrameBudget() {
    return kPageCount * 150;
}

} // namespace lr_gui
