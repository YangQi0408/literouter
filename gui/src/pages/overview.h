#pragma once

#include "../app_state.h"
#include "../components/lr_theme.h"
#include "../components/widgets.h"

// Page 1 — Overview: metric tiles, a breaker banner, one card per relay, and a
// preview of the newest log entries.
namespace lr_gui {

namespace detail {

inline std::string percentText(double rate) {
    const int whole = static_cast<int>(rate * 1000.0 + 0.5);
    return std::to_string(whole / 10) + "." + std::to_string(whole % 10) + "%";
}

} // namespace detail

inline float composeMetricRow(eui::Ui& ui, float x, float y, float width) {
    const AppState& state = appState();
    const literouter::Snapshot& snap = state.snapshot;

    const double total = static_cast<double>(snap.total_requests);
    const double successRate = total > 0.0 ? static_cast<double>(snap.total_success) / total : 1.0;
    const std::uint64_t tokens = snap.tokens_prompt + snap.tokens_completion;

    const bool twoRows = (width < 880.0f);
    const int columns = twoRows ? 3 : 6;
    const float gap = 14.0f;
    const float tileWidth = (width - 2.0f * layout::pagePadding - gap * (columns - 1)) / columns;
    const float tileHeight = 96.0f;
    const float startX = x + layout::pagePadding;

    const std::string labels[6] = {
        std::string(literouter::i18n::tr("TOTAL REQUESTS")),
        std::string(literouter::i18n::tr("SUCCESS RATE")),
        std::string(literouter::i18n::tr("IN FLIGHT")),
        std::string(literouter::i18n::tr("AVG LATENCY")),
        std::string(literouter::i18n::tr("TOKENS")),
        std::string(literouter::i18n::tr("BYTES IN / OUT"))
    };
    const std::string values[6] = {
        literouter::humanCount(snap.total_requests),
        detail::percentText(successRate),
        std::to_string(snap.active_requests),
        literouter::humanMillis(snap.latency_ms_avg),
        literouter::humanCount(tokens),
        literouter::humanBytes(snap.bytes_out),
    };
    const eui::Color colors[6] = {
        palette().text,
        successBand(successRate),
        snap.active_requests > 0 ? palette().accent : palette().text,
        palette().text,
        palette().text,
        palette().text,
    };
    const eui::Color underlines[6] = {
        palette().accent, successBand(successRate), palette().accent,
        palette().info,   palette().info,           palette().info,
    };

    for (int i = 0; i < 6; ++i) {
        const int col = twoRows ? (i % 3) : i;
        const int row = twoRows ? (i / 3) : 0;
        const float tileX = startX + static_cast<float>(col) * (tileWidth + gap);
        const float tileY = y + static_cast<float>(row) * (tileHeight + gap);
        metricTile(ui, "overview.metric." + std::to_string(i), tileX, tileY, tileWidth, tileHeight,
                   labels[i], values[i], colors[i], underlines[i]);
    }
    return twoRows ? (tileHeight * 2.0f + gap) : tileHeight;
}

inline void composeBreakerBanner(eui::Ui& ui, float x, float y, float width) {
    const AppState& state = appState();
    if (state.snapshot.breakers_open <= 0) {
        return;
    }

    std::string names;
    for (const auto& health : state.snapshot.health) {
        if (health.state != literouter::ProviderHealth::State::Open) {
            continue;
        }
        if (!names.empty()) {
            names += ", ";
        }
        names += health.provider;
        if (health.cooldown_remaining > 0.5) {
            names += " (" + literouter::humanDuration(health.cooldown_remaining) +
                     std::string(literouter::i18n::tr(" left")) + ")";
        }
    }

    const std::string count = std::to_string(state.snapshot.breakers_open);
    const float height = 54.0f;
    ui.stack("overview.breaker")
        .position(x + layout::pagePadding, y)
        .size(width - 2.0f * layout::pagePadding, height)
        .content([&] {
            ui.rect("overview.breaker.bg")
                .size(width - 2.0f * layout::pagePadding, height)
                .radius(palette().radiusCard)
                .color(withAlpha(palette().danger, 0.13f))
                .border(1.0f, withAlpha(palette().danger, 0.36f))
                .build();
            dot(ui, "overview.breaker.dot", 18.0f, height * 0.5f - 5.0f, 10.0f, palette().danger);
            ui.text("overview.breaker.title")
                .position(38.0f, 12.0f)
                .size(width - 80.0f, 18.0f)
                .text(count + std::string(literouter::i18n::tr(
                          state.snapshot.breakers_open == 1 ? " relay breaker is open"
                                                            : " relay breakers are open")))
                .fontSize(13.5f)
                .lineHeight(18.0f)
                .fontWeight(700)
                .color(palette().danger)
                .build();
            ui.text("overview.breaker.body")
                .position(38.0f, 30.0f)
                .size(width - 80.0f, 18.0f)
                .text(names.empty() ? std::string(literouter::i18n::tr("No relay is currently eligible."))
                                    : names)
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(palette().textMuted)
                .build();
        })
        .build();
}

inline void composeRelayCard(eui::Ui& ui, float x, float y, float width, std::size_t index,
                             const literouter::ProviderConfig& provider) {
    const AppState& state = appState();
    const Palette& p = palette();
    const literouter::ProviderStat* stat = statFor(state.snapshot, provider.id);
    const literouter::ProviderHealth* health = healthFor(state.snapshot, provider.id);
    const std::string stateName = health != nullptr ? health->stateName() : "Unknown";
    const eui::Color stateColor = healthColor(stateName);
    const std::uint64_t requests = stat != nullptr ? stat->requests : 0;
    const std::uint64_t successes = stat != nullptr ? stat->successes : 0;
    const std::uint64_t failures = stat != nullptr ? stat->failures : 0;
    const double latency = stat != nullptr ? stat->latency_ms_avg : 0.0;
    const std::uint64_t bytesOut = stat != nullptr ? stat->bytes_out : 0;
    // Both directions, because "how much did this relay cost me in traffic" is
    // only half a picture: the request is usually the expensive half.
    const std::uint64_t bytesIn = stat != nullptr ? stat->bytes_in : 0;

    const float height = 150.0f;
    const std::string displayName = providerDisplayName(provider);

    ui.stack("overview.relay." + std::to_string(index))
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("overview.relay." + std::to_string(index) + ".bg")
                .size(width, height)
                .radius(p.radiusCard)
                .color(p.surface)
                .border(1.0f, withAlpha(p.border, 0.8f))
                .build();

            const std::string base = "overview.relay." + std::to_string(index);
            const float pillWidth = measureText(stateName, 12.0f, 600) + 34.0f;
            statusPill(ui, base + ".state", width - 18.0f - pillWidth, 12.0f, stateColor,
                       std::string(literouter::i18n::tr(stateName)));

            const float maxNameWidth = std::max(60.0f, width - 18.0f - pillWidth - 180.0f);
            const float nameWidth = std::min(maxNameWidth, measureText(displayName, 16.0f, 720));
            ui.text(base + ".name")
                .position(18.0f, 14.0f)
                .size(nameWidth, 22.0f)
                .text(displayName)
                .fontSize(16.0f)
                .lineHeight(21.0f)
                .fontWeight(720)
                .color(provider.enabled ? p.text : p.textFaint)
                .build();
            chip(ui, base + ".id", 18.0f + nameWidth + 10.0f, 14.0f, provider.id, p.textMuted,
                 withAlpha(p.text, 0.06f));
            if (!provider.enabled) {
                chip(ui, base + ".off", 18.0f + nameWidth + 20.0f + measureText(provider.id, 12.0f) + 20.0f,
                     14.0f, literouter::i18n::tr("disabled"), p.textFaint, withAlpha(p.text, 0.04f));
            }

            ui.text(base + ".url")
                .position(18.0f, 42.0f)
                .size(width - 36.0f, 18.0f)
                .text(provider.base_url.empty() ? std::string(literouter::i18n::tr("(no base url)"))
                                                : provider.base_url)
                .fontSize(12.5f)
                .lineHeight(16.0f)
                .color(p.textMuted)
                .build();

            divider(ui, base + ".divider", 18.0f, 70.0f, width - 36.0f);

            const float columnGap = 14.0f;
            const float columnWidth = (width - 36.0f - columnGap * 5.0f) / 6.0f;
            fieldValue(ui, base + ".priority", 18.0f, 82.0f, columnWidth, literouter::i18n::tr("PRIORITY"),
                       std::to_string(provider.priority), p.text);
            fieldValue(ui, base + ".requests", 18.0f + (columnWidth + columnGap), 82.0f, columnWidth,
                       literouter::i18n::tr("REQUESTS"), literouter::humanCount(requests), p.text);
            fieldValue(ui, base + ".success", 18.0f + (columnWidth + columnGap) * 2.0f, 82.0f,
                       columnWidth, literouter::i18n::tr("SUCCESS"), literouter::humanCount(successes),
                       successes > 0 ? p.success : p.textFaint);
            fieldValue(ui, base + ".failure", 18.0f + (columnWidth + columnGap) * 3.0f, 82.0f,
                       columnWidth, literouter::i18n::tr("FAILURES"), literouter::humanCount(failures),
                       failures > 0 ? p.danger : p.textFaint);
            fieldValue(ui, base + ".latency", 18.0f + (columnWidth + columnGap) * 4.0f, 82.0f,
                       columnWidth, literouter::i18n::tr("AVG LATENCY"), literouter::humanMillis(latency), p.text);
            fieldValue(ui, base + ".bytes", 18.0f + (columnWidth + columnGap) * 5.0f, 82.0f,
                       columnWidth, literouter::i18n::tr("BYTES IN / OUT"),
                       literouter::humanBytes(bytesIn) + " / " + literouter::humanBytes(bytesOut),
                       p.text);

            const std::string error = health != nullptr ? health->last_error : std::string{};
            ui.text(base + ".error")
                .position(18.0f, 122.0f)
                .size(width - 36.0f, 18.0f)
                .text(error.empty() ? std::string(literouter::i18n::tr("No recent upstream errors"))
                                    : std::string(literouter::i18n::tr("last error · ")) +
                                          literouter::truncateUtf8(error, 140))
                .fontSize(11.5f)
                .lineHeight(15.0f)
                .color(error.empty() ? p.textFaint
                                     : (stateName == "Open" ? p.danger : p.warn))
                .build();
        })
        .build();
}

