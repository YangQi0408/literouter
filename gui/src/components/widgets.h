#pragma once

#include "../app_state.h"
#include "theme.h"

// Small reusable pieces built from the DSL primitives. Every page composes
// through these so the console keeps one rhythm: same label style, same card
// padding, same pill geometry.
namespace lr_gui {

inline float measureText(const std::string& value, float fontSize, int fontWeight = 400) {
    return core::TextPrimitive::measureTextWidth(value, {}, fontSize, fontWeight);
}

inline void dot(eui::Ui& ui, const std::string& id, float x, float y, float size,
                const eui::Color& color) {
    ui.rect(id)
        .position(x, y)
        .size(size, size)
        .radius(size * 0.5f)
        .color(color)
        .transition(motion())
        .animate(eui::AnimProperty::Color)
        .build();
}

inline void divider(eui::Ui& ui, const std::string& id, float x, float y, float width) {
    ui.rect(id)
        .position(x, y)
        .size(width, 1.0f)
        .color(withAlpha(palette().border, 0.7f))
        .build();
}

// A one-line label in the console's uppercase eyebrow style.
inline void eyebrow(eui::Ui& ui, const std::string& id, float x, float y, float width,
                    const std::string& text, const eui::Color& color) {
    ui.text(id)
        .position(x, y)
        .size(width, 16.0f)
        .text(text)
        .fontSize(11.0f)
        .lineHeight(14.0f)
        .fontWeight(650)
        .color(color)
        .build();
}

inline void sectionTitle(eui::Ui& ui, const std::string& id, float x, float y, float width,
                         const std::string& text, const std::string& sub = {}) {
    ui.text(id)
        .position(x, y)
        .size(width, 26.0f)
        .text(text)
        .fontSize(18.0f)
        .lineHeight(24.0f)
        .fontWeight(700)
        .color(palette().text)
        .build();
    if (!sub.empty()) {
        ui.text(id + ".sub")
            .position(x, y + 24.0f)
            .size(width, 20.0f)
            .text(sub)
            .fontSize(13.0f)
            .lineHeight(18.0f)
            .color(palette().textMuted)
            .build();
    }
}

// Rounded status chip with a dot and a word, e.g. "Healthy".
inline void statusPill(eui::Ui& ui, const std::string& id, float x, float y,
                       const eui::Color& color, const std::string& word) {
    const float width = measureText(word, 12.0f, 600) + 34.0f;
    const float height = 24.0f;
    ui.stack(id)
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .radius(height * 0.5f)
                .color(withAlpha(color, 0.14f))
                .border(1.0f, withAlpha(color, 0.34f))
                .transition(motion())
                .animate(eui::AnimProperty::Color | eui::AnimProperty::Border)
                .build();
            dot(ui, id + ".dot", 10.0f, height * 0.5f - 3.5f, 7.0f, color);
            ui.text(id + ".text")
                .position(23.0f, 0.0f)
                .size(width - 23.0f, height)
                .text(word)
                .fontSize(12.0f)
                .lineHeight(height)
                .fontWeight(600)
                .color(color)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

// Neutral chip for ids, priorities and other short metadata.
inline void chip(eui::Ui& ui, const std::string& id, float x, float y, const std::string& text,
                 const eui::Color& color, const eui::Color& fill,
                 float horizontalPadding = 10.0f) {
    const float width = measureText(text, 12.0f, 500) + horizontalPadding * 2.0f;
    const float height = 22.0f;
    ui.stack(id)
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .radius(height * 0.5f)
                .color(fill)
                .build();
            ui.text(id + ".text")
                .size(width, height)
                .text(text)
                .fontSize(12.0f)
                .lineHeight(height)
                .fontWeight(500)
                .color(color)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();
        })
        .build();
}

// Metric tile: large number, muted label, thin accent underline.
inline void metricTile(eui::Ui& ui, const std::string& id, float x, float y, float width,
                       float height, const std::string& label, const std::string& value,
                       const eui::Color& valueColor, const eui::Color& underline) {
    ui.stack(id)
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .radius(palette().radiusCard)
                .color(palette().surface)
                .border(1.0f, withAlpha(palette().border, 0.8f))
                .build();
            eyebrow(ui, id + ".label", 16.0f, 14.0f, width - 32.0f, label, palette().textMuted);
            ui.text(id + ".value")
                .position(16.0f, 34.0f)
                .size(width - 32.0f, 36.0f)
                .text(value)
                .fontSize(27.0f)
                .lineHeight(34.0f)
                .fontWeight(760)
                .color(valueColor)
                .build();
            ui.rect(id + ".underline")
                .position(16.0f, height - 13.0f)
                .size(std::min(width - 32.0f, 34.0f), 2.0f)
                .radius(1.0f)
                .color(underline)
                .build();
        })
        .build();
}

