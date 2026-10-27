#pragma once
#include <cstddef>
#include <cstdint>

struct BuddyVisionFrame {
    uint8_t *jpeg = nullptr; // Ownership transfers only if offer_vision succeeds.
    size_t bytes = 0;
    int64_t captured_ms = 0;
    bool requested = false;
    unsigned change_score = 0;
};

bool buddy_camera_init(); // Must precede both IMU and audio I2C initialization.
bool buddy_camera_start();
bool buddy_camera_available();
bool buddy_camera_enabled();
bool buddy_camera_observing();
struct BuddyCameraSnapshot {
    bool available, enabled, orientation_verified, rotation_selftest;
    unsigned rotation_degrees, captured, photos, rotated_frames, rotation_max_us;
};
BuddyCameraSnapshot buddy_camera_snapshot();
bool buddy_camera_request();
struct BuddySearchCapture {
    unsigned search_id = 0, view = 0;
    bool ok = false;
    BuddyVisionFrame frame{}; // Only the third result owns a JPEG panorama.
};
bool buddy_camera_search_request(unsigned search_id, unsigned view);
bool buddy_camera_search_result(BuddySearchCapture &result);
void buddy_camera_search_cancel(unsigned search_id);
// Worker-only blocking capture; caller owns jpeg and must wipe/free it.
// This separate path does not send the photo to Gemini.
bool buddy_camera_take_photo(BuddyVisionFrame &frame, uint32_t timeout_ms = 6000);
bool buddy_camera_command(const char *line);
void buddy_camera_status();