inline void composeRecentActivity(eui::Ui& ui, float x, float y, float width, float height) {
    const AppState& state = appState();
    const Palette& p = palette();

    ui.stack("overview.activity")
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect("overview.activity.bg")
                .size(width, height)
                .radius(p.radiusCard)
                .color(p.surface)
                .border(1.0f, withAlpha(p.border, 0.8f))
                .build();
            ui.text("overview.activity.title")
                .position(18.0f, 14.0f)
                .size(width - 200.0f, 22.0f)
                .text(literouter::i18n::tr("Recent activity"))
                .fontSize(15.0f)
                .lineHeight(20.0f)
                .fontWeight(720)
                .color(p.text)
                .build();
            ui.text("overview.activity.hint")
                .position(width - 190.0f, 16.0f)
                .size(172.0f, 18.0f)
                .text(std::string(literouter::i18n::tr("newest first · full log on Logs")))
                .fontSize(11.5f)
                .lineHeight(15.0f)
                .color(p.textFaint)
                .horizontalAlign(eui::HorizontalAlign::Right)
                .build();
            divider(ui, "overview.activity.divider", 18.0f, 42.0f, width - 36.0f);

            if (state.logs.empty()) {
                ui.text("overview.activity.empty")
                    .position(18.0f, 60.0f)
                    .size(width - 36.0f, 24.0f)
                    .text(std::string(literouter::i18n::tr(
                        "No requests yet — start the server and point a client at it.")))
                    .fontSize(13.0f)
                    .lineHeight(20.0f)
                    .color(p.textMuted)
                    .build();
                return;
            }

            const int rows = std::min<int>(8, static_cast<int>(state.logs.size()));
            for (int row = 0; row < rows; ++row) {
                const literouter::LogEntry& entry =
                    state.logs[state.logs.size() - 1 - static_cast<std::size_t>(row)];
                const float rowY = 54.0f + static_cast<float>(row) * 28.0f;
                const std::string base = "overview.activity.row." + std::to_string(row);
                const eui::Color level = levelColor(entry.level);
                const bool statusBad = entry.status != 0 && (entry.status < 200 || entry.status >= 300);

                if (row % 2 == 0) {
                    ui.rect(base + ".zebra")
                        .position(10.0f, rowY)
                        .size(width - 20.0f, 26.0f)
                        .radius(6.0f)
                        .color(withAlpha(p.text, 0.025f))
                        .build();
                }
                dot(ui, base + ".dot", 18.0f, rowY + 9.0f, 8.0f, level);
                ui.text(base + ".time")
                    .position(34.0f, rowY)
                    .size(88.0f, 26.0f)
                    .text(entry.timeText())
                    .fontSize(11.5f)
                    .lineHeight(26.0f)
                    .color(p.textFaint)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                ui.text(base + ".model")
                    .position(126.0f, rowY)
                    .size(150.0f, 26.0f)
                    .text(entry.model.empty() ? "—" : literouter::truncateUtf8(entry.model, 24))
                    .fontSize(12.0f)
                    .lineHeight(26.0f)
                    .fontWeight(550)
                    .color(p.text)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                ui.text(base + ".relay")
                    .position(280.0f, rowY)
                    .size(130.0f, 26.0f)
                    .text(entry.provider.empty() ? "—" : entry.provider)
                    .fontSize(12.0f)
                    .lineHeight(26.0f)
                    .color(p.textMuted)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                ui.text(base + ".status")
                    .position(414.0f, rowY)
                    .size(52.0f, 26.0f)
                    .text(entry.status == 0 ? "—" : std::to_string(entry.status))
                    .fontSize(12.0f)
                    .lineHeight(26.0f)
                    .fontWeight(600)
                    .color(statusBad ? p.danger : p.textMuted)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
                ui.text(base + ".message")
                    .position(474.0f, rowY)
                    .size(std::max(80.0f, width - 494.0f), 26.0f)
                    .text(entry.message.empty() ? "—" : literouter::truncateUtf8(entry.message, 96))
                    .fontSize(12.0f)
                    .lineHeight(26.0f)
                    .color(level)
                    .verticalAlign(eui::VerticalAlign::Center)
                    .build();
            }
        })
        .build();
}

