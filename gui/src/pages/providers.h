#pragma once

#include "../app_state.h"
#include "../components/lr_theme.h"
#include "../components/widgets.h"

// Page 2 — Providers: the relay list, the row actions, and the entry point to
// the editor dialog (composed in overlays.h).
namespace lr_gui {

inline void composeProviderRow(eui::Ui& ui, float x, float y, float width, int index,
                               const literouter::ProviderConfig& provider) {
    AppState& state = appState();
    const Palette& p = palette();
    const std::string base = "providers.row." + std::to_string(index);
    const float height = 116.0f;
    const literouter::ProviderStat* stat = statFor(state.snapshot, provider.id);
    const literouter::ProviderHealth* health = healthFor(state.snapshot, provider.id);
    const std::string stateName = health != nullptr ? health->stateName() : "Unknown";
    const eui::Color stateColor = healthColor(stateName);
    const std::string displayName = providerDisplayName(provider);
    const std::string id = provider.id;
    const std::string keyText = providerKeySource(provider);
    const std::uint64_t requests = stat != nullptr ? stat->requests : 0;
    const std::uint64_t successes = stat != nullptr ? stat->successes : 0;
    const std::uint64_t failures = stat != nullptr ? stat->failures : 0;
    const double latency = stat != nullptr ? stat->latency_ms_avg : 0.0;

    const auto probe = state.probes.find(id);
    const bool probed = probe != state.probes.end() && probe->second.done;
    const ProbeView* probeView = probed ? &probe->second : nullptr;

    const float rightColumn = 356.0f;
    const float textWidth = std::max(160.0f, width - 84.0f - rightColumn);

    ui.stack(base)
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect(base + ".hit")
                .size(width, height)
                .radius(p.radiusCard)
                .color(p.surface)
                .states(p.surface, p.surfaceHover, p.surfaceActive)
                .border(1.0f, withAlpha(p.border, 0.8f))
                .transition(motion())
                .animate(eui::AnimProperty::Color)
                .onClick([index] { appState().openProviderEditor(index, false); })
                .build();

            ui.stack(base + ".toggle")
                .position(16.0f, 40.0f)
                .size(52.0f, 30.0f)
                .content([&] {
                    components::toggleSwitch(ui, base + ".enable")
                        .theme(uiTokens())
                        .size(52.0f, 30.0f)
                        .trackSize(40.0f, 22.0f)
                        .checked(provider.enabled)
                        .onChange([index](bool value) {
                            auto& providers = appState().store.config().providers;
                            if (index >= 0 && index < static_cast<int>(providers.size())) {
                                providers[static_cast<std::size_t>(index)].enabled = value;
                                appState().saveConfig();
                            }
                        })
                        .build();
                })
                .build();

            ui.text(base + ".name")
                .position(84.0f, 14.0f)
                .size(textWidth, 22.0f)
                .text(displayName)
                .fontSize(15.0f)
                .lineHeight(20.0f)
                .fontWeight(700)
                .color(provider.enabled ? p.text : p.textFaint)
                .build();

            const float nameWidth = std::min(textWidth - 10.0f, measureText(displayName, 15.0f, 700));
            chip(ui, base + ".id", 84.0f + nameWidth + 10.0f, 14.0f, id, p.textMuted,
                 withAlpha(p.text, 0.06f));
            if (!provider.enabled) {
                chip(ui, base + ".disabled",
                     84.0f + nameWidth + 20.0f + measureText(id, 12.0f) + 20.0f, 14.0f, literouter::i18n::tr("disabled"),
                     p.textFaint, withAlpha(p.text, 0.04f));
            }

            dot(ui, base + ".health.dot", width - rightColumn + 2.0f, 19.0f, 8.0f, stateColor);
            ui.text(base + ".health.word")
                .position(width - rightColumn + 16.0f, 12.0f)
                .size(160.0f, 20.0f)
                .text(std::string(literouter::i18n::tr(stateName)))
                .fontSize(12.5f)
                .lineHeight(19.0f)
                .fontWeight(550)
                .color(stateColor)
                .build();

            const int maxMetaChars = std::max(20, static_cast<int>(textWidth / 6.8f));
            const bool isZh = literouter::i18n::resolveLang(appState().currentLanguage()) == literouter::i18n::Lang::Zh;
            const std::string protoSuffix =
                (!provider.protocol.empty() && provider.protocol != "openai")
                    ? (" [" + provider.protocol + "]")
                    : "";
            ui.text(base + ".url")
                .position(84.0f, 38.0f)
                .size(textWidth, 18.0f)
                .text(literouter::truncateUtf8(
                    provider.base_url.empty() ? std::string(literouter::i18n::tr("(no base url)"))
                                              : (provider.base_url + protoSuffix),
                    maxMetaChars))
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(p.textMuted)
                .build();

            const std::string meta = isZh
                ? "优先级 " + std::to_string(provider.priority) + " · 权重 " +
                  std::to_string(provider.weight) + " · 密钥 " + keyText + " · " +
                  std::to_string(provider.models.size()) + " 个模型 · 请求 " +
                  literouter::humanCount(requests) + " 成功 " + literouter::humanCount(successes) +
                  " 失败 " + literouter::humanCount(failures) + " · 均延 " +
                  literouter::humanMillis(latency)
                : "priority " + std::to_string(provider.priority) + " · weight " +
                  std::to_string(provider.weight) + " · key " + keyText + " · " +
                  std::to_string(provider.models.size()) + " models · req " +
                  literouter::humanCount(requests) + " ok " + literouter::humanCount(successes) +
                  " fail " + literouter::humanCount(failures) + " · avg " +
                  literouter::humanMillis(latency);
            ui.text(base + ".meta")
                .position(84.0f, 60.0f)
                .size(textWidth, 18.0f)
                .text(literouter::truncateUtf8(meta, maxMetaChars))
                .fontSize(11.5f)
                .lineHeight(16.0f)
                .color(p.textFaint)
                .build();

            std::string statusText;
            eui::Color statusColor = p.textFaint;
            if (probeView != nullptr) {
                if (probeView->reachable) {
                    statusText = isZh ? ("探测 · 连通 · HTTP " + std::to_string(probeView->status) +
                                         " · " + literouter::humanMillis(probeView->latencyMs) + " · " +
                                         std::to_string(probeView->models.size()) + " 个模型")
                                      : ("probe · reachable · HTTP " + std::to_string(probeView->status) +
                                         " · " + literouter::humanMillis(probeView->latencyMs) + " · " +
                                         std::to_string(probeView->models.size()) + " models");
                    statusColor = p.success;
                } else {
                    statusText = (isZh ? "探测 · 无法连通" : "probe · unreachable") +
                                 (probeView->detail.empty() ? "" : " · " + probeView->detail);
                    statusColor = p.danger;
                }
            } else if (probe != state.probes.end() && probe->second.running) {
                statusText = isZh ? "探测 · 运行中…" : "probe · running…";
                statusColor = p.accent;
            } else if (health != nullptr && !health->last_error.empty()) {
                statusText = (isZh ? "最近错误 · " : "last error · ") +
                             literouter::truncateUtf8(health->last_error, maxMetaChars);
                statusColor = stateName == "Open" ? p.danger : p.warn;
            }
            if (!statusText.empty()) {
                ui.text(base + ".status")
                    .position(84.0f, 82.0f)
                    .size(textWidth, 18.0f)
                    .text(literouter::truncateUtf8(statusText, maxMetaChars))
                    .fontSize(11.5f)
                    .lineHeight(16.0f)
                    .color(statusColor)
                    .build();
            }

            const float buttonY = 58.0f;
            const float buttonWidth = 66.0f;
            const float buttonGap = 8.0f;
            const float buttonsX = width - 18.0f - (buttonWidth * 4.0f + buttonGap * 3.0f);
            actionButton(ui, base + ".edit", buttonsX, buttonY, buttonWidth, 30.0f, literouter::i18n::tr("Edit"), false,
                         [index] { appState().openProviderEditor(index, false); });
            actionButton(ui, base + ".test", buttonsX + (buttonWidth + buttonGap), buttonY,
                         buttonWidth, 30.0f, literouter::i18n::tr("Test"), false,
                         [index] { appState().testProvider(index); });
            actionButton(ui, base + ".duplicate", buttonsX + (buttonWidth + buttonGap) * 2.0f,
                         buttonY, buttonWidth, 30.0f, literouter::i18n::tr("Copy"), false,
                         [index] { appState().duplicateProvider(index); });
            actionButton(ui, base + ".delete", buttonsX + (buttonWidth + buttonGap) * 3.0f, buttonY,
                         buttonWidth, 30.0f, literouter::i18n::tr("Delete"), false, [index, id, isZh] {
                             const std::string confirmMsg = isZh
                                 ? "中转站 `" + id + "` 将从配置中移除。现有引用它的路由将跳过此节点。"
                                 : "Relay `" + id + "` will be removed from the config. Existing routes that name it will skip this hop.";
                             appState().requestConfirm(
                                 ConfirmKind::DeleteProvider, index,
                                 std::string(literouter::i18n::tr("Delete provider?")),
                                 confirmMsg,
                                 std::string(literouter::i18n::tr("Delete relay")));
                         });
        })
        .build();
}

