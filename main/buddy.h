#pragma once
#include "motion_engine.h"
#include "audio/voice_effect.h"

bool buddy_start();
bool buddy_healthy();
bool buddy_motion_selftest_pass();
struct BuddyMotionSnapshot {
    float dynamic_g, rotation_dps, peak_dynamic_g, peak_rotation_dps;
    bool output_suppressed, microphone_active, servo_suppressed;
    unsigned samples, moves, shakes, tilts;
    int64_t sample_age_ms;
};
BuddyMotionSnapshot buddy_motion_snapshot();
bool buddy_audio_busy();
void buddy_say(BuddyEvent event);
void buddy_set_voice_style(VoiceStyle style);
VoiceStyle buddy_voice_style();
