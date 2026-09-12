#pragma once

#include "../app_state.h"
#include "../components/theme.h"
#include "../components/widgets.h"

// Page 3 — Routes: one card per logical model, with its ordered failover chain
// rendered as a row of hops, plus a live plain-English preview of the chain.
namespace lr_gui {

inline void composeRoutes(eui::Ui& ui, float x, float y, float width, float height) {
    AppState& state = appState();
    const Palette& p = palette();
    const literouter::AppConfig& config = state.store.config();
    const std::size_t count = config.routes.size();
    const bool isZh = literouter::i18n::resolveLang(state.currentLanguage()) == literouter::i18n::Lang::Zh;
    const std::string countText = isZh
        ? "已配置 " + std::to_string(count) + " 条路由 · 每个名称对应一个对外逻辑模型"
        : std::to_string(count) + " " +
          std::string(literouter::i18n::tr(count == 1 ? "route" : "routes")) +
          " · " + std::string(literouter::i18n::tr("each name is one client-facing model"));

    const float headerY = y + 20.0f;
    ui.text("routes.count")
        .position(x + layout::pagePadding, headerY)
        .size(std::max(160.0f, width - 480.0f), 22.0f)
        .text(countText)
        .fontSize(13.0f)
        .lineHeight(20.0f)
        .color(p.textMuted)
        .build();

    const float passthroughWidth = 220.0f;
    const float passthroughX = x + width - layout::pagePadding - 118.0f - passthroughWidth - 16.0f;
    if (passthroughX > x + layout::pagePadding + 180.0f) {
        ui.stack("routes.passthrough.wrap")
            .position(passthroughX, headerY - 6.0f)
            .size(passthroughWidth, 32.0f)
            .content([&] {
                components::toggleSwitch(ui, "routes.passthrough")
                    .theme(uiTokens())
                    .size(passthroughWidth, 32.0f)
                    .text(std::string(literouter::i18n::tr("Pass-through unknown models")))
                    .fontSize(12.0f)
                    .checked(config.server.pass_through_unknown)
                    .onChange([](bool value) {
                        appState().store.config().server.pass_through_unknown = value;
                        appState().saveConfig();
                    })
                    .build();
            })
            .build();
    }

    actionButton(ui, "routes.add", x + width - layout::pagePadding - 118.0f, headerY - 8.0f,
                 118.0f, 36.0f, literouter::i18n::tr("Add route"), true, [] { appState().openRouteEditor(-1); });

    const float listY = y + 60.0f;
    const float listHeight = std::max(0.0f, height - 60.0f);
    const float cardHeight = 212.0f;
    const float cardGap = 14.0f;
    const float canvasHeight =
        std::max(listHeight, layout::pagePadding + static_cast<float>(count) * (cardHeight + cardGap) +
                                 24.0f);

    components::scrollView(ui, "routes.scroll")
        .theme(uiTokens())
        .position(x, listY)
        .size(width, listHeight)
        .offset(state.routesScroll)
        .step(layout::scrollStep)
        .scrollbarWidth(9.0f)
        .scrollbarGap(6.0f)
        .onChange([&state](float value) { state.routesScroll = value; })
        .content([&](eui::Ui& contentUi, float contentWidth, float) {
            contentUi.stack("routes.canvas")
                .size(contentWidth, canvasHeight)
                .content([&] {
                    const float cardWidth = contentWidth - 2.0f * layout::pagePadding;
                    if (count == 0) {
                        emptyState(contentUi, "routes.empty", layout::pagePadding, 18.0f, cardWidth,
                                   244.0f, literouter::i18n::tr("No routes yet"),
                                   literouter::i18n::tr("A route maps a model name your clients ask for onto an ordered chain of relays. Add one, then add hops to it."),
                                   literouter::i18n::tr("Add route"), [] { appState().openRouteEditor(-1); });
                        return;
                    }

                    for (std::size_t i = 0; i < count; ++i) {
                        const literouter::RouteConfig& route = config.routes[i];
                        const int routeIndex = static_cast<int>(i);
                        const float cardY =
                            18.0f + static_cast<float>(i) * (cardHeight + cardGap);
                        const std::string base = "routes.card." + std::to_string(i);

                        contentUi.stack(base)
                            .position(layout::pagePadding, cardY)
                            .size(cardWidth, cardHeight)
                            .content([&] {
                                contentUi.rect(base + ".bg")
                                    .size(cardWidth, cardHeight)
                                    .radius(p.radiusCard)
                                    .color(p.surface)
                                    .border(1.0f, withAlpha(p.border, 0.8f))
                                    .build();

                                contentUi.stack(base + ".toggle")
                                    .position(16.0f, 18.0f)
                                    .size(52.0f, 30.0f)
                                    .content([&] {
                                        components::toggleSwitch(contentUi, base + ".enable")
                                            .theme(uiTokens())
                                            .size(52.0f, 30.0f)
                                            .trackSize(40.0f, 22.0f)
                                            .checked(route.enabled)
                                            .onChange([routeIndex](bool value) {
                                                auto& routes =
                                                    appState().store.config().routes;
                                                if (routeIndex >= 0 &&
                                                    routeIndex <
                                                        static_cast<int>(routes.size())) {
                                                    routes[static_cast<std::size_t>(routeIndex)]
                                                        .enabled = value;
                                                    appState().saveConfig();
                                                }
                                            })
                                            .build();
                                    })
                                    .build();

                                contentUi.text(base + ".model")
                                    .position(80.0f, 14.0f)
                                    .size(cardWidth - 320.0f, 24.0f)
                                    .text(route.model.empty()
                                              ? std::string(literouter::i18n::tr("(unnamed route)"))
                                              : route.model)
                                    .fontSize(16.0f)
                                    .lineHeight(22.0f)
                                    .fontWeight(740)
                                    .color(route.enabled ? p.text : p.textFaint)
                                    .build();
                                const std::string hopCountText =
                                    std::to_string(route.targets.size()) + " " +
                                    std::string(literouter::i18n::tr(route.targets.size() == 1 ? "hop" : "hops"));
                                const std::string advText =
                                    std::string(literouter::i18n::tr(route.enabled ? "advertised" : "not advertised"));
                                contentUi.text(base + ".hint")
                                    .position(80.0f, 36.0f)
                                    .size(cardWidth - 320.0f, 18.0f)
                                    .text(hopCountText + " · " + advText)
                                    .fontSize(11.5f)
                                    .lineHeight(15.0f)
                                    .color(p.textFaint)
                                    .build();

                                const float headerButtonWidth = 68.0f;
                                actionButton(contentUi, base + ".edit",
                                             cardWidth - 18.0f - 2.0f * headerButtonWidth - 8.0f, 18.0f,
                                             headerButtonWidth, 30.0f, literouter::i18n::tr("Edit"), false,
                                             [routeIndex] {
                                                 appState().openRouteEditor(routeIndex);
                                             });
                                actionButton(contentUi, base + ".delete",
                                             cardWidth - 18.0f - headerButtonWidth, 18.0f,
                                             headerButtonWidth, 30.0f, literouter::i18n::tr("Delete"), false,
                                             [routeIndex, model = route.model, isZh] {
                                                 const std::string confirmMsg = isZh
                                                     ? "请求模型 “" + model + "” 的客户端将不再匹配此路由。"
                                                     : "Clients asking for \"" + model + "\" will no longer be matched by this route.";
                                                 appState().requestConfirm(
                                                     ConfirmKind::DeleteRoute, routeIndex,
                                                     std::string(literouter::i18n::tr("Delete route?")),
                                                     confirmMsg,
                                                     std::string(literouter::i18n::tr("Delete route")));
                                             });

                                // Hop chain.
                                float hopX = 16.0f;
                                const float chainY = 62.0f;
                                if (route.targets.empty()) {
                                    contentUi.text(base + ".empty")
                                        .position(hopX, chainY + 18.0f)
                                        .size(cardWidth - 32.0f, 20.0f)
                                        .text(std::string(literouter::i18n::tr("No hops yet — add the first relay this model should be tried on.")))
                                        .fontSize(12.5f)
                                        .lineHeight(18.0f)
                                        .color(p.warn)
                                        .build();
                                }
                                for (std::size_t h = 0; h < route.targets.size(); ++h) {
                                    const literouter::RouteTarget& target = route.targets[h];
                                    const literouter::ProviderConfig* provider =
                                        config.provider(target.provider);
                                    const bool missing = provider == nullptr;
                                    const bool disabled = provider != nullptr && !provider->enabled;
                                    const literouter::ProviderHealth* health =
                                        healthFor(state.snapshot, target.provider);
                                    const bool open =
                                        health != nullptr &&
                                        health->state ==
                                            literouter::ProviderHealth::State::Open;
                                    const eui::Color edge =
                                        missing || open ? p.danger
                                        : disabled      ? p.warn
                                                        : withAlpha(p.border, 0.9f);

                                     const std::string modelText =
                                        target.model.empty() ? std::string(literouter::i18n::tr("(same model name)")) : target.model;
                                    const std::string providerText = target.provider;
                                    const float textWidth =
                                        std::max(measureText(providerText, 13.0f, 650),
                                                 measureText(modelText, 11.5f));
                                     const float blockWidth =
                                        std::clamp(textWidth + 120.0f, 190.0f, 280.0f);
                                    const float blockHeight = 56.0f;
                                    const std::string hopBase =
                                        base + ".hop." + std::to_string(h);

                                    contentUi.stack(hopBase)
                                        .position(hopX, chainY)
                                        .size(blockWidth, blockHeight)
                                        .content([&] {
                                            contentUi.rect(hopBase + ".bg")
                                                .size(blockWidth, blockHeight)
                                                .radius(p.radiusControl)
                                                .color(withAlpha(p.surfaceHover, 0.9f))
                                                .border(1.0f, edge)
                                                .build();
                                            contentUi.text(hopBase + ".index")
                                                .position(10.0f, 7.0f)
                                                .size(20.0f, 16.0f)
                                                .text(std::to_string(h + 1))
                                                .fontSize(10.5f)
                                                .lineHeight(14.0f)
                                                .fontWeight(700)
                                                .color(p.textFaint)
                                                .build();
                                            contentUi.text(hopBase + ".provider")
                                                .position(28.0f, 6.0f)
                                                .size(blockWidth - 100.0f, 18.0f)
                                                .text(providerText)
                                                .fontSize(13.0f)
                                                .lineHeight(17.0f)
                                                .fontWeight(650)
                                                .color(missing ? p.danger : p.text)
                                                .build();
                                            contentUi.text(hopBase + ".model")
                                                .position(28.0f, 26.0f)
                                                .size(blockWidth - 100.0f, 18.0f)
                                                .text(modelText)
                                                .fontSize(11.5f)
                                                .lineHeight(15.0f)
                                                .color(p.textMuted)
                                                .build();
                                            if (missing || disabled || open) {
                                                const std::string tag =
                                                    missing ? std::string(literouter::i18n::tr("missing"))
                                                    : open  ? std::string(literouter::i18n::tr("breaker open"))
                                                            : std::string(literouter::i18n::tr("disabled"));
                                                contentUi.text(hopBase + ".tag")
                                                    .position(12.0f, 42.0f)
                                                    .size(blockWidth - 100.0f, 14.0f)
                                                    .text(tag)
                                                    .fontSize(10.5f)
                                                    .lineHeight(13.0f)
                                                    .fontWeight(600)
                                                    .color(missing || open ? p.danger : p.warn)
                                                    .build();
                                            }

                                            const float smallX = blockWidth - 8.0f - 88.0f;
                                            actionButton(contentUi, hopBase + ".edit", smallX,
                                                         30.0f, 20.0f, 20.0f, "✎", false,
                                                         [routeIndex, h] {
                                                             appState().openHopEditor(
                                                                 routeIndex, static_cast<int>(h));
                                                         });
                                            actionButton(contentUi, hopBase + ".earlier", smallX + 22.0f,
                                                         30.0f, 20.0f, 20.0f, "<", false,
                                                         [routeIndex, h] {
                                                             appState().moveHop(
                                                                 routeIndex, static_cast<int>(h),
                                                                 -1);
                                                         },
                                                         h == 0);
                                            actionButton(contentUi, hopBase + ".later",
                                                         smallX + 44.0f, 30.0f, 20.0f, 20.0f, ">",
                                                         false,
                                                         [routeIndex, h] {
                                                             appState().moveHop(
                                                                 routeIndex, static_cast<int>(h), 1);
                                                         },
                                                         h + 1 == route.targets.size());
                                            actionButton(contentUi, hopBase + ".remove",
                                                         smallX + 66.0f, 30.0f, 20.0f, 20.0f, "x",
                                                         false,
                                                         [routeIndex, h] {
                                                             appState().removeHop(
                                                                 routeIndex, static_cast<int>(h));
                                                         });
                                        })
                                        .build();

                                    hopX += blockWidth;
                                    const bool more = h + 1 < route.targets.size();
                                    if (more) {
                                        contentUi.text(hopBase + ".arrow")
                                            .position(hopX, chainY)
                                            .size(24.0f, blockHeight)
                                            .text("→")
                                            .fontSize(15.0f)
                                            .lineHeight(blockHeight)
                                            .color(p.textMuted)
                                            .horizontalAlign(eui::HorizontalAlign::Center)
                                            .verticalAlign(eui::VerticalAlign::Center)
                                            .build();
                                        hopX += 24.0f;
                                    }
                                }

                                // Live preview line.
                                std::string preview;
                                if (route.targets.empty()) {
                                    preview = std::string(literouter::i18n::tr("A client asking for ")) +
                                              route.model +
                                              std::string(literouter::i18n::tr(" has no relay to try yet."));
                                } else {
                                    preview = std::string(literouter::i18n::tr("A client asking for ")) +
                                              route.model +
                                              std::string(literouter::i18n::tr(" will try "));
                                    for (std::size_t h = 0; h < route.targets.size(); ++h) {
                                        if (h > 0) {
                                            preview += h + 1 == route.targets.size()
                                                           ? std::string(literouter::i18n::tr(", then "))
                                                           : ", ";
                                        }
                                        preview += route.targets[h].provider;
                                        if (!route.targets[h].model.empty()) {
                                            preview += " (" + route.targets[h].model + ")";
                                        }
                                    }
                                    preview += ".";
                                }
                                contentUi.rect(base + ".preview.bg")
                                    .position(16.0f, 130.0f)
                                    .size(cardWidth - 32.0f, 30.0f)
                                    .radius(p.radiusControl)
                                    .color(withAlpha(p.accent, 0.09f))
                                    .build();
                                contentUi.text(base + ".preview")
                                    .position(26.0f, 130.0f)
                                    .size(cardWidth - 52.0f, 30.0f)
                                    .text(preview)
                                    .fontSize(12.5f)
                                    .lineHeight(30.0f)
                                    .color(p.info)
                                    .verticalAlign(eui::VerticalAlign::Center)
                                    .build();

                                actionButton(contentUi, base + ".addhop", 16.0f, 170.0f, 120.0f,
                                             30.0f, literouter::i18n::tr("Add hop"), false,
                                             [routeIndex] {
                                                 appState().openHopEditor(routeIndex);
                                             });
                            })
                            .build();
                    }
                })
                .build();
        })
        .build();
}

} // namespace lr_gui
