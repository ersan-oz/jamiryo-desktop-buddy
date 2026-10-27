#pragma once
#include <cstdint>

enum class BuddySleepStage : uint8_t { Awake, Drowsy, Asleep };
inline const char *buddy_sleep_name(BuddySleepStage stage)
{
    switch (stage) {
    case BuddySleepStage::Drowsy: return "drowsy";
    case BuddySleepStage::Asleep: return "asleep";
    default: return "awake";
    }
}
struct BuddySleepSnapshot {
    BuddySleepStage stage;
    int64_t idle_ms;
    bool mumble_due, mumble_used, demo;
};

// No clocks, network or display dependencies: sleep is a local character state.
class IdleSleepEngine {
public:
    static constexpr int64_t drowsy_ms = 120000, asleep_ms = 180000, mumble_ms = 300000;
    void activity(int64_t now) { last_activity_ = now; mumble_used_ = demo_ = false; }
    void demo(int64_t now) { activity(now); demo_ = true; }
    BuddySleepSnapshot snapshot(int64_t now)
    {
        if (demo_ && now - last_activity_ >= 20000) activity(now);
        const int64_t idle = now >= last_activity_ ? now - last_activity_ : 0;
        const int64_t drowsy = demo_ ? 3000 : drowsy_ms;
        const int64_t asleep = demo_ ? 7000 : asleep_ms;
        const int64_t mumble = demo_ ? 11000 : mumble_ms;
        return {idle >= asleep ? BuddySleepStage::Asleep : idle >= drowsy ? BuddySleepStage::Drowsy : BuddySleepStage::Awake,
                idle, idle >= mumble && !mumble_used_, mumble_used_, demo_};
    }
    bool take_mumble(int64_t now)
    {
        if (!snapshot(now).mumble_due) return false;
        mumble_used_ = true;
        return true;
    }
private:
    int64_t last_activity_ = 0;
    bool mumble_used_ = false, demo_ = false;
};

inline bool idle_sleep_self_test()
{
    IdleSleepEngine engine; engine.activity(1000);
    if (engine.snapshot(120999).stage != BuddySleepStage::Awake
        || engine.snapshot(121000).stage != BuddySleepStage::Drowsy
        || engine.snapshot(180999).stage != BuddySleepStage::Drowsy
        || engine.snapshot(181000).stage != BuddySleepStage::Asleep
        || engine.take_mumble(300999) || !engine.take_mumble(301000)
        || engine.take_mumble(900000)) return false;
    // The robot's own mumble must not reset the idle timer or repeat on reconnect.
    if (engine.snapshot(900000).stage != BuddySleepStage::Asleep) return false;
    engine.activity(900001);
    if (engine.snapshot(900001).stage != BuddySleepStage::Awake
        || engine.snapshot(900001).mumble_due || !engine.take_mumble(1200001)) return false;
    // Diagnostic speed-up cannot persist or override a real wake-up.
    engine.demo(1300000);
    if (engine.snapshot(1303000).stage != BuddySleepStage::Drowsy
        || engine.snapshot(1307000).stage != BuddySleepStage::Asleep
        || !engine.take_mumble(1311000)
        || engine.snapshot(1320000).stage != BuddySleepStage::Awake
        || engine.snapshot(1320000).demo) return false;
    engine.demo(1400000); engine.activity(1408000);
    return engine.snapshot(1411000).stage == BuddySleepStage::Awake
        && !engine.snapshot(1411000).demo && !engine.take_mumble(1411000);
}

bool buddy_idle_start();
void buddy_idle_activity();
BuddySleepSnapshot buddy_idle_snapshot();
bool buddy_idle_take_mumble();
bool buddy_idle_command(const char *line);
void buddy_idle_status();
