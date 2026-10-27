#pragma once
#include <cstdint>

namespace buddy_faces {
// Camera task owns this RAM-only schedule; no periodic NVS writes.
template<unsigned People> class GreetingCadence {
public:
    static constexpr int64_t person_gap_ms = 5 * 60 * 1000;
    static constexpr int64_t global_gap_ms = 90 * 1000;

    constexpr bool due(int person, int64_t now) const
    {
        return person >= 0 && static_cast<unsigned>(person) < People && now >= 0
            && (!any_ || now - last_any_ >= global_gap_ms)
            && (!seen_[person] || now - last_person_[person] >= person_gap_ms);
    }
    constexpr bool record(int person, int64_t now)
    {
        if (!due(person, now)) return false;
        seen_[person] = true; last_person_[person] = now;
        any_ = true; last_any_ = now; return true;
    }
    constexpr void reset_profiles()
    {
        // Enrollment/deletion can shift profile indexes; retain global quiet time.
        for (unsigned i = 0; i < People; ++i) seen_[i] = false;
    }
private:
    bool any_ = false, seen_[People]{};
    int64_t last_any_ = 0, last_person_[People]{};
};

constexpr bool greeting_cadence_self_test()
{
    GreetingCadence<2> schedule;
    if (schedule.due(-1, 0) || schedule.due(2, 0) || schedule.due(0, -1)) return false;
    if (!schedule.record(0, 1000) || schedule.due(0, 1001)) return false;
    if (schedule.due(1, 90999) || !schedule.record(1, 91000)) return false;
    if (schedule.due(0, 300999) || !schedule.record(0, 301000)) return false;
    if (schedule.due(1, 390999) || !schedule.record(1, 391000)) return false;
    if (schedule.due(0, 0) || schedule.record(2, 999999)) return false;
    schedule.reset_profiles();
    return !schedule.due(0, 480999) && schedule.record(0, 481000);
}
static_assert(greeting_cadence_self_test(), "Face greeting cooldown regression");
} // namespace buddy_faces
