#pragma once
#include <cmath>
#include <cstdint>

enum class BuddyEvent : uint8_t { Hello, Move, Shake, Tilt, Settled, Heard, Listen, Echo, Chirp, None };

struct MotionSample {
    float ax, ay, az; // g
    float gx, gy, gz; // degrees per second
    int64_t ms;
};

namespace motion_settings {
inline constexpr float move_acceleration_g = 0.18f;
inline constexpr float move_rotation_dps = 40.0f;
inline constexpr int64_t move_hold_ms = 220;
inline constexpr int64_t move_gap_ms = 120;
inline constexpr float shake_high_g = 1.30f;
inline constexpr float shake_low_g = 0.45f;
inline constexpr unsigned shake_peaks = 4;
inline constexpr int64_t shake_window_ms = 1300;
inline constexpr int64_t shake_peak_gap_ms = 100;
inline constexpr float tilt_angle_degrees = 55.0f;
inline constexpr int64_t tilt_hold_ms = 700;
}

class MotionEngine {
public:
    void reset(const MotionSample &s)
    {
        *this = MotionEngine{};
        rx_ = px_ = s.ax;
        ry_ = py_ = s.ay;
        rz_ = pz_ = s.az;
        initialized_ = true;
    }

    BuddyEvent update(const MotionSample &s, bool suppress_motion = false)
    {
        if (!initialized_) { reset(s); return BuddyEvent::None; }
        const float dynamic = dynamic_g_ = norm(s.ax - px_, s.ay - py_, s.az - pz_);
        px_ += 0.06f * (s.ax - px_);
        py_ += 0.06f * (s.ay - py_);
        pz_ += 0.06f * (s.az - pz_);
        // Keep tracking gravity during speech, but do not turn speaker vibration
        // into new motion. Preserve a real motion's pending settled response.
        if (suppress_motion) {
            peaks_ = 0;
            peak_high_ = false;
            motion_start_ = tilt_start_ = -1;
            return BuddyEvent::None;
        }
        const float denom = norm(px_, py_, pz_) * norm(rx_, ry_, rz_);
        const float cosine = denom > 0.01f ? (px_ * rx_ + py_ * ry_ + pz_ * rz_) / denom : 1.0f;
        const float angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, cosine))) * 57.29578f;
        const bool active = dynamic > motion_settings::move_acceleration_g
            || norm(s.gx, s.gy, s.gz) > motion_settings::move_rotation_dps;
        if (active) {
            last_active_ = s.ms;
            moving_ = true;
            if (motion_start_ < 0) motion_start_ = s.ms;
        } else if (s.ms - last_active_ > motion_settings::move_gap_ms) {
            // A brief bump must not become a Move after the robot is still.
            motion_start_ = -1;
        }

        if (s.ms - first_peak_ > motion_settings::shake_window_ms) peaks_ = 0;
        if (dynamic > motion_settings::shake_high_g && !peak_high_) {
            peak_high_ = true;
            if (peaks_ == 0 || s.ms - last_peak_ >= motion_settings::shake_peak_gap_ms) {
                if (peaks_ == 0) first_peak_ = s.ms;
                ++peaks_;
                last_peak_ = s.ms;
            }
        } else if (dynamic < motion_settings::shake_low_g) {
            peak_high_ = false;
        }
        if (peaks_ >= motion_settings::shake_peaks && ready(s.ms)) {
            peaks_ = 0;
            return emit(BuddyEvent::Shake, s.ms);
        }

        if (angle <= motion_settings::tilt_angle_degrees) tilt_start_ = -1;
        if (angle < 25.0f) tilt_latched_ = false;
        if (angle > motion_settings::tilt_angle_degrees) {
            if (tilt_start_ < 0) tilt_start_ = s.ms;
            if (!tilt_latched_ && s.ms - tilt_start_ >= motion_settings::tilt_hold_ms && ready(s.ms)) {
                tilt_latched_ = true;
                return emit(BuddyEvent::Tilt, s.ms);
            }
        } else if (active && motion_start_ >= 0 && s.ms - motion_start_ >= motion_settings::move_hold_ms
                   && !move_announced_ && ready(s.ms)) {
            move_announced_ = true;
            return emit(BuddyEvent::Move, s.ms);
        }
        if (moving_ && s.ms - last_active_ >= 1600 && ready(s.ms)) {
            moving_ = move_announced_ = false;
            motion_start_ = -1;
            return emit(BuddyEvent::Settled, s.ms);
        }
        return BuddyEvent::None;
    }

    float dynamic_acceleration_g() const { return dynamic_g_; }

