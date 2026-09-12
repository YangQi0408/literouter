#pragma once

#include "../app_state.h"
#include "../components/theme.h"
#include "../components/widgets.h"

// Global overlays: the provider editor, the add-hop dialog, the confirmation
// dialog, the log detail dialog, and the toast. Composed at the root so they sit
// above the shell and are never clipped by a page's scroll view.
namespace lr_gui {

namespace detail {

inline void labelledInput(eui::Ui& ui, const std::string& id, float x, float y, float width,
                          const std::string& label, const std::string& hint,
                          const std::string& value, const std::string& placeholder,
                          std::function<void(const std::string&)> onChange, bool multiline = false,
                          float inputHeight = 36.0f) {
    fieldLabel(ui, id + ".label", x, y, width, label, hint);
    ui.stack(id + ".wrap")
        .position(x, y + (hint.empty() ? 18.0f : 26.0f))
        .size(width, inputHeight)
        .content([&] {
            components::input(ui, id)
                .theme(uiTokens())
                .size(width, inputHeight)
                .value(value)
                .placeholder(placeholder)
                .multiline(multiline)
                .onChange(std::move(onChange))
                .build();
        })
        .build();
}

inline void labelledStepper(eui::Ui& ui, const std::string& id, float x, float y, float width,
                            const std::string& label, const std::string& hint, long long value,
                            long long step, long long minValue, long long maxValue,
                            std::function<void(long long)> onChange) {
    fieldLabel(ui, id + ".label", x, y, width, label, hint);
    ui.stack(id + ".wrap")
        .position(x, y + 18.0f)
        .size(width, 36.0f)
        .content([&] {
            components::stepper(ui, id)
                .theme(uiTokens())
                .size(width, 36.0f)
                .value(value)
                .step(step)
                .min(minValue)
                .max(maxValue)
                .onChange(std::move(onChange))
                .build();
        })
        .build();
}

inline const std::vector<std::string>& providerIssues(const AppState& state) {
    static std::uint64_t lastRevision = static_cast<std::uint64_t>(-1);
    static int lastEditorIndex = -2;
    static std::vector<std::string> cachedOut;
    
    if (lastRevision == state.configRevision_ && lastEditorIndex == state.editor.index) {
        return cachedOut;
    }
    
    lastRevision = state.configRevision_;
    lastEditorIndex = state.editor.index;
    cachedOut.clear();
    
    if (state.editor.index < 0) {
        return cachedOut;
    }
    const std::string prefix = "providers[" + std::to_string(state.editor.index) + "]";
    const literouter::ValidationReport report = literouter::validate(state.store.config());
    for (const auto& issue : report.issues) {
        if (issue.path.rfind(prefix, 0) == 0) {
            cachedOut.push_back(issue.levelName() + " · " + issue.path + " · " + issue.message);
        }
    }
    return cachedOut;
}

inline void keyValue(eui::Ui& ui, const std::string& id, float x, float y, float width,
                     const std::string& label, const std::string& value,
                     const eui::Color& valueColor) {
    ui.text(id + ".label")
        .position(x, y)
        .size(width, 16.0f)
        .text(label)
        .fontSize(10.5f)
        .lineHeight(14.0f)
        .fontWeight(700)
        .color(palette().textFaint)
        .build();
    ui.text(id + ".value")
        .position(x, y + 16.0f)
        .size(width, 20.0f)
        .text(value.empty() ? "—" : value)
        .fontSize(12.5f)
        .lineHeight(18.0f)
        .color(valueColor)
        .build();
}

} // namespace detail

inline void composeProviderEditor(eui::Ui& ui, const eui::Screen& screen) {
    AppState& state = appState();
    const Palette& p = palette();
    ProviderEditor& editor = state.editor;
    if (!editor.open) {
        return;
    }

    constexpr float panelWidth = 860.0f;
    constexpr float panelHeight = 700.0f;
    const float leftX = 28.0f;
    const float rightX = 448.0f;
    const float columnWidth = 384.0f;

    components::dialog(ui, "overlays.editor")
        .open(editor.open)
        .screen(screen.width, screen.height)
        .size(panelWidth, panelHeight)
        .theme(uiTokens())
        .transition(motion())
        .onOpenChange([&editor](bool open) { editor.open = open; })
        .content([&] {
            ui.text("overlays.editor.title")
                .position(leftX, 20.0f)
                .size(panelWidth - 56.0f, 26.0f)
                .text(editor.isNew ? literouter::i18n::tr("Add relay") : literouter::i18n::tr("Edit relay"))
                .fontSize(19.0f)
                .lineHeight(24.0f)
                .fontWeight(760)
                .color(p.text)
                .build();
            ui.text("overlays.editor.sub")
                .position(leftX, 46.0f)
                .size(panelWidth - 56.0f, 20.0f)
                .text(literouter::i18n::tr("A relay is one upstream endpoint. Routes reference it by id, so the id is "
                      "stable and unique."))
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(p.textMuted)
                .build();

            detail::labelledInput(ui, "overlays.editor.id", leftX, 84.0f, columnWidth,
                                  std::string(literouter::i18n::tr("Id")), {},
                                  editor.id, std::string(literouter::i18n::tr("e.g. openai")), [&editor](const std::string& value) {
                                      editor.id = value;
                                      editor.statusLine.clear();
                                  });
            detail::labelledInput(ui, "overlays.editor.name", leftX, 150.0f, columnWidth,
                                  std::string(literouter::i18n::tr("Display name")), {}, editor.name,
                                  std::string(literouter::i18n::tr("e.g. OpenAI official")),
                                  [&editor](const std::string& value) { editor.name = value; });
            detail::labelledInput(ui, "overlays.editor.baseurl", leftX, 216.0f, columnWidth,
                                  std::string(literouter::i18n::tr("Base URL")),
                                  {std::string(literouter::i18n::tr("Includes the /v1 root of the relay."))},
                                  editor.baseUrl, "https://api.example.com/v1",
                                  [&editor](const std::string& value) { editor.baseUrl = value; });
            detail::labelledInput(
                ui, "overlays.editor.apikey", leftX, 282.0f, columnWidth,
                std::string(literouter::i18n::tr("API key")),
                {std::string(literouter::i18n::tr("Literal, or ${ENV_VAR} / $ENV_VAR to read it from the environment at request time."))},
                editor.apiKey, "${OPENAI_API_KEY}", [&editor](const std::string& value) {
                    editor.apiKey = value;
                });

            detail::labelledStepper(ui, "overlays.editor.priority", rightX, 84.0f, columnWidth,
                                    std::string(literouter::i18n::tr("Priority")),
                                    {std::string(literouter::i18n::tr("Lower wins; ties fall back to declaration order."))},
                                    editor.priority, 1, -1000, 100000,
                                    [&editor](long long value) {
                                        editor.priority = static_cast<int>(value);
                                    });
            detail::labelledStepper(ui, "overlays.editor.weight", rightX, 150.0f, columnWidth,
                                    std::string(literouter::i18n::tr("Weight")),
                                    {std::string(literouter::i18n::tr("Tie-break inside one priority band."))}, editor.weight,
                                    1, 0, 1000, [&editor](long long value) {
                                        editor.weight = static_cast<int>(value);
                                    });
            detail::labelledStepper(ui, "overlays.editor.timeout", rightX, 216.0f, columnWidth,
                                    std::string(literouter::i18n::tr("Request timeout (seconds)")), {}, editor.timeoutSec, 5, 1, 3600,
                                    [&editor](long long value) {
                                        editor.timeoutSec = static_cast<int>(value);
                                    });
            detail::labelledStepper(ui, "overlays.editor.connecttimeout", rightX, 282.0f,
                                    columnWidth, std::string(literouter::i18n::tr("Connect timeout (seconds)")), {},
                                    editor.connectTimeoutSec, 1, 1, 600,
                                    [&editor](long long value) {
                                        editor.connectTimeoutSec = static_cast<int>(value);
                                    });

            ui.text("overlays.editor.protocol.label")
                .position(leftX, 348.0f)
                .size(panelWidth - 56.0f, 16.0f)
                .text(std::string(literouter::i18n::tr("Protocol")))
                .fontSize(11.0f)
                .fontWeight(700)
                .color(p.textMuted)
                .build();
            ui.stack("overlays.editor.protocol.wrap")
                .position(leftX, 366.0f)
                .size(panelWidth - 56.0f, 30.0f)
                .content([&] {
                    components::segmented(ui, "overlays.editor.protocol")
                        .theme(uiTokens())
                        .size(panelWidth - 56.0f, 30.0f)
                        .items({"OpenAI", "Anthropic Claude", "Google Gemini", "OpenAI Responses"})
                        .selected(editor.protocolChoice)
                        .onChange([&editor](int idx) {
                            editor.protocolChoice = idx;
                            if (idx == 1) editor.protocol = "anthropic";
                            else if (idx == 2) editor.protocol = "gemini";
                            else if (idx == 3) editor.protocol = "openai_responses";
                            else editor.protocol = "openai";
                        })
                        .build();
                })
                .build();

            detail::labelledInput(ui, "overlays.editor.models", leftX, 404.0f, panelWidth - 56.0f,
                                  std::string(literouter::i18n::tr("Models")),
                                  {std::string(literouter::i18n::tr("Comma or newline separated. Used by /v1/models and "
                                             "for pass-through matching."))},
                                  editor.modelsText, "gpt-4o, gpt-4o-mini, text-embedding-3-small",
                                  [&editor](const std::string& value) { editor.modelsText = value; },
                                  true, 58.0f);

            detail::labelledInput(ui, "overlays.editor.headers", leftX, 480.0f, panelWidth - 56.0f,
                                  std::string(literouter::i18n::tr("Extra headers")),
                                  {std::string(literouter::i18n::tr("One per line, \"Name: value\"."))},
                                  editor.headersText, "X-Title: literouter", [&editor](
                                                                                 const std::string&
                                                                                     value) {
                                      editor.headersText = value;
                                  },
                                  true, 52.0f);

            ui.stack("overlays.editor.enabled.wrap")
                .position(leftX, 546.0f)
                .size(240.0f, 30.0f)
                .content([&] {
                    components::toggleSwitch(ui, "overlays.editor.enabled")
                        .theme(uiTokens())
                        .size(240.0f, 30.0f)
                        .text(std::string(literouter::i18n::tr("Enabled")))
                        .fontSize(13.0f)
                        .checked(editor.enabled)
                        .onChange([&editor](bool value) { editor.enabled = value; })
                        .build();
                })
                .build();
            ui.stack("overlays.editor.stream.wrap")
                .position(leftX + 260.0f, 546.0f)
                .size(280.0f, 30.0f)
                .content([&] {
                    components::toggleSwitch(ui, "overlays.editor.stream")
                        .theme(uiTokens())
                        .size(280.0f, 30.0f)
                        .text(std::string(literouter::i18n::tr("Supports streaming")))
                        .fontSize(13.0f)
                        .checked(editor.supportsStream)
                        .onChange([&editor](bool value) { editor.supportsStream = value; })
                        .build();
                })
                .build();

            const ProbeView* probe = state.editorProbe();
            std::string status = editor.statusLine;
            bool statusError = editor.statusError;
            const bool isZh = literouter::i18n::resolveLang(state.currentLanguage()) == literouter::i18n::Lang::Zh;
            if (status.empty() && probe != nullptr && probe->done) {
                if (probe->reachable) {
                    status = (isZh ? "探测 · HTTP " : "probe · HTTP ") +
                             std::to_string(probe->status) + " · " +
                             literouter::humanMillis(probe->latencyMs) + " · " +
                             std::to_string(probe->models.size()) + " " +
                             std::string(literouter::i18n::tr("models discovered"));
                    statusError = false;
                } else {
                    status = std::string(literouter::i18n::tr("probe · unreachable")) +
                             (probe->detail.empty() ? "" : " · " + probe->detail);
                    statusError = true;
                }
            } else if (status.empty() && probe != nullptr && probe->running) {
                status = std::string(literouter::i18n::tr("probe · running…"));
                statusError = false;
            }

            const std::vector<std::string> issues = detail::providerIssues(state);
            if (status.empty() && !issues.empty()) {
                status = issues.front();
                statusError = true;
            }
            if (status.empty()) {
                status = editor.isNew ? std::string(literouter::i18n::tr("Fill in the fields, then Save to write the config."))
                                      : std::string(literouter::i18n::tr("Editing an existing relay. Save writes the config file."));
                statusError = false;
            }
            ui.text("overlays.editor.status")
                .position(leftX, 590.0f)
                .size(panelWidth - 56.0f, 18.0f)
                .text(literouter::truncateUtf8(status, 150))
                .fontSize(11.5f)
                .lineHeight(16.0f)
                .color(statusError ? p.danger : p.textFaint)
                .build();

            actionButton(ui, "overlays.editor.test", leftX, 624.0f, 96.0f, 40.0f,
                         std::string(literouter::i18n::tr("Test")), false,
                         [] { appState().testEditorProvider(); });
            const bool hasModels = probe != nullptr && probe->done && !probe->models.empty();
            actionButton(ui, "overlays.editor.usemodels", leftX + 106.0f, 624.0f, 188.0f, 40.0f,
                         std::string(literouter::i18n::tr("Use discovered models")), false, [] {
                             AppState& app = appState();
                             const ProbeView* view = app.editorProbe();
                             if (view == nullptr || view->models.empty()) {
                                 return;
                             }
                             app.editor.modelsText = AppState::joinLines(view->models);
                             app.editor.statusLine = std::string(literouter::i18n::tr("Filled in ")) +
                                                     std::to_string(view->models.size()) +
                                                     std::string(literouter::i18n::tr(" discovered models."));
                             app.editor.statusError = false;
                         },
                         !hasModels);

            actionButton(ui, "overlays.editor.cancel", panelWidth - 28.0f - 96.0f - 12.0f - 116.0f,
                         624.0f, 116.0f, 40.0f, std::string(literouter::i18n::tr("Cancel")), false,
                         [&editor] { editor.open = false; });
            actionButton(ui, "overlays.editor.save", panelWidth - 28.0f - 116.0f, 624.0f, 116.0f,
                         40.0f, std::string(literouter::i18n::tr("Save relay")), true, [] { appState().applyProviderEditor(); });
        })
        .build();
}

inline void composeRouteEditor(eui::Ui& ui, const eui::Screen& screen) {
    AppState& state = appState();
    const Palette& p = palette();
    RouteEditor& routeEditor = state.routeEditor;
    if (!routeEditor.open) {
        return;
    }

    constexpr float panelWidth = 520.0f;
    constexpr float panelHeight = 310.0f;

    components::dialog(ui, "overlays.route")
        .open(routeEditor.open)
        .screen(screen.width, screen.height)
        .size(panelWidth, panelHeight)
        .theme(uiTokens())
        .transition(motion())
        .onOpenChange([&routeEditor](bool open) { routeEditor.open = open; })
        .content([&] {
            ui.text("overlays.route.title")
                .position(28.0f, 20.0f)
                .size(panelWidth - 56.0f, 26.0f)
                .text(routeEditor.isNew ? literouter::i18n::tr("Add route")
                                        : literouter::i18n::tr("Edit route"))
                .fontSize(19.0f)
                .lineHeight(24.0f)
                .fontWeight(760)
                .color(p.text)
                .build();
            ui.text("overlays.route.sub")
                .position(28.0f, 46.0f)
                .size(panelWidth - 56.0f, 20.0f)
                .text(std::string(literouter::i18n::tr("Model name clients will request")) + ".")
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(p.textMuted)
                .build();

            detail::labelledInput(
                ui, "overlays.route.model", 28.0f, 82.0f, panelWidth - 56.0f,
                std::string(literouter::i18n::tr("Logical model name")),
                {std::string(literouter::i18n::tr("Model name clients will request"))},
                routeEditor.model, std::string(literouter::i18n::tr("e.g. gpt-4o, claude-3-5-sonnet")),
                [&routeEditor](const std::string& value) { routeEditor.model = value; });

            ui.stack("overlays.route.enable.wrap")
                .position(28.0f, 168.0f)
                .size(panelWidth - 56.0f, 32.0f)
                .content([&] {
                    components::toggleSwitch(ui, "overlays.route.enable")
                        .theme(uiTokens())
                        .size(panelWidth - 56.0f, 32.0f)
                        .text(std::string(literouter::i18n::tr("Enable route")))
                        .fontSize(13.0f)
                        .checked(routeEditor.enabled)
                        .onChange([&routeEditor](bool value) { routeEditor.enabled = value; })
                        .build();
                })
                .build();

            ui.text("overlays.route.status")
                .position(28.0f, 208.0f)
                .size(panelWidth - 56.0f, 18.0f)
                .text(routeEditor.statusLine)
                .fontSize(11.5f)
                .lineHeight(16.0f)
                .color(routeEditor.statusError ? p.danger : p.textFaint)
                .build();

            actionButton(ui, "overlays.route.cancel",
                         panelWidth - 28.0f - 96.0f - 12.0f - 120.0f, 246.0f, 96.0f, 40.0f,
                         std::string(literouter::i18n::tr("Cancel")), false,
                         [&routeEditor] { routeEditor.open = false; });
            actionButton(ui, "overlays.route.save", panelWidth - 28.0f - 120.0f, 246.0f, 120.0f,
                         40.0f,
                         routeEditor.isNew ? std::string(literouter::i18n::tr("Add route"))
                                          : std::string(literouter::i18n::tr("Save route")),
                         true, [] { appState().applyRouteEditor(); });
        })
        .build();
}

inline void composeHopEditor(eui::Ui& ui, const eui::Screen& screen) {
    AppState& state = appState();
    const Palette& p = palette();
    HopEditor& hop = state.hop;
    if (!hop.open) {
        return;
    }
    const literouter::AppConfig& config = state.store.config();
    std::vector<std::string> providerIds;
    for (const auto& provider : config.providers) {
        providerIds.push_back(provider.id.empty() ? "(unnamed)" : provider.id);
    }
    const std::string routeModel =
        hop.routeIndex >= 0 && hop.routeIndex < static_cast<int>(config.routes.size())
            ? config.routes[static_cast<std::size_t>(hop.routeIndex)].model
            : std::string{};

    const bool isEdit = hop.hopIndex >= 0;
    const literouter::ProviderConfig* currentProvider =
        (hop.providerIndex >= 0 && hop.providerIndex < static_cast<int>(config.providers.size()))
            ? &config.providers[static_cast<std::size_t>(hop.providerIndex)]
            : nullptr;

    const bool hasChips = currentProvider != nullptr && !currentProvider->models.empty();
    const float panelWidth = 560.0f;
    const float panelHeight = hasChips ? 418.0f : 368.0f;

    components::dialog(ui, "overlays.hop")
        .open(hop.open)
        .screen(screen.width, screen.height)
        .size(panelWidth, panelHeight)
        .theme(uiTokens())
        .transition(motion())
        .onOpenChange([&hop](bool open) { hop.open = open; })
        .content([&] {
            ui.text("overlays.hop.title")
                .position(28.0f, 20.0f)
                .size(panelWidth - 56.0f, 26.0f)
                .text(isEdit ? literouter::i18n::tr("Edit hop") : literouter::i18n::tr("Add hop"))
                .fontSize(19.0f)
                .lineHeight(24.0f)
                .fontWeight(760)
                .color(p.text)
                .build();
            ui.text("overlays.hop.sub")
                .position(28.0f, 46.0f)
                .size(panelWidth - 56.0f, 20.0f)
                .text(isEdit
                          ? (std::string(literouter::i18n::tr("Configure relay and model for route: ")) +
                             (routeModel.empty() ? std::string(literouter::i18n::tr("this route")) : routeModel))
                          : (std::string(literouter::i18n::tr("Append failover relay for route: ")) +
                             (routeModel.empty() ? std::string(literouter::i18n::tr("this route")) : routeModel)))
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(p.textMuted)
                .build();

            fieldLabel(ui, "overlays.hop.provider.caption", 28.0f, 84.0f, panelWidth - 56.0f,
                       std::string(literouter::i18n::tr("Relay")));
            if (hop.providerOpen) {
                ui.rect("overlays.hop.provider.backdrop")
                    .position(0.0f, 0.0f)
                    .size(panelWidth, panelHeight)
                    .color(transparent())
                    .zIndex(150)
                    .onClick([&hop] { hop.providerOpen = false; })
                    .build();
            }
            ui.stack("overlays.hop.provider.wrap")
                .position(28.0f, 106.0f)
                .size(panelWidth - 56.0f, 38.0f)
                .zIndex(hop.providerOpen ? 200 : 1)
                .content([&] {
                    components::dropdown(ui, "overlays.hop.provider")
                        .theme(uiTokens())
                        .size(panelWidth - 56.0f, 38.0f)
                        .items(providerIds)
                        .selected(hop.providerIndex)
                        .open(hop.providerOpen)
                        .zIndex(hop.providerOpen ? 200 : 1)
                        .placeholder(providerIds.empty() ? std::string(literouter::i18n::tr("No relays configured"))
                                                         : std::string(literouter::i18n::tr("Select")))
                        .onChange([&hop](int value) {
                            hop.providerIndex = value;
                            hop.providerOpen = false;
                        })
                        .onOpenChange([&hop](bool open) { hop.providerOpen = open; })
                        .build();
                })
                .build();

            detail::labelledInput(ui, "overlays.hop.model", 28.0f, 160.0f, panelWidth - 56.0f,
                                  std::string(literouter::i18n::tr("Upstream model")),
                                  {std::string(literouter::i18n::tr("Leave empty to use client model name"))},
                                  hop.model, std::string(literouter::i18n::tr("e.g. gpt-4o-2024-11-20")),
                                  [&hop](const std::string& value) { hop.model = value; });

            float nextY = 244.0f;
            if (hasChips) {
                ui.text("overlays.hop.chips.label")
                    .position(28.0f, nextY)
                    .size(panelWidth - 56.0f, 16.0f)
                    .text(std::string(literouter::i18n::tr("Quick select model")) + ":")
                    .fontSize(11.0f)
                    .lineHeight(14.0f)
                    .color(p.textFaint)
                    .build();
                nextY += 20.0f;

                float chipX = 28.0f;
                const std::size_t maxChips = std::min<std::size_t>(currentProvider->models.size(), 4);
                for (std::size_t m = 0; m < maxChips; ++m) {
                    const std::string& mName = currentProvider->models[m];
                    const float mWidth = std::clamp(measureText(mName, 11.0f) + 18.0f, 56.0f, 120.0f);
                    if (chipX + mWidth > panelWidth - 28.0f) break;
                    actionButton(ui, "overlays.hop.chip." + std::to_string(m), chipX, nextY,
                                 mWidth, 24.0f, mName, false, [&hop, mName] {
                                     hop.model = mName;
                                 });
                    chipX += mWidth + 8.0f;
                }
                nextY += 32.0f;
            }

            ui.text("overlays.hop.status")
                .position(28.0f, nextY)
                .size(panelWidth - 56.0f, 18.0f)
                .text(hop.statusLine.empty()
                          ? (providerIds.empty()
                                 ? std::string(literouter::i18n::tr("Add a relay on the Providers page before adding a hop."))
                                 : (isEdit ? std::string(literouter::i18n::tr("Modify the upstream relay or model mapping for this hop."))
                                           : std::string(literouter::i18n::tr("The hop is appended at the end; reorder it with < and > on the route card."))))
                          : hop.statusLine)
                .fontSize(11.5f)
                .lineHeight(16.0f)
                .color(hop.statusError ? p.danger : p.textFaint)
                .build();

            const float btnY = nextY + 28.0f;
            actionButton(ui, "overlays.hop.cancel", panelWidth - 28.0f - 96.0f - 12.0f - 120.0f,
                         btnY, 96.0f, 40.0f, std::string(literouter::i18n::tr("Cancel")), false, [&hop] { hop.open = false; });
            actionButton(ui, "overlays.hop.add", panelWidth - 28.0f - 120.0f, btnY, 120.0f, 40.0f,
                         isEdit ? std::string(literouter::i18n::tr("Save hop"))
                                : std::string(literouter::i18n::tr("Add hop")),
                         true, [] { appState().addHopFromEditor(); },
                         providerIds.empty());
        })
        .build();
}

inline void composeConfirmDialog(eui::Ui& ui, const eui::Screen& screen) {
    AppState& state = appState();
    ConfirmState& confirm = state.confirm;
    if (!confirm.open) {
        return;
    }

    components::dialog(ui, "overlays.confirm")
        .open(confirm.open)
        .screen(screen.width, screen.height)
        .size(480.0f, 236.0f)
        .theme(uiTokens())
        .transition(motion())
        .title(std::string(literouter::i18n::tr(confirm.title)))
        .message(std::string(literouter::i18n::tr(confirm.message)))
        .primaryText(std::string(literouter::i18n::tr(confirm.primary.empty() ? "Confirm" : confirm.primary)))
        .secondaryText(std::string(literouter::i18n::tr("Cancel")))
        .onPrimary([] { appState().confirmAccepted(); })
        .onSecondary([] {
            appState().confirm.open = false;
            appState().confirm.kind = ConfirmKind::None;
        })
        .onOpenChange([&confirm](bool open) {
            confirm.open = open;
            if (!open) {
                confirm.kind = ConfirmKind::None;
            }
        })
        .build();
}

inline void composeLogDetail(eui::Ui& ui, const eui::Screen& screen) {
    AppState& state = appState();
    const Palette& p = palette();
    LogsView& view = state.logsView;
    if (!view.detailOpen) {
        return;
    }
    const literouter::LogEntry& entry = view.detail;

    constexpr float panelWidth = 900.0f;
    constexpr float panelHeight = 560.0f;

    components::dialog(ui, "overlays.logdetail")
        .open(view.detailOpen)
        .screen(screen.width, screen.height)
        .size(panelWidth, panelHeight)
        .theme(uiTokens())
        .transition(motion())
        .onOpenChange([&view](bool open) { view.detailOpen = open; })
        .content([&] {
            ui.text("overlays.logdetail.title")
                .position(28.0f, 20.0f)
                .size(panelWidth - 56.0f, 26.0f)
                .text(std::string(literouter::i18n::tr("Log entry #")) + std::to_string(entry.seq))
                .fontSize(19.0f)
                .lineHeight(24.0f)
                .fontWeight(760)
                .color(p.text)
                .build();
            ui.text("overlays.logdetail.sub")
                .position(28.0f, 46.0f)
                .size(panelWidth - 56.0f, 20.0f)
                .text(entry.timeText() + " · " +
                      (entry.request_id.empty() ? std::string(literouter::i18n::tr("no request id"))
                                                : std::string(literouter::i18n::tr("request ")) + entry.request_id))
                .fontSize(12.0f)
                .lineHeight(16.0f)
                .color(p.textMuted)
                .build();

            const float colWidth = (panelWidth - 56.0f - 24.0f) / 3.0f;
            const float rowY0 = 88.0f;
            const float rowGap = 44.0f;
            detail::keyValue(ui, "overlays.logdetail.level", 28.0f, rowY0, colWidth,
                             std::string(literouter::i18n::tr("LEVEL")),
                             entry.level, levelColor(entry.level));
            detail::keyValue(ui, "overlays.logdetail.kind", 28.0f + colWidth + 12.0f, rowY0,
                             colWidth, std::string(literouter::i18n::tr("KIND")), entry.kind, p.text);
            detail::keyValue(ui, "overlays.logdetail.status", 28.0f + (colWidth + 12.0f) * 2.0f,
                             rowY0, colWidth, std::string(literouter::i18n::tr("STATUS")),
                             entry.status == 0 ? "—" : std::to_string(entry.status),
                             entry.status >= 200 && entry.status < 300 ? p.success : p.danger);

            detail::keyValue(ui, "overlays.logdetail.model", 28.0f, rowY0 + rowGap, colWidth,
                             std::string(literouter::i18n::tr("MODEL (AS ASKED)")), entry.model, p.text);
            detail::keyValue(ui, "overlays.logdetail.relay", 28.0f + colWidth + 12.0f,
                             rowY0 + rowGap, colWidth, std::string(literouter::i18n::tr("RELAY")), entry.provider, p.text);
            detail::keyValue(ui, "overlays.logdetail.upstream", 28.0f + (colWidth + 12.0f) * 2.0f,
                             rowY0 + rowGap, colWidth, std::string(literouter::i18n::tr("UPSTREAM MODEL")), entry.upstream_model,
                             p.text);

            detail::keyValue(ui, "overlays.logdetail.latency", 28.0f, rowY0 + rowGap * 2.0f,
                             colWidth, std::string(literouter::i18n::tr("LATENCY")), literouter::humanMillis(entry.latency_ms), p.text);
            detail::keyValue(ui, "overlays.logdetail.bytes", 28.0f + colWidth + 12.0f,
                             rowY0 + rowGap * 2.0f, colWidth, std::string(literouter::i18n::tr("BYTES")),
                             literouter::humanBytes(entry.bytes), p.text);
            detail::keyValue(ui, "overlays.logdetail.attempt", 28.0f + (colWidth + 12.0f) * 2.0f,
                             rowY0 + rowGap * 2.0f, colWidth, std::string(literouter::i18n::tr("ATTEMPT")),
                             std::to_string(entry.attempt) + " / " +
                                 std::to_string(entry.attempts_total) +
                                 (entry.failover ? std::string(" · ") + std::string(literouter::i18n::tr("after failover")) : "") +
                                 (entry.stream ? std::string(" · ") + std::string(literouter::i18n::tr("streamed")) : ""),
                             entry.failover ? p.warn : p.text);

            const float messageY = rowY0 + rowGap * 2.0f + 40.0f;
            fieldLabel(ui, "overlays.logdetail.message.label", 28.0f, messageY, panelWidth - 56.0f,
                       std::string(literouter::i18n::tr("MESSAGE")));
            ui.rect("overlays.logdetail.message.bg")
                .position(28.0f, messageY + 18.0f)
                .size(panelWidth - 56.0f, 56.0f)
                .radius(p.radiusControl)
                .color(p.surfaceSunken)
                .border(1.0f, withAlpha(p.border, 0.7f))
                .build();
            ui.text("overlays.logdetail.message")
                .position(38.0f, messageY + 18.0f)
                .size(panelWidth - 76.0f, 56.0f)
                .text(entry.message.empty() ? std::string(literouter::i18n::tr("(no message)")) : entry.message)
                .fontSize(12.5f)
                .lineHeight(18.0f)
                .wrap(true)
                .maxWidth(panelWidth - 76.0f)
                .color(p.text)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            const float bodyY = messageY + 88.0f;
            const bool hasBodies = !entry.request_body.empty() || !entry.response_body.empty();
            fieldLabel(ui, "overlays.logdetail.body.label", 28.0f, bodyY, panelWidth - 56.0f,
                       hasBodies ? std::string(literouter::i18n::tr("REQUEST / RESPONSE BODY"))
                                 : std::string(literouter::i18n::tr("BODIES")));
            ui.rect("overlays.logdetail.body.bg")
                .position(28.0f, bodyY + 18.0f)
                .size(panelWidth - 56.0f, 108.0f)
                .radius(p.radiusControl)
                .color(p.surfaceSunken)
                .border(1.0f, withAlpha(p.border, 0.7f))
                .build();
            const std::string bodyText =
                hasBodies ? (entry.request_body + (entry.response_body.empty() ? "" : "\n— — —\n") +
                             entry.response_body)
                          : std::string(literouter::i18n::tr("Body retention is off (server.log_bodies). Turn it on in Settings to "
                                        "inspect prompts and responses here."));
            ui.text("overlays.logdetail.body")
                .position(38.0f, bodyY + 18.0f)
                .size(panelWidth - 76.0f, 108.0f)
                .text(literouter::truncateUtf8(bodyText, 1200))
                .fontSize(11.5f)
                .lineHeight(16.0f)
                .wrap(true)
                .maxWidth(panelWidth - 76.0f)
                .color(hasBodies ? p.textMuted : p.textFaint)
                .verticalAlign(eui::VerticalAlign::Top)
                .build();

            actionButton(ui, "overlays.logdetail.close", panelWidth - 28.0f - 110.0f, 498.0f, 110.0f,
                         40.0f, std::string(literouter::i18n::tr("Close")), false, [&view] { view.detailOpen = false; });
        })
        .build();
}

inline void composeOverlays(eui::Ui& ui, const eui::Screen& screen) {
    AppState& state = appState();

    composeProviderEditor(ui, screen);
    composeRouteEditor(ui, screen);
    composeHopEditor(ui, screen);
    composeConfirmDialog(ui, screen);
    composeLogDetail(ui, screen);

    components::toast(ui, "overlays.toast")
        .theme(uiTokens())
        .visible(state.toast.visible)
        .screen(screen.width, screen.height)
        .size(400.0f, 84.0f)
        .title(std::string(literouter::i18n::tr(state.toast.title)))
        .message(std::string(literouter::i18n::tr(state.toast.message)))
        .icon("")
        .duration(3.4f)
        .zIndex(2000)
        .transition(motion())
        .onAutoDismiss([] { appState().toast.visible = false; })
        .onDismiss([] { appState().toast.visible = false; })
        .build();
}

} // namespace lr_gui
