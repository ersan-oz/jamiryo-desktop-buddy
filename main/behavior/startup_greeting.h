#pragma once
#include <cstdint>

// RAM-only, once per boot. A reconnect cannot re-arm a greeting. A person or
// dashboard starting a conversation takes priority over a late greeting.
class StartupGreeting {
public:
    enum class State { Waiting, Requested, Skipped };
    constexpr bool take(int64_t uptime_ms, bool ready, bool quiet, bool interacted)
    {
        if (state_ != State::Waiting) return false;
        if (interacted || uptime_ms >= 120000) { state_ = State::Skipped; return false; }
        if (uptime_ms < 10000 || !ready || !quiet) return false;
        state_ = State::Requested; return true;
    }
    constexpr State state() const { return state_; }
private:
    State state_ = State::Waiting;
};
constexpr bool startup_greeting_self_test()
{
    StartupGreeting g;
    if (g.take(9999, true, true, false) || g.take(10000, false, true, false)
        || g.take(15000, true, false, false) || !g.take(16000, true, true, false)
        || g.take(17000, true, true, false) || g.take(50000, false, true, false)
        || g.take(51000, true, true, false)) return false;
    StartupGreeting busy;
    if (busy.take(9000, false, false, true) || busy.take(10000, true, true, false)
        || busy.state() != StartupGreeting::State::Skipped) return false;
    StartupGreeting late;
    return !late.take(120000, false, true, false) && !late.take(130000, true, true, false)
        && late.state() == StartupGreeting::State::Skipped;
}
static_assert(startup_greeting_self_test(), "Startup speech must be once per boot, ready/quiet gated and never replayed on reconnect");
