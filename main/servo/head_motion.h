#pragma once
#include <algorithm>
#include <cstdint>

enum class HeadMode : uint8_t { Off, Hold, Test, Auto, Follow, Routine, Directed, Scan, Object, Person };
inline const char *head_mode_name(HeadMode mode)
{
    switch (mode) {
    case HeadMode::Person: return "person";
    case HeadMode::Object: return "object";
    case HeadMode::Scan: return "scan";
    case HeadMode::Hold: return "hold";
    case HeadMode::Test: return "test";
    case HeadMode::Auto: return "auto";
    case HeadMode::Follow: return "follow";
    case HeadMode::Routine: return "routine";
    case HeadMode::Directed: return "directed";
    default: return "off";
    }
}

// Nominal +/-45 degrees using the MG90S 1-2ms / 180-degree datasheet mapping.
// Actual shaft angle must be checked on this servo after mounting.
// No hardware dependency: startup, interrupted tests and bounds can be tested
// without powering an unknown mechanism.
class HeadMotion {
public:
    static constexpr int minimum_us = 1250, center_us = 1500, maximum_us = 1750;
    constexpr HeadMode mode() const { return mode_; }
    constexpr int pulse() const { return pulse_; }
    constexpr int target() const { return target_; }
    constexpr int test_gaze_x() const { return (pulse_ - center_us) * 12 / (maximum_us - center_us); }
    constexpr bool moving() const { return ramping_; }
    constexpr void command(HeadMode mode, int64_t now)
    {
        update(now);
        if (mode == HeadMode::Off) {
            mode_ = mode; freeze(now); return;
        }
        // Actual shaft position is unknown while PWM is off. The first pulse
        // acquires center; only subsequent moves can be eased without feedback.
        if (mode_ == HeadMode::Off) pulse_ = center_us;
        mode_ = mode;
        move_to(center_us, now, 800);
        test_step_ = 0;
        test_at_ = now + 900;
    }
    constexpr bool move_to(int target, int64_t now, int duration_ms)
    {
        if (mode_ == HeadMode::Off) return false;
        sample(now);
        from_ = pulse_;
        target_ = std::clamp(target, minimum_us, maximum_us);
        began_ = now;
        duration_ = std::clamp(duration_ms, 300, 2000);
        ramping_ = from_ != target_;
        return true;
    }
    constexpr void freeze(int64_t now)
    {
        sample(now); from_ = target_ = pulse_; ramping_ = false;
    }
    constexpr int update(int64_t now)
    {
        if (mode_ == HeadMode::Off) return pulse_;
        sample(now);
        if (mode_ == HeadMode::Test && now >= test_at_) {
            constexpr int targets[] = {minimum_us, maximum_us, center_us};
            constexpr int durations[] = {800, 1000, 800};
            constexpr int holds[] = {400, 400, 600};
            if (test_step_ == 3) { mode_ = HeadMode::Off; freeze(now); }
            else {
                move_to(targets[test_step_], now, durations[test_step_]);
                test_at_ = now + durations[test_step_] + holds[test_step_];
                ++test_step_;
            }
        }
        return pulse_;
    }
private:
    constexpr void sample(int64_t now)
    {
        if (!ramping_) return;
        const double t = std::clamp(double(now - began_) / duration_, 0.0, 1.0);
        const double eased = t * t * (3.0 - 2.0 * t);
        pulse_ = std::clamp(int(from_ + (target_ - from_) * eased + 0.5), minimum_us, maximum_us);
        if (t >= 1.0) ramping_ = false;
    }
    HeadMode mode_ = HeadMode::Off;
    int pulse_ = center_us, from_ = center_us, target_ = center_us, duration_ = 800;
    int64_t began_ = 0, test_at_ = 0;
    unsigned test_step_ = 0;
    bool ramping_ = false;
};

inline constexpr bool head_motion_self_test()
{
    HeadMotion head;
    if (head.mode() != HeadMode::Off || head.move_to(1600, 0, 300)) return false;
    head.command(HeadMode::Hold, 0);
    head.move_to(9999, 0, 1000);
    int last = head.pulse();
    for (int ms = 0; ms <= 1000; ms += 20) {
        const int value = head.update(ms);
        if (value < last || value < HeadMotion::minimum_us || value > HeadMotion::maximum_us) return false;
        if (ms == 100 && value >= 1510) return false; // eased start
        last = value;
    }
    if (last != HeadMotion::maximum_us || head.moving()) return false;
    head.move_to(-9999, 1000, 1000);
    for (int ms = 1000; ms <= 2000; ms += 20) {
        const int value = head.update(ms);
        if (value > last || value < HeadMotion::minimum_us || value > HeadMotion::maximum_us) return false;
        last = value;
    }
    if (last != HeadMotion::minimum_us) return false;
    head.move_to(1600, 2000, 1000);
    const int mid = head.update(2500);
    head.move_to(1400, 2500, 1); // rapid retarget has no pulse discontinuity
    if (head.update(2500) != mid || head.update(2600) <= HeadMotion::minimum_us) return false;
    head.command(HeadMode::Off, 2600);
    const int stopped = head.pulse();
    if (head.update(10000) != stopped || head.moving() || head.move_to(1500, 10000, 800)) return false;
    head.command(HeadMode::Test, 10000);
    head.update(10900); head.update(11700);
    if (head.pulse() != HeadMotion::minimum_us) return false;
    head.update(12100); head.update(13100);
    if (head.pulse() != HeadMotion::maximum_us) return false;
    head.update(13500); head.update(14300);
    if (head.pulse() != 1500) return false;
    head.update(14900);
    if (head.mode() != HeadMode::Off) return false;
    head.command(HeadMode::Test, 15000); head.update(15900); head.update(16200);
    head.command(HeadMode::Off, 16200); head.update(999999);
    return head.mode() == HeadMode::Off && !head.moving();
}

static_assert(head_motion_self_test(), "Servo commissioning bounds and cancellation must pass before building firmware");
