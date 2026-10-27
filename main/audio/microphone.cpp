#include "microphone.h"
#include "network/firmware_update.h"
#include "buddy.h"
#include "network/gemini_live.h"
#include "behavior/idle_sleep.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cinttypes>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char tag[] = "buddy_mic";
constexpr std::size_t block_frames = 256;
constexpr std::size_t recording_frames = 16000 * 5;
enum class Recording { Idle, Prompting, Requested, Capturing, Ready, Playing };
constexpr double minimum_sound_level = 120.0;
std::atomic<Recording> recording{Recording::Idle};
std::atomic<bool> ready{false}, probe_requested{false}, cloud_active{false};
int16_t *recording_buffer = nullptr;
constexpr size_t preroll_frames = 4000; // 250 ms preserves the start of a word.
int16_t preroll[preroll_frames]{};

struct Level { double rms; int peak; };

Level mono_block(const int16_t *stereo, int16_t *mono)
{
    double sum[2]{}, squares[2]{};
    for (std::size_t i = 0; i < block_frames; ++i) {
        for (unsigned channel = 0; channel < 2; ++channel) {
            const double value = stereo[2 * i + channel];
            sum[channel] += value;
            squares[channel] += value * value;
        }
    }
    double variance[2];
    for (unsigned channel = 0; channel < 2; ++channel)
        variance[channel] = std::max(0.0, squares[channel] / block_frames
            - (sum[channel] / block_frames) * (sum[channel] / block_frames));
    // ES8311 is mono; select the active I2S slot and remove its DC offset.
    const unsigned channel = variance[1] > variance[0] ? 1 : 0;
    const int mean = static_cast<int>(sum[channel] / block_frames);
    int peak = 0;
    for (std::size_t i = 0; i < block_frames; ++i) {
        const int value = std::clamp(static_cast<int>(stereo[2*i + channel]) - mean, -32768, 32767);
        mono[i] = static_cast<int16_t>(value);
        peak = std::max(peak, std::abs(value));
    }
    return {std::sqrt(variance[channel]), peak};
}

