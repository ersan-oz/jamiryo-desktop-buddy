#pragma once
#include "head_motion.h"

enum class RoutinePhase { Stopped, Waiting, Center, Left, Right, Return, Hold, Track };
inline const char *routine_phase_name(RoutinePhase phase)
{
    switch (phase) {
    case RoutinePhase::Waiting: return "waiting";
    case RoutinePhase::Center: return "scan-center";
    case RoutinePhase::Left: return "scan-left";
    case RoutinePhase::Right: return "scan-right";
    case RoutinePhase::Return: return "scan-return";
    case RoutinePhase::Hold: return "hold";
    case RoutinePhase::Track: return "track";
    default: return "stopped";
    }
}
struct RoutinePlan { bool move = false; int target = HeadMotion::center_us, duration_ms = 900; };
class HeadRoutine {
public:
    static constexpr int64_t boot_at_ms = 10000, hold_ms = 30000, track_ms = 12000;
    constexpr void begin(int64_t now, bool startup_scan)
    {
        phase_ = startup_scan ? RoutinePhase::Waiting : RoutinePhase::Hold;
        due_ = startup_scan ? boot_at_ms : now + hold_ms;
        last_ = now; paused_ = false;
    }
    constexpr void cancel() { phase_ = RoutinePhase::Stopped; paused_ = false; }
    constexpr RoutinePhase phase() const { return phase_; }
    constexpr bool output() const { return phase_ != RoutinePhase::Stopped && phase_ != RoutinePhase::Waiting; }
    constexpr bool tracking() const { return phase_ == RoutinePhase::Track && !paused_; }
    constexpr RoutinePlan update(int64_t now, bool can_move)
    {
        if (phase_ == RoutinePhase::Stopped) return {};
        if (!can_move) {
            // Pause active movement budgets. Idle waiting uses wall-clock time so
            // normal conversation cannot keep postponing the next window forever.
            if (now > last_ && phase_ != RoutinePhase::Waiting && phase_ != RoutinePhase::Hold) due_ += now - last_;
            last_ = now; paused_ = true; return {};
        }
        last_ = now;
        if (paused_) {
            paused_ = false;
            if (phase_ != RoutinePhase::Track && phase_ != RoutinePhase::Waiting
                && !(phase_ == RoutinePhase::Hold && now >= due_))
                return {true, target(), static_cast<int>(std::clamp<int64_t>(due_ - now, 300, 2000))};
        }
        if (now < due_) return {};
        switch (phase_) {
        case RoutinePhase::Waiting: phase_ = RoutinePhase::Center; due_ = now + 1000; return {true, HeadMotion::center_us, 800};
        case RoutinePhase::Center: phase_ = RoutinePhase::Left; due_ = now + 2100; return {true, HeadMotion::minimum_us, 1800};
        case RoutinePhase::Left: phase_ = RoutinePhase::Right; due_ = now + 2300; return {true, HeadMotion::maximum_us, 2000};
        case RoutinePhase::Right: phase_ = RoutinePhase::Return; due_ = now + 2000; return {true, HeadMotion::center_us, 1600};
        case RoutinePhase::Return: phase_ = RoutinePhase::Hold; due_ = now + hold_ms; return {};
        case RoutinePhase::Hold: phase_ = RoutinePhase::Track; due_ = now + track_ms; return {};
        case RoutinePhase::Track: phase_ = RoutinePhase::Hold; due_ = now + hold_ms; return {true, HeadMotion::center_us, 900};
        default: return {};
        }
    }
private:
    constexpr int target() const
    {
        return phase_ == RoutinePhase::Left ? HeadMotion::minimum_us
            : phase_ == RoutinePhase::Right ? HeadMotion::maximum_us : HeadMotion::center_us;
    }
    RoutinePhase phase_ = RoutinePhase::Stopped;
    int64_t due_ = 0, last_ = 0;
    bool paused_ = false;
};

constexpr bool head_routine_self_test()
{
    HeadRoutine routine; routine.begin(5000, true);
    HeadMotion head; head.command(HeadMode::Routine, 5000);
    unsigned left = 0, right = 0, windows = 0;
    RoutinePhase previous = routine.phase();
    for (int64_t now = 5000; now <= 150000; now += 20) {
        const auto plan = routine.update(now, true);
        if (plan.move) head.move_to(plan.target, now, plan.duration_ms);
        if (routine.phase() != previous) {
            if (routine.phase() == RoutinePhase::Left) ++left;
            if (routine.phase() == RoutinePhase::Right) ++right;
            if (routine.phase() == RoutinePhase::Track) ++windows;
            previous = routine.phase();
        }
        const int pulse = head.update(now);
        if (pulse < HeadMotion::minimum_us || pulse > HeadMotion::maximum_us) return false;
        if (now < 10000 && routine.output()) return false;
        if (now == 12800 && pulse != HeadMotion::minimum_us) return false;
        if (now == 15100 && pulse != HeadMotion::maximum_us) return false;
        if (now == 17000 && pulse != HeadMotion::center_us) return false;
        if (now == 47380 && routine.tracking()) return false;
        if (now == 47400 && !routine.tracking()) return false;
        if (now == 59400 && routine.tracking()) return false;
    }
    if (left != 1 || right != 1 || windows != 3) return false;
    routine.begin(0, true); routine.update(10000, true); routine.update(11000, true);
    routine.update(11500, false); routine.update(16500, false);
    if (routine.update(16520, true).target != HeadMotion::minimum_us || routine.phase() != RoutinePhase::Left) return false;
    routine.cancel(); if (routine.update(999999, true).move || routine.output() || routine.tracking()) return false;
    routine.begin(100000, false);
    if (routine.update(129999, true).move || routine.tracking()
        || routine.update(130000, true).move || !routine.tracking()) return false;
    routine.begin(100000, false);
    routine.update(110000, false); routine.update(140000, false);
    routine.update(140020, true);
    return routine.tracking(); // A busy conversation must not restart the idle wait.
}
static_assert(head_routine_self_test(), "Startup sweep and periodic follow timing must remain bounded and cancelable");
