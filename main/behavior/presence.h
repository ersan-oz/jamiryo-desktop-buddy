#pragma once
#include <algorithm>
#include <cstdint>
#include "emotions.h"
#include "idle_sleep.h"

enum class PresencePhase : uint8_t { Idle, Listening, Thinking, Speaking };
enum class PresenceBlink : uint8_t { Single, Double, Slow };
struct PresenceSnapshot {
    BuddyEmotion emotion = BuddyEmotion::Neutral;
    int energy = 70, curiosity = 65, comfort = 55;
    int gaze_x = 0, gaze_y = 0;
    float openness = 1;
    PresenceBlink blink = PresenceBlink::Single;
    unsigned blinks = 0, double_blinks = 0, slow_blinks = 0, glances = 0, engagements = 0;
    unsigned head_sequence = 0;
    int head_offset_us = 0, head_duration_ms = 1200;
    int64_t head_due_ms = 0;
};

// Bounded local animation policy. No audio, network, storage or wall-clock API.
// Calling update more often never speeds up time-based personality changes.
class PresenceEngine {
public:
    constexpr void begin(int64_t now, uint32_t seed)
    {
        *this = PresenceEngine{};
        random_ = seed ? seed : 1; last_tick_ = now; next_minute_ = now + 60000;
        next_blink_ = now + 3000; next_glance_ = now + 6000;
    }
    constexpr void engage(int64_t now)
    {
        // Recognized speech/explicit interaction, not VAD blocks or our own motion.
        if (last_engagement_ >= 0 && now - last_engagement_ < 10000) return;
        last_engagement_ = now; ++value_.engagements;
        value_.energy = std::min(90, value_.energy + 2);
        value_.comfort = std::min(85, value_.comfort + 3);
    }
    constexpr void emotion(BuddyEmotion emotion, int64_t now)
    {
        if (static_cast<unsigned>(emotion) >= buddy_emotion_count) return;
        reaction_ = emotion; reaction_until_ = now + 20000;
    }
    constexpr PresenceSnapshot update(int64_t now, PresencePhase phase, BuddySleepStage sleep, bool blocked = false)
    {
        if (now < last_tick_) return value_; // No backwards-time catch-up.
        last_tick_ = now;
        if (now >= next_minute_) {
            // O(1), even after a long pause; no animation or missed-event burst.
            const int minutes = static_cast<int>(std::min<int64_t>(120, 1 + (now - next_minute_) / 60000));
            value_.energy = std::clamp(value_.energy + (sleep == BuddySleepStage::Asleep ? 2 : -1) * minutes, 35, 90);
            value_.comfort += value_.comfort < 55 ? 1 : value_.comfort > 55 ? -1 : 0;
            next_minute_ = now + 60000;
        }
        value_.curiosity = sleep == BuddySleepStage::Awake ? std::clamp(value_.energy - 5, 30, 85) : 20;
        const bool speaking = phase == PresencePhase::Speaking;
        if (speaking && previous_phase_ != PresencePhase::Speaking && now < reaction_until_) reaction_until_ = now + 45000;
        if (!speaking && previous_phase_ == PresencePhase::Speaking && now < reaction_until_) reaction_until_ = now + 12000;
        value_.emotion = sleep != BuddySleepStage::Awake ? BuddyEmotion::Sleepy
            : now < reaction_until_ ? reaction_ : ambient_;

        const bool idle = phase == PresencePhase::Idle && sleep == BuddySleepStage::Awake && !blocked;
        const bool quiet_break = !idle && was_idle_;
        if (!idle) {
            value_.gaze_x = value_.gaze_y = 0; glance_return_ = 0;
            if (was_idle_) next_glance_ = now + 8000 + random(8000);
            if (quiet_break) cancel_head(now);
        } else {
            if (!was_idle_) next_glance_ = now + 8000 + random(8000);
            if (glance_return_ && now >= glance_return_) {
                value_.gaze_x = value_.gaze_y = 0; glance_return_ = 0;
                plan_head(now, 0, 1300);
            }
            if (!glance_return_ && now >= next_glance_) {
                const int direction = random(2) ? 1 : -1;
                value_.gaze_x = direction * (4 + random(3)); value_.gaze_y = random(5) - 2;
                ++value_.glances; glance_return_ = now + 2400 + random(1400);
                next_glance_ = now + 14000 + random(16000) + (80 - value_.curiosity) * 100;
                // Quiet expression changes also work offline; no new spoken turn.
                ambient_ = value_.comfort >= 65 && random(4) == 0 ? BuddyEmotion::Happy : BuddyEmotion::Curious;
                ambient_until_ = now + 4500;
                plan_head(now, direction * (40 + random(26)), 1500 + random(500));
            }
        }
        if (ambient_until_ && now >= ambient_until_) ambient_ = BuddyEmotion::Neutral;
        if (now >= reaction_until_ && sleep == BuddySleepStage::Awake) value_.emotion = ambient_;

        const bool gesture = speaking && sleep == BuddySleepStage::Awake && !blocked;
        if (gesture) {
            if (previous_phase_ != PresencePhase::Speaking) next_speech_ = now;
            if (now >= next_speech_) {
                if (random(4) != 0) speech_direction_ = -speech_direction_;
                const auto style = speech_style(value_.emotion);
                const int span = style.span * (75 + random(26)) / 100;
                plan_head(now, speech_direction_ * span, std::clamp(style.duration + random(400), 800, 2000));
                next_speech_ = now + style.interval + 500 + random(1700);
            }
        } else if (previous_phase_ == PresencePhase::Speaking) cancel_head(now);
        previous_phase_ = phase; was_idle_ = idle;

        // Asleep uses the existing closed-eye/Zzz renderer. No blinks wake it.
        if (sleep == BuddySleepStage::Asleep || blocked) {
            blink_started_ = -1; next_blink_ = now + 2500;
            value_.openness = 1;
        } else {
            if (now >= next_blink_) {
                const int pick = random(10);
                value_.blink = sleep == BuddySleepStage::Drowsy || value_.energy < 45 || pick == 0 ? PresenceBlink::Slow
                    : pick < 3 ? PresenceBlink::Double : PresenceBlink::Single;
                ++value_.blinks;
                if (value_.blink == PresenceBlink::Double) ++value_.double_blinks;
                if (value_.blink == PresenceBlink::Slow) ++value_.slow_blinks;
                blink_started_ = now;
                next_blink_ = now + (phase == PresencePhase::Listening ? 5000 : 3000) + random(5000);
            }
            value_.openness = blink_openness(value_.blink, blink_started_ < 0 ? -1 : now - blink_started_);
        }
        return value_;
    }
    static constexpr float blink_openness(PresenceBlink kind, int64_t elapsed)
    {
        if (elapsed < 0) return 1;
        const int duration = kind == PresenceBlink::Slow ? 400 : 160;
        if (kind == PresenceBlink::Double && elapsed >= 260) elapsed -= 260;
        if (elapsed >= duration) return 1;
        const float half = duration / 2.0f;
        return elapsed < half ? 1 - elapsed / half : (elapsed - half) / half;
    }
private:
    struct SpeechStyle { int span, duration, interval; };
    static constexpr SpeechStyle speech_style(BuddyEmotion emotion)
    {
        switch (emotion) {
        case BuddyEmotion::Happy: return {38, 900, 2200};
        case BuddyEmotion::Excited: return {48, 700, 1800};
        case BuddyEmotion::Angry: return {28, 700, 2900};
        case BuddyEmotion::Shy: return {20, 1500, 3800};
        case BuddyEmotion::Sad: case BuddyEmotion::Sleepy: return {18, 1600, 4000};
        case BuddyEmotion::Surprised: return {55, 800, 2500};
        case BuddyEmotion::Curious: return {34, 1200, 2800};
        default: return {24, 1100, 3000};
        }
    }
    constexpr int random(unsigned limit)
    {
        random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
        return static_cast<int>(random_ % limit);
    }
    constexpr void plan_head(int64_t now, int offset, int duration)
    {
        if (!++value_.head_sequence) ++value_.head_sequence;
        value_.head_offset_us = offset; value_.head_duration_ms = duration;
        value_.head_due_ms = now + 180; // Eyes establish intent first.
    }
    constexpr void cancel_head(int64_t now)
    {
        // Consume a cancellation without a later delayed return-to-center.
        if (!++value_.head_sequence) ++value_.head_sequence;
        value_.head_due_ms = now - 1000;
    }
    PresenceSnapshot value_{};
    uint32_t random_ = 1;
    int64_t last_tick_ = 0, next_minute_ = 60000, last_engagement_ = -1;
    int64_t next_blink_ = 3000, blink_started_ = -1, next_glance_ = 6000, glance_return_ = 0;
    int64_t next_speech_ = 0, reaction_until_ = 0, ambient_until_ = 0;
    BuddyEmotion reaction_ = BuddyEmotion::Neutral, ambient_ = BuddyEmotion::Neutral;
    PresencePhase previous_phase_ = PresencePhase::Idle;
    bool was_idle_ = true;
    int speech_direction_ = 1;
};

