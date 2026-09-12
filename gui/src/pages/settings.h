#pragma once

#include "../app_state.h"
#include "../components/theme.h"
#include "../components/widgets.h"

// Page 5 — Settings: the whole ServerConfig, the persistence controls, and a
// copy-ready "How to use it" block.
namespace lr_gui {

namespace detail {

inline void settingsCard(eui::Ui& ui, const std::string& id, float x, float y, float width,
                         float height, const std::string& title, const std::string& sub = {}) {
    const Palette& p = palette();
    ui.rect(id).position(x, y).size(width, height).radius(p.radiusCard).color(p.surface)
        .border(1.0f, withAlpha(p.border, 0.8f)).build();
    ui.text(id + ".title")
        .position(x + 18.0f, y + 16.0f)
        .size(width - 36.0f, 22.0f)
        .text(title)
        .fontSize(15.0f)
        .lineHeight(20.0f)
        .fontWeight(720)
        .color(p.text)
        .build();
    if (!sub.empty()) {
        ui.text(id + ".sub")
            .position(x + 18.0f, y + 38.0f)
            .size(width - 36.0f, 18.0f)
            .text(sub)
            .fontSize(11.5f)
            .lineHeight(15.0f)
            .color(p.textFaint)
            .build();
    }
    divider(ui, id + ".divider", x + 18.0f, y + 60.0f, width - 36.0f);
}

} // namespace detail

inline void composeSettings(eui::Ui& ui, float x, float y, float width, float height) {
    AppState& state = appState();
    const Palette& p = palette();
    literouter::AppConfig& config = state.store.config();
    literouter::ServerConfig& server = config.server;

    components::scrollView(ui, "settings.scroll")
        .theme(uiTokens())
        .position(x, y)
        .size(width, height)
        .offset(state.settingsScroll)
        .step(layout::scrollStep)
        .scrollbarWidth(9.0f)
        .scrollbarGap(6.0f)
        .onChange([&state](float value) { state.settingsScroll = value; })
        .content([&](eui::Ui& contentUi, float contentWidth, float) {
            const float leftX = layout::pagePadding;
            const float rightEdge = contentWidth - layout::pagePadding;
            const float headerY = 16.0f;
            const float statusY = headerY + 44.0f;

            // ── cards ───────────────────────────────────────────────────────────
            const float cardsTop = statusY + 46.0f;
            const bool twoColumns = (contentWidth >= 780.0f);
            const float columnGap = 16.0f;
            const float columnWidth = twoColumns
                ? (contentWidth - 2.0f * layout::pagePadding - columnGap) * 0.5f
                : (contentWidth - 2.0f * layout::pagePadding);
            const float fieldWidth = columnWidth - 36.0f;
            const float halfWidth = (fieldWidth - 14.0f) * 0.5f;
            constexpr float kCardBottomPadding = 18.0f;

            // Derived card heights
            constexpr float kListenerHeight = 412.0f;
            constexpr float kBreakerHeight = 96.0f + 34.0f + kCardBottomPadding;
            constexpr float kLoggingHeight = 144.0f + 32.0f + kCardBottomPadding;
            constexpr float kDisplayHeight = 236.0f + 18.0f + kCardBottomPadding;
            constexpr float kHowHeight = kDisplayHeight;

            const float listenerX = leftX;
            const float listenerY = cardsTop;

            const float breakerX = twoColumns ? (leftX + columnWidth + columnGap) : leftX;
            const float breakerY = twoColumns ? cardsTop : (listenerY + kListenerHeight + columnGap);

            const float loggingX = breakerX;
            const float loggingY = breakerY + kBreakerHeight + columnGap;

            const float row1Bottom = twoColumns
                ? std::max(listenerY + kListenerHeight, loggingY + kLoggingHeight)
                : (loggingY + kLoggingHeight);
            const float row2Y = row1Bottom + columnGap;

            const float displayX = leftX;
            const float displayY = row2Y;

            const float howX = twoColumns ? (leftX + columnWidth + columnGap) : leftX;
            const float howY = twoColumns ? row2Y : (displayY + kDisplayHeight + columnGap);
            const float howWidth = columnWidth;
            const float howHeight = kHowHeight;

            const float canvasHeight = (twoColumns ? row2Y : howY) + howHeight + layout::pagePadding + 16.0f;

            contentUi.stack("settings.canvas")
                .size(contentWidth, canvasHeight)
                .content([&] {
                    // Persistence action buttons
                    const float clearW = 90.0f;
                    const float resetW = 124.0f;
                    const float reloadW = 96.0f;
                    const float saveW = 86.0f;
                    const float btnGap = 8.0f;

                    float btnRight = rightEdge;
                    actionButton(contentUi, "settings.save", btnRight - saveW, headerY, saveW, 34.0f,
                                 std::string(literouter::i18n::tr("Save")), true,
                                 [] { appState().saveConfig(); });
                    btnRight -= saveW + btnGap;
                    actionButton(contentUi, "settings.reload", btnRight - reloadW, headerY, reloadW, 34.0f,
                                 std::string(literouter::i18n::tr("Reload")),
                                 false, [] { appState().reloadFromDisk(); });
                    btnRight -= reloadW + btnGap;
                    actionButton(contentUi, "settings.reset", btnRight - resetW, headerY, resetW, 34.0f,
                                 std::string(literouter::i18n::tr("Reset counters")), false, [] {
                                     appState().requestConfirm(ConfirmKind::ResetCounters, -1,
                                                               std::string(literouter::i18n::tr("Reset counters?")),
                                                               std::string(literouter::i18n::tr("Traffic totals and circuit-breaker state go back to "
                                                                           "zero. The listener keeps running.")),
                                                               std::string(literouter::i18n::tr("Reset counters")));
                                 });
                    btnRight -= resetW + btnGap;
                    actionButton(contentUi, "settings.clear", btnRight - clearW, headerY, clearW, 34.0f,
                                 std::string(literouter::i18n::tr("Clear log")),
                                 false, [] { appState().clearLogs(); });
                    const float buttonsLeftEdge = btnRight - clearW;

                    // Heading on the left if space permits
                    const float maxHeadingW = buttonsLeftEdge - leftX - 16.0f;
                    if (maxHeadingW > 200.0f) {
                        contentUi.text("settings.heading")
                            .position(leftX, headerY + 8.0f)
                            .size(maxHeadingW, 20.0f)
                            .text(std::string(literouter::i18n::tr("Everything here is written to a single JSON file. Edits are held in memory "
                                  "until you save.")))
                            .fontSize(12.0f)
                            .lineHeight(16.0f)
                            .color(p.textMuted)
                            .build();
                    }

                    // Status line: path and existence, placed on row 2, safely BELOW the buttons row
                    const std::string pathLine =
                        state.store.path().string() + (state.store.existsOnDisk()
                                                           ? std::string("  ·  ") + std::string(literouter::i18n::tr("exists on disk"))
                                                           : std::string("  ·  ") + std::string(literouter::i18n::tr("not created yet")));
                    contentUi.text("settings.path")
                        .position(leftX, statusY)
                        .size(contentWidth - 2.0f * layout::pagePadding, 18.0f)
                        .text(pathLine)
                        .fontSize(11.5f)
                        .lineHeight(16.0f)
                        .color(p.textFaint)
                        .build();

                    std::string status = state.actionStatus;
                    if (status.empty()) {
                        status = !state.store.existsOnDisk() ? std::string(literouter::i18n::tr("Not yet written to disk"))
                               : state.hasUnsavedChanges()    ? std::string(literouter::i18n::tr("Unsaved changes"))
                                                              : std::string(literouter::i18n::tr("In sync with disk"));
                    }
                    const bool statusBad = state.actionStatusError || state.hasUnsavedChanges() ||
                                           !state.store.existsOnDisk();
                    contentUi.text("settings.status")
                        .position(leftX, statusY + 20.0f)
                        .size(contentWidth - 2.0f * layout::pagePadding, 18.0f)
                        .text(status)
                        .fontSize(11.5f)
                        .lineHeight(16.0f)
                        .color(statusBad ? (state.actionStatusError ? p.danger : p.warn) : p.success)
                        .build();
                    // Listener card
                    detail::settingsCard(contentUi, "settings.listener", listenerX, listenerY, columnWidth, kListenerHeight,
                                         std::string(literouter::i18n::tr("Listener")),
                                         std::string(literouter::i18n::tr("Where this proxy accepts client connections")));

                    fieldLabel(contentUi, "settings.host.label", listenerX + 18.0f, listenerY + 76.0f, fieldWidth,
                               std::string(literouter::i18n::tr("Bind host")),
                               std::string(literouter::i18n::tr("127.0.0.1 keeps the proxy on this machine")));
                    contentUi.stack("settings.host.wrap")
                        .position(listenerX + 18.0f, listenerY + 112.0f)
                        .size(fieldWidth, 38.0f)
                        .content([&] {
                            components::input(contentUi, "settings.host")
                                .theme(uiTokens())
                                .size(fieldWidth, 38.0f)
                                .value(server.host)
                                .placeholder("127.0.0.1")
                                .onChange([](const std::string& value) {
                                    appState().store.config().server.host = value;
                                })
                                .build();
                        })
                        .build();

                    fieldLabel(contentUi, "settings.port.label", listenerX + 18.0f, listenerY + 166.0f, halfWidth,
                               std::string(literouter::i18n::tr("Port")));
                    contentUi.stack("settings.port.wrap")
                        .position(listenerX + 18.0f, listenerY + 186.0f)
                        .size(halfWidth, 34.0f)
                        .content([&] {
                            components::stepper(contentUi, "settings.port")
                                .theme(uiTokens())
                                .size(halfWidth, 34.0f)
                                .value(server.port)
                                .step(1)
                                .min(1)
                                .max(65535)
                                .onChange([](long long value) {
                                    appState().store.config().server.port = static_cast<int>(value);
                                })
                                .build();
                        })
                        .build();

                    fieldLabel(contentUi, "settings.attempts.label", listenerX + 18.0f + halfWidth + 14.0f, listenerY + 166.0f,
                               halfWidth, std::string(literouter::i18n::tr("Max attempts")));
                    contentUi.stack("settings.attempts.wrap")
                        .position(listenerX + 18.0f + halfWidth + 14.0f, listenerY + 186.0f)
                        .size(halfWidth, 34.0f)
                        .content([&] {
                            components::stepper(contentUi, "settings.attempts")
                                .theme(uiTokens())
                                .size(halfWidth, 34.0f)
                                .value(server.max_attempts)
                                .step(1)
                                .min(0)
                                .max(64)
                                .onChange([](long long value) {
                                    appState().store.config().server.max_attempts = static_cast<int>(value);
                                })
                                .build();
                        })
                        .build();

                    fieldLabel(contentUi, "settings.apikey.label", listenerX + 18.0f, listenerY + 240.0f, fieldWidth,
                               std::string(literouter::i18n::tr("Client API key")),
                               std::string(literouter::i18n::tr("Empty disables the check. Set it and clients must send "
                                                                "Authorization: Bearer <key>.")));
                    contentUi.stack("settings.apikey.wrap")
                        .position(listenerX + 18.0f, listenerY + 276.0f)
                        .size(fieldWidth, 38.0f)
                        .content([&] {
                            components::input(contentUi, "settings.apikey")
                                .theme(uiTokens())
                                .size(fieldWidth, 38.0f)
                                .value(server.api_key)
                                .placeholder(std::string(literouter::i18n::tr("leave empty to accept any key")))
                                .onChange([](const std::string& value) {
                                    appState().store.config().server.api_key = value;
                                })
                                .build();
                        })
                        .build();

                    contentUi.stack("settings.passthrough.wrap")
                        .position(listenerX + 18.0f, listenerY + 324.0f)
                        .size(fieldWidth, 32.0f)
                        .content([&] {
                            components::toggleSwitch(contentUi, "settings.passthrough")
                                .theme(uiTokens())
                                .size(fieldWidth, 32.0f)
                                .text(std::string(literouter::i18n::tr("Pass through unknown models to matching relays")))
                                .fontSize(13.0f)
                                .checked(server.pass_through_unknown)
                                .onChange([](bool value) {
                                    appState().store.config().server.pass_through_unknown = value;
                                })
                                .build();
                        })
                        .build();
                    contentUi.stack("settings.skipbreakers.wrap")
                        .position(listenerX + 18.0f, listenerY + 358.0f)
                        .size(fieldWidth, 32.0f)
                        .content([&] {
                            components::toggleSwitch(contentUi, "settings.skipbreakers")
                                .theme(uiTokens())
                                .size(fieldWidth, 32.0f)
                                .text(std::string(literouter::i18n::tr("Skip relays whose breaker is open")))
                                .fontSize(13.0f)
                                .checked(server.skip_open_circuits)
                                .onChange([](bool value) {
                                    appState().store.config().server.skip_open_circuits = value;
                                })
                                .build();
                        })
                        .build();

                    // Display card
                    detail::settingsCard(contentUi, "settings.display.card", displayX, displayY, columnWidth, kDisplayHeight,
                                         std::string(literouter::i18n::tr("Display")),
                                         std::string(literouter::i18n::tr("Display and appearance settings")));

                    fieldLabel(contentUi, "settings.language.label", displayX + 18.0f, displayY + 68.0f, fieldWidth,
                               std::string(literouter::i18n::tr("Interface language")));

                    const auto curLang = state.currentLanguage();
                    int selectedLangIdx = 0;
                    if (curLang == literouter::i18n::Lang::En) {
                        selectedLangIdx = 1;
                    } else if (curLang == literouter::i18n::Lang::Zh) {
                        selectedLangIdx = 2;
                    }

                    contentUi.stack("settings.language.wrap")
                        .position(displayX + 18.0f, displayY + 88.0f)
                        .size(fieldWidth, 34.0f)
                        .content([&] {
                            components::segmented(contentUi, "settings.language")
                                .theme(uiTokens())
                                .size(fieldWidth, 34.0f)
                                .items({std::string(literouter::i18n::tr("Auto (System)")),
                                        "English",
                                        "简体中文"})
                                .selected(selectedLangIdx)
                                .onChange([](int idx) {
                                    if (idx == 1) {
                                        appState().setLanguage(literouter::i18n::Lang::En);
                                    } else if (idx == 2) {
                                        appState().setLanguage(literouter::i18n::Lang::Zh);
                                    } else {
                                        appState().setLanguage(literouter::i18n::Lang::Auto);
                                    }
                                })
                                .build();
                        })
                        .build();

                    fieldLabel(contentUi, "settings.scale.label", displayX + 18.0f, displayY + 132.0f, fieldWidth,
                               std::string(literouter::i18n::tr("Page scale")));

                    const float curScale = state.currentScale();
                    int selectedPresetIdx = -1;
                    for (std::size_t i = 0; i < std::size(AppState::kScalePresets); ++i) {
                        if (std::abs(curScale - AppState::kScalePresets[i]) < 0.01f) {
                            selectedPresetIdx = static_cast<int>(i);
                            break;
                        }
                    }

                    contentUi.stack("settings.scale.wrap")
                        .position(displayX + 18.0f, displayY + 152.0f)
                        .size(fieldWidth, 34.0f)
                        .content([&] {
                            components::segmented(contentUi, "settings.scale")
                                .theme(uiTokens())
                                .size(fieldWidth, 34.0f)
                                .items({"80%", "90%", "100%", "110%", "125%", "150%"})
                                .selected(selectedPresetIdx)
                                .onChange([](int idx) {
                                    if (idx >= 0 && idx < static_cast<int>(std::size(AppState::kScalePresets))) {
                                        appState().setScale(AppState::kScalePresets[idx], true);
                                    }
                                })
                                .build();
                        })
                        .build();

                    const float zoomBtnGap = 8.0f;
                    const float zoomBtnWidth = (fieldWidth - 2.0f * zoomBtnGap) / 3.0f;
                    actionButton(contentUi, "settings.zoom.out", displayX + 18.0f, displayY + 196.0f, zoomBtnWidth, 32.0f,
                                 std::string(literouter::i18n::tr("Zoom out")), false,
                                 [] { appState().zoomOut(); });
                    actionButton(contentUi, "settings.zoom.reset", displayX + 18.0f + zoomBtnWidth + zoomBtnGap, displayY + 196.0f, zoomBtnWidth, 32.0f,
                                 std::string(literouter::i18n::tr("Reset zoom")), false,
                                 [] { appState().resetZoom(); });
                    actionButton(contentUi, "settings.zoom.in", displayX + 18.0f + (zoomBtnWidth + zoomBtnGap) * 2.0f, displayY + 196.0f, zoomBtnWidth, 32.0f,
                                 std::string(literouter::i18n::tr("Zoom in")), false,
                                 [] { appState().zoomIn(); });

                    contentUi.text("settings.zoom.hint")
                        .position(displayX + 18.0f, displayY + 236.0f)
                        .size(fieldWidth, 18.0f)
                        .text(std::string(literouter::i18n::tr("Keyboard shortcuts: Ctrl +/- to zoom, Ctrl 0 to reset.")))
                        .fontSize(11.5f)
                        .lineHeight(16.0f)
                        .color(p.textFaint)
                        .build();

                    // Breaker card
                    detail::settingsCard(contentUi, "settings.breaker", breakerX, breakerY, columnWidth, kBreakerHeight,
                                         std::string(literouter::i18n::tr("Circuit breaker")),
                                         std::string(literouter::i18n::tr("When a relay fails repeatedly, back off before retrying it")));
                    fieldLabel(contentUi, "settings.threshold.label", breakerX + 18.0f, breakerY + 76.0f, halfWidth,
                               std::string(literouter::i18n::tr("Consecutive failures before opening")));
                    contentUi.stack("settings.threshold.wrap")
                        .position(breakerX + 18.0f, breakerY + 96.0f)
                        .size(halfWidth, 34.0f)
                        .content([&] {
                            components::stepper(contentUi, "settings.threshold")
                                .theme(uiTokens())
                                .size(halfWidth, 34.0f)
                                .value(server.circuit_failure_threshold)
                                .step(1)
                                .min(1)
                                .max(100)
                                .onChange([](long long value) {
                                    appState().store.config().server.circuit_failure_threshold =
                                        static_cast<int>(value);
                                })
                                .build();
                        })
                        .build();
                    fieldLabel(contentUi, "settings.cooldown.label", breakerX + 18.0f + halfWidth + 14.0f, breakerY + 76.0f, halfWidth,
                               std::string(literouter::i18n::tr("Cooldown (seconds)")));
                    contentUi.stack("settings.cooldown.wrap")
                        .position(breakerX + 18.0f + halfWidth + 14.0f, breakerY + 96.0f)
                        .size(halfWidth, 34.0f)
                        .content([&] {
                            components::stepper(contentUi, "settings.cooldown")
                                .theme(uiTokens())
                                .size(halfWidth, 34.0f)
                                .value(server.circuit_cooldown_sec)
                                .step(5)
                                .min(1)
                                .max(3600)
                                .onChange([](long long value) {
                                    appState().store.config().server.circuit_cooldown_sec =
                                        static_cast<int>(value);
                                })
                                .build();
                        })
                        .build();

                    // Logging card
                    detail::settingsCard(contentUi, "settings.logging", loggingX, loggingY, columnWidth, kLoggingHeight,
                                         std::string(literouter::i18n::tr("Logging")),
                                         std::string(literouter::i18n::tr("How much request history the console keeps")));
                    fieldLabel(contentUi, "settings.capacity.label", loggingX + 18.0f, loggingY + 76.0f, halfWidth,
                               std::string(literouter::i18n::tr("Ring buffer depth (entries)")));
                    contentUi.stack("settings.capacity.wrap")
                        .position(loggingX + 18.0f, loggingY + 96.0f)
                        .size(halfWidth, 34.0f)
                        .content([&] {
                            components::stepper(contentUi, "settings.capacity")
                                .theme(uiTokens())
                                .size(halfWidth, 34.0f)
                                .value(server.log_capacity)
                                .step(50)
                                .min(50)
                                .max(100000)
                                .onChange([](long long value) {
                                    appState().store.config().server.log_capacity = static_cast<int>(value);
                                })
                                .build();
                        })
                        .build();
                    fieldLabel(contentUi, "settings.bodylimit.label", loggingX + 18.0f + halfWidth + 14.0f, loggingY + 76.0f, halfWidth,
                               std::string(literouter::i18n::tr("Retained body bytes per entry")));
                    contentUi.stack("settings.bodylimit.wrap")
                        .position(loggingX + 18.0f + halfWidth + 14.0f, loggingY + 96.0f)
                        .size(halfWidth, 34.0f)
                        .content([&] {
                            components::stepper(contentUi, "settings.bodylimit")
                                .theme(uiTokens())
                                .size(halfWidth, 34.0f)
                                .value(server.log_body_limit)
                                .step(1024)
                                .min(0)
                                .max(1048576)
                                .onChange([](long long value) {
                                    appState().store.config().server.log_body_limit = static_cast<int>(value);
                                })
                                .build();
                        })
                        .build();
                    contentUi.stack("settings.bodies.wrap")
                        .position(loggingX + 18.0f, loggingY + 144.0f)
                        .size(fieldWidth, 32.0f)
                        .content([&] {
                            components::toggleSwitch(contentUi, "settings.bodies")
                                .theme(uiTokens())
                                .size(fieldWidth, 32.0f)
                                .text(std::string(literouter::i18n::tr("Retain request / response bodies")))
                                .fontSize(13.0f)
                                .checked(server.log_bodies)
                                .onChange([](bool value) {
                                    appState().store.config().server.log_bodies = value;
                                })
                                .build();
                        })
                        .build();

                    // "How to use it"
                    detail::settingsCard(contentUi, "settings.how", howX, howY, howWidth, howHeight,
                                         std::string(literouter::i18n::tr("How to use it")),
                                         std::string(literouter::i18n::tr("Point any OpenAI-compatible client at this proxy")));

                    const std::string baseUrl = state.snapshot.running
                                                    ? state.snapshot.base_url
                                                    : "http://" + server.host + ":" + std::to_string(server.port);
                    contentUi.text("settings.how.base.label")
                        .position(howX + 18.0f, howY + 76.0f)
                        .size(110.0f, 18.0f)
                        .text(std::string(literouter::i18n::tr("LOCAL BASE URL")))
                        .fontSize(10.5f)
                        .lineHeight(16.0f)
                        .fontWeight(700)
                        .color(p.textFaint)
                        .build();
                    contentUi.text("settings.how.base")
                        .position(howX + 130.0f, howY + 74.0f)
                        .size(howWidth - 148.0f, 20.0f)
                        .text(baseUrl.empty() ? std::string(literouter::i18n::tr("(start the server to see the bound address)")) : baseUrl)
                        .fontSize(13.0f)
                        .lineHeight(18.0f)
                        .fontWeight(600)
                        .color(p.accent)
                        .build();

                    const bool isZh = literouter::i18n::resolveLang(state.currentLanguage()) == literouter::i18n::Lang::Zh;
                    const std::string keyText = server.api_key.empty()
                        ? (isZh ? "<无需密钥>" : "<no client key>")
                        : (isZh ? "<客户端密钥>" : "<key>");
                    const std::string curl =
                        "curl " + (baseUrl.empty() ? "http://127.0.0.1:" + std::to_string(server.port) : baseUrl) +
                        "/chat/completions \\\n  -H \"Authorization: Bearer " + keyText + "\" \\\n  -d '{\"model\":\"gpt-4o\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}'";
                    contentUi.rect("settings.how.curl.bg")
                        .position(howX + 18.0f, howY + 104.0f)
                        .size(howWidth - 36.0f, 62.0f)
                        .radius(p.radiusControl)
                        .color(p.surfaceSunken)
                        .border(1.0f, withAlpha(p.border, 0.7f))
                        .build();
                    contentUi.text("settings.how.curl")
                        .position(howX + 26.0f, howY + 107.0f)
                        .size(howWidth - 52.0f, 56.0f)
                        .text(curl)
                        .fontSize(11.0f)
                        .lineHeight(16.0f)
                        .wrap(true)
                        .color(p.textMuted)
                        .build();

                    const std::string tip =
                        std::string(literouter::i18n::tr("Every model in a route is advertised under /models. Unknown models pass through to any "
                                    "enabled relay that lists them."));
                    contentUi.text("settings.how.tip")
                        .position(howX + 18.0f, howY + 176.0f)
                        .size(howWidth - 36.0f, 48.0f)
                        .text(tip)
                        .fontSize(11.5f)
                        .lineHeight(17.0f)
                        .wrap(true)
                        .maxWidth(howWidth - 36.0f)
                        .color(p.textFaint)
                        .build();
                })
                .build();
        })
        .build();
}

} // namespace lr_gui
