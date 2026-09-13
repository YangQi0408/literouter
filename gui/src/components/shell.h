#pragma once

#include "../app_state.h"
#include "lr_theme.h"
#include "widgets.h"

// The persistent shell: left navigation, the content top bar, and the
// contextual banner strip (missing file / external edit / unsaved changes).
namespace lr_gui {

inline void composeSidebar(eui::Ui& ui, float width, float height) {
    AppState& state = appState();
    const Palette& p = palette();

    ui.rect("shell.sidebar.bg").position(0.0f, 0.0f).size(width, height).color(p.sidebar).build();
    ui.rect("shell.sidebar.edge").position(width - 1.0f, 0.0f).size(1.0f, height).color(p.borderSoft).build();

    // Product mark + version.
    ui.rect("shell.brand.mark").position(18.0f, 20.0f).size(30.0f, 30.0f).radius(9.0f).color(p.accent).build();
    ui.text("shell.brand.mark.text")
        .position(18.0f, 20.0f)
        .size(30.0f, 30.0f)
        .text("LR")
        .fontSize(13.0f)
        .lineHeight(30.0f)
        .fontWeight(800)
        .color(eui::Color{1.0f, 1.0f, 1.0f, 1.0f})
        .horizontalAlign(eui::HorizontalAlign::Center)
        .verticalAlign(eui::VerticalAlign::Center)
        .build();
    ui.text("shell.brand.name")
        .position(58.0f, 18.0f)
        .size(width - 76.0f, 22.0f)
        .text("literouter")
        .fontSize(16.0f)
        .lineHeight(20.0f)
        .fontWeight(760)
        .color(p.text)
        .build();
    ui.text("shell.brand.version")
        .position(58.0f, 38.0f)
        .size(width - 76.0f, 18.0f)
        .text(std::string("v") + std::string(literouter::kVersion))
        .fontSize(11.5f)
        .lineHeight(15.0f)
        .color(p.textFaint)
        .build();

    eyebrow(ui, "shell.nav.label", 18.0f, 92.0f, width - 36.0f, literouter::i18n::tr("CONSOLE"), p.textFaint);

    constexpr float navY = 116.0f;
    constexpr float itemHeight = 40.0f;
    constexpr float itemGap = 4.0f;
    const float itemWidth = width - 20.0f;

    for (int i = 0; i < kPageCount; ++i) {
        const Page page = static_cast<Page>(i);
        const bool selected = state.page == page;
        const float y = navY + static_cast<float>(i) * (itemHeight + itemGap);
        const std::string id = "shell.nav." + std::to_string(i);

        ui.stack(id)
            .position(10.0f, y)
            .size(itemWidth, itemHeight)
            .content([&] {
                ui.rect(id + ".hit")
                    .size(itemWidth, itemHeight)
                    .radius(palette().radiusControl)
                    .states(selected ? withAlpha(p.accent, 0.15f) : transparent(),
                            withAlpha(p.text, 0.06f), withAlpha(p.text, 0.10f))
                    .transition(motion())
                    .animate(eui::AnimProperty::Color)
                    .onClick([i] { appState().page = static_cast<Page>(i); })
                    .build();
                if (selected) {
                    ui.rect(id + ".bar")
                        .position(0.0f, 10.0f)
                        .size(3.0f, itemHeight - 20.0f)
                        .radius(2.0f)
                        .color(p.accent)
                        .transition(motion())
                        .animate(eui::AnimProperty::Color)
                        .build();
                }
                ui.text(id + ".label")
                    .position(18.0f, 0.0f)
                    .size(itemWidth - 30.0f, itemHeight)
                    .text(pageTitle(page))
                    .fontSize(14.0f)
                    .lineHeight(itemHeight)
                    .fontWeight(selected ? 650 : 520)
                    .color(selected ? p.text : p.textMuted)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .transition(motion())
                    .animate(eui::AnimProperty::TextColor)
                    .build();
            })
            .build();
    }

    // Server status strip.
    constexpr float stripHeight = 94.0f;
    const float stripY = height - stripHeight;
    const bool running = state.server.running();
    const bool transitioning = state.serverStarting || state.serverStopping;
    const eui::Color stateColor = running ? p.success : (transitioning ? p.warn : p.textFaint);
    const std::string word = state.serverStarting ? std::string(literouter::i18n::tr("Starting"))
                             : state.serverStopping ? std::string(literouter::i18n::tr("Stopping"))
                             : running             ? std::string(literouter::i18n::tr("Running"))
                                                   : std::string(literouter::i18n::tr("Stopped"));

    ui.rect("shell.status.bg").position(0.0f, stripY).size(width, stripHeight).color(p.surfaceSunken).build();
    ui.rect("shell.status.edge").position(0.0f, stripY).size(width, 1.0f).color(p.borderSoft).build();
    dot(ui, "shell.status.dot", 18.0f, stripY + 20.0f, 9.0f, stateColor);
    ui.text("shell.status.word")
        .position(34.0f, stripY + 14.0f)
        .size(width - 52.0f, 20.0f)
        .text(word)
        .fontSize(13.0f)
        .lineHeight(19.0f)
        .fontWeight(650)
        .color(stateColor)
        .transition(motion())
        .animate(eui::AnimProperty::TextColor)
        .build();
    const int port = state.snapshot.port > 0 ? state.snapshot.port : state.store.config().server.port;
    const std::string host = !state.snapshot.host.empty() ? state.snapshot.host
                                                          : state.store.config().server.host;
    ui.text("shell.status.addr")
        .position(18.0f, stripY + 42.0f)
        .size(width - 36.0f, 18.0f)
        .text(host + ":" + std::to_string(port))
        .fontSize(12.0f)
        .lineHeight(16.0f)
        .color(p.textMuted)
        .build();
    ui.text("shell.status.uptime")
        .position(18.0f, stripY + 62.0f)
        .size(width - 36.0f, 18.0f)
        .text(running ? std::string(literouter::i18n::tr("up ")) + literouter::humanDuration(state.snapshot.uptime_sec)
                      : std::string(literouter::i18n::tr("listener closed")))
        .fontSize(12.0f)
        .lineHeight(16.0f)
        .color(p.textFaint)
        .build();
}

inline void composeTopBar(eui::Ui& ui, float x, float y, float width, float height) {
    AppState& state = appState();
    const Palette& p = palette();
    const bool running = state.server.running();
    const bool transitioning = state.serverStarting || state.serverStopping;

    ui.rect("shell.topbar.bg").position(x, y).size(width, height).color(p.canvas).build();
    ui.rect("shell.topbar.edge").position(x, y + height - 1.0f).size(width, 1.0f).color(p.borderSoft).build();

    // Right side controls placed first to calculate exact remaining space for title
    constexpr float buttonWidth = 108.0f;
    constexpr float buttonHeight = 36.0f;
    const float buttonX = x + width - layout::pagePadding - buttonWidth;
    const float centerY = y + (height - buttonHeight) * 0.5f;

    actionButton(ui, "shell.server.toggle", buttonX, centerY, buttonWidth, buttonHeight,
                 running ? literouter::i18n::tr("Stop proxy") : literouter::i18n::tr("Start proxy"),
                 !running, [] { appState().toggleServer(); },
                 transitioning);

    // Live indicator: compact when space is tight
    const bool compactIndicator = (width < 820.0f);
    const float indicatorWidth = compactIndicator ? 80.0f : 136.0f;
    const float indicatorX = buttonX - 8.0f - indicatorWidth;
    const std::uint64_t inFlight = state.snapshot.active_requests;
    const std::string liveText = running
        ? (compactIndicator
               ? (inFlight == 0 ? std::string(literouter::i18n::tr("idle"))
                                : std::to_string(inFlight) + " " + std::string(literouter::i18n::tr("req")))
               : (inFlight == 0 ? std::string(literouter::i18n::tr("idle"))
                                : std::to_string(inFlight) + " " +
                                      std::string(literouter::i18n::tr(inFlight == 1 ? "active request" : "active requests"))))
        : std::string(literouter::i18n::tr("offline"));

    ui.stack("shell.live")
        .position(indicatorX, centerY)
        .size(indicatorWidth, buttonHeight)
        .content([&] {
            ui.rect("shell.live.bg")
                .size(indicatorWidth, buttonHeight)
                .radius(palette().radiusControl)
                .color(withAlpha(p.surface, 0.9f))
                .border(1.0f, withAlpha(p.border, 0.75f))
                .build();
            dot(ui, "shell.live.dot", 12.0f, buttonHeight * 0.5f - 4.0f, 8.0f,
                running ? (inFlight > 0 ? p.accent : p.success) : p.textFaint);
            ui.text("shell.live.text")
                .position(24.0f, 0.0f)
                .size(indicatorWidth - 28.0f, buttonHeight)
                .text(liveText)
                .fontSize(compactIndicator ? 11.5f : 12.5f)
                .lineHeight(buttonHeight)
                .fontWeight(550)
                .color(running ? p.textMuted : p.textFaint)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();

    // Quick Language toggle button
    const float langWidth = 64.0f;
    const float langX = indicatorX - 8.0f - langWidth;
    const bool isZh = literouter::i18n::resolveLang(state.currentLanguage()) == literouter::i18n::Lang::Zh;
    actionButton(ui, "shell.lang.toggle", langX, centerY, langWidth, buttonHeight,
                 isZh ? "中/EN" : "EN/中", false, [] { appState().toggleLanguage(); });

    // Zoom controls: [-] 100% [+]
    const float zoomBtnGap = 4.0f;
    const float zoomW = 26.0f;
    const float resetW = 44.0f;
    const float zoomTotalW = zoomW * 2.0f + resetW + zoomBtnGap * 2.0f;
    const float zoomX = langX - 8.0f - zoomTotalW;
    const int currentPct = static_cast<int>(std::round(state.currentScale() * 100.0f));
    actionButton(ui, "shell.zoom.out", zoomX, centerY, zoomW, buttonHeight, "-", false,
                 [] { appState().zoomOut(); });
    actionButton(ui, "shell.zoom.reset", zoomX + zoomW + zoomBtnGap, centerY, resetW, buttonHeight,
                 std::to_string(currentPct) + "%", false,
                 [] { appState().resetZoom(); });
    actionButton(ui, "shell.zoom.in", zoomX + zoomW + resetW + zoomBtnGap * 2.0f, centerY, zoomW, buttonHeight, "+", false,
                 [] { appState().zoomIn(); });

    // Title and Subtitle dynamically bounded by available space to zoom controls
    const float titleLeft = x + layout::pagePadding;
    const float maxTitleWidth = std::max(60.0f, zoomX - titleLeft - 12.0f);

    ui.text("shell.topbar.title")
        .position(titleLeft, y + 15.0f)
        .size(maxTitleWidth, 26.0f)
        .text(pageTitle(state.page))
        .fontSize(21.0f)
        .lineHeight(26.0f)
        .fontWeight(760)
        .color(p.text)
        .transition(motion())
        .animate(eui::AnimProperty::TextColor)
        .build();

    if (width >= 720.0f && maxTitleWidth > 120.0f) {
        ui.text("shell.topbar.subtitle")
            .position(titleLeft, y + 41.0f)
            .size(maxTitleWidth, 18.0f)
            .text(pageSubtitle(state.page))
            .fontSize(12.5f)
            .lineHeight(16.0f)
            .color(p.textMuted)
            .build();
    }
}

// Contextual banner strip. Returns the height it consumed (0 when nothing needs
// saying), so the page body starts below it.
inline float composeBanners(eui::Ui& ui, float x, float y, float width) {
    AppState& state = appState();
    const Palette& p = palette();

    std::string message;
    std::string actionLabel;
    eui::Color accent = p.accent;
    std::function<void()> action;

    if (!state.loadError.empty()) {
        message = std::string(literouter::i18n::tr(
            "The configuration on disk could not be parsed — this session is running on the seed defaults."));
        actionLabel = std::string(literouter::i18n::tr("Reload"));
        accent = p.danger;
        action = [] { appState().reloadFromDisk(); };
    } else if (!state.configFileExisted) {
        message = std::string(literouter::i18n::tr(
            "No config file yet. This session uses the seeded example until you create one."));
        actionLabel = std::string(literouter::i18n::tr("Create config file"));
        accent = p.warn;
        action = [] { appState().createConfigFile(); };
    } else if (state.diskChanged) {
        message = std::string(literouter::i18n::tr(
            "The configuration file changed on disk. Reload to pick up the new routing model."));
        actionLabel = std::string(literouter::i18n::tr("Reload from disk"));
        action = [] { appState().reloadFromDisk(); };
    } else if (state.hasUnsavedChanges()) {
        message = std::string(literouter::i18n::tr("You have unsaved changes."));
        actionLabel = std::string(literouter::i18n::tr("Save now"));
        action = [] { appState().saveConfig(); };
    }

    if (message.empty()) {
        return 0.0f;
    }

    constexpr float height = 46.0f;
    ui.stack("shell.banner")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("shell.banner.bg")
                .size(width, height)
                .color(withAlpha(accent, 0.12f))
                .build();
            ui.rect("shell.banner.edge")
                .position(0.0f, height - 1.0f)
                .size(width, 1.0f)
                .color(withAlpha(accent, 0.35f))
                .build();
            dot(ui, "shell.banner.dot", layout::pagePadding, height * 0.5f - 4.0f, 8.0f, accent);
            ui.text("shell.banner.text")
                .position(layout::pagePadding + 18.0f, 0.0f)
                .size(width - layout::pagePadding * 2.0f - 210.0f, height)
                .text(message)
                .fontSize(13.0f)
                .lineHeight(height)
                .color(p.text)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            const float buttonWidth = 176.0f;
            actionButton(ui, "shell.banner.action", width - layout::pagePadding - buttonWidth,
                         (height - 30.0f) * 0.5f, buttonWidth, 30.0f, actionLabel, false,
                         std::move(action));
        })
        .build();
    return height;
}

} // namespace lr_gui