// Only the servo task consumes plans. Dropped plans never replay after a manual
// command, camera lease, sleep, update, busy motor or delayed scheduler tick.
class PresenceHeadGate {
public:
    constexpr bool take(const PresenceSnapshot &value, int64_t now, bool allowed, bool moving)
    {
        if (!allowed || moving || now > value.head_due_ms + 500) {
            seen_ = value.head_sequence; return false;
        }
        if (!value.head_sequence || seen_ == value.head_sequence || now < value.head_due_ms) return false;
        seen_ = value.head_sequence; return true;
    }
private:
    unsigned seen_ = 0;
};

constexpr unsigned presence_self_test_failure()
{
    using Phase = PresencePhase; using Sleep = BuddySleepStage;
    PresenceEngine e; e.begin(1000, 17);
    e.emotion(BuddyEmotion::Angry, 1000);
    if (e.update(1000, Phase::Idle, Sleep::Awake).emotion != BuddyEmotion::Angry
        || e.update(22000, Phase::Idle, Sleep::Awake).emotion == BuddyEmotion::Angry) return __LINE__;
    e.engage(22000); e.engage(22001);
    if (e.update(22001, Phase::Idle, Sleep::Awake).engagements != 1) return __LINE__;
    PresenceHeadGate gate;
    PresenceSnapshot p; p.head_sequence = 1; p.head_due_ms = 1000;
    if (gate.take(p, 999, true, false) || !gate.take(p, 1000, true, false) || gate.take(p, 1001, true, false)) return __LINE__;
    p.head_sequence = 2;
    if (gate.take(p, 999, false, false) || gate.take(p, 1000, true, false)) return __LINE__;
    p.head_sequence = 3;
    if (gate.take(p, 1600, true, false)) return __LINE__;
    p.head_sequence = 4;
    if (gate.take(p, 1000, true, true) || gate.take(p, 1001, true, false)) return __LINE__;
    if (PresenceEngine::blink_openness(PresenceBlink::Double, 80) != 0
        || PresenceEngine::blink_openness(PresenceBlink::Double, 200) != 1
        || PresenceEngine::blink_openness(PresenceBlink::Double, 340) != 0
        || PresenceEngine::blink_openness(PresenceBlink::Double, 500) != 1
        || PresenceEngine::blink_openness(PresenceBlink::Slow, 200) != 0) return __LINE__;
    e.begin(0, 1234);
    for (int64_t t = 0; t < 240000; t += 40) {
        const auto s = e.update(t, Phase::Idle, Sleep::Awake);
        if (s.openness < 0 || s.openness > 1 || s.head_offset_us < -65 || s.head_offset_us > 65
            || s.energy < 35 || s.energy > 90 || s.gaze_x < -6 || s.gaze_x > 6) return __LINE__;
    }
    auto s = e.update(240000, Phase::Idle, Sleep::Awake);
    if (!s.double_blinks || !s.slow_blinks || s.glances < 5 || s.glances > 15) return __LINE__;
    const unsigned glances = s.glances, blinks = s.blinks;
    const int energy = s.energy;
    for (int64_t t = 240040; t <= 420000; t += 1000) {
        s = e.update(t, Phase::Idle, Sleep::Asleep);
        if (s.glances != glances || s.blinks != blinks || s.gaze_x || s.gaze_y || s.emotion != BuddyEmotion::Sleepy) return __LINE__;
    }
    if (s.energy <= energy) return __LINE__;
    for (int64_t t = 420040; t < 480000; t += 1000) {
        s = e.update(t, Phase::Listening, Sleep::Awake);
        if (s.glances != glances || s.gaze_x || s.gaze_y) return __LINE__;
    }
    e.emotion(BuddyEmotion::Shy, 480000);
    s = e.update(480000, Phase::Speaking, Sleep::Awake);
    if (s.emotion != BuddyEmotion::Shy || s.head_offset_us < -20 || s.head_offset_us > 20) return __LINE__;
    e.emotion(BuddyEmotion::Excited, 484000);
    s = e.update(486000, Phase::Speaking, Sleep::Awake);
    if (s.emotion != BuddyEmotion::Excited || s.head_offset_us < -48 || s.head_offset_us > 48
        || (s.head_offset_us > -36 && s.head_offset_us < 36) || s.head_duration_ms < 800) return __LINE__;
    const auto before = s.head_sequence;
    s = e.update(489000, Phase::Idle, Sleep::Awake, true);
    if (s.head_sequence == before || s.gaze_x || s.gaze_y) return __LINE__;
    if (e.update(900000000, Phase::Idle, Sleep::Asleep).energy > 90) return __LINE__;
    e.begin(0, 42); IdleSleepEngine sleeper; sleeper.activity(0);
    unsigned sleeping_glances = 0;
    for (int64_t t = 0; t <= 600000; t += 1000) {
        const auto sleep = sleeper.snapshot(t);
        s = e.update(t, Phase::Idle, sleep.stage);
        if (t == 120000) sleeping_glances = s.glances;
        if (t > 120000 && s.glances != sleeping_glances) return __LINE__;
        if (s.engagements || sleep.idle_ms != t) return __LINE__;
    }
    s = e.update(600001, Phase::Idle, Sleep::Awake);
    const auto wake_glances = s.glances;
    if (e.update(600002, Phase::Idle, Sleep::Awake).glances != wake_glances) return __LINE__;
    e.begin(0, 42);
    s = e.update(6000, Phase::Idle, Sleep::Awake);
    if (s.head_due_ms != 6180 || !s.gaze_x || s.glances != 1) return __LINE__;
    return 0;
}
constexpr bool presence_self_test() { return presence_self_test_failure() == 0; }
static_assert(presence_self_test_failure() == 0, "Presence must respect sleep, interaction, gesture bounds, priority, timing and blink variation");

bool buddy_presence_start();
PresenceSnapshot buddy_presence_snapshot();
void buddy_presence_engage();
void buddy_presence_emotion(BuddyEmotion emotion);
bool buddy_presence_selftest_pass();