// A rectangular metric used inside cards: label above, value below.
inline void fieldValue(eui::Ui& ui, const std::string& id, float x, float y, float width,
                       const std::string& label, const std::string& value,
                       const eui::Color& valueColor) {
    eyebrow(ui, id + ".label", x, y, width, label, palette().textFaint);
    ui.text(id + ".value")
        .position(x, y + 16.0f)
        .size(width, 20.0f)
        .text(value)
        .fontSize(14.0f)
        .lineHeight(19.0f)
        .fontWeight(600)
        .color(valueColor)
        .build();
}

inline void fieldLabel(eui::Ui& ui, const std::string& id, float x, float y, float width,
                       const std::string& text, const std::string& hint = {}) {
    ui.text(id)
        .position(x, y)
        .size(width, 18.0f)
        .text(text)
        .fontSize(12.0f)
        .lineHeight(16.0f)
        .fontWeight(600)
        .color(palette().textMuted)
        .build();
    if (!hint.empty()) {
        ui.text(id + ".hint")
            .position(x, y + 17.0f)
            .size(width, 16.0f)
            .text(hint)
            .fontSize(11.0f)
            .lineHeight(15.0f)
            .color(palette().textFaint)
            .build();
    }
}

inline void emptyState(eui::Ui& ui, const std::string& id, float x, float y, float width,
                       float height, const std::string& title, const std::string& body,
                       const std::string& actionLabel, std::function<void()> onAction) {
    ui.stack(id)
        .position(x, y)
        .size(width, height)
        .content([&] {
            ui.rect(id + ".bg")
                .size(width, height)
                .radius(palette().radiusCard)
                .color(withAlpha(palette().surface, 0.55f))
                .border(1.0f, withAlpha(palette().border, 0.7f))
                .build();

            const float centerX = width * 0.5f;
            ui.rect(id + ".mark")
                .position(centerX - 21.0f, 34.0f)
                .size(42.0f, 42.0f)
                .radius(12.0f)
                .color(withAlpha(palette().accent, 0.16f))
                .border(1.0f, withAlpha(palette().accent, 0.4f))
                .build();
            ui.text(id + ".mark.text")
                .position(centerX - 21.0f, 34.0f)
                .size(42.0f, 42.0f)
                .text("LR")
                .fontSize(14.0f)
                .lineHeight(42.0f)
                .fontWeight(800)
                .color(palette().accent)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .verticalAlign(eui::VerticalAlign::Center)
                .build();

            ui.text(id + ".title")
                .position(24.0f, 92.0f)
                .size(width - 48.0f, 24.0f)
                .text(title)
                .fontSize(17.0f)
                .lineHeight(22.0f)
                .fontWeight(700)
                .color(palette().text)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();
            ui.text(id + ".body")
                .position(24.0f, 120.0f)
                .size(width - 48.0f, 44.0f)
                .text(body)
                .fontSize(13.0f)
                .lineHeight(19.0f)
                .maxWidth(width - 48.0f)
                .wrap(true)
                .color(palette().textMuted)
                .horizontalAlign(eui::HorizontalAlign::Center)
                .build();

            if (!actionLabel.empty() && onAction) {
                const float buttonWidth = std::min(190.0f, width - 48.0f);
                ui.stack(id + ".action")
                    .position(centerX - buttonWidth * 0.5f, height - 58.0f)
                    .size(buttonWidth, 38.0f)
                    .content([&] {
                        components::button(ui, id + ".action.button")
                            .theme(uiTokens(), true)
                            .size(buttonWidth, 38.0f)
                            .fontSize(14.0f)
                            .text(actionLabel)
                            .radius(palette().radiusControl)
                            .transition(motion())
                            .onClick(std::move(onAction))
                            .build();
                    })
                    .build();
            }
        })
        .build();
}

// The console's standard button. primary = accent fill, otherwise a quiet
// bordered surface.
inline void actionButton(eui::Ui& ui, const std::string& id, float x, float y, float width,
                         float height, const std::string& label, bool primary,
                         std::function<void()> onClick, bool disabled = false) {
    ui.stack(id + ".wrap")
        .position(x, y)
        .size(width, height)
        .content([&] {
            components::button(ui, id)
                .theme(uiTokens(), primary)
                .size(width, height)
                .text(label)
                .fontSize(13.0f)
                .radius(palette().radiusControl)
                .disabled(disabled)
                .transition(motion())
                .onClick(std::move(onClick))
                .build();
        })
        .build();
}

} // namespace lr_gui
