#pragma once
#include <algorithm>
#include <cstdint>

namespace conversation_policy {
inline constexpr int64_t autonomous_quiet_ms = 45000;
inline constexpr int64_t stale_server_ms = 60000;
inline constexpr int context_trigger_tokens = 32768, context_target_tokens = 16384;
constexpr bool recover_silent_turn(bool speech_confirmed, int64_t now, int64_t last_server)
{
    return speech_confirmed || (last_server > 0 && now >= last_server && now - last_server >= stale_server_ms);
}

// User speech and the reply both reserve the conversational floor. A pause
// between follow-up questions is not permission to start a sensor conversation.
constexpr bool allow_autonomous(int64_t now, int64_t last_input, int64_t last_reply)
{
    const int64_t latest = std::max(last_input, last_reply);
    return latest <= 0 || (now >= latest && now - latest >= autonomous_quiet_ms);
}

constexpr bool self_test()
{
    return allow_autonomous(0, 0, 0)
        && !allow_autonomous(3000, 1000, 0)
        && !allow_autonomous(45999, 1000, 0)
        && allow_autonomous(46000, 1000, 0)
        && !allow_autonomous(46000, 1000, 5000)
        && allow_autonomous(50000, 1000, 5000)
        && !allow_autonomous(1000, 2000, 0)
        && !allow_autonomous(100000, 99000, 5000)
        && !recover_silent_turn(false, 21000, 1000)
        && recover_silent_turn(true, 21000, 1000)
        && !recover_silent_turn(false, 60999, 1000)
        && recover_silent_turn(false, 61000, 1000)
        && context_target_tokens > 0 && context_trigger_tokens > context_target_tokens;
}
static_assert(self_test(), "Autonomous observations must respect an ongoing user conversation");
}
