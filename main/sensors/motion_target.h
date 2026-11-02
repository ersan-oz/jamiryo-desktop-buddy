#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

struct MotionTarget {
    bool valid = false, broad_change = false;
    int x_milli = 0;
    unsigned cells = 0, strength = 0, changed_cells = 0, threshold = 0;
};

// Anonymous 32x24 luminance differences. Camera task owns this bounded workspace.
// Global exposure is removed by the median difference; disconnected single-pixel
// noise never forms a target. A target requires motion in TWO distinct intervals.
class BuddyMotionTarget {
public:
    static constexpr unsigned width = 32, height = 24, cells = width * height;
    using Grid = std::array<uint8_t, cells>;
    constexpr void reset() { ready_ = false; confirmations_ = 0; previous_at_ = -1; }
    constexpr MotionTarget update(const Grid &grid, int64_t captured_at)
    {
        if (previous_at_ >= 0 && captured_at <= previous_at_) return {};
        if (!ready_ || captured_at - previous_at_ > 1800) {
            previous_ = grid; previous_at_ = captured_at; ready_ = true; confirmations_ = 0; return {};
        }
        histogram_.fill(0);
        for (unsigned i = 0; i < cells; ++i) ++histogram_[int(grid[i]) - previous_[i] + 255];
        unsigned count = 0; int exposure = 0;
        for (unsigned i = 0; i < histogram_.size(); ++i) {
            count += histogram_[i];
            if (count >= cells / 2) { exposure = int(i) - 255; break; }
        }
        // Learn the residual noise floor after removing the exposure shift.
        // Low-contrast hands should not need the old fixed 18-level difference.
        histogram_.fill(0);
        for (unsigned i = 0; i < cells; ++i) {
            const int delta = int(grid[i]) - previous_[i] - exposure;
            ++histogram_[delta < 0 ? -delta : delta];
        }
        count = 0; unsigned noise = 0;
        for (unsigned i = 0; i < histogram_.size(); ++i) {
            count += histogram_[i];
            if (count >= cells / 2) { noise = i; break; }
        }
        const unsigned threshold = std::clamp(noise * 2 + 6, 10u, 40u);
        unsigned changed = 0;
        for (unsigned i = 0; i < cells; ++i) {
            const int delta = int(grid[i]) - previous_[i] - exposure;
            const int magnitude = delta < 0 ? -delta : delta;
            mask_[i] = unsigned(magnitude) >= threshold ? static_cast<uint8_t>(std::min(255, magnitude)) : 0;
            changed += mask_[i] != 0;
        }
        previous_ = grid; previous_at_ = captured_at;
        // Large camera/room-wide changes are not a localized moving object.
        if (changed > cells * 45 / 100) { confirmations_ = 0; return {false, true, 0, 0, 0, changed, threshold}; }
        struct Region { unsigned area = 0, weight = 0, weighted_x = 0; int x = 0; } best, second;
        for (unsigned start = 0; start < cells; ++start) {
            if (!mask_[start]) continue;
            unsigned read = 0, written = 0; queue_[written++] = start;
            Region region;
            const auto consume = [&](unsigned index) constexpr {
                const unsigned weight = mask_[index];
                ++region.area; region.weight += weight; region.weighted_x += weight * (index % width);
                mask_[index] = 0;
            };
            consume(start);
            while (read < written) {
                const unsigned cell = queue_[read++]; const int x = cell % width, y = cell / width;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= int(width) || ny >= int(height)) continue;
                    const unsigned index = ny * width + nx;
                    if (!mask_[index]) continue;
                    consume(index); queue_[written++] = index;
                }
            }
            if (region.area < 6) continue;
            region.x = int((region.weighted_x * 2000u / region.weight + 1000u) / width) - 1000;
            if (region.weight > best.weight) { second = best; best = region; }
            else if (region.weight > second.weight) second = region;
        }
        const int separation = best.x > second.x ? best.x - second.x : second.x - best.x;
        if (!best.area || (second.area && second.weight * 4 >= best.weight * 3 && separation > 900)) {
            confirmations_ = 0; return {false, false, 0, 0, 0, changed, threshold};
        }
        const int displacement = best.x > candidate_x_ ? best.x - candidate_x_ : candidate_x_ - best.x;
        if (!confirmations_ || captured_at - candidate_at_ > 1600 || displacement > 650) confirmations_ = 1;
        else confirmations_ = 2;
        candidate_x_ = best.x; candidate_at_ = captured_at;
        return {confirmations_ >= 2, false, best.x, best.area, best.weight / best.area, changed, threshold};
    }