inline void composeProviders(eui::Ui& ui, float x, float y, float width, float height) {
    AppState& state = appState();
    const Palette& p = palette();
    const literouter::AppConfig& config = state.store.config();
    const std::size_t count = config.providers.size();
    std::size_t enabled = 0;
    for (const auto& provider : config.providers) {
        if (provider.enabled) {
            ++enabled;
        }
    }

    const bool isZh = literouter::i18n::resolveLang(state.currentLanguage()) == literouter::i18n::Lang::Zh;
    const std::string countText = isZh
        ? "已配置 " + std::to_string(count) + " 个中转站 · 已启用 " + std::to_string(enabled) + " 个"
        : std::to_string(count) + (count == 1 ? " relay" : " relays") + " configured · " +
          std::to_string(enabled) + " enabled";

    const float headerY = y + 20.0f;
    ui.text("providers.count")
        .position(x + layout::pagePadding, headerY)
        .size(std::max(160.0f, width - 480.0f), 22.0f)
        .text(countText)
        .fontSize(13.0f)
        .lineHeight(20.0f)
        .color(p.textMuted)
        .build();

    actionButton(ui, "providers.testAll", x + width - layout::pagePadding - 108.0f, headerY - 8.0f,
                 108.0f, 36.0f, literouter::i18n::tr("Test all"), false,
                 [] { appState().testAllProviders(); }, count == 0 || state.testingAll);
    actionButton(ui, "providers.add", x + width - layout::pagePadding - 108.0f - 12.0f - 118.0f,
                 headerY - 8.0f, 118.0f, 36.0f, literouter::i18n::tr("Add provider"), true,
                 [] { appState().openProviderEditor(-1, true); });

    const float listY = y + 60.0f;
    const float listHeight = std::max(0.0f, height - 60.0f);
    const float rowHeight = 116.0f;
    const float rowGap = 12.0f;
    const float canvasHeight =
        std::max(listHeight, layout::pagePadding + static_cast<float>(count) * (rowHeight + rowGap) +
                                 24.0f);

    components::scrollView(ui, "providers.scroll")
        .theme(uiTokens())
        .position(x, listY)
        .size(width, listHeight)
        .offset(state.providersScroll)
        .step(layout::scrollStep)
        .scrollbarWidth(9.0f)
        .scrollbarGap(6.0f)
        .onChange([&state](float value) { state.providersScroll = value; })
        .content([&](eui::Ui& contentUi, float contentWidth, float) {
            contentUi.stack("providers.canvas")
                .size(contentWidth, canvasHeight)
                .content([&] {
                    const float cardWidth = contentWidth - 2.0f * layout::pagePadding;
                    if (count == 0) {
                        emptyState(contentUi, "providers.empty", layout::pagePadding, 18.0f,
                                   cardWidth, 244.0f, literouter::i18n::tr("No providers configured"),
                                   literouter::i18n::tr("Providers define the upstream relays this proxy forwards to."),
                                   literouter::i18n::tr("Add provider"),
                                   [] { appState().openProviderEditor(-1, true); });
                        return;
                    }
                    for (std::size_t i = 0; i < count; ++i) {
                        const float rowY = 18.0f + static_cast<float>(i) * (rowHeight + rowGap);
                        composeProviderRow(contentUi, layout::pagePadding, rowY, cardWidth,
                                           static_cast<int>(i), config.providers[i]);
                    }
                })
                .build();
        })
        .build();
}

} // namespace lr_gui
