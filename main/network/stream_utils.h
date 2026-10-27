#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Google sends JSON in binary WebSocket frames as well as text frames.
// A bounded collector handles both WebSocket fragmentation and IDF's splitting
// of one frame into several callbacks. Control frames must not reset a message.
class TextFrames {
public:
    TextFrames(char *buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}
    void reset() { used_ = offset_ = 0; active_ = false; }
    int append(uint8_t opcode, bool fin, size_t offset, size_t payload,
               const char *data, size_t length)
    {
        if (opcode != 0 && opcode != 1 && opcode != 2) return 0;
        if (offset == 0) {
            if (opcode == 1 || opcode == 2) { used_ = 0; active_ = true; }
            else if (!active_) return -1;
            offset_ = 0;
        }
        if (!active_ || offset != offset_ || offset > payload
            || length > payload - offset || length >= capacity_ - used_) {
            reset(); return -1;
        }
        if (length) std::memcpy(buffer_ + used_, data, length);
        used_ += length; offset_ += length;
        if (fin && offset_ == payload) {
            buffer_[used_] = '\0'; active_ = false; return 1;
        }
        return 0;
    }
    const char *text() const { return buffer_; }
private:
    char *buffer_;
    size_t capacity_, used_ = 0, offset_ = 0;
    bool active_ = false;
};

// Streaming low-pass FIR before 24 -> 16 kHz conversion. Fractional sample
// position and filter history survive network chunk boundaries.
class Audio24To16 {
public:
    Audio24To16()
    {
        double total = 0;
        for (int i = 0; i < taps; ++i) {
            const int k = i - taps / 2;
            const double sinc = k == 0 ? 0.60 : std::sin(0.60 * pi * k) / (pi * k);
            weights_[i] = sinc * (0.54 - 0.46 * std::cos(2 * pi * i / (taps - 1)));
            total += weights_[i];
        }
        for (auto &weight : weights_) weight /= total;
    }
    void reset() { std::memset(history_, 0, sizeof(history_)); pos_ = 0; index_ = next_ = 0; previous_ = 0; }
    bool sample(int16_t input, int16_t &output)
    {
        history_[pos_] = input;
        float filtered = 0;
        int cursor = pos_;
        for (int i = 0; i < taps; ++i) {
            filtered += weights_[i] * history_[cursor];
            if (--cursor < 0) cursor = taps - 1;
        }
        if (++pos_ == taps) pos_ = 0;
        const uint64_t tick = index_++ * 2;
        const bool emit = tick >= next_;
        if (emit) {
            const float value = tick == next_ ? filtered : (filtered + previous_) * 0.5f;
            output = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(value)), -30000, 30000));
            next_ += 3;
        }
        previous_ = filtered;
        return emit;
    }
private:
    static constexpr int taps = 31;
    static constexpr double pi = 3.14159265358979323846;
    // ESP32-S3 has a single-precision FPU; double math in the audio loop is slow.
    float weights_[taps]{}, previous_ = 0;
    int16_t history_[taps]{};
    int pos_ = 0;
    uint64_t index_ = 0, next_ = 0;
};

inline bool stream_utils_self_test()
{
    char buffer[32]; TextFrames frames(buffer, sizeof(buffer));
    if (frames.append(1, false, 0, 3, "{\"x", 3) != 0
        || frames.append(9, true, 0, 1, "p", 1) != 0
        || frames.append(0, true, 0, 4, "\":1}", 2) != 0
        || frames.append(0, true, 2, 4, "1}", 2) != 1
        || std::strcmp(frames.text(), "{\"x\":1}") != 0) return false;
    if (frames.append(1, true, 4, 5, "a", 1) != -1) return false;
    if (frames.append(2, true, 0, 2, "{}", 2) != 1 || std::strcmp(frames.text(), "{}") != 0) return false;
    char small[4]; TextFrames limited(small, sizeof(small));
    if (limited.append(1, true, 0, 4, "abcd", 4) != -1) return false;
    Audio24To16 resampler;
    int count = 0; int16_t output = 0;
    for (int i = 0; i < 24000; ++i) if (resampler.sample(1000, output)) ++count;
    if (count != 16000 || std::abs(output - 1000) > 1) return false;
    resampler.reset(); count = 0;
    for (int i = 0; i < 24; ++i) if (resampler.sample(0, output)) ++count;
    return count == 16 && output == 0;
}
