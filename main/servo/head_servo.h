#pragma once
#include <cstdint>
#include "head_turn.h"
#include "cJSON.h"

bool buddy_head_start();
bool buddy_head_command(const char *line);
void buddy_head_status();
bool buddy_head_turn(BuddyHeadDirection direction);
bool buddy_head_scan();
bool buddy_head_parse_scan(const cJSON *args);
bool buddy_head_parse_turn(const cJSON *args, BuddyHeadDirection &direction);
struct BuddyHeadSnapshot {
    bool available, moving, manual, voice_selftest;
    int pulse_us, target_us, requested_degrees;
    unsigned voice_requests;
    const char *mode;
    bool scan_active, actions_selftest;
    unsigned scan_requests, scans_completed, gesture_moves;
    const char *routine_phase;
};
BuddyHeadSnapshot buddy_head_snapshot();
// Diagnostic eyes follow the same eased pulse plan, without shaft feedback.
bool buddy_head_test_gaze(int &gaze_x);
// Ignore robot-generated IMU/scene changes during motion and settling.
bool buddy_head_motion_recent();
// Photo capture holds the current position without waking the idle engine.
void buddy_head_photo_hold();
// Exclusive, RAM-only object-search lease. Manual commands replace it.
bool buddy_head_object_begin(unsigned token);
bool buddy_head_object_move(unsigned token, int pulse_us);
bool buddy_head_object_current(unsigned token);
bool buddy_head_object_settled(unsigned token);
bool buddy_head_object_hold(unsigned token);
void buddy_head_object_cancel(unsigned token);
// Anonymous, local face search on the first recognized speech of a conversation.
void buddy_head_note_conversation();
unsigned buddy_head_person_generation(); // Nonzero only while a stationary view needs samples.
void buddy_head_person_observe(bool valid, int x_milli, int64_t captured_ms, unsigned generation);
struct BuddyPersonSnapshot {
    bool active, selftest;
    const char *phase;
    unsigned starts, matched, not_found, canceled, frames;
};
BuddyPersonSnapshot buddy_head_person_snapshot();

struct BuddyHeadFollowSettings { bool enabled, reverse; };
BuddyHeadFollowSettings buddy_head_follow_settings();
bool buddy_head_set_follow(bool enabled, bool reverse);
bool buddy_head_follow_scanning();
unsigned buddy_head_follow_generation();
void buddy_head_follow_observe(bool valid, int x_milli, int64_t observed_ms, unsigned generation);
bool buddy_head_follow_gaze(int &gaze_x);
