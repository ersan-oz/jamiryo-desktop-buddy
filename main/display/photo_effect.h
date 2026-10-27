#pragma once
#include "face_renderer.h"

// Camera timestamps drive this temporary face; it never changes the AI mood.
inline void render_photo_face(uint8_t *frame, int64_t focus_ms, int64_t shutter_ms)
{
    if (shutter_ms >= 80 && shutter_ms < 160) {
        std::memset(frame, 0xff, 1024); // Two display frames of a camera flash.
        return;
    }
    FaceCanvas canvas(frame);
    FacePose pose; pose.width = 28; pose.left_height = pose.right_height = 24;
    float openness = 0.78f;
    if (shutter_ms >= 0) {
        pose.width = 32; pose.left_height = pose.right_height = 34;
        openness = shutter_ms < 80 ? 1.0f - shutter_ms / 80.0f
            : std::clamp((shutter_ms - 160) / 140.0f, 0.0f, 1.0f);
    } else {
        // The small pulse and corner marks read as focusing on a subject.
        pose.left_height = pose.right_height = 22 + 2 * std::sin(focus_ms * 0.015f);
        for (int x : {8, 119}) for (int y : {8, 55}) {
            canvas.line(x, y, x + (x < 64 ? 8 : -8), y);
            canvas.line(x, y, x, y + (y < 32 ? 8 : -8));
        }
    }
    canvas.eye(36, 32, pose, true, openness); canvas.eye(92, 32, pose, false, openness);
}
