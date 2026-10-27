#pragma once
#include "cJSON.h"
#include "sensors/camera.h"

using BuddyObjectControl = bool (*)(cJSON *); // Consumes the JSON, like other tool workers.
using BuddyObjectImage = bool (*)(const BuddyVisionFrame &);
bool buddy_object_start(BuddyObjectControl control, BuddyObjectImage image);
void buddy_object_add_tools(cJSON *declarations);
bool buddy_object_is_tool(const char *name);
bool buddy_object_queue_tool(const char *name, const char *id, const cJSON *args);
void buddy_object_tick(); // Cooperative network worker: no camera/servo blocking.
void buddy_object_cancel_session();
bool buddy_object_busy();
struct BuddyObjectSnapshot {
    bool active, selftest;
    const char *state;
    unsigned searches, panoramas, aimed, misses, failures;
};
BuddyObjectSnapshot buddy_object_snapshot();
