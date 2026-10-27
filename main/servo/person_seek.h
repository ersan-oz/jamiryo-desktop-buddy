#pragma once
#include "head_routine.h"

enum class PersonPhase { Idle, Moving, Sampling, Aligning, Holding };
inline const char *person_phase_name(PersonPhase p)
{
    switch (p) {
    case PersonPhase::Moving: return "search-moving";
    case PersonPhase::Sampling: return "search-sampling";
    case PersonPhase::Aligning: return "aligning";
    case PersonPhase::Holding: return "holding";
    default: return "idle";
    }
}
namespace person_seek {
constexpr int64_t conversation_gap_ms = 60000, pending_ms = 10000;
constexpr bool can_start(HeadMode mode, PersonPhase phase)
{
    return mode == HeadMode::Routine || mode == HeadMode::Auto || mode == HeadMode::Hold
        || mode == HeadMode::Follow || (mode == HeadMode::Person && phase == PersonPhase::Holding);
}
constexpr bool new_conversation(int64_t now, int64_t previous)
{
    return now > 0 && (previous <= 0 || (now >= previous && now - previous >= conversation_gap_ms));
}
constexpr bool face_box(int x1, int y1, int x2, int y2, float score, unsigned w, unsigned h)
{
    return score >= 0.60f && score <= 1.0f && x1 >= 0 && y1 >= 0
        && x2 > x1 && y2 > y1 && x2 <= static_cast<int>(w) && y2 <= static_cast<int>(h)
        && x2 - x1 >= 30 && y2 - y1 >= 30;
}
}

// A bounded, anonymous search: center, left, right, stopping at the first
// independently confirmed face. No identities, embeddings or images are kept.
class PersonSeek {
public:
    static constexpr int64_t search_ms = 18000, sample_ms = 2500;
    constexpr void begin(int64_t now, bool reverse)
    {
        phase_ = PersonPhase::Moving; reverse_ = reverse; view_ = 0;
        began_ = now; target_ = HeadMotion::center_us; samples_ = 0; matched_ = false;
        last_sample_ = 0; last_update_ = now; paused_ = false;
    }
    constexpr void cancel() { phase_ = PersonPhase::Idle; samples_ = 0; paused_ = false; }
    constexpr bool active() const { return phase_ != PersonPhase::Idle; }
    constexpr PersonPhase phase() const { return phase_; }
    constexpr bool matched() const { return matched_; }
    constexpr bool wants_frame() const { return phase_ == PersonPhase::Sampling && !paused_; }
    constexpr int target() const { return target_; }
    constexpr void observe(bool valid, int x, int64_t captured_at, int64_t now)
    {
        if (!wants_frame() || captured_at < sample_at_ || captured_at <= last_sample_
            || now < captured_at || now - captured_at > 1500) return;
        last_sample_ = captured_at;
        if (!valid || x < -1000 || x > 1000) { samples_ = 0; return; }
        if (!samples_ || captured_at - first_sample_ > 1300 || x - first_x_ > 200 || first_x_ - x > 200) {
            first_x_ = x; first_sample_ = captured_at; samples_ = 1; return;
        }
        // Same conservative within-view correction used by object search.
        const int correction = ((first_x_ + x) / 2) / 10 * (reverse_ ? -1 : 1);
        target_ = std::clamp(target_ + correction, HeadMotion::minimum_us, HeadMotion::maximum_us);
        phase_ = PersonPhase::Aligning; samples_ = 0;
    }
    constexpr RoutinePlan update(int64_t now, bool allowed, int pulse, bool moving, bool settling, int64_t last_speech)
    {
        if (!active()) return {};
        if (phase_ != PersonPhase::Holding && now - began_ >= search_ms) { cancel(); return {}; }
        if (!allowed && phase_ == PersonPhase::Sampling && now > last_update_) sample_at_ += now - last_update_;
        last_update_ = now;
        paused_ = !allowed;
        if (!allowed) return {};
        if (phase_ == PersonPhase::Holding) {
            if (now >= last_speech && now - last_speech >= person_seek::conversation_gap_ms) cancel();
            return {};
        }
        if (phase_ == PersonPhase::Moving || phase_ == PersonPhase::Aligning) {
            // A pause freezes PWM; resume the target instead of treating the
            // half-finished position as a completed sweep stop.
            if (!moving && pulse != target_) return {true, target_, 900};
            if (moving || settling) return {};
            if (phase_ == PersonPhase::Aligning) { matched_ = true; phase_ = PersonPhase::Holding; return {}; }
            phase_ = PersonPhase::Sampling; sample_at_ = now; samples_ = 0; last_sample_ = 0;
        }
        if (phase_ == PersonPhase::Sampling && now - sample_at_ >= sample_ms) {
            if (++view_ >= 3) { cancel(); return {}; }
            target_ = HeadMotion::center_us + (view_ == 1 ? -250 : 250) * (reverse_ ? -1 : 1);
            phase_ = PersonPhase::Moving; samples_ = 0;
            return {true, target_, 1100};
        }
        return {};
    }
private:
    PersonPhase phase_ = PersonPhase::Idle;
    bool reverse_ = false, matched_ = false, paused_ = false;
    unsigned view_ = 0, samples_ = 0;
    int target_ = HeadMotion::center_us, first_x_ = 0;
    int64_t began_ = 0, sample_at_ = 0, first_sample_ = 0, last_sample_ = 0, last_update_ = 0;
};

