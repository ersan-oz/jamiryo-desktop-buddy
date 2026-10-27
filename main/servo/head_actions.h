#pragma once
#include "head_routine.h"
#include "head_turn.h"

// One explicitly requested sweep; it never starts another sweep by itself.
class HeadScan {
public:
    constexpr void begin(int64_t now, bool reverse)
    {
        active_ = true; reverse_ = reverse; paused_ = false; step_ = 0;
        due_ = last_ = now; target_ = HeadMotion::center_us;
    }
    constexpr void cancel() { active_ = false; }
    constexpr bool active() const { return active_; }
    constexpr RoutinePlan update(int64_t now, bool allowed)
    {
        if (!active_) return {};
        if (!allowed) {
            if (step_ && now > last_) due_ += now - last_;
            last_ = now; paused_ = true; return {};
        }
        last_ = now;
        if (paused_ && step_ && now < due_) {
            paused_ = false;
            return {true, target_, static_cast<int>(std::clamp<int64_t>(due_ - now, 300, 2000))};
        }
        paused_ = false;
        if (now < due_) return {};
        if (step_ == 4) { active_ = false; return {}; }
        constexpr int durations[] = {700, 1500, 1800, 1200};
        constexpr int holds[] = {200, 250, 250, 250};
        target_ = step_ == 1 ? directed_head_target(BuddyHeadDirection::Left, reverse_)
            : step_ == 2 ? directed_head_target(BuddyHeadDirection::Right, reverse_) : HeadMotion::center_us;
        const int duration = durations[step_];
        due_ = now + duration + holds[step_++];
        return {true, target_, duration};
    }
private:
    bool active_ = false, reverse_ = false, paused_ = false;
    unsigned step_ = 0;
    int target_ = HeadMotion::center_us;
    int64_t due_ = 0, last_ = 0;
};

// Small gestures also run in the normal routine's Hold phase, even without a face.
class HeadGestures {
public:
    constexpr void reset(int64_t now) { due_ = now + 9000; speaking_ = false; }
    constexpr RoutinePlan update(int64_t now, bool allowed, bool speaking, bool moving, bool reverse)
    {
        if (!allowed) { due_ = now + 1200; speaking_ = speaking; return {}; }
        if (speaking && !speaking_) due_ = now + 160;
        speaking_ = speaking;
        if (moving || now < due_) return {};
        direction_ = -direction_;
        const int span = speaking ? 32 : 55;
        due_ = now + (speaking ? 2500 : 13000 + (sequence_++ % 3) * 2000);
        return {true, HeadMotion::center_us + direction_ * (reverse ? -span : span), speaking ? 850 : 1300};
    }
private:
    int64_t due_ = 9000;
    int direction_ = 1;
    unsigned sequence_ = 0;
    bool speaking_ = false;
};

constexpr bool head_actions_self_test()
{
    for (bool reverse : {false, true}) {
        HeadScan scan; scan.begin(1000, reverse);
        HeadMotion head; head.command(HeadMode::Scan, 1000);
        unsigned moves = 0;
        for (int64_t now = 1000; now < 10000; now += 20) {
            const auto plan = scan.update(now, true);
            if (plan.move) {
                ++moves;
                if (moves == 2 && plan.target != directed_head_target(BuddyHeadDirection::Left, reverse)) return false;
                if (moves == 3 && plan.target != directed_head_target(BuddyHeadDirection::Right, reverse)) return false;
                head.move_to(plan.target, now, plan.duration_ms);
            }
            const int pulse = head.update(now);
            if (pulse < HeadMotion::minimum_us || pulse > HeadMotion::maximum_us) return false;
        }
        if (moves != 4 || scan.active() || head.pulse() != HeadMotion::center_us) return false;
        if (scan.update(100000, true).move) return false;
        scan.begin(100000, reverse); scan.update(100000, true);
        scan.update(100900, true); scan.update(101000, false); scan.update(106000, false);
        if (scan.update(106020, true).target != directed_head_target(BuddyHeadDirection::Left, reverse)) return false;
        scan.cancel(); if (scan.active() || scan.update(999999, true).move) return false;
    }
    HeadGestures normal, reversed; normal.reset(0); reversed.reset(0);
    if (normal.update(8999, true, false, false, false).move) return false;
    const auto left = normal.update(9000, true, false, false, false);
    const auto right = reversed.update(9000, true, false, false, true);
    if (!left.move || !right.move || left.target + right.target != 3000 || left.target < 1445 || right.target > 1555) return false;
    if (normal.update(10000, true, false, false, false).move) return false;
    if (normal.update(23000, false, false, false, false).move) return false;
    normal.update(24000, true, true, false, false);
    if (normal.update(24159, true, true, false, false).move) return false;
    const auto speech = normal.update(24160, true, true, false, false);
    if (!speech.move || speech.target < 1468 || speech.target > 1532) return false;
    if (normal.update(27000, true, true, true, false).move) return false;
    normal.reset(30000);
    return !normal.update(38000, true, false, false, false).move;
}
static_assert(head_actions_self_test(), "Requested scan and small automatic gestures must be bounded, paced, reversible and cancelable");
