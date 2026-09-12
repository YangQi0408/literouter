#pragma once

#include "../app_state.h"
#include "../components/theme.h"
#include "../components/widgets.h"

// Page 4 — Logs: a live table fed by ProxyServer::logsSince, with level / kind /
// text filters, pause-and-resume that keeps the sequence cursor, and a detail
// dialog for the full message.
namespace lr_gui {

namespace detail {

inline bool matchesSearch(const literouter::LogEntry& entry, const std::string& needleLower) {
    if (needleLower.empty()) {
        return true;
    }
    return literouter::toLower(entry.model).find(needleLower) != std::string::npos ||
           literouter::toLower(entry.provider).find(needleLower) != std::string::npos ||
           literouter::toLower(entry.message).find(needleLower) != std::string::npos;
}

inline bool matchesLevel(const literouter::LogEntry& entry, int filter) {
    switch (filter) {
        case 1: return entry.level == "info";
        case 2: return entry.level == "warn";
        case 3: return entry.level == "error";
        default: return true;
    }
}

inline bool matchesKind(const literouter::LogEntry& entry, int filter) {
    static const char* kinds[] = {"chat", "embeddings", "models", "admin", "system"};
    if (filter <= 0 || filter > 5) {
        return true;
    }
    return entry.kind == kinds[filter - 1];
}

} // namespace detail

inline void composeLogs(eui::Ui& ui, float x, float y, float width, float height) {
    AppState& state = appState();
    const Palette& p = palette();
    LogsView& view = state.logsView;

    // ── filter bar ──────────────────────────────────────────────────────
    const float barY = y + 16.0f;
    const float controlHeight = 36.0f;

    const float rightEdge = x + width - layout::pagePadding;
    const bool twoRows = (width < 1000.0f);
    const float row2Y = barY + controlHeight + 8.0f;

    if (twoRows) {
        // Row 1: Filters & Search across the whole row
        const float levelWidth = 220.0f;
        const float kindWidth = 140.0f;
        ui.stack("logs.level.wrap")
            .position(x + layout::pagePadding, barY)
            .size(levelWidth, controlHeight)
            .content([&] {
                components::segmented(ui, "logs.level")
                    .theme(uiTokens())
                    .size(levelWidth, controlHeight)
                    .items({std::string(literouter::i18n::tr("All")),
                            std::string(literouter::i18n::tr("Info")),
                            std::string(literouter::i18n::tr("Warn")),
                            std::string(literouter::i18n::tr("Error"))})
                    .selected(view.levelFilter)
                    .onChange([&view](int value) { view.levelFilter = value; })
                    .build();
            })
            .build();

        if (view.kindOpen) {
            ui.rect("logs.kind.backdrop.tworows")
                .position(0.0f, 0.0f)
                .size(width, height)
                .color(transparent())
                .zIndex(150)
                .onClick([&view] { view.kindOpen = false; })
                .build();
        }
        ui.stack("logs.kind.wrap")
            .position(x + layout::pagePadding + levelWidth + 8.0f, barY)
            .size(kindWidth, controlHeight)
            .zIndex(view.kindOpen ? 200 : 1)
            .content([&] {
                components::dropdown(ui, "logs.kind")
                    .theme(uiTokens())
                    .size(kindWidth, controlHeight)
                    .zIndex(view.kindOpen ? 200 : 1)
                    .items({std::string(literouter::i18n::tr("All kinds")),
                            std::string(literouter::i18n::tr("Chat")),
                            std::string(literouter::i18n::tr("Embeddings")),
                            std::string(literouter::i18n::tr("Models")),
                            std::string(literouter::i18n::tr("Admin")),
                            std::string(literouter::i18n::tr("System"))})
                    .selected(view.kindFilter)
                    .open(view.kindOpen)
                    .onChange([&view](int value) {
                        view.kindFilter = value;
                        view.kindOpen = false;
                    })
                    .onOpenChange([&view](bool open) { view.kindOpen = open; })
                    .build();
            })
            .build();

        const float searchX = x + layout::pagePadding + levelWidth + 8.0f + kindWidth + 8.0f;
        const float searchWidth = std::max(100.0f, rightEdge - searchX);
        ui.stack("logs.search.wrap")
            .position(searchX, barY)
            .size(searchWidth, controlHeight)
            .content([&] {
                components::input(ui, "logs.search")
                    .theme(uiTokens())
                    .size(searchWidth, controlHeight)
                    .value(view.search)
                    .placeholder(std::string(literouter::i18n::tr("Filter model, relay or message")))
                    .onChange([&view](const std::string& value) { view.search = value; })
                    .build();
            })
            .build();

        // Row 2: Action buttons & Capacity
        const float clearWidth = 84.0f;
        const float pauseWidth = 100.0f;
        const float capacityWidth = 160.0f;
        actionButton(ui, "logs.clear", x + layout::pagePadding, row2Y, clearWidth, controlHeight,
                     literouter::i18n::tr("Clear log"), false,
                     [] { appState().clearLogs(); });
        actionButton(ui, "logs.pause", x + layout::pagePadding + clearWidth + 8.0f, row2Y, pauseWidth, controlHeight,
                     view.paused ? literouter::i18n::tr("Resume") : literouter::i18n::tr("Pause"), false,
                     [&view] { view.paused = !view.paused; });
        ui.stack("logs.capacity.wrap")
            .position(x + layout::pagePadding + clearWidth + 8.0f + pauseWidth + 8.0f, row2Y)
            .size(capacityWidth, controlHeight)
            .content([&] {
                components::segmented(ui, "logs.capacity")
                    .theme(uiTokens())
                    .size(capacityWidth, controlHeight)
                    .items({"100", "250", "500"})
                    .selected(view.capacityChoice)
                    .onChange([&view](int value) { view.capacityChoice = value; })
                    .build();
            })
            .build();
    } else {
        ui.stack("logs.level.wrap")
            .position(x + layout::pagePadding, barY)
            .size(224.0f, controlHeight)
            .content([&] {
                components::segmented(ui, "logs.level")
                    .theme(uiTokens())
                    .size(224.0f, controlHeight)
                    .items({std::string(literouter::i18n::tr("All")),
                            std::string(literouter::i18n::tr("Info")),
                            std::string(literouter::i18n::tr("Warn")),
                            std::string(literouter::i18n::tr("Error"))})
                    .selected(view.levelFilter)
                    .onChange([&view](int value) { view.levelFilter = value; })
                    .build();
            })
            .build();

        if (view.kindOpen) {
            ui.rect("logs.kind.backdrop")
                .position(0.0f, 0.0f)
                .size(width, height)
                .color(transparent())
                .zIndex(150)
                .onClick([&view] { view.kindOpen = false; })
                .build();
        }
        ui.stack("logs.kind.wrap")
            .position(x + layout::pagePadding + 236.0f, barY)
            .size(176.0f, controlHeight)
            .zIndex(view.kindOpen ? 200 : 1)
            .content([&] {
                components::dropdown(ui, "logs.kind")
                    .theme(uiTokens())
                    .size(176.0f, controlHeight)
                    .zIndex(view.kindOpen ? 200 : 1)
                    .items({std::string(literouter::i18n::tr("All kinds")),
                            std::string(literouter::i18n::tr("Chat")),
                            std::string(literouter::i18n::tr("Embeddings")),
                            std::string(literouter::i18n::tr("Models")),
                            std::string(literouter::i18n::tr("Admin")),
                            std::string(literouter::i18n::tr("System"))})
                    .selected(view.kindFilter)
                    .open(view.kindOpen)
                    .onChange([&view](int value) {
                        view.kindFilter = value;
                        view.kindOpen = false;
                    })
                    .onOpenChange([&view](bool open) { view.kindOpen = open; })
                    .build();
            })
            .build();

        const float capacityWidth = 188.0f;
        const float clearWidth = 84.0f;
        const float pauseWidth = 104.0f;
        const float capacityX = rightEdge - capacityWidth;
        const float pauseX = capacityX - 12.0f - pauseWidth;
        const float clearX = pauseX - 12.0f - clearWidth;
        const float searchX = x + layout::pagePadding + 424.0f;
        const float searchWidth = std::max(120.0f, clearX - 12.0f - searchX);

        ui.stack("logs.search.wrap")
            .position(searchX, barY)
            .size(searchWidth, controlHeight)
            .content([&] {
                components::input(ui, "logs.search")
                    .theme(uiTokens())
                    .size(searchWidth, controlHeight)
                    .value(view.search)
                    .placeholder(std::string(literouter::i18n::tr("Filter model, relay or message")))
                    .onChange([&view](const std::string& value) { view.search = value; })
                    .build();
            })
            .build();

        ui.stack("logs.capacity.wrap")
            .position(capacityX, barY)
            .size(capacityWidth, controlHeight)
            .content([&] {
                components::segmented(ui, "logs.capacity")
                    .theme(uiTokens())
                    .size(capacityWidth, controlHeight)
                    .items({"100", "250", "500"})
                    .selected(view.capacityChoice)
                    .onChange([&view](int value) { view.capacityChoice = value; })
                    .build();
            })
            .build();

        actionButton(ui, "logs.pause", pauseX, barY, pauseWidth, controlHeight,
                     view.paused ? literouter::i18n::tr("Resume") : literouter::i18n::tr("Pause"), false,
                     [&view] { view.paused = !view.paused; });
        actionButton(ui, "logs.clear", clearX, barY, clearWidth, controlHeight,
                     literouter::i18n::tr("Clear log"), false,
                     [] { appState().clearLogs(); });
    }

    // ── table header ────────────────────────────────────────────────────
    const float headerY = twoRows ? (row2Y + controlHeight + 14.0f) : (barY + controlHeight + 14.0f);
    ui.rect("logs.header.bg")
        .position(x + layout::pagePadding, headerY)
        .size(width - 2.0f * layout::pagePadding, 26.0f)
        .radius(6.0f)
        .color(withAlpha(p.text, 0.035f))
        .build();

    const bool compact = (width - 2.0f * layout::pagePadding) < 840.0f;
    const float colGap = compact ? 6.0f : 12.0f;

    const float offTime = 12.0f;
    const float wTime = compact ? 96.0f : 136.0f;

    const float offLevel = offTime + wTime + colGap;
    const float wLevel = compact ? 50.0f : 62.0f;

    const float offKind = offLevel + wLevel + colGap;
    const float wKind = compact ? 68.0f : 84.0f;

    const float offModel = offKind + wKind + colGap;
    const float wModel = compact ? 104.0f : 150.0f;

    const float offRelay = offModel + wModel + colGap;
    const float wRelay = compact ? 88.0f : 116.0f;

    const float offStatus = offRelay + wRelay + colGap;
    const float wStatus = compact ? 46.0f : 56.0f;

    const float offLatency = offStatus + wStatus + colGap;
    const float wLatency = compact ? 56.0f : 76.0f;

    const float offBytes = offLatency + wLatency + colGap;
    const float wBytes = compact ? 56.0f : 76.0f;

    const float offMessage = offBytes + wBytes + colGap;

    struct Column {
        const char* label;
        float offset;
        float width;
    };
    const Column columns[] = {
        {"TIME", offTime, wTime},       {"LEVEL", offLevel, wLevel},   {"KIND", offKind, wKind},
        {"MODEL", offModel, wModel},    {"RELAY", offRelay, wRelay},   {"STATUS", offStatus, wStatus},
        {"LATENCY", offLatency, wLatency}, {"BYTES", offBytes, wBytes}, {"MESSAGE", offMessage, 0.0f},
    };
    for (const Column& column : columns) {
        const float columnWidth =
            column.width > 0.0f ? column.width : std::max(60.0f, width - 52.0f - column.offset);
        ui.text(std::string("logs.header.") + column.label)
            .position(x + layout::pagePadding + column.offset, headerY)
            .size(columnWidth, 26.0f)
            .text(literouter::i18n::tr(column.label))
            .fontSize(10.5f)
            .lineHeight(26.0f)
            .fontWeight(700)
            .color(p.textFaint)
            .verticalAlign(eui::VerticalAlign::Center)
            .build();
    }

    // ── filtered rows ───────────────────────────────────────────────────
    if (view.cachedLevelFilter != view.levelFilter ||
        view.cachedKindFilter != view.kindFilter ||
        view.cachedSearch != view.search ||
        view.cachedLogSeq != state.lastLogSeq ||
        view.cachedLogCount != state.logs.size()) {
        
        view.cachedLevelFilter = view.levelFilter;
        view.cachedKindFilter = view.kindFilter;
        view.cachedSearch = view.search;
        view.cachedLogSeq = state.lastLogSeq;
        view.cachedLogCount = state.logs.size();
        
        view.filteredIndices.clear();
        view.filteredIndices.reserve(state.logs.size());
        const std::string needleLower = literouter::toLower(view.search);
        
        for (std::size_t i = 0; i < state.logs.size(); ++i) {
            const auto& entry = state.logs[i];
            if (detail::matchesLevel(entry, view.levelFilter) &&
                detail::matchesKind(entry, view.kindFilter) &&
                detail::matchesSearch(entry, needleLower)) {
                view.filteredIndices.push_back(i);
            }
        }
    }
    
    const auto& filteredIndices = view.filteredIndices;

    const float listY = headerY + 30.0f;
    const float listHeight = std::max(40.0f, height - (listY - y) - 34.0f);
    const float rowHeight = 26.0f;
    const float offset = view.follow && !filteredIndices.empty() ? 1000000.0f : view.scroll;
    const bool isZh = literouter::i18n::resolveLang(state.currentLanguage()) == literouter::i18n::Lang::Zh;
    const std::string footerText = isZh
        ? "共 " + std::to_string(state.logs.size()) + " 条，当前匹配 " +
              std::to_string(filteredIndices.size()) + " 条" +
              (view.paused ? " · 已暂停（点击继续后恢复接收）"
                           : (view.follow ? " · 自动跟踪" : " · 历史查看"))
        : std::to_string(filteredIndices.size()) + " of " + std::to_string(state.logs.size()) +
              " entries" + (view.paused ? " · paused (buffer resumes on Resume)"
                                        : (view.follow ? " · following" : " · scrolled"));

    ui.stack("logs.footer")
        .position(x + layout::pagePadding, listY + listHeight + 4.0f)
        .size(width - 2.0f * layout::pagePadding, 18.0f)
        .content([&] {
            ui.text("logs.footer.text")
                .size(width - 2.0f * layout::pagePadding, 18.0f)
                .text(footerText)
                .fontSize(11.5f)
                .lineHeight(16.0f)
                .color(view.paused ? p.warn : p.textFaint)
                .build();
        })
        .build();

    if (filteredIndices.empty()) {
        const std::string emptyTitle = state.logs.empty()
            ? std::string(literouter::i18n::tr("No log entries"))
            : std::string(literouter::i18n::tr("No matching log entries"));
        const std::string emptySub = state.logs.empty()
            ? std::string(literouter::i18n::tr("Requests appear here as the proxy handles them."))
            : std::string(literouter::i18n::tr("Try adjusting your level, kind or search filter."));
        emptyState(ui, "logs.empty", x + layout::pagePadding, listY + 20.0f,
                   width - 2.0f * layout::pagePadding, 220.0f, emptyTitle, emptySub,
                   state.logs.empty() ? "" : std::string(literouter::i18n::tr("Reset filters")),
                   state.logs.empty() ? std::function<void()>{} : [&view] {
                       view.levelFilter = 0;
                       view.kindFilter = 0;
                       view.search.clear();
                   });
    }

    components::virtualList(ui, "logs.rows")
        .theme(uiTokens())
        .position(x + layout::pagePadding, listY)
        .size(width - 2.0f * layout::pagePadding, listHeight)
        .itemCount(static_cast<std::int64_t>(filteredIndices.size()))
        .rowHeight(rowHeight)
        .offset(offset)
        .step(layout::scrollStep)
        .overscanViewports(1.5f)
        .scrollbarWidth(9.0f)
        .scrollbarGap(8.0f)
        .onChange([&view](float value) {
            view.scroll = value;
            view.follow = false;
        })
        .row([&](eui::Ui& rowUi, const std::string& rowId, std::int64_t index, float rowWidth,
                 float) {
            if (index < 0 || index >= static_cast<std::int64_t>(filteredIndices.size())) {
                return;
            }
            const literouter::LogEntry& entry = state.logs[filteredIndices[static_cast<std::size_t>(index)]];
            const Palette& rowPalette = palette();
            const eui::Color level = levelColor(entry.level);
            const bool statusBad = entry.status != 0 && (entry.status < 200 || entry.status >= 300);

            rowUi.rect(rowId + ".hit")
                .size(rowWidth, rowHeight)
                .color(static_cast<int>(index) % 2 == 0 ? withAlpha(rowPalette.text, 0.022f)
                                                        : transparent())
                .states(static_cast<int>(index) % 2 == 0 ? withAlpha(rowPalette.text, 0.022f)
                                                         : transparent(),
                        withAlpha(rowPalette.accent, 0.12f), withAlpha(rowPalette.accent, 0.18f))
                .onClick([entry] {
                    appState().logsView.detail = entry;
                    appState().logsView.detailOpen = true;
                })
                .build();

            dot(rowUi, rowId + ".dot", offTime, rowHeight * 0.5f - 3.5f, 7.0f, level);
            rowUi.text(rowId + ".time")
                .position(offTime + 10.0f, 0.0f)
                .size(wTime - 10.0f, rowHeight)
                .text(compact ? entry.shortDateTimeText()
                              : (entry.dateText() + " " + entry.timeText().substr(0, 8)))
                .fontSize(11.0f)
                .lineHeight(rowHeight)
                .color(rowPalette.textFaint)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".level")
                .position(offLevel, 0.0f)
                .size(wLevel, rowHeight)
                .text(entry.level)
                .fontSize(11.0f)
                .lineHeight(rowHeight)
                .fontWeight(600)
                .color(level)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".kind")
                .position(offKind, 0.0f)
                .size(wKind, rowHeight)
                .text(entry.kind.empty() ? "—" : entry.kind)
                .fontSize(11.5f)
                .lineHeight(rowHeight)
                .color(rowPalette.textMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".model")
                .position(offModel, 0.0f)
                .size(wModel, rowHeight)
                .text(entry.model.empty() ? "—" : literouter::truncateUtf8(entry.model, compact ? 16 : 24))
                .fontSize(11.5f)
                .lineHeight(rowHeight)
                .fontWeight(520)
                .color(rowPalette.text)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".relay")
                .position(offRelay, 0.0f)
                .size(wRelay, rowHeight)
                .text(entry.provider.empty() ? "—" : literouter::truncateUtf8(entry.provider, compact ? 12 : 18))
                .fontSize(11.5f)
                .lineHeight(rowHeight)
                .color(entry.failover ? rowPalette.warn : rowPalette.textMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".status")
                .position(offStatus, 0.0f)
                .size(wStatus, rowHeight)
                .text(entry.status == 0 ? "—" : std::to_string(entry.status))
                .fontSize(11.5f)
                .lineHeight(rowHeight)
                .fontWeight(600)
                .color(statusBad ? rowPalette.danger : rowPalette.textMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".latency")
                .position(offLatency, 0.0f)
                .size(wLatency, rowHeight)
                .text(literouter::humanMillis(entry.latency_ms))
                .fontSize(11.5f)
                .lineHeight(rowHeight)
                .color(rowPalette.textMuted)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".bytes")
                .position(offBytes, 0.0f)
                .size(wBytes, rowHeight)
                .text(literouter::humanBytes(entry.bytes))
                .fontSize(11.5f)
                .lineHeight(rowHeight)
                .color(rowPalette.textFaint)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
            rowUi.text(rowId + ".message")
                .position(offMessage, 0.0f)
                .size(std::max(60.0f, rowWidth - offMessage - 12.0f), rowHeight)
                .text(entry.message.empty() ? "—" : literouter::truncateUtf8(entry.message, 90))
                .fontSize(11.5f)
                .lineHeight(rowHeight)
                .color(level)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

} // namespace lr_gui
