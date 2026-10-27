#pragma once
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>

// Compare coarse spatial structure after removing global exposure changes.
// Two changed frames avoid a single noisy frame triggering a cloud reaction.
class BuddySceneChange {
public:
    static constexpr size_t cells = 32 * 24;
    using Grid = std::array<uint8_t, cells>;
    unsigned update(const Grid &grid)
    {
        if (!ready_) { baseline_ = grid; ready_ = true; return 0; }
        int global = 0;
        for (size_t i = 0; i < cells; ++i) global += int(grid[i]) - baseline_[i];
        global /= int(cells);
        unsigned total = 0, changed = 0;
        for (size_t i = 0; i < cells; ++i) {
            const unsigned difference = std::abs(int(grid[i]) - baseline_[i] - global);
            total += difference;
            if (difference >= 18) ++changed;
        }
        const unsigned score = total / cells;
        if (score >= 12 && changed >= cells / 5) {
            if (++confirmations_ < 2) return 0;
            baseline_ = grid; confirmations_ = 0; return score;
        }
        baseline_ = grid; confirmations_ = 0; return 0;
    }
    void reset() { ready_ = false; confirmations_ = 0; }
private:
    Grid baseline_{};
    unsigned confirmations_ = 0;
    bool ready_ = false;
};

inline bool buddy_scene_self_test()
{
    auto detector = std::make_unique<BuddySceneChange>();
    BuddySceneChange::Grid grid{};
    grid.fill(70);
    if (detector->update(grid)) return false;
    grid.fill(100); // Exposure change alone must remain quiet.
    if (detector->update(grid)) return false;
    for (size_t i = 0; i < grid.size(); ++i) grid[i] = 100 + (i % 3) - 1;
    if (detector->update(grid)) return false; // Small sensor noise.
    std::fill_n(grid.begin(), grid.size() / 3, uint8_t(210));
    if (detector->update(grid)) return false; // Wait for confirmation.
    if (!detector->update(grid)) return false; // A new stable foreground object.
    if (detector->update(grid)) return false; // No repeat for an unchanged scene.
    detector->reset();
    return detector->update(grid) == 0;
}
