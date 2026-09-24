#include "cli_ui.hpp"

#include <cstdlib>
#include <iostream>
#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

import literouter.core;

namespace lrcli {

namespace {

bool g_configured = false;
bool g_color = false;

std::string pad(std::string_view text, std::size_t width, Align align) {
    const std::size_t visible = visibleWidth(text);
    const std::size_t fill = visible < width ? width - visible : 0;
    std::string out;
    out.reserve(text.size() + fill);
    if (align == Align::Right) {
        out.append(fill, ' ');
    }
    out.append(text);
    if (align == Align::Left) {
        out.append(fill, ' ');
    }
    return out;
}

std::string repeat(std::string_view unit, std::size_t times) {
    std::string out;
    out.reserve(unit.size() * times);
    for (std::size_t i = 0; i < times; ++i) {
        out += unit;
    }
    return out;
}

} // namespace

void configureColor(bool disabled) {
    g_configured = true;
#if defined(_WIN32)
    const bool is_a_tty = _isatty(_fileno(stdout)) != 0;
#else
    const bool is_a_tty = isatty(fileno(stdout)) != 0;
#endif
    if (disabled || std::getenv("NO_COLOR") != nullptr || !is_a_tty) {
        g_color = false;
    } else {
        g_color = true;
    }
}

bool colorEnabled() {
    if (!g_configured) {
        configureColor(false);
    }
    return g_color;
}

std::string paint(std::string_view text, std::string_view sgr) {
    if (!colorEnabled() || sgr.empty()) {
        return std::string{text};
    }
    std::string out;
    out.reserve(text.size() + sgr.size() + 8);
    out += "\x1b[";
    out += sgr;
    out += "m";
    out += text;
    out += "\x1b[0m";
    return out;
}

std::string bold(std::string_view text) { return paint(text, "1"); }
std::string dim(std::string_view text) { return paint(text, "2"); }

std::string colorHealth(std::string_view state, std::string_view text) {
    if (state == "healthy") {
        return paint(text, "32");
    }
    if (state == "degraded") {
        return paint(text, "33");
    }
    if (state == "open") {
        return paint(text, "31;1");
    }
    return dim(text);
}

std::string colorLevel(std::string_view level, std::string_view text) {
    if (level == "error") {
        return paint(text, "31;1");
    }
    if (level == "warning" || level == "warn") {
        return paint(text, "33");
    }
    if (level == "info") {
        return paint(text, "36");
    }
    return text.empty() ? std::string{} : std::string{text};
}

std::string colorStatus(int status, std::string_view text) {
    if (status >= 200 && status < 300) {
        return paint(text, "32");
    }
    if (status >= 300 && status < 400) {
        return paint(text, "36");
    }
    if (status >= 400) {
        return paint(text, "31");
    }
    return dim(text);
}

std::string colorEnabled(bool enabled, std::string_view text) {
    return enabled ? paint(text, "32") : dim(text);
}

std::string stripAnsi(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !(static_cast<unsigned char>(text[i]) >= 0x40 &&
                     static_cast<unsigned char>(text[i]) <= 0x7e)) {
                ++i;
            }
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::size_t visibleWidth(std::string_view text) {
    const std::string plain = stripAnsi(text);
    std::size_t width = 0;
    std::size_t i = 0;
    const std::size_t n = plain.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(plain[i]);
        char32_t cp = 0;
        std::size_t bytes = 1;
        if (c < 0x80) {
            cp = c;
            bytes = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(plain[i + 1]) & 0x3F);
            bytes = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(plain[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(plain[i + 2]) & 0x3F);
            bytes = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(plain[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(plain[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(plain[i + 3]) & 0x3F);
            bytes = 4;
        } else {
            ++i;
            continue;
        }
        i += bytes;

        // East Asian Wide / Fullwidth characters take 2 terminal columns
        if ((cp >= 0x1100 && cp <= 0x115F) ||
            (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE10 && cp <= 0xFE19) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x20000 && cp <= 0x3FFFD)) {
            width += 2;
        } else {
            width += 1;
        }
    }
    return width;
}

// ── Table ────────────────────────────────────────────────────────────────────

void Table::column(std::string header, Align align) {
    columns_.push_back(Column{std::move(header), align});
}

void Table::row(std::vector<std::string> cells) {
    cells.resize(columns_.size());
    rows_.push_back(std::move(cells));
}

void Table::print() const { print(std::cout); }

void Table::print(std::ostream &out) const {
    if (columns_.empty()) {
        return;
    }
    std::vector<std::size_t> widths;
    widths.reserve(columns_.size());
    for (std::size_t c = 0; c < columns_.size(); ++c) {
        widths.push_back(visibleWidth(bold(columns_[c].header)));
    }
    for (const auto &cols : rows_) {
        for (std::size_t c = 0; c < columns_.size(); ++c) {
            widths[c] = std::max(widths[c], visibleWidth(cols[c]));
        }
    }

    const auto border = [&](std::string_view left, std::string_view mid,
                            std::string_view right) {
        std::string line{left};
        for (std::size_t c = 0; c < columns_.size(); ++c) {
            if (c > 0) {
                line += mid;
            }
            line += repeat("─", widths[c] + 2);
        }
        line += right;
        out << line << "\n";
    };

    border("┌", "┬", "┐");
    out << "│";
    for (std::size_t c = 0; c < columns_.size(); ++c) {
        out << " " << pad(bold(columns_[c].header), widths[c], columns_[c].align) << " │";
    }
    out << "\n";
    border("├", "┼", "┤");
    for (const auto &cols : rows_) {
        out << "│";
        for (std::size_t c = 0; c < columns_.size(); ++c) {
            out << " " << pad(cols[c], widths[c], columns_[c].align) << " │";
        }
        out << "\n";
    }
    border("└", "┴", "┘");
}

// ── KeyValues ────────────────────────────────────────────────────────────────

void KeyValues::section(std::string title) {
    items_.push_back(Item{.isSection = true, .key = std::move(title), .value = {}});
}

void KeyValues::add(std::string key, std::string value) {
    items_.push_back(Item{.isSection = false, .key = std::move(key), .value = std::move(value)});
}

void KeyValues::print() const { print(std::cout); }

void KeyValues::print(std::ostream &out) const {
    std::size_t width = 0;
    for (const auto &item : items_) {
        if (!item.isSection) {
            width = std::max(width, visibleWidth(item.key));
        }
    }
    bool firstSection = true;
    for (const auto &item : items_) {
        if (item.isSection) {
            if (!firstSection) {
                out << "\n";
            }
            firstSection = false;
            out << bold(item.key) << "\n";
            continue;
        }
        std::string key{};
        key.append(item.key);
        key.append(width - visibleWidth(item.key), ' ');
        out << "  " << dim(key) << "  " << item.value << "\n";
    }
}

// ── diagnostics ──────────────────────────────────────────────────────────────

void printError(const std::string &message) {
    std::cerr << paint("literouter: error:", "31;1") << " " << message << "\n";
}

void printWarn(const std::string &message) {
    std::cerr << paint("literouter: warning:", "33;1") << " " << message << "\n";
}

void printHint(const std::string &message) {
    std::cerr << "  " << dim("hint: " + message) << "\n";
}

void printInfo(const std::string &message) { std::cout << message << "\n"; }

void printNote(const std::string &message, bool quiet) {
    if (!quiet) {
        std::cout << message << "\n";
    }
}

} // namespace lrcli
