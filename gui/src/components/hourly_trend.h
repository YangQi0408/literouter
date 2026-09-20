#pragma once

#include "../app_state.h"
#include "lr_theme.h"
#include "widgets.h"

namespace lr_gui {

inline constexpr float kHourlyTrendHeight = 238.0f;

/** A time offset in the unit that makes it readable: minutes under an hour,
 *  hours under a day, days beyond that. */
inline std::string trendOffset(double seconds) {
    if (seconds < 3600.0) {
        return std::to_string(static_cast<long long>(std::llround(seconds / 60.0))) + "m";
    }
    if (seconds < 86400.0) {
        return std::to_string(static_cast<long long>(std::llround(seconds / 3600.0))) + "h";
    }
    return std::to_string(static_cast<long long>(std::llround(seconds / 86400.0))) + "d";
}

inline void composeHourlyTrend(eui::Ui& ui, float x, float y, float width) {
    const Palette& p = palette();
    // The window is the operator's, not a constant: server.traffic_bucket_sec and
    // traffic_bucket_count decide it, so a relay being debugged can be watched
    // per minute instead of per hour. The bucket width is read from the snapshot
    // rather than from the config so a chart restored from a telemetry file
    // recorded at a different width is not relabelled.
    const literouter::Snapshot& snap = appState().snapshot;
    const double bucketSec = static_cast<double>(std::max(60, snap.traffic_bucket_sec));
    const std::size_t hours = static_cast<std::size_t>(
        std::clamp(snap.traffic_bucket_count > 0 ? snap.traffic_bucket_count : 24, 2, 240));
    const double currentHour = std::floor(literouter::nowUnix() / bucketSec) * bucketSec;
    const double firstHour = currentHour - static_cast<double>(hours - 1) * bucketSec;
    std::vector<literouter::TrafficBucket> buckets(hours);
    for (std::size_t i = 0; i < hours; ++i) {
        buckets[i].hour_unix = firstHour + static_cast<double>(i) * bucketSec;
        buckets[i].bucket_sec = static_cast<int>(bucketSec);
    }
    // The core retains nonempty buckets, which need not be consecutive. Keep
    // idle slots empty instead of compressing days of sparse traffic into the
    // window.
    for (const auto& bucket : snap.hourly) {
        if (!std::isfinite(bucket.hour_unix) || bucket.hour_unix < firstHour ||
            bucket.hour_unix > currentHour) {
            continue;
        }
        const auto index = static_cast<std::size_t>((bucket.hour_unix - firstHour) / bucketSec);
        if (index < buckets.size()) {
            buckets[index] = bucket;
        }
    }
    std::uint64_t peak = 0;
    for (const auto& bucket : buckets) {
        peak = std::max(peak, bucket.requests);
    }

    const float plotX = 18.0f;
    const float plotY = 92.0f;
    const float plotWidth = std::max(24.0f, width - 36.0f);
    const float plotHeight = 106.0f;
    const float slotWidth = plotWidth / static_cast<float>(hours);
    const float barWidth = std::max(1.0f, slotWidth - std::min(5.0f, slotWidth * 0.25f));

    ui.stack("overview.hourly")
        .position(x, y)
        .size(width, kHourlyTrendHeight)
        .content([&] {
            ui.rect("overview.hourly.bg")
                .size(width, kHourlyTrendHeight)
                .radius(p.radiusCard)
                .color(p.surface)
                .border(1.0f, withAlpha(p.border, 0.8f))
                .build();
            ui.text("overview.hourly.title")
                .position(18.0f, 14.0f)
                .size(std::max(80.0f, width - 170.0f), 22.0f)
                .text(literouter::i18n::tr("Hourly traffic"))
                .fontSize(15.0f).lineHeight(20.0f).fontWeight(720).color(p.text).build();
            ui.text("overview.hourly.peak")
                .position(width - 150.0f, 16.0f).size(132.0f, 18.0f)
                .text(std::string(literouter::i18n::tr("Peak")) + " " + literouter::humanCount(peak))
                .fontSize(11.5f).lineHeight(16.0f).color(p.textMuted)
                .horizontalAlign(eui::HorizontalAlign::Right).build();
            ui.text("overview.hourly.note")
                .position(18.0f, 39.0f).size(width - 36.0f, 18.0f)
                .text(literouter::i18n::tr("Requests per bucket, over the configured trend window"))
                .fontSize(12.0f).lineHeight(17.0f).color(p.textMuted).build();

            dot(ui, "overview.hourly.legend.requests", 18.0f, 66.0f, 7.0f, p.accent);
            const std::string requestsLabel = std::string(literouter::i18n::tr("Requests"));
            const float failureX = 38.0f + measureText(requestsLabel, 11.5f) + 18.0f;
            ui.text("overview.hourly.legend.requests.label")
                .position(31.0f, 60.0f).size(failureX - 36.0f, 20.0f).text(requestsLabel)
                .fontSize(11.5f).lineHeight(20.0f).color(p.textMuted).build();
            dot(ui, "overview.hourly.legend.failures", failureX, 66.0f, 7.0f, p.warn);
            ui.text("overview.hourly.legend.failures.label")
                .position(failureX + 13.0f, 60.0f).size(width - failureX - 31.0f, 20.0f)
                .text(literouter::i18n::tr("Hour with failures"))
                .fontSize(11.5f).lineHeight(20.0f).color(p.textMuted).build();

            for (int i = 0; i < 3; ++i) {
                divider(ui, "overview.hourly.grid." + std::to_string(i), plotX,
                        plotY + plotHeight * static_cast<float>(i) * 0.5f, plotWidth);
            }
            for (std::size_t i = 0; i < hours; ++i) {
                const auto& bucket = buckets[i];
                const std::string id = "overview.hourly.bar." + std::to_string(i);
                const float barX = plotX + static_cast<float>(i) * slotWidth + (slotWidth - barWidth) * 0.5f;
                const float barHeight = peak == 0 ? 0.0f :
                    static_cast<float>(static_cast<double>(bucket.requests) / static_cast<double>(peak)) * plotHeight;
                const eui::Color color = bucket.failures > 0 ? p.warn : p.accent;
                if (bucket.requests > 0) {
                    ui.rect(id + ".fill")
                        .position(barX, plotY + plotHeight - std::max(2.0f, barHeight))
                        .size(barWidth, std::max(2.0f, barHeight)).radius(3.0f)
                        .color(withAlpha(color, 0.72f)).build();
                }
                // A full-height hit region keeps even an idle or tiny hour
                // inspectable. Its tooltip contains absolute counts, not a
                // percentage of an automatically scaled axis.
                ui.rect(id)
                    .position(barX, plotY).size(barWidth, plotHeight)
                    .states(transparent(), withAlpha(color, 0.14f), withAlpha(color, 0.20f))
                    .instantStates().build();
            }

            if (peak == 0) {
                ui.text("overview.hourly.empty")
                    .position(26.0f, plotY + 37.0f).size(width - 52.0f, 28.0f)
                    .text(literouter::i18n::tr("No traffic in the configured trend window"))
                    .fontSize(13.0f).lineHeight(22.0f).color(p.textMuted)
                    .horizontalAlign(eui::HorizontalAlign::Center).build();
            }
            // Five ticks spread across the window, labelled in the bucket's own
            // unit: "−3h" for hour-wide buckets, "−30m" for minute-wide ones.
            for (std::size_t tick = 0; tick < 5; ++tick) {
                const std::size_t i = std::min(hours - 1, tick * (hours - 1) / 4);
                const std::string label = i == hours - 1
                    ? std::string(literouter::i18n::tr("Now"))
                    : "−" + trendOffset(static_cast<double>(hours - 1 - i) * bucketSec);
                const float labelWidth = 42.0f;
                const float labelX = std::clamp(plotX + (static_cast<float>(i) + 0.5f) * slotWidth - labelWidth * 0.5f,
                                               plotX, plotX + plotWidth - labelWidth);
                ui.text("overview.hourly.label." + std::to_string(tick))
                    .position(labelX, 206.0f).size(labelWidth, 18.0f).text(label)
                    .fontSize(11.0f).lineHeight(16.0f).color(p.textFaint)
                    .horizontalAlign(eui::HorizontalAlign::Center).build();
            }

            // EUI's generic tooltip is sized for a short percentage; the
            // request inspector needs a readable, bounded panel of metrics.
            for (std::size_t i = 0; i < hours; ++i) {
                const auto& bucket = buckets[i];
                const std::string id = "overview.hourly.bar." + std::to_string(i);
                const float tooltipWidth = std::min(264.0f, width - 36.0f);
                const float tooltipX = std::clamp(plotX + (static_cast<float>(i) + 0.5f) * slotWidth - tooltipWidth * 0.5f,
                                                 18.0f, width - 18.0f - tooltipWidth);
                literouter::LogEntry time;
                time.time_unix = bucket.hour_unix;
                const std::string heading = time.shortDateTimeText().substr(0, 11);
                const std::string lines[] = {
                    heading + " · " + std::string(literouter::i18n::tr("Local time")),
                    std::string(literouter::i18n::tr("REQUESTS")) + "  " + std::to_string(bucket.requests),
                    std::string(literouter::i18n::tr("SUCCESS")) + "  " + std::to_string(bucket.successes) +
                        "   " + std::string(literouter::i18n::tr("FAILURES")) + "  " + std::to_string(bucket.failures),
                    std::string(literouter::i18n::tr("TOKENS")) + "  " +
                        literouter::humanCount(bucket.tokens_prompt + bucket.tokens_completion),
                    std::string(literouter::i18n::tr("BYTES OUT")) + "  " + literouter::humanBytes(bucket.bytes_out),
                    std::string(literouter::i18n::tr("Estimated cost")) + "  " + std::format("${:.4f}", bucket.cost_usd),
                };
                ui.stack(id + ".tooltip")
                    .position(tooltipX, 47.0f).size(tooltipWidth, 148.0f)
                    .zIndex(10).hoverOpacityFrom(id)
                    .content([&] {
                        ui.rect(id + ".tooltip.bg").size(tooltipWidth, 148.0f)
                            .radius(p.radiusControl).color(p.surfaceSunken).border(1.0f, p.border).build();
                        for (std::size_t row = 0; row < std::size(lines); ++row) {
                            ui.text(id + ".tooltip.line." + std::to_string(row))
                                .position(12.0f, 9.0f + static_cast<float>(row) * 22.0f)
                                .size(tooltipWidth - 24.0f, 22.0f).text(lines[row])
                                .fontSize(11.5f).lineHeight(20.0f)
                                .color(row == 0 ? p.textMuted : p.text).build();
                        }
                    }).build();
            }
        }).build();
}

} // namespace lr_gui
