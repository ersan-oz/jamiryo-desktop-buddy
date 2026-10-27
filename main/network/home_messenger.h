#pragma once
#include <cstddef>
#include <cstdint>
#include "cJSON.h"

enum class BuddyMessageMode { Ask, Say, Photo };
struct BuddyMessageRequest { char id[33]{}, text[561]{}; BuddyMessageMode mode = BuddyMessageMode::Ask; };
struct BuddyMessageResult { char id[33]{}, text[2049]{}, error[48]{}; bool audio = false, truncated = false; };
bool buddy_message_decode(const cJSON *object, double server_now, BuddyMessageRequest &request);
bool buddy_messenger_start();
bool buddy_messenger_queue(const BuddyMessageRequest &request);
bool buddy_messenger_take(BuddyMessageRequest &request);
bool buddy_messenger_generating();
bool buddy_messenger_reading();
void buddy_messenger_text(const char *text);
void buddy_messenger_audio();
void buddy_messenger_complete();
void buddy_messenger_fail(const char *reason);
bool buddy_messenger_result(BuddyMessageResult &result, bool output_idle);
void buddy_messenger_release();
struct BuddyMessengerSnapshot { bool available, selftest; const char *phase; unsigned accepted, completed, failures; };
BuddyMessengerSnapshot buddy_messenger_snapshot();
