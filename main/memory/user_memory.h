#pragma once
#include "cJSON.h"

using BuddyMemoryReply = bool (*)(cJSON *); // Sink always consumes the JSON.
bool buddy_memory_start(BuddyMemoryReply reply);
void buddy_memory_add_tools(cJSON *declarations);
cJSON *buddy_memory_context(); // Caller owns the bounded data snapshot.
bool buddy_memory_is_tool(const char *name);
bool buddy_memory_queue_tool(const char *name, const char *id, const cJSON *args);
void buddy_memory_cancel_session();
bool buddy_memory_take_refresh();
bool buddy_memory_command(const char *line);
void buddy_memory_status();
