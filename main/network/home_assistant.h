#pragma once
#include <cstddef>
#include "cJSON.h"
#include "memory/user_memory.h"

bool buddy_home_start(BuddyMemoryReply reply, const char *legacy_key);
void buddy_home_apply_configuration(char *ssid, size_t ssid_capacity, char *password, size_t password_capacity,
                                    char *key, size_t key_capacity);
struct BuddyHomeSnapshot { bool storage_ready, selftest, gemini_configured; unsigned schema, profiles, worker_stack_min_bytes; };
BuddyHomeSnapshot buddy_home_snapshot();
void buddy_home_add_tools(cJSON *declarations);
bool buddy_home_is_tool(const char *name);
bool buddy_home_queue_tool(const char *name, const char *id, const cJSON *args);
void buddy_home_cancel_session();
bool buddy_home_command(const char *line);
void buddy_home_status();
void buddy_home_setup_tick(bool wifi_connected);
