#pragma once
#include <cstddef>
#include <cstdint>
#include "motion_engine.h"
#include "sensors/camera.h"
#include "behavior/emotions.h"

bool buddy_ai_start();
bool buddy_ai_configured();
bool buddy_ai_ready();
struct BuddyAiSnapshot {
    bool ready, conversation_guard, resume_available, conversation_selftest;
    unsigned sessions, recoveries;
    int64_t last_input_age_ms, last_reply_age_ms;
    const char *state;
    unsigned failures, retry_ms, internal_free, internal_largest, failure_free, failure_largest;
    int socket_error;
    const char *socket_stage;
    unsigned confirmed_inputs, ignored_vad_turns;
};
BuddyAiSnapshot buddy_ai_snapshot();
bool buddy_ai_input_allowed();
bool buddy_ai_push_microphone(const int16_t *samples, size_t count);
void buddy_ai_note_speech(bool new_utterance);
void buddy_ai_pause_microphone(bool expect_reply = false);
bool buddy_ai_output_pending();
bool buddy_ai_response_active();
BuddyEmotion buddy_ai_emotion();
bool buddy_ai_waiting_reply();
size_t buddy_ai_output_buffered();
size_t buddy_ai_read_output(int16_t *samples, size_t capacity);
void buddy_ai_motion(BuddyEvent event);
bool buddy_ai_command(const char *line);
void buddy_ai_status();
bool buddy_ai_offer_vision(BuddyVisionFrame frame);
bool buddy_ai_scene_allowed();
