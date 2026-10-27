#pragma once
#include "head_motion.h"

namespace head_settings {
// Commissioning tests are explicit commands; a firmware restart never moves
// the head unexpectedly, including after an OTA update.
inline constexpr bool boot_test_enabled = false;
}

class HeadBootTest {
public:
    static constexpr int64_t delay_ms = 10000;
    constexpr explicit HeadBootTest(bool enabled = true) : pending_(enabled) {}
    constexpr bool pending() const { return pending_; }
    constexpr void cancel() { pending_ = false; }
    constexpr bool take_due(int64_t uptime_ms)
    {
        if (!pending_ || uptime_ms < delay_ms) return false;
        pending_ = false;
        return true;
    }
private:
    bool pending_;
};

inline constexpr bool head_boot_test_self_test()
{
    HeadBootTest boundary;
    if (boundary.take_due(9999) || !boundary.pending()
        || !boundary.take_due(10000) || boundary.take_due(10000)
        || boundary.take_due(999999)) return false;
    HeadBootTest canceled; canceled.cancel();
    HeadBootTest disabled(false);
    if (canceled.take_due(10000) || disabled.take_due(10000)) return false;
    // Run the actual movement plan: no early pulses, one bounded sweep, no
    // restart after completing the sweep even if the device remains powered.
    HeadBootTest boot;
    HeadMotion head;
    unsigned starts = 0;
    for (int64_t now = 0; now <= 60000; now += 20) {
        if (boot.take_due(now)) {
            if (now != 10000) return false;
            head.command(HeadMode::Test, now); ++starts;
        }
        head.update(now);
        if (head.test_gaze_x() < -12 || head.test_gaze_x() > 12) return false;
        if (now == 11700 && head.test_gaze_x() != -12) return false;
        if (now == 13100 && head.test_gaze_x() != 12) return false;
        if (now >= 14300 && head.test_gaze_x() != 0) return false;
        if (now < 10000 && head.mode() != HeadMode::Off) return false;
        if (now >= 10000 && now < 14900 && head.mode() != HeadMode::Test) return false;
        if (now >= 14900 && head.mode() != HeadMode::Off) return false;
        if (head.pulse() < HeadMotion::minimum_us || head.pulse() > HeadMotion::maximum_us) return false;
    }
    return starts == 1 && !boot.pending() && !head.moving()
        && head.pulse() == HeadMotion::center_us;
}

static_assert(head_boot_test_self_test(), "Delayed servo test must be bounded, cancelable and run once per boot");
