#pragma once
#include <cstddef>
#include <cstdint>
#include "cJSON.h"

enum class BuddyMessageMode { Ask, Say, Photo };
struct BuddyMessageRequest { char id[33]{}, text[561]{}; BuddyMessageMode mode = BuddyMessageMode::Ask; };
struct BuddyMessageResult { char id[33]{}, text[2049]{}, error[48]{}; bool audio = false, truncated = false, transcript = false; };
bool buddy_message_decode(const cJSON *object, double server_now, BuddyMessageRequest &request);
bool buddy_messenger_start();
bool buddy_messenger_queue(const BuddyMessageRequest &request);
bool buddy_messenger_take(BuddyMessageRequest &request);
// Reserve the conversational floor from queue acceptance through playback/result.
bool buddy_messenger_pending();
bool buddy_messenger_generating();
bool buddy_messenger_reading();
void buddy_messenger_text(const char *text);
void buddy_messenger_audio();
// False for an empty previous realtime turn: the dashboard reply is still due.
bool buddy_messenger_complete();
void buddy_messenger_fail(const char *reason);
bool buddy_messenger_result(BuddyMessageResult &result, bool output_idle);
void buddy_messenger_release();
struct BuddyMessengerSnapshot {
    bool available, selftest; const char *phase; unsigned accepted, completed, failures;
    unsigned empty_boundaries = 0;
    int64_t phase_age_ms = 0, failure_age_ms = -1;
    char last_failure[48]{};
};
BuddyMessengerSnapshot buddy_messenger_snapshot();
