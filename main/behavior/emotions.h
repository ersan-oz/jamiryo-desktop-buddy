#pragma once
#include <cstdint>
#include <cstring>

// Keep the first six values compatible with moods already stored in NVS.
enum class BuddyEmotion : uint8_t {
    Curious, Neutral, Happy, Surprised, Dizzy, Sleepy, Angry, Shy, Sad, Excited, Count
};
inline constexpr const char *buddy_emotions[] = {
    "curious", "neutral", "happy", "surprised", "dizzy", "sleepy",
    "angry", "shy", "sad", "excited"
};
inline constexpr unsigned buddy_emotion_count = static_cast<unsigned>(BuddyEmotion::Count);

inline const char *buddy_emotion_name(BuddyEmotion emotion)
{
    const auto index = static_cast<unsigned>(emotion);
    return buddy_emotions[index < buddy_emotion_count ? index : 1];
}
inline bool buddy_emotion_parse(const char *name, BuddyEmotion &emotion)
{
    if (!name) return false;
    for (unsigned i = 0; i < buddy_emotion_count; ++i) {
        if (std::strcmp(name, buddy_emotions[i]) == 0) {
            emotion = static_cast<BuddyEmotion>(i); return true;
        }
    }
    return false;
}