inline void composeOverview(eui::Ui& ui, float x, float y, float width, float height) {
    AppState& state = appState();
    const literouter::AppConfig& config = state.store.config();
    const std::size_t providerCount = config.providers.size();
    const int previewRows = std::min<int>(8, static_cast<int>(state.logs.size()));
    const float activityHeight = previewRows > 0
        ? (54.0f + static_cast<float>(previewRows) * 28.0f + 16.0f)
        : 110.0f;

    const float metricRowH = width < 880.0f ? (96.0f * 2.0f + 14.0f) : 96.0f;
    float cursorY = 22.0f;
    cursorY += metricRowH + 22.0f; // metric row
    if (state.snapshot.breakers_open > 0) {
        cursorY += 54.0f + 20.0f;
    }
    cursorY += 50.0f; // relays heading
    cursorY += providerCount > 0 ? static_cast<float>(providerCount) * 164.0f : 234.0f;
    cursorY += 30.0f; // gap + activity heading
    cursorY += activityHeight;
    cursorY += 40.0f; // bottom padding
    const float canvasHeight = std::max(height, cursorY);

    components::scrollView(ui, "overview.scroll")
        .theme(uiTokens())
        .position(x, y)
        .size(width, height)
        .offset(state.overviewScroll)
        .step(layout::scrollStep)
        .scrollbarWidth(9.0f)
        .scrollbarGap(6.0f)
        .onChange([&state](float value) { state.overviewScroll = value; })
        .content([&](eui::Ui& contentUi, float contentWidth, float) {
            contentUi.stack("overview.canvas")
                .size(contentWidth, canvasHeight)
                .content([&] {
                    const float cardWidth = contentWidth - 2.0f * layout::pagePadding;
                    float rowY = 22.0f;
                    const float metricH = composeMetricRow(contentUi, 0.0f, rowY, contentWidth);
                    rowY += metricH + 22.0f;

                    if (state.snapshot.breakers_open > 0) {
                        composeBreakerBanner(contentUi, 0.0f, rowY, contentWidth);
                        rowY += 54.0f + 20.0f;
                    }

                    sectionTitle(contentUi, "overview.relays.title", layout::pagePadding, rowY,
                                 cardWidth, literouter::i18n::tr("Relays"),
                                 providerCount == 0
                                     ? std::string(literouter::i18n::tr("No providers configured"))
                                     : std::to_string(providerCount) +
                                           std::string(literouter::i18n::tr(" configured · ordered by priority")));
                    rowY += 50.0f;

                    if (providerCount == 0) {
                        emptyState(contentUi, "overview.empty", layout::pagePadding, rowY, cardWidth,
                                   234.0f, std::string(literouter::i18n::tr("No relays configured")),
                                   std::string(literouter::i18n::tr(
                                       "A relay is one upstream zhong-zhuan endpoint. Add one on the "
                                       "Providers page and its models become routable.")),
                                   std::string(literouter::i18n::tr("Open Providers")),
                                   [] { appState().page = Page::Providers; });
                        rowY += 234.0f;
                    } else {
                        for (std::size_t i = 0; i < providerCount; ++i) {
                            composeRelayCard(contentUi, layout::pagePadding, rowY, cardWidth, i,
                                             config.providers[i]);
                            rowY += 164.0f;
                        }
                    }

                    rowY += 30.0f;
                    composeRecentActivity(contentUi, layout::pagePadding, rowY, cardWidth,
                                          activityHeight);
                })
                .build();
        })
        .build();
}

} // namespace lr_gui
