#pragma once
#include <cstdint>

// Optional 128x64 SH1106-class OLED on the existing audio I2C bus.
bool buddy_display_start();
bool buddy_display_command(const char *line);
void buddy_display_status();

// Non-blocking signals; only the display task writes the shared audio I2C bus.
void buddy_display_photo_begin();
void buddy_display_photo_end(bool captured);
void buddy_display_pickup();

struct BuddyDisplaySnapshot {
    bool available, renderer_selftest, pickup_visible;
    unsigned frames, write_errors, pickups;
    int64_t last_pickup_age_ms;
};
BuddyDisplaySnapshot buddy_display_snapshot();
