#pragma once
#include <cstdint>
#include "cJSON.h"
#include "memory/user_memory.h"

struct BuddyFaceEvent {
    enum class Type { Enrolled, EnrollmentFailed, Seen } type = Type::Seen;
    char name[48]{}, reason[64]{};
    int64_t time_ms = 0;
};
bool buddy_faces_start(BuddyMemoryReply reply); // Local storage; models load only in camera task.
void buddy_faces_tick(); // Only the camera task owns models and storage mutations.
bool buddy_faces_wants_frame(int64_t now, bool otherwise_idle);
bool buddy_faces_wants_tracking_frame(int64_t now);
bool buddy_faces_tracking_enabled();
bool buddy_faces_wants_person_frame(int64_t now);
bool buddy_faces_exclusive_capture();
void buddy_faces_process(uint8_t *rgb565, unsigned width, unsigned height, bool recognize = true,
                         unsigned person_generation = 0, int64_t captured_ms = 0);
bool buddy_faces_take_event(BuddyFaceEvent &event);
bool buddy_faces_has_profile(const char *name);
bool buddy_faces_seen_current(const BuddyFaceEvent &event);
bool buddy_faces_is_tool(const char *name);
bool buddy_faces_queue_tool(const char *name, const char *id, const cJSON *args);
void buddy_faces_add_tools(cJSON *declarations);
void buddy_faces_cancel_session();
bool buddy_faces_command(const char *line);
void buddy_faces_status();
