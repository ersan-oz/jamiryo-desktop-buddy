#pragma once
#include "head_motion.h"
#include <cstddef>
#include <cstdint>

namespace object_view {
constexpr unsigned width = 320, height = 240, count = 3;
constexpr size_t panel_bytes = width * height * 2, panorama_bytes = panel_bytes * count;
// Panels are stacked top-to-bottom: 1 left, 2 center, 3 right. The
// within-panel correction is deliberately small: lens/shaft are uncalibrated.
constexpr int target(unsigned view, int x_milli, bool reverse)
{
    const int offset = (static_cast<int>(view) - 2) * 250 + (x_milli - 500) * 100 / 500;
    return std::clamp(1500 + (reverse ? -offset : offset), HeadMotion::minimum_us, HeadMotion::maximum_us);
}
constexpr bool valid(unsigned view, int x) { return view >= 1 && view <= count && x >= 0 && x <= 1000; }
constexpr bool evidence_current(unsigned expected, unsigned supplied, int64_t image_at, int64_t now, int64_t deadline)
{
    return expected && supplied == expected && image_at > 0 && now >= image_at
        && now - image_at <= 20000 && now < deadline;
}
constexpr bool copy_panel(uint8_t *out, size_t capacity, const uint8_t *in, size_t bytes, unsigned view)
{
    if (!out || !in || capacity < panorama_bytes || bytes < panel_bytes || !valid(view, 500)) return false;
    for (size_t i = 0; i < panel_bytes; ++i) out[(view - 1) * panel_bytes + i] = in[i];
    // A high-contrast numbered badge identifies the panel without changing
    // image geometry. RGB565 byte order is irrelevant for black/white pixels.
    constexpr uint16_t digits[] = {0b010110010010111, 0b111001111100111, 0b111001111001111};
    for (unsigned y = 0; y < 23; ++y) for (unsigned x = 0; x < 17; ++x) {
        const bool ink = x >= 4 && x < 13 && y >= 4 && y < 19
            && (digits[view - 1] & (1u << (14 - ((y - 4) / 3 * 3 + (x - 4) / 3))));
        const size_t offset = ((view - 1) * height * width + (y + 2) * width + x + 2) * 2;
        out[offset] = out[offset + 1] = ink ? 0xff : 0;
    }
    return true;
}
constexpr bool self_test()
{
    if (valid(0, 500) || valid(4, 500) || valid(2, -1) || valid(2, 1001)) return false;
    for (unsigned view = 1; view <= count; ++view) for (int x : {0, 250, 500, 750, 1000}) {
        const int normal = target(view, x, false), reversed = target(view, x, true);
        if (normal < 1250 || normal > 1750 || normal + reversed != 3000) return false;
        if (x < 1000 && target(view, x + 1, false) < normal) return false;
    }
    return target(1, 500, false) == 1250 && target(2, 500, false) == 1500
        && target(3, 500, false) == 1750 && target(2, 750, true) == 1450
        && evidence_current(7, 7, 1000, 20999, 21000)
        && !evidence_current(7, 8, 1000, 1100, 21000)
        && !evidence_current(0, 0, 1000, 1100, 21000)
        && !evidence_current(7, 7, 1000, 999, 21000)
        && !evidence_current(7, 7, 0, 1100, 21000)
        && !evidence_current(7, 7, 1000, 21000, 21000)
        && !evidence_current(7, 7, 1000, 21001, 30000)
        && !copy_panel(nullptr, panorama_bytes, nullptr, panel_bytes, 1);
}
static_assert(self_test(), "Object aiming must respect panel direction, reversed mounting and servo bounds");
} // namespace object_view
