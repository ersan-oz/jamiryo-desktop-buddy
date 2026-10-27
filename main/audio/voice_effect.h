#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <new>

enum class VoiceStyle : uint8_t { Natural, Buddy, Robot };

// Two overlapping variable-delay read heads change pitch while retaining the
// 16 kHz sample count and speaking speed. The delay and window state persist
// across all network/I2S blocks. No microphone data passes through this effect.
class BuddyVoiceEffect {
public:
    static constexpr int tail_frames = 2080;
    BuddyVoiceEffect()
    {
        for (int i = 0; i < 256; ++i) {
            const float wave = std::sin(3.14159265358979323846f * i / 256);
            window_[i] = wave * wave;
        }
        reset(VoiceStyle::Buddy);
    }
    void reset(VoiceStyle style)
    {
        std::fill(std::begin(history_), std::end(history_), int16_t(0));
        write_ = 0; phase_ = span * 0.5f; modulation_phase_ = 0;
        style_ = style;
        ratio_ = style == VoiceStyle::Robot ? 1.1892071f : 1.1224620f;
        depth_ = style == VoiceStyle::Robot ? 0.25f : 0.06f;
        modulation_step_ = style == VoiceStyle::Robot ? 19327353u : 12884902u;
    }
    bool active() const { return style_ != VoiceStyle::Natural; }
    int16_t sample(int16_t input)
    {
        if (!active()) return input;
        history_[write_] = input;
        float other = phase_ + span * 0.5f;
        if (other >= span) other -= span;
        const float weight = window_[static_cast<unsigned>(phase_ * 256 / span) & 255];
        float value = read(32 + phase_) * weight + read(32 + other) * (1 - weight);
        // The stronger profile uses 72 Hz/25% modulation for a clearer robot tone.
        const float wave = 1 - 2 * window_[modulation_phase_ >> 24];
        value *= 1 - depth_ + depth_ * wave;
        modulation_phase_ += modulation_step_;
        write_ = (write_ + 1) & (capacity - 1);
        phase_ += 1 - ratio_;
        if (phase_ < 0) phase_ += span;
        return static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(value)), -30000, 30000));
    }
private:
    float read(float delay) const
    {
        const int whole = static_cast<int>(delay);
        const float fraction = delay - whole;
        const int newer = (write_ - whole) & (capacity - 1);
        const int older = (newer - 1) & (capacity - 1);
        return history_[newer] * (1 - fraction) + history_[older] * fraction;
    }
    static constexpr int capacity = 4096;
    static constexpr float span = 2048;
    int16_t history_[capacity]{};
    float window_[256]{};
    float phase_ = 0, ratio_ = 1, depth_ = 0;
    uint32_t modulation_phase_ = 0, modulation_step_ = 12884902u;
    int write_ = 0;
    VoiceStyle style_ = VoiceStyle::Natural;
};

inline bool buddy_voice_self_test()
{
    // Allocate off the small app_main stack. Exercise bypass, pitch, silence,
    // clipping bounds, reset, and the complete delayed tail.
    std::unique_ptr<BuddyVoiceEffect> effect(new (std::nothrow) BuddyVoiceEffect);
    if (!effect) return false;
    effect->reset(VoiceStyle::Natural);
    for (int value : {-32768, -1200, 0, 500, 32767})
        if (effect->sample(static_cast<int16_t>(value)) != value) return false;
    effect->reset(VoiceStyle::Buddy);
    for (int i = 0; i < 4096; ++i) if (effect->sample(0) != 0) return false;
    int crossings = 0; int16_t previous = 0;
    constexpr int warmup = 4096, measured = 32000;
    for (int i = 0; i < warmup + measured; ++i) {
        const auto input = static_cast<int16_t>(12000 * std::sin(2 * 3.14159265358979323846 * 1000 * i / 16000));
        const int16_t output = effect->sample(input);
        if (i >= warmup && previous <= 0 && output > 0) ++crossings;
        previous = output;
    }
    if (std::abs(crossings / 2.0 - 1122.462) > 12) return false;
    for (int i = 0; i < BuddyVoiceEffect::tail_frames + 256; ++i) previous = effect->sample(0);
    if (previous != 0) return false;
    effect->reset(VoiceStyle::Robot);
    for (int i = 0; i < 8192; ++i) {
        const int value = effect->sample(i % 4 < 2 ? 32767 : -32768);
        if (value < -30000 || value > 30000) return false;
    }
    effect->reset(VoiceStyle::Buddy);
    return effect->sample(0) == 0;
}
