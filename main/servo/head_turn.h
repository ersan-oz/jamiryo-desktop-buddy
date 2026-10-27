#pragma once
#include "head_motion.h"

enum class BuddyHeadDirection : int { Left = -1, Center = 0, Right = 1 };

constexpr bool valid_head_direction(BuddyHeadDirection direction)
{
    const int value = static_cast<int>(direction);
    return value >= -1 && value <= 1;
}
constexpr int directed_head_target(BuddyHeadDirection direction, bool reverse)
{
    return HeadMotion::center_us + static_cast<int>(direction) * (reverse ? -1 : 1)
        * (HeadMotion::maximum_us - HeadMotion::center_us);
}

// Explicit directions are absolute offsets from center, never cumulative.
// The motor task owns this RAM-only override; user preferences remain unchanged.
class HeadTurn {
public:
    static constexpr int64_t hold_ms = 20000;
    constexpr bool begin(BuddyHeadDirection direction, bool reverse, int64_t now)
    {
        if (!valid_head_direction(direction) || now < 0) return false;
        active_ = true; target_ = directed_head_target(direction, reverse);
        due_ = now + hold_ms; return true;
    }
    constexpr void cancel() { active_ = false; }
    constexpr bool active() const { return active_; }
    constexpr int target() const { return target_; }
    constexpr bool expired(int64_t now) const { return active_ && now >= due_; }
    constexpr void pause(int64_t delta) { if (active_ && delta > 0) due_ += delta; }
private:
    bool active_ = false;
    int target_ = HeadMotion::center_us;
    int64_t due_ = 0;
};

constexpr bool head_turn_self_test()
{
    HeadTurn turn;
    if (turn.active() || turn.expired(999999)) return false;
    for (bool reverse : {false, true}) {
        for (auto direction : {BuddyHeadDirection::Left, BuddyHeadDirection::Center, BuddyHeadDirection::Right}) {
            if (!turn.begin(direction, reverse, 1000) || !turn.active()) return false;
            const int target = turn.target();
            if (target < HeadMotion::minimum_us || target > HeadMotion::maximum_us) return false;
            if (direction == BuddyHeadDirection::Center && target != HeadMotion::center_us) return false;
            if (direction == BuddyHeadDirection::Right && target != (reverse ? HeadMotion::minimum_us : HeadMotion::maximum_us)) return false;
            if (direction == BuddyHeadDirection::Left && target != (reverse ? HeadMotion::maximum_us : HeadMotion::minimum_us)) return false;
            if (turn.expired(20999) || !turn.expired(21000)) return false;
            turn.pause(500); if (turn.expired(21499) || !turn.expired(21500)) return false;
            // Same-direction requests must not accumulate beyond +/-45 degrees.
            if (!turn.begin(direction, reverse, 2000) || turn.target() != target) return false;
        }
    }
    if (turn.begin(static_cast<BuddyHeadDirection>(2), false, 0)) return false;
    if (turn.begin(BuddyHeadDirection::Left, false, -1)) return false;
    HeadMotion head; head.command(HeadMode::Directed, 0);
    head.move_to(directed_head_target(BuddyHeadDirection::Right, false), 0, 1000);
    const int midway = head.update(500);
    head.command(HeadMode::Directed, 500);
    head.move_to(directed_head_target(BuddyHeadDirection::Left, false), 500, 1000);
    if (head.update(500) != midway || head.update(1500) != HeadMotion::minimum_us) return false;
    head.command(HeadMode::Off, 1500); turn.cancel();
    return !turn.active() && !turn.expired(999999) && !head.moving();
}
static_assert(head_turn_self_test(), "Voice head turns must be bounded, reversible, non-cumulative and cancelable");