void microphone_task(void *arg)
{
    const auto codec = static_cast<esp_codec_dev_handle_t>(arg);
    int16_t stereo[block_frames * 2], mono[block_frames];
    uint64_t frames = 0;
    unsigned read_errors = 0, calibration_blocks = 0, above_blocks = 0;
    double noise = 0, recording_energy = 0;
    int recording_peak = 0;
    std::size_t recorded = 0;
    int64_t last_level = 0, last_output = 0, last_heard = -15000;
    int64_t probe_start = 0;
    double probe_quiet = 0, probe_speaker = 0;
    unsigned quiet_blocks = 0, speaker_blocks = 0;
    bool probe_played = false;
    int probe_peak = 0;
    size_t preroll_used = 0, preroll_cursor = 0;
    bool cloud_streaming = false;
    unsigned cloud_above = 0;
    unsigned cloud_voiced_blocks = 0;
    unsigned wake_voiced_blocks = 0;
    int64_t wake_last_sound = 0;
    int64_t cloud_last_sound = 0;

    for (;;) {
        const int result = esp_codec_dev_read(codec, stereo, sizeof(stereo));
        if (result != ESP_OK) {
            if (++read_errors >= 3) {
                ready.store(false);
                ESP_LOGE(tag, "MIC_READ: FAIL error=%d", result);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        read_errors = 0;
        frames += block_frames;
        if (buddy_ota_busy()) {
            cloud_active.store(false); recording.store(Recording::Idle); probe_requested.store(false);
            cloud_streaming = false; cloud_above = cloud_voiced_blocks = 0;
            preroll_used = preroll_cursor = 0; above_blocks = wake_voiced_blocks = 0;
            continue;
        }
        const Level level = mono_block(stereo, mono);
        const int64_t now = esp_timer_get_time() / 1000;
        const bool output_busy = buddy_audio_busy();
        if (output_busy) last_output = now;
        const bool quiet_output = !output_busy && now - last_output >= 600;
        const bool cloud_allowed = buddy_ai_input_allowed() && quiet_output
            && recording.load() == Recording::Idle && !probe_requested.load();
        if (!cloud_allowed) {
            if (cloud_streaming) buddy_ai_pause_microphone();
            cloud_streaming = false; cloud_above = cloud_voiced_blocks = 0; preroll_used = preroll_cursor = 0;
            cloud_active.store(false);
        }

        if (now - last_level >= 2000) {
            ESP_LOGI(tag, "MIC_LEVEL: frames=%" PRIu64 " rms=%.0f peak=%d output=%d",
                     frames, level.rms, level.peak, output_busy);
            last_level = now;
        }

        if (probe_requested.load()) {
            if (probe_start == 0 && quiet_output) {
                probe_start = now;
                probe_quiet = probe_speaker = 0;
                quiet_blocks = speaker_blocks = 0;
                probe_peak = 0;
                probe_played = false;
                ESP_LOGI(tag, "MIC_PROBE_START: quiet then speaker");
            }
            if (probe_start != 0) {
                if (now - probe_start < 800) {
                    probe_quiet += level.rms;
                    ++quiet_blocks;
                } else if (!probe_played) {
                    probe_played = true;
                    buddy_say(BuddyEvent::Hello);
                }
                if (probe_played && output_busy) {
                    probe_speaker += level.rms;
                    probe_peak = std::max(probe_peak, level.peak);
                    ++speaker_blocks;
                }
                if (now - probe_start >= 4500) {
                    const double quiet = quiet_blocks ? probe_quiet / quiet_blocks : 0;
                    const double speaker = speaker_blocks ? probe_speaker / speaker_blocks : 0;
                    const bool signal = speaker_blocks >= 20 && speaker > std::max(50.0, quiet * 1.25)
                                        && probe_peak > 100;
                    ESP_LOGI(tag, "MIC_PROBE: %s quiet_rms=%.0f speaker_rms=%.0f peak=%d blocks=%u",
                             signal ? "PASS" : "FAIL", quiet, speaker, probe_peak, speaker_blocks);
                    probe_start = 0;
                    probe_requested.store(false);
                }
            }
            above_blocks = 0;
            continue;
        }

        auto state = recording.load();
        if (state == Recording::Requested && !output_busy && now - last_output >= 200) {
            recorded = 0;
            recording_peak = 0;
            recording_energy = 0;
            recording.store(Recording::Capturing);
            state = Recording::Capturing;
            ESP_LOGI(tag, "LISTENING_START: 5 seconds; speak now");
        }
        if (state == Recording::Capturing) {
            const std::size_t count = std::min(block_frames, recording_frames - recorded);
            std::memcpy(recording_buffer + recorded, mono, count * sizeof(int16_t));
            recording_peak = std::max(recording_peak, level.peak);
            for (std::size_t i = 0; i < count; ++i)
                recording_energy += static_cast<double>(mono[i]) * mono[i];
            recorded += count;
            if (recorded == recording_frames) {
                ESP_LOGI(tag, "MIC_RECORD: PASS frames=%zu rms=%.0f peak=%d",
                         recorded, std::sqrt(recording_energy / recorded), recording_peak);
                recording.store(Recording::Ready);
                buddy_say(BuddyEvent::Echo);
            }
            above_blocks = 0;
            continue;
        }
        if (state != Recording::Idle || !quiet_output) {
            wake_voiced_blocks = 0;
            above_blocks = 0;
            continue;
        }

        if (calibration_blocks < 100) {
            noise += level.rms;
            if (++calibration_blocks == 100) {
                noise /= calibration_blocks;
                ESP_LOGI(tag, "MIC_MONITOR: READY noise_rms=%.0f threshold=%.0f",
                         noise, std::max(minimum_sound_level, noise * 3.0));
            }
            continue;
        }
        // Local waking also works with Wi-Fi/AI unavailable. Ignore the robot's
        // speaker (including its sleepy mumble) and short isolated noise bursts.
        if (now - wake_last_sound >= 600) wake_voiced_blocks = 0;
        if (level.rms > std::max(45.0, noise * 2.5)) {
            wake_last_sound = now;
            wake_voiced_blocks = std::min(wake_voiced_blocks + 1, 15U);
            if (wake_voiced_blocks >= 15) buddy_idle_activity();
        }
        if (buddy_ai_ready()) {
            if (cloud_allowed) {
                for (size_t i = 0; i < block_frames; ++i) {
                    preroll[preroll_cursor] = mono[i]; preroll_cursor = (preroll_cursor + 1) % preroll_frames;
                    preroll_used = std::min(preroll_frames, preroll_used + 1);
                }
                const double gate = std::max(45.0, noise * 2.5);
                if (level.rms > gate) {
                    cloud_voiced_blocks = std::min(cloud_voiced_blocks + 1, 15U);
                    ++cloud_above; cloud_last_sound = now;
                    if (cloud_streaming || cloud_above >= 3) buddy_ai_note_speech(!cloud_streaming);
                }
                else { cloud_above = 0; if (!cloud_streaming) noise += 0.005 * (level.rms - noise); }
                if (!cloud_streaming && cloud_above >= 3) {
                    cloud_streaming = true;
                    cloud_voiced_blocks = cloud_above;
                    const size_t start = (preroll_cursor + preroll_frames - preroll_used) % preroll_frames;
                    const size_t first = std::min(preroll_used, preroll_frames - start);
                    bool pushed = buddy_ai_push_microphone(preroll + start, first);
                    if (pushed && first < preroll_used) pushed = buddy_ai_push_microphone(preroll, preroll_used - first);
                    if (!pushed) cloud_streaming = false;
                    cloud_active.store(cloud_streaming);
                    ESP_LOGI(tag, "AI_LISTENING: speech onset rms=%.0f", level.rms);
                } else if (cloud_streaming) {
                    if (!buddy_ai_push_microphone(mono, block_frames)) cloud_streaming = false;
                    if (now - cloud_last_sound >= 600) {
                        // Brief taps/background clicks need not produce a model reply.
                        // Fifteen voiced blocks are 240 ms of actual speech, excluding preroll.
                        buddy_ai_pause_microphone(cloud_voiced_blocks >= 15);
                        cloud_streaming = false; cloud_voiced_blocks = 0; preroll_used = preroll_cursor = 0;
                    }
                    cloud_active.store(cloud_streaming);
                }
            }
            above_blocks = 0; continue;
        }
        // In the saved Gemini mode, waiting/reconnecting/quota errors stay quiet.
        // The legacy "Seni duydum" clip belongs only to unconfigured diagnostics.
        if (buddy_ai_configured()) {
            above_blocks = 0; noise += 0.005 * (level.rms - noise); continue;
        }
        const double threshold = std::max(minimum_sound_level, noise * 3.0);
        if (level.rms > threshold) {
            ++above_blocks;
            if (above_blocks >= 12 && now - last_heard >= 15000) {
                ESP_LOGI(tag, "SOUND_DETECTED: rms=%.0f threshold=%.0f", level.rms, threshold);
                last_heard = now;
                above_blocks = 0;
                buddy_say(BuddyEvent::Heard);
            }
        } else {
            above_blocks = 0;
            noise += 0.01 * (level.rms - noise);
        }
    }
}
} // namespace

bool microphone_start(esp_codec_dev_handle_t codec)
{
    recording_buffer = static_cast<int16_t *>(heap_caps_malloc(
        recording_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!recording_buffer) return false;
    ready.store(true);
    if (xTaskCreate(microphone_task, "buddy_mic", 6144, codec, 5, nullptr) != pdPASS) {
        ready.store(false);
        heap_caps_free(recording_buffer);
        recording_buffer = nullptr;
        return false;
    }
    ESP_LOGI(tag, "MIC_INIT: PASS 16000Hz mono gain=24dB record_limit=5s");
    return true;
}

bool microphone_healthy() { return ready.load(); }
bool microphone_interacting() { return recording.load() != Recording::Idle || probe_requested.load() || cloud_active.load(); }
bool microphone_request_recording()
{
    if (!ready.load() || probe_requested.load() || buddy_ai_ready()) return false;
    auto expected = Recording::Idle;
    if (!recording.compare_exchange_strong(expected, Recording::Prompting)) return false;
    buddy_say(BuddyEvent::Listen);
    return true;
}
bool microphone_request_probe()
{
    if (!ready.load() || recording.load() != Recording::Idle || buddy_ai_ready()) return false;
    bool expected = false;
    return probe_requested.compare_exchange_strong(expected, true);
}
bool microphone_take_recording(const int16_t **data, std::size_t *frames)
{
    auto expected = Recording::Ready;
    if (!recording.compare_exchange_strong(expected, Recording::Playing)) return false;
    *data = recording_buffer;
    *frames = recording_frames;
    return true;
}
void microphone_playback_done()
{
    std::memset(recording_buffer, 0, recording_frames * sizeof(int16_t));
    recording.store(Recording::Idle);
}
void microphone_recording_cue_done()
{
    auto expected = Recording::Prompting;
    recording.compare_exchange_strong(expected, Recording::Requested);
}
