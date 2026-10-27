#pragma once
#include <algorithm>
#include <cstdint>

namespace live_health {
constexpr int64_t reply_timeout_ms = 20000;
constexpr int64_t renewal_age_ms = 8 * 60 * 1000;

// Transient failures never turn off a companion left running overnight.
// Clamp before shifting so an arbitrarily long outage remains safe.
constexpr int64_t retry_delay_ms(unsigned failures)
{
    return std::min<int64_t>(60000, 5000LL << std::min(failures ? failures - 1 : 0, 4U));
}
constexpr bool reply_stalled(int64_t now, int64_t waiting_since, bool receiving_speech)
{
    return waiting_since > 0 && !receiving_speech && now - waiting_since >= reply_timeout_ms;
}
constexpr bool renew_when_idle(int64_t now, int64_t connected_at, bool idle)
{
    return connected_at > 0 && idle && now - connected_at >= renewal_age_ms;
}

inline bool self_test()
{
    // No false recovery during silence or while a person is still speaking.
    if (reply_stalled(900000, 0, false) || reply_stalled(21000, 1000, true)
        || reply_stalled(20999, 1000, false) || !reply_stalled(21000, 1000, false)) return false;
    // A missing first packet, a late complete response and planned rotation.
    if (renew_when_idle(481000, 1000, false) || renew_when_idle(480999, 1000, true)
        || !renew_when_idle(481000, 1000, true)) return false;
    // More than five failures, including overflow-sized counters, keep retrying.
    return retry_delay_ms(1) == 5000 && retry_delay_ms(5) == 60000
        && retry_delay_ms(1000) == 60000 && retry_delay_ms(UINT32_MAX) == 60000;
}
} // namespace live_health
