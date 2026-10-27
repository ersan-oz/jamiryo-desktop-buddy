#pragma once
#include <cstddef>
#include <cstdint>

// Reverse pixel positions, never the two bytes inside an RGB565 pixel.
// One in-place pass; no allocation or JPEG decode/encode is required.
constexpr bool buddy_rotate_rgb565_180(uint8_t *pixels, size_t bytes, unsigned width, unsigned height)
{
    if (!pixels || !width || !height || size_t(width) > bytes / 2 / height) return false;
    const size_t count = size_t(width) * height;
    for (size_t left = 0; left < count / 2; ++left) {
        const size_t a = left * 2, b = (count - 1 - left) * 2;
        const uint8_t first = pixels[a], second = pixels[a + 1];
        pixels[a] = pixels[b]; pixels[a + 1] = pixels[b + 1];
        pixels[b] = first; pixels[b + 1] = second;
    }
    return true;
}

constexpr bool buddy_rotation_self_test()
{
    uint8_t guarded[] = {0x5a, 1,2, 3,4, 5,6, 7,8, 9,10, 11,12, 0xa5};
    constexpr uint8_t expected[] = {11,12, 9,10, 7,8, 5,6, 3,4, 1,2};
    if (!buddy_rotate_rgb565_180(guarded + 1, 12, 3, 2)) return false;
    for (unsigned i = 0; i < 12; ++i) if (guarded[i + 1] != expected[i]) return false;
    if (guarded[0] != 0x5a || guarded[13] != 0xa5) return false;
    if (!buddy_rotate_rgb565_180(guarded + 1, 12, 3, 2)) return false;
    for (unsigned i = 0; i < 12; ++i) if (guarded[i + 1] != i + 1) return false;
    if (buddy_rotate_rgb565_180(guarded + 1, 11, 3, 2)
        || buddy_rotate_rgb565_180(nullptr, 12, 3, 2)
        || buddy_rotate_rgb565_180(guarded, 14, 0, 2)
        || buddy_rotate_rgb565_180(guarded, 14, ~0u, ~0u)) return false;
    for (unsigned i = 0; i < 12; ++i) if (guarded[i + 1] != i + 1) return false;
    uint8_t odd[] = {1,2, 3,4, 5,6};
    if (!buddy_rotate_rgb565_180(odd, sizeof(odd), 3, 1)) return false;
    return odd[0] == 5 && odd[1] == 6 && odd[2] == 3 && odd[3] == 4 && odd[4] == 1 && odd[5] == 2;
}
static_assert(buddy_rotation_self_test(), "180-degree RGB565 rotation must preserve colors and bounds");
