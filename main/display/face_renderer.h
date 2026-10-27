#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "behavior/emotions.h"

enum class FaceActivity : uint8_t { Idle, Listening, Thinking, Speaking };
inline const char *face_activity_name(FaceActivity activity)
{
    const char *names[] = {"idle", "listening", "thinking", "speaking"};
    return names[static_cast<unsigned>(activity)];
}

struct FacePose {
    float width = 30, left_height = 30, right_height = 30, corner = 6;
    float skew = 0, smile = 0, blush = 0, dizzy = 0, sparkle = 0;
    void approach(const FacePose &target, float speed = 0.22f)
    {
        width += speed * (target.width - width);
        left_height += speed * (target.left_height - left_height);
        right_height += speed * (target.right_height - right_height);
        corner += speed * (target.corner - corner);
        skew += speed * (target.skew - skew);
        smile += speed * (target.smile - smile);
        blush += speed * (target.blush - blush);
        dizzy += speed * (target.dizzy - dizzy);
        sparkle += speed * (target.sparkle - sparkle);
    }
};

inline FacePose face_pose(BuddyEmotion emotion, FaceActivity activity = FaceActivity::Idle)
{
    FacePose pose;
    switch (emotion) {
    case BuddyEmotion::Happy: pose.left_height = pose.right_height = 26; pose.smile = 1; break;
    case BuddyEmotion::Angry: pose.left_height = pose.right_height = 24; pose.skew = 14; pose.corner = 4; break;
    case BuddyEmotion::Shy: pose.width = 25; pose.left_height = pose.right_height = 13; pose.corner = 6; pose.blush = 1; break;
    case BuddyEmotion::Surprised: pose.width = 34; pose.left_height = pose.right_height = 40; pose.corner = 17; break;
    case BuddyEmotion::Sad: pose.left_height = pose.right_height = 22; pose.skew = -12; break;
    case BuddyEmotion::Excited: pose.width = 32; pose.left_height = pose.right_height = 32; pose.smile = 1; pose.sparkle = 1; break;
    case BuddyEmotion::Sleepy: pose.width = 29; pose.left_height = pose.right_height = 8; break;
    case BuddyEmotion::Dizzy: pose.width = 26; pose.left_height = pose.right_height = 26; pose.dizzy = 1; break;
    case BuddyEmotion::Curious: pose.left_height = 32; pose.right_height = 23; break;
    default: break;
    }
    if (activity == FaceActivity::Listening) {
        pose.width += 2; pose.left_height += 3; pose.right_height += 3;
    } else if (activity == FaceActivity::Thinking) {
        pose.left_height *= 0.75f; pose.right_height += 3;
    }
    return pose;
}