private:
    Grid previous_{}, mask_{};
    std::array<uint16_t, cells> queue_{};
    std::array<uint16_t, 511> histogram_{};
    bool ready_ = false;
    unsigned confirmations_ = 0;
    int candidate_x_ = 0;
    int64_t previous_at_ = -1, candidate_at_ = 0;
};

constexpr bool motion_target_self_test()
{
    BuddyMotionTarget detector; BuddyMotionTarget::Grid grid{}; grid.fill(80);
    if (detector.update(grid, 0).valid) return false;
    grid.fill(120); if (detector.update(grid, 400).valid) return false;
    // A single appearance then a still frame cannot confirm motion.
    for (unsigned y = 8; y < 16; ++y) for (unsigned x = 23; x < 28; ++x) grid[y*32+x] = 200;
    if (detector.update(grid, 800).valid || detector.update(grid, 1200).valid) return false;
    detector.reset(); grid.fill(80); detector.update(grid, 2000);
    for (unsigned step = 0; step < 3; ++step) {
        grid.fill(80);
        for (unsigned y = 7; y < 17; ++y) for (unsigned x = 22 + step; x < 27 + step; ++x) grid[y*32+x] = 180;
        const auto target = detector.update(grid, 2400 + step*400);
        if ((step == 0 && target.valid) || (step > 0 && (!target.valid || target.x_milli < 250))) return false;
        if (detector.update(grid, 2400 + step*400).valid) return false; // Duplicate frame.
    }
    detector.reset(); grid.fill(80); detector.update(grid, 4000);
    for (unsigned step = 0; step < 3; ++step) {
        grid.fill(80);
        for (unsigned y = 7; y < 17; ++y) for (unsigned x = 3 + step; x < 8 + step; ++x) grid[y*32+x] = 180;
        const auto target = detector.update(grid, 4400 + step*400);
        if (step > 0 && (!target.valid || target.x_milli > -250)) return false;
    }
    detector.reset(); grid.fill(100); detector.update(grid, 6000);
    for (unsigned step = 0; step < 4; ++step) {
        grid.fill(100);
        for (unsigned i = step; i < grid.size(); i += 13) grid[i] = 150;
        if (detector.update(grid, 6400+step*400).valid) return false;
    }
    detector.reset(); grid.fill(100); detector.update(grid, 9000);
    for (unsigned i = 0; i < grid.size(); ++i) grid[i] = i%2 ? 180 : 20;
    const auto broad = detector.update(grid, 9400);
    if (broad.valid || !broad.broad_change) return false;
    // Pauses, manual commands and camera moves re-arm from a fresh baseline.
    detector.reset();
    return !detector.update(grid, 10000).valid && !detector.update(grid, 13000).valid;
}
constexpr bool motion_low_contrast_test()
{
    BuddyMotionTarget detector; BuddyMotionTarget::Grid grid{}; grid.fill(80);
    detector.update(grid, 0);
    // Six connected edge cells with only 13/255 contrast were invisible to 1.2.18.
    for (unsigned step = 0; step < 3; ++step) {
        grid.fill(80);
        for (unsigned y = 8; y < 14; ++y) for (unsigned x = 22 + step; x < 26 + step; ++x) grid[y*32+x] = 93;
        const auto target = detector.update(grid, 300 + step*300);
        if (target.threshold != 10 || (step > 0 && (!target.valid || target.x_milli < 250))) return false;
    }
    detector.reset(); grid.fill(80); detector.update(grid, 2000);
    for (unsigned step = 0; step < 4; ++step) {
        for (unsigned i = 0; i < grid.size(); ++i) grid[i] = 80 + ((i+step)%9) - 4;
        if (detector.update(grid, 2300+step*300).valid) return false;
    }
    return true;
}
inline constexpr bool motion_detector_self_test() { return motion_target_self_test() && motion_low_contrast_test(); }
static_assert(motion_detector_self_test(), "Motion attention must find low-contrast left/right movement and reject exposure/noise/still/duplicate/broad changes");