private:
    static float norm(float x, float y, float z) { return std::sqrt(x*x + y*y + z*z); }
    bool ready(int64_t ms) const { return ms - last_event_ >= 3500; }
    BuddyEvent emit(BuddyEvent event, int64_t ms) { last_event_ = ms; return event; }
    float rx_ = 0, ry_ = 0, rz_ = 1, px_ = 0, py_ = 0, pz_ = 1;
    float dynamic_g_ = 0;
    bool initialized_ = false, moving_ = false, move_announced_ = false;
    bool peak_high_ = false, tilt_latched_ = false;
    unsigned peaks_ = 0;
    int64_t first_peak_ = 0, last_peak_ = 0, last_active_ = 0, motion_start_ = -1;
    int64_t tilt_start_ = -1, last_event_ = -3500;
};

inline bool motion_engine_self_test()
{
    const MotionSample rest{0, 0, 1, 0, 0, 0, 0};
    MotionEngine engine;
    engine.reset(rest);
    for (int i = 0; i < 500; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (engine.update(sample) != BuddyEvent::None) return false;
    }
    engine.reset(rest);
    bool moved = false;
    for (int i = 0; i < 60; ++i) {
        auto sample = rest; sample.ms = i * 20; sample.gz = 100;
        moved |= engine.update(sample) == BuddyEvent::Move;
    }
    if (!moved) return false;
    engine.reset(rest);
    bool shaken = false;
    for (int i = 0; i < 40; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (i == 2 || i == 10 || i == 18 || i == 26) sample.ax = 1.8f;
        shaken |= engine.update(sample) == BuddyEvent::Shake;
    }
    if (!shaken) return false;
    // Moderate handling now reacts earlier, while tiny tremors remain ignored.
    engine.reset(rest);
    int move_count = 0;
    for (int i = 0; i < 60; ++i) {
        auto sample = rest; sample.ms = i * 20; sample.gz = 60;
        const auto event = engine.update(sample);
        if (event == BuddyEvent::Shake) return false;
        if (event == BuddyEvent::Move) {
            if (sample.ms < 220 || sample.ms > 260) return false;
            ++move_count;
        }
    }
    if (move_count != 1) return false;
    // Repeated modest acceleration also works without any angular velocity.
    engine.reset(rest);
    moved = false;
    for (int i = 0; i < 40; ++i) {
        auto sample = rest; sample.ms = i * 20;
        sample.ax = (i / 5) % 2 ? -0.30f : 0.30f;
        const auto event = engine.update(sample);
        if (event == BuddyEvent::Shake) return false;
        moved |= event == BuddyEvent::Move;
    }
    if (!moved) return false;
    engine.reset(rest);
    for (int i = 0; i < 300; ++i) {
        auto sample = rest; sample.ms = i * 20;
        sample.ax = (i % 2 ? 0.02f : -0.02f); sample.gz = 35;
        if (engine.update(sample) != BuddyEvent::None) return false;
    }
    // Several short turns separated by rest cannot accumulate into a pickup.
    engine.reset(rest);
    for (int i = 0; i < 150; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (i % 25 < 10) sample.gz = 60;
        const auto event = engine.update(sample);
        if (event == BuddyEvent::Move || event == BuddyEvent::Shake) return false;
    }
    engine.reset(rest);
    for (int i = 0; i < 40; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (i == 2 || i == 10 || i == 18 || i == 26) sample.ax = 0.95f;
        const auto event = engine.update(sample);
        if (event == BuddyEvent::Shake || event == BuddyEvent::Move) return false;
    }
    engine.reset(rest);
    for (int i = 0; i < 150; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (i == 10) sample.ax = 2.0f;
        const auto event = engine.update(sample);
        if (event == BuddyEvent::Shake || event == BuddyEvent::Move) return false;
    }
    // Widely separated knocks do not constitute a shake.
    engine.reset(rest);
    for (int i = 0; i < 130; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (i == 2 || i == 32 || i == 62 || i == 92) sample.ax = 1.8f;
        if (engine.update(sample) == BuddyEvent::Shake) return false;
    }
    engine.reset(rest);
    bool tilted = false;
    // A quicker pickup can precede a sustained tilt; the shared cooldown must
    // still allow exactly one subsequent tilt instead of losing it forever.
    int tilt_count = 0;
    for (int i = 0; i < 300; ++i) {
        auto sample = rest; sample.ms = i * 20; sample.ax = 0.866f; sample.az = 0.5f;
        const auto event = engine.update(sample);
        if (event == BuddyEvent::Tilt) { tilted = true; ++tilt_count; }
        if (event == BuddyEvent::Shake) return false;
    }
    if (!tilted || tilt_count != 1) return false;
    engine.reset(rest);
    for (int i = 0; i < 200; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (i % 8 == 0) sample.ax = 1.8f;
        if (engine.update(sample, true) != BuddyEvent::None) return false;
    }
    for (int i = 200; i < 400; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (engine.update(sample) != BuddyEvent::None) return false;
    }
    engine.reset(rest);
    int settled_count = 0;
    for (int i = 0; i < 400; ++i) {
        auto sample = rest; sample.ms = i * 20;
        if (i < 60) sample.gz = 100;
        if (engine.update(sample) == BuddyEvent::Settled) ++settled_count;
    }
    return settled_count == 1;
}
