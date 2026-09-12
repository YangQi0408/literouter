#pragma once

#include <eui_neo.h>

#include <string>

// Design tokens for the literouter console. One dark operations palette, one
// spacing scale, one motion curve — the pages read these and nothing else, so a
// colour or radius change happens in exactly one place.
namespace lr_gui {

struct Palette {
    eui::Color canvas{0.055f, 0.063f, 0.080f, 1.0f};
    eui::Color sidebar{0.071f, 0.080f, 0.099f, 1.0f};
    eui::Color surface{0.098f, 0.110f, 0.133f, 1.0f};
    eui::Color surfaceHover{0.129f, 0.145f, 0.176f, 1.0f};
    eui::Color surfaceActive{0.157f, 0.176f, 0.212f, 1.0f};
    eui::Color surfaceSunken{0.043f, 0.051f, 0.067f, 1.0f};
    eui::Color border{0.180f, 0.202f, 0.247f, 1.0f};
    eui::Color borderSoft{0.137f, 0.157f, 0.196f, 1.0f};
    eui::Color text{0.914f, 0.933f, 0.961f, 1.0f};
    eui::Color textMuted{0.565f, 0.604f, 0.678f, 1.0f};
    eui::Color textFaint{0.392f, 0.427f, 0.494f, 1.0f};
    eui::Color accent{0.333f, 0.545f, 0.980f, 1.0f};
    eui::Color accentSoft{0.196f, 0.290f, 0.502f, 1.0f};
    eui::Color success{0.290f, 0.780f, 0.520f, 1.0f};
    eui::Color warn{0.949f, 0.706f, 0.286f, 1.0f};
    eui::Color danger{0.937f, 0.376f, 0.416f, 1.0f};
    eui::Color info{0.451f, 0.671f, 0.949f, 1.0f};

    float radiusCard = 12.0f;
    float radiusControl = 8.0f;
    float radiusPill = 999.0f;
};

inline const Palette& palette() {
    static const Palette value;
    return value;
}

// Spacing scale — 4 / 8 / 12 / 16 / 22 / 28.
namespace space {
inline constexpr float xs = 4.0f;
inline constexpr float sm = 8.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 16.0f;
inline constexpr float xl = 22.0f;
inline constexpr float xxl = 28.0f;
} // namespace space

namespace layout {
inline constexpr float sidebarWidth = 232.0f;
inline constexpr float topBarHeight = 66.0f;
inline constexpr float pagePadding = 26.0f;
inline constexpr float scrollStep = 52.0f;
} // namespace layout

// Fully transparent fill for hit rects that should not paint.
inline const eui::Color& transparent() {
    static const eui::Color value{0.0f, 0.0f, 0.0f, 0.0f};
    return value;
}

inline eui::Transition motion() {
    return eui::Transition::make(0.18f, eui::Ease::OutCubic);
}

inline eui::Transition fastMotion() {
    return eui::Transition::make(0.12f, eui::Ease::OutCubic);
}

inline eui::Color withAlpha(const eui::Color& color, float alpha) {
    eui::Color out = color;
    out.a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    return out;
}

inline eui::Color mix(const eui::Color& from, const eui::Color& to, float amount) {
    return eui::mixColor(from, to, amount);
}

// Built-in components (button / input / dropdown / dialog / toast / scrollView)
// take the framework's own token bag; this projects our palette onto it so they
// sit in the same visual system as the hand-built surfaces.
inline components::theme::ThemeColorTokens uiTokens() {
    auto tokens = components::theme::dark();
    tokens.background = palette().canvas;
    tokens.surface = palette().surface;
    tokens.surfaceHover = palette().surfaceHover;
    tokens.surfaceActive = palette().surfaceActive;
    tokens.text = palette().text;
    tokens.border = palette().border;
    tokens.primary = palette().accent;
    return tokens;
}

// Semantic colour for a relay health state. Used by the status dots, the relay
// cards and the sidebar strip.
inline eui::Color healthColor(const std::string& state) {
    if (state == "Healthy") {
        return palette().success;
    }
    if (state == "Degraded") {
        return palette().warn;
    }
    if (state == "Open") {
        return palette().danger;
    }
    return palette().textFaint;
}

// Success-rate banding: green at or above 98%, amber from 90%, red below.
inline eui::Color successBand(double rate) {
    if (rate >= 0.98) {
        return palette().success;
    }
    if (rate >= 0.90) {
        return palette().warn;
    }
    return palette().danger;
}

inline eui::Color levelColor(const std::string& level) {
    if (level == "error") {
        return palette().danger;
    }
    if (level == "warn") {
        return palette().warn;
    }
    return palette().textMuted;
}

// Two-letter badge colour rotation, so adjacent rows are distinguishable
// without adding decoration.
inline eui::Color badgeColor(std::size_t index) {
    static const eui::Color colors[] = {
        {0.333f, 0.545f, 0.980f, 1.0f},
        {0.290f, 0.780f, 0.520f, 1.0f},
        {0.949f, 0.706f, 0.286f, 1.0f},
        {0.729f, 0.486f, 0.949f, 1.0f},
        {0.949f, 0.502f, 0.400f, 1.0f},
        {0.290f, 0.729f, 0.780f, 1.0f},
    };
    return colors[index % 6];
}

} // namespace lr_gui