constexpr bool person_seek_self_test()
{
    using namespace person_seek;
    if (!new_conversation(1000, 0) || new_conversation(60999, 1000)
        || !new_conversation(61000, 1000) || new_conversation(500, 1000)) return false;
    for (auto mode : {HeadMode::Off, HeadMode::Test, HeadMode::Directed, HeadMode::Scan, HeadMode::Object})
        if (can_start(mode, PersonPhase::Idle)) return false;
    if (!can_start(HeadMode::Routine, PersonPhase::Idle) || can_start(HeadMode::Person, PersonPhase::Sampling)
        || !can_start(HeadMode::Person, PersonPhase::Holding)) return false;
    if (!face_box(0, 0, 30, 30, .60f, 320, 240) || face_box(-1, 0, 40, 40, .9f, 320, 240)
        || face_box(0, 0, 29, 40, .9f, 320, 240) || face_box(0, 0, 50, 50, .59f, 320, 240)
        || face_box(300, 0, 340, 40, .9f, 320, 240)) return false;
    for (bool reverse : {false, true}) {
        PersonSeek seek; seek.begin(1000, reverse);
        seek.update(1000, true, 1500, false, false, 1000);
        seek.observe(true, 500, 1000, 1100);
        if (seek.phase() != PersonPhase::Sampling) return false;
        seek.observe(true, 500, 1000, 1200); // Duplicate frame cannot confirm.
        seek.observe(true, 500, 1500, 3001); // Stale frame cannot confirm.
        if (seek.phase() != PersonPhase::Sampling) return false;
        seek.observe(true, 600, 1500, 1600);
        if (seek.phase() != PersonPhase::Aligning || seek.target() != (reverse ? 1445 : 1555)) return false;
        auto plan = seek.update(1620, true, 1500, false, false, 1000);
        if (!plan.move || plan.target != seek.target()) return false;
        seek.update(1700, false, 1520, false, false, 1000);
        plan = seek.update(2200, true, 1520, false, false, 1000);
        if (!plan.move || plan.target != seek.target()) return false;
        seek.update(3000, true, seek.target(), false, false, 1000);
        if (!seek.matched() || seek.phase() != PersonPhase::Holding) return false;
        seek.update(61000, true, seek.target(), false, false, 30000);
        if (!seek.active()) return false; // Next real utterance extends hold.
        seek.update(90000, true, seek.target(), false, false, 30000);
        if (seek.active()) return false;
        seek.begin(1000, reverse); seek.update(1000, true, 1500, false, false, 1000);
        plan = seek.update(3500, true, 1500, false, false, 1000);
        if (!plan.move || plan.target != (reverse ? 1750 : 1250)) return false;
        seek.update(5000, true, plan.target, false, false, 1000);
        plan = seek.update(7500, true, plan.target, false, false, 1000);
        if (!plan.move || plan.target != (reverse ? 1250 : 1750)) return false;
        seek.update(9000, true, plan.target, false, false, 1000);
        seek.update(11500, true, plan.target, false, false, 1000);
        if (seek.active() || seek.matched()) return false;
        seek.begin(1000, reverse); seek.update(19000, false, 1500, false, false, 1000);
        if (seek.active()) return false; // Busy speech cannot extend search forever.
        seek.begin(20000, reverse); seek.cancel();
        if (seek.active() || seek.wants_frame() || seek.update(30000, true, 1500, false, false, 20000).move) return false;
    }
    PersonSeek seek; seek.begin(1000, false); seek.update(1000, true, 1500, false, false, 1000);
    seek.observe(true, -800, 1100, 1200); seek.observe(true, 800, 1600, 1700);
    if (seek.phase() != PersonPhase::Sampling) return false; // Different faces/positions cannot confirm one another.
    seek.observe(false, 0, 2100, 2200); seek.observe(true, 800, 2600, 2700);
    return seek.phase() == PersonPhase::Sampling;
}
static_assert(person_seek_self_test(), "Conversation search must be paced, bounded, reversible and require fresh stable faces");

constexpr bool person_seek_pose_test()
{
    // A person outside the center view must be found on either side. Model
    // observations are expressed in the camera frame as the head actually turns.
    for (bool reverse : {false, true}) for (int side : {-1, 1}) {
        HeadMotion head; head.command(HeadMode::Person, 0);
        PersonSeek seek; seek.begin(1, reverse);
        const int expected = 1500 + side * 210 * (reverse ? -1 : 1);
        int64_t last_moving = 0;
        for (int64_t now = 20; now <= 18000; now += 20) {
            const int pulse = head.update(now);
            if (head.moving()) last_moving = now;
            const auto plan = seek.update(now, true, pulse, head.moving(), now - last_moving < 700, 1);
            if (plan.move) head.move_to(plan.target, now, plan.duration_ms);
            if (seek.wants_frame() && now % 600 == 0) {
                const int x = (expected - pulse) * (reverse ? -10 : 10);
                seek.observe(x >= -1000 && x <= 1000, x, now, now);
            }
            if (pulse < 1250 || pulse > 1750) return false;
        }
        if (!seek.matched() || seek.phase() != PersonPhase::Holding || head.pulse() != expected) return false;
    }
    return true;
}
static_assert(person_seek_pose_test(), "A real moving camera must converge toward a person on either side with either mounting");
