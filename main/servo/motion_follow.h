#pragma once
#include "head_motion.h"
#include "head_routine.h"

struct MotionFollowPlan { bool move = false; int pulse_us = HeadMotion::center_us; };
class MotionFollow {
public:
    constexpr void reset(int64_t now) { pending_ = active_ = false; last_valid_ = now; gaze_ = 0; }
    constexpr void observe(bool valid, int x, int64_t now)
    {
        if (!valid || x < -1000 || x > 1000) { pending_ = false; return; }
        last_valid_ = now; active_ = true; x_ = x;
        gaze_ = x < -120 || x > 120 ? x*12/1000 : 0;
        pending_ = gaze_ != 0; due_ = now + 180;
    }
    constexpr MotionFollowPlan update(int64_t now, int pulse, bool reverse)
    {
        if (now < last_valid_ || now - last_valid_ >= 4000) {
            active_ = pending_ = false; gaze_ = 0; return {};
        }
        if (!pending_ || now < due_) return {};
        pending_ = false;
        const int magnitude = std::clamp((x_ < 0 ? -x_ : x_)/6, 35, 100);
        const int step = magnitude * (x_ < 0 ? -1 : 1) * (reverse ? -1 : 1);
        const int target = std::clamp(pulse + step, HeadMotion::minimum_us, HeadMotion::maximum_us);
        return {target != pulse, target};
    }
    constexpr bool active() const { return active_; }
    constexpr int gaze_x() const { return gaze_; }
private:
    bool active_ = false, pending_ = false;
    int x_ = 0, gaze_ = 0;
    int64_t last_valid_ = 0, due_ = 0;
};
constexpr bool motion_follow_interrupt_test()
{
    HeadMotion head; head.command(HeadMode::Routine, 0);
    HeadRoutine routine; routine.begin(0, false); routine.update(30000, true);
    if (!routine.tracking()) return false;
    head.move_to(1600, 30000, 850);
    bool owns_motion = true;
    for (int64_t now = 30020; now <= 31000; now += 20) {
        const bool listening = now >= 30200; // Servo noise or real speech starts mid-turn.
        routine.update(now, !listening);
        if (!routine_attention_window(routine.phase())) return false;
        if (routine_freezes_head(!listening, false, false, owns_motion)) head.freeze(now);
        head.update(now);
        if (!head.moving()) owns_motion = false;
    }
    if (head.pulse() != 1600) return false;
    // A different owner must still be freezable; this is not a global bypass.
    if (!routine_freezes_head(false, false, false, false)) return false;
    head.move_to(1400, 32000, 850); head.update(32200); head.freeze(32200);
    const int stopped = head.pulse(); head.update(34000);
    return head.pulse() == stopped && !head.moving(); // Hard-priority cancellation.
}
constexpr bool motion_follow_self_test()
{
    if (!motion_follow_interrupt_test()) return false;
    MotionFollow f; f.reset(0);
    if (f.update(5000, 1620, false).move) return false; // Loss holds position.
    f.observe(true, 700, 6000);
    if (f.update(6179, 1500, false).move) return false;
    auto p = f.update(6180,1500,false);
    if (!p.move || p.pulse_us != 1600 || f.update(6200,1500,false).move) return false;
    f.observe(true,700,7000); if (f.update(7180,1500,true).pulse_us != 1400) return false;
    f.observe(true,-700,8000); if (f.update(8180,1500,false).pulse_us != 1400) return false;
    f.observe(true,-700,9000); if (f.update(9180,HeadMotion::minimum_us,false).move) return false;
    f.observe(true,100,10000); if (f.gaze_x() || f.update(10180,1500,false).move) return false;
    f.observe(true,185,10500); if (f.update(10680,1500,true).pulse_us != 1465) return false; // Visible minimum correction.
    f.observe(true,700,11000); f.reset(11100); if (f.update(12000,1500,false).move) return false;
    f.observe(true,700,13000); f.observe(false,0,13100);
    return !f.update(14000,1500,false).move && !f.update(18000,1500,false).move && !f.active();
}
static_assert(motion_follow_self_test(), "Motion steering must respect delay, deadband, mounting direction, limits, holds and cancellation");