// Same bounded rasterizer is used for live frames and USB visual previews.
class FaceCanvas {
public:
    explicit FaceCanvas(uint8_t *frame) : frame_(frame) { std::memset(frame_, 0, 1024); }
    void pixel(int x, int y)
    {
        if (x >= 0 && x < 128 && y >= 0 && y < 64)
            frame_[(y / 8) * 128 + x] |= static_cast<uint8_t>(1u << (y % 8));
    }
    void line(int x, int y, int x2, int y2)
    {
        const int dx = std::abs(x2 - x), sx = x < x2 ? 1 : -1;
        const int dy = -std::abs(y2 - y), sy = y < y2 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            pixel(x, y);
            if (x == x2 && y == y2) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x += sx; }
            if (twice <= dx) { error += dx; y += sy; }
        }
    }
    void sleep_marks(int64_t ms)
    {
        constexpr uint8_t capital[7] = {31, 1, 2, 4, 8, 16, 31};
        constexpr uint8_t small[7] = {0, 0, 15, 1, 2, 4, 15};
        const int rise = static_cast<int>((ms % 3000) / 1000);
        for (int letter = 0; letter < 3; ++letter) {
            const auto *glyph = letter == 0 ? capital : small;
            const int x0 = 88 + letter * 9;
            const int y0 = 18 - letter * 5 - rise;
            for (int y = 0; y < 7; ++y) for (int x = 0; x < 5; ++x)
                if (glyph[y] & (1u << (4 - x))) pixel(x0 + x, y0 + y);
        }
    }
    void at_eye(int cx, int cy)
    {
        // A 15x15 @ glyph enlarged 2x: outer curl, enclosed pupil and tail.
        constexpr uint16_t glyph[] = {
            0b000001111100000,
            0b000110000011000,
            0b001000000000100,
            0b010000000000010,
            0b010001110110010,
            0b100010001010001,
            0b100100001010001,
            0b100100001010001,
            0b100100001010001,
            0b100010011010010,
            0b010001101101100,
            0b010000000000000,
            0b001000000000100,
            0b000110000011000,
            0b000001111100000,
        };
        for (int y = 0; y < 15; ++y) for (int x = 0; x < 15; ++x) {
            if (!(glyph[y] & (1u << (14 - x)))) continue;
            for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
                pixel(cx - 15 + x * 2 + dx, cy - 15 + y * 2 + dy);
        }
    }
    void eye(int cx, int cy, const FacePose &pose, bool left, float openness)
    {
        const float w = pose.width, h = std::max(2.0f, (left ? pose.left_height : pose.right_height) * openness);
        const float hw = w / 2, hh = h / 2;
        const float radius = std::min({pose.corner, hw, hh});
        for (int y = -static_cast<int>(std::ceil(hh)); y <= static_cast<int>(std::ceil(hh)); ++y) {
            for (int x = -static_cast<int>(std::ceil(hw)); x <= static_cast<int>(std::ceil(hw)); ++x) {
                if (std::abs(x) > hw || std::abs(y) > hh) continue;
                const float dx = std::max(std::abs(x) - (hw - radius), 0.0f);
                const float dy = std::max(std::abs(y) - (hh - radius), 0.0f);
                if (dx * dx + dy * dy > radius * radius) continue;
                const float u = x / hw;
                const float curvature = std::min(12.0f, h * 0.45f) * u * u;
                const float slant = openness * (std::abs(pose.skew) / 2 + (left ? 1 : -1) * pose.skew * x / w);
                const float top = -hh + slant + pose.smile * curvature;
                const float bottom = hh * (1 - pose.smile) + pose.smile * (-hh + curvature + std::min(7.0f, h));
                if (y < top || y > bottom) continue;
                if (pose.dizzy > 0.5f && openness > 0.3f) {
                    // Crossed eyes remain legible at this small resolution.
                    if (std::abs(y - x * h / w) > 2 && std::abs(y + x * h / w) > 2) continue;
                }
                pixel(cx + x, cy + y);
            }
        }
    }
private:
    uint8_t *frame_;
};

inline void render_pickup_face(uint8_t *frame, int64_t elapsed_ms)
{
    FaceCanvas canvas(frame);
    const int bob = static_cast<int>(std::lround(std::sin(elapsed_ms * 0.009f)));
    canvas.at_eye(36, 32 + bob);
    canvas.at_eye(92, 32 - bob);
}

inline void render_face(uint8_t *frame, const FacePose &pose, int gaze_x, int gaze_y,
                        float openness, FaceActivity activity, int64_t ms, bool sleep_marks = false)
{
    FaceCanvas canvas(frame);
    const int bob = activity == FaceActivity::Speaking ? static_cast<int>(std::lround(std::sin(ms * 0.018f))) : 0;
    canvas.eye(36 + gaze_x, 32 + gaze_y + bob, pose, true, openness);
    canvas.eye(92 + gaze_x, 32 + gaze_y - bob, pose, false, openness);
    if (pose.blush > 0.5f) {
        // A monochrome OLED uses cheek hatching instead of a pink color.
        for (int side : {0, 1}) for (int i = 0; i < 3; ++i) {
            const int x = (side ? 99 : 23) + i * 4;
            canvas.line(x, 48, x - 2, 53);
        }
    }
    if (pose.sparkle > 0.5f) {
        const int y = 18 + static_cast<int>((ms / 400) % 2) * 2;
        for (int x : {10, 117}) { canvas.line(x - 3, y, x + 3, y); canvas.line(x, y - 3, x, y + 3); }
    }
    if (sleep_marks) canvas.sleep_marks(ms);
}
