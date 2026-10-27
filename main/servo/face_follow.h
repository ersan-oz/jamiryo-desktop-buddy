#pragma once
#include "head_motion.h"
#include <algorithm>
#include <cstdint>

struct FaceFollowPlan {
    bool move = false;
    int pulse_us = HeadMotion::center_us;
};

// Each frame supplies one bounded correction, never a free-running velocity.
// All times are monotonic milliseconds. No identity or image is retained.
class FaceFollow {
public:
    static constexpr int64_t reaction_delay_ms = 180, lost_ms = 2500;
    constexpr void reset(int64_t now)
    {
        last_seen_ = now; previous_sample_ = -1; move_at_ = 0;
        samples_ = 0; filtered_ = gaze_ = 0;
        acquired_ = pending_ = centered_ = false;
    }
    constexpr void observe(bool valid, int x, int64_t now)
    {
        if (!valid || x < -1000 || x > 1000) {
            samples_ = 0; pending_ = false; previous_sample_ = -1; return;
        }
        if (previous_sample_ < 0 || now - previous_sample_ > 1300) {
            samples_ = 0; filtered_ = x;
        } else filtered_ = (2 * filtered_ + x) / 3;
        previous_sample_ = last_seen_ = now;
        if (samples_ < 2) ++samples_;
        if (samples_ < 2) return;
        acquired_ = true; centered_ = false;
        const int magnitude = filtered_ < 0 ? -filtered_ : filtered_;
        const int threshold = gaze_ == 0 ? 120 : 80;
        gaze_ = magnitude <= threshold ? 0 : std::clamp(filtered_ * 12 / 1000, -12, 12);
        pending_ = gaze_ != 0;
        move_at_ = now + reaction_delay_ms;
    }
    constexpr FaceFollowPlan update(int64_t now, int current_pulse, bool reverse)
    {
        if (!centered_ && now - last_seen_ >= lost_ms) {
            centered_ = true; acquired_ = pending_ = false;
            samples_ = 0; previous_sample_ = -1; gaze_ = filtered_ = 0;
            return {true, HeadMotion::center_us};
        }
        if (!pending_ || now < move_at_) return {};
        pending_ = false;
        const int step = std::clamp(filtered_ / 20, -25, 25) * (reverse ? -1 : 1);
        const int target = std::clamp(current_pulse + step, HeadMotion::minimum_us, HeadMotion::maximum_us);
        return {target != current_pulse, target};
    }
    constexpr bool active() const { return acquired_; }
    constexpr int gaze_x() const { return gaze_; }
private:
    int64_t last_seen_ = 0, previous_sample_ = -1, move_at_ = 0;
    unsigned samples_ = 0;
    int filtered_ = 0, gaze_ = 0;
    bool acquired_ = false, pending_ = false, centered_ = false;
};

inline constexpr bool face_follow_self_test()
{
    FaceFollow follow; follow.reset(0);
    follow.observe(true, 800, 500);
    if (follow.active() || follow.update(900, 1500, false).move) return false;
    follow.observe(true, 800, 1000);
    if (!follow.active() || follow.gaze_x() <= 0 || follow.update(1179, 1500, false).move) return false;
    auto plan = follow.update(1180, 1500, false);
    if (!plan.move || plan.pulse_us != 1525 || follow.update(1200, 1500, false).move) return false;
    follow.observe(false, 0, 1500);
    if (follow.update(3499, 1525, false).move) return false;
    plan = follow.update(3500, 1525, false);
    if (!plan.move || plan.pulse_us != 1500 || follow.active() || follow.gaze_x() != 0
        || follow.update(10000, 1525, false).move) return false;
    follow.reset(0); follow.observe(true, -1000, 500); follow.observe(true, -1000, 1000);
    if (follow.update(1180, HeadMotion::minimum_us, false).move) return false;
    follow.observe(true, -1000, 1500);
    if (follow.update(1680, 1500, true).pulse_us != 1525) return false;
    follow.reset(0); follow.observe(true, 100, 500); follow.observe(true, 100, 1000);
    if (follow.gaze_x() || follow.update(1180, 1500, false).move) return false;
    follow.reset(0); follow.observe(true, 700, 500); follow.observe(true, 700, 1900);
    if (follow.active()) return false;
    follow.observe(true, 1001, 2000); follow.observe(true, 700, 2100);
    if (follow.active()) return false;
    follow.reset(2200);
    if (follow.active() || follow.gaze_x() || follow.update(2300, 1500, false).move) return false;
    // Simulate a fixed face with the camera mounted on the turning head.
    // Both installation directions must converge without exceeding the limits.
    for (bool reverse : {false, true}) {
        HeadMotion head; head.command(HeadMode::Follow, 0); follow.reset(0);
        for (int now = 0; now <= 20000; now += 20) {
            const int pulse = head.update(now);
            if (pulse < HeadMotion::minimum_us || pulse > HeadMotion::maximum_us) return false;
            if (now && now % 600 == 0) {
                const int error = 650 - (pulse - 1500) * (reverse ? -10 : 10);
                follow.observe(true, error, now);
            }
            const auto correction = follow.update(now, pulse, reverse);
            if (correction.move) head.move_to(correction.pulse_us, now, 700);
        }
        const int error = 650 - (head.pulse() - 1500) * (reverse ? -10 : 10);
        if (error < -180 || error > 180) return false;
    }
    return true;
}
static_assert(face_follow_self_test(), "Face following must reject jitter/stale frames and bound each correction");
