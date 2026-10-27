#include "buddy.h"
#include "network/firmware_update.h"
#include "network/remote_update.h"
#include "audio/microphone.h"
#include "network/gemini_live.h"
#include "sensors/camera.h"
#include "display/eyes.h"
#include "behavior/idle_sleep.h"
#include "servo/head_servo.h"
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>

#include "bmi270.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/usb_serial_jtag.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr char tag[] = "buddy_io";
std::atomic<bool> healthy{false}, audio_busy{false};
std::atomic<bool> motion_selftest_pass{false};
std::atomic<float> motion_dynamic_g{0}, motion_rotation_dps{0};
std::atomic<float> motion_peak_dynamic_g{0}, motion_peak_rotation_dps{0};
std::atomic<bool> motion_output_suppressed{false}, motion_mic_active{false}, motion_servo_suppressed{false};
std::atomic<unsigned> motion_samples{0}, motion_moves{0}, motion_shakes{0}, motion_tilts{0};
std::atomic<int64_t> motion_sample_at{0};
std::atomic<int64_t> last_audio_end{0};
std::atomic<VoiceStyle> voice_style{VoiceStyle::Buddy};
BuddyVoiceEffect voice_effect;
QueueHandle_t audio_queue = nullptr;
esp_codec_dev_handle_t codec = nullptr;
bmi270_handle_t *imu = nullptr;

#define CLIP(name) \
    extern const uint8_t name##_start[] asm("_binary_" #name "_pcm_start"); \
    extern const uint8_t name##_end[] asm("_binary_" #name "_pcm_end")
CLIP(hello); CLIP(move); CLIP(shake); CLIP(tilt); CLIP(settled); CLIP(heard); CLIP(listen);
#undef CLIP
struct Clip { const char *name; const uint8_t *start; const uint8_t *end; };
const Clip clips[] = {
    {"hello", hello_start, hello_end}, {"move", move_start, move_end},
    {"shake", shake_start, shake_end}, {"tilt", tilt_start, tilt_end},
    {"settled", settled_start, settled_end}, {"heard", heard_start, heard_end},
    {"listen", listen_start, listen_end}
};

bool check(esp_err_t result, const char *step)
{
    if (result == ESP_OK) return true;
    ESP_LOGE(tag, "%s: FAIL (%s)", step, esp_err_to_name(result));
    return false;
}

bool make_bus(int port, int sda, int scl, i2c_master_bus_handle_t *bus)
{
    i2c_master_bus_config_t config{};
    config.i2c_port = port;
    config.sda_io_num = static_cast<gpio_num_t>(sda);
    config.scl_io_num = static_cast<gpio_num_t>(scl);
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    return check(i2c_new_master_bus(&config, bus), "I2C bus");
}

bool init_imu()
{
    i2c_master_bus_handle_t bus = nullptr;
    if (!make_bus(0, 45, 0, &bus)) return false;
    bmi270_driver_config_t driver{};
    driver.addr = 0x68;
    driver.interface = BMI270_USE_I2C;
    driver.i2c_bus = bus;
    if (!check(bmi270_create(&driver, &imu), "BMI270 init")) return false;
    bmi270_config_t config{};
    config.acce_odr = BMI270_ACC_ODR_100_HZ;
    config.acce_range = BMI270_ACC_RANGE_4_G;
    config.gyro_odr = BMI270_GYR_ODR_100_HZ;
    config.gyro_range = BMI270_GYR_RANGE_500_DPS;
    if (!check(bmi270_start(imu, &config), "BMI270 start")) return false;
    uint8_t chip_id = 0;
    if (!check(bmi270_get_chip_id(imu, &chip_id), "BMI270 ID") || chip_id != 0x24) return false;
    ESP_LOGI(tag, "BMI270_INIT: PASS chip_id=0x%02x", chip_id);
    return true;
}

bool init_audio()
{
    i2c_master_bus_handle_t bus = nullptr;
    if (!make_bus(1, 38, 39, &bus)) return false;
    i2c_device_config_t expander_config{};
    expander_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    expander_config.device_address = 0x43;
    expander_config.scl_speed_hz = 100000;
    i2c_master_dev_handle_t expander = nullptr;
    if (!check(i2c_master_bus_add_device(bus, &expander_config, &expander), "Amplifier expander")) return false;
    // Manufacturer's Echo Base sequence: enable pulls, direction, then PA output.
    const uint8_t registers[][2] = {{0x07, 0x00}, {0x0d, 0xff}, {0x03, 0x6f}, {0x05, 0xff}};
    for (const auto &reg : registers)
        if (!check(i2c_master_transmit(expander, reg, 2, 100), "Amplifier enable")) return false;
    uint8_t reg = 0x05, output = 0;
    if (!check(i2c_master_transmit_receive(expander, &reg, 1, &output, 1, 100), "Amplifier readback")
        || output != 0xff) return false;
    ESP_LOGI(tag, "AMPLIFIER_INIT: PASS output=0x%02x", output);

    i2s_chan_handle_t tx = nullptr, rx = nullptr;
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 6;
    channel.dma_frame_num = 256;
    // A late network packet must leave silence, never replay stale DMA audio.
    channel.auto_clear_after_cb = true;
    if (!check(i2s_new_channel(&channel, &tx, &rx), "I2S channels")) return false;
    i2s_std_config_t standard{};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    standard.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    standard.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    standard.gpio_cfg.bclk = GPIO_NUM_8;
    standard.gpio_cfg.ws = GPIO_NUM_6;
    standard.gpio_cfg.dout = GPIO_NUM_5;
    standard.gpio_cfg.din = GPIO_NUM_7;
    if (!check(i2s_channel_init_std_mode(tx, &standard), "I2S config")) return false;
    if (!check(i2s_channel_init_std_mode(rx, &standard), "I2S RX config")) return false;

    audio_codec_i2s_cfg_t data_config{};
    data_config.port = 0;
    data_config.tx_handle = tx;
    data_config.rx_handle = rx;
    const auto *data = audio_codec_new_i2s_data(&data_config);
    audio_codec_i2c_cfg_t control_config{};
    control_config.port = 1;
    control_config.addr = ES8311_CODEC_DEFAULT_ADDR;
    control_config.bus_handle = bus;
    const auto *control = audio_codec_new_i2c_ctrl(&control_config);
    const auto *gpio = audio_codec_new_gpio();
    if (!data || !control || !gpio) return false;
    es8311_codec_cfg_t es8311{};
    es8311.ctrl_if = control;
    es8311.gpio_if = gpio;
    es8311.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311.pa_pin = -1;
    es8311.use_mclk = false;
    es8311.mclk_div = 256;
    es8311.hw_gain.pa_voltage = 5.0f;
    es8311.hw_gain.codec_dac_voltage = 3.3f;
    const auto *codec_if = es8311_codec_new(&es8311);
    if (!codec_if) return false;
    esp_codec_dev_cfg_t device{};
    device.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT;
    device.codec_if = codec_if;
    device.data_if = data;
    codec = esp_codec_dev_new(&device);
    if (!codec) return false;
    esp_codec_dev_sample_info_t format{};
    format.sample_rate = 16000;
    format.bits_per_sample = 16;
    format.channel = 2;
    format.mclk_multiple = 256;
    if (!check(esp_codec_dev_open(codec, &format), "ES8311 open")) return false;
    if (!check(esp_codec_dev_set_out_vol(codec, 100), "ES8311 volume")) return false;
    if (!check(esp_codec_dev_set_in_gain(codec, 24.0f), "ES8311 microphone gain")) return false;
    ESP_LOGI(tag, "AUDIO_INIT: PASS ES8311 16000Hz 16bit stereo volume=100 duplex=TX+RX");
    return true;
}

void audio_task(void *)
{
    BuddyEvent event;
    int16_t stereo[512];
    int16_t cloud[256];
    bool cloud_playing = false;
    size_t cloud_frames = 0;
    constexpr size_t prebuffer_frames = 16000; // Refill one second for unusually long replies.
    constexpr size_t safe_start_frames = 16000 * 10; // Leave >6 seconds free in the 512 KiB queue.
    int64_t first_buffer_at = 0, buffer_wait_at = 0;
    unsigned buffer_waits = 0;
    int64_t max_buffer_wait = 0;
    for (;;) {
        const size_t buffered = buddy_ai_output_buffered();
        const bool pending = buddy_ai_output_pending();
        const bool response_active = buddy_ai_response_active();
        const int64_t now = esp_timer_get_time() / 1000;
        if (pending || cloud_playing) {
            if (!cloud_playing) {
                if (buffered && !first_buffer_at) first_buffer_at = now;
                // Normal short replies wait for generationComplete, not the
                // later turnComplete playback timer. A long reply starts before
                // the bounded queue fills. Playback never outruns a short reply.
                if (!buffered || (buffered < safe_start_frames && response_active)) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                cloud_playing = true; cloud_frames = 0; audio_busy.store(true);
                buffer_waits = 0; max_buffer_wait = 0; buffer_wait_at = 0;
                voice_effect.reset(voice_style.load());
                ESP_LOGI(tag, "CLOUD_AUDIO_START: buffered_ms=%zu startup_ms=%" PRId64 " mode=%s",
                         buffered / 16, first_buffer_at ? now - first_buffer_at : 0,
                         response_active ? "long_stream" : "complete_reply");
                first_buffer_at = 0;
            }
            if (response_active && (!buffered || (buffer_wait_at && buffered < prebuffer_frames))) {
                if (!buffer_wait_at) { buffer_wait_at = now; ++buffer_waits; }
                // Keep pitch history intact during packet gaps. DMA already has
                // queued audio and auto-clears if the gap outlasts that audio.
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (buffer_wait_at) {
                max_buffer_wait = std::max(max_buffer_wait, now - buffer_wait_at);
                buffer_wait_at = 0;
            }
            const size_t received = buddy_ai_read_output(cloud, 256);
            if (received) {
                for (size_t i = 0; i < received; ++i)
                    stereo[2 * i] = stereo[2 * i + 1] = voice_effect.sample(cloud[i]);
                if (esp_codec_dev_write(codec, stereo, received * 4) != ESP_OK) healthy.store(false);
                cloud_frames += received;
            } else if (!response_active) {
                if (voice_effect.active()) {
                    for (int left = BuddyVoiceEffect::tail_frames; left > 0;) {
                        const int count = std::min(left, 256);
                        for (int i = 0; i < count; ++i)
                            stereo[2*i] = stereo[2*i+1] = voice_effect.sample(0);
                        if (esp_codec_dev_write(codec, stereo, count * 4) != ESP_OK) healthy.store(false);
                        left -= count;
                    }
                }
                std::memset(stereo, 0, sizeof(stereo));
                for (int i = 0; i < 6; ++i)
                    if (esp_codec_dev_write(codec, stereo, sizeof(stereo)) != ESP_OK) healthy.store(false);
                vTaskDelay(pdMS_TO_TICKS(110));
                last_audio_end.store(esp_timer_get_time() / 1000);
                audio_busy.store(false); cloud_playing = false;
                ESP_LOGI(tag, "CLOUD_AUDIO_DONE: frames=%zu status=%s buffer_waits=%u max_wait_ms=%" PRId64,
                         cloud_frames, healthy.load() ? "PASS" : "FAIL", buffer_waits, max_buffer_wait);
            }
            continue;
        }
        first_buffer_at = 0;
        if (xQueueReceive(audio_queue, &event, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        const uint8_t *source = nullptr;
        const char *name = nullptr;
        size_t samples = 0;
        if (event == BuddyEvent::Chirp) {
            source = hello_start; samples = 1280; name = "chirp";
        } else if (event == BuddyEvent::Echo) {
            const int16_t *recorded = nullptr;
            if (!microphone_take_recording(&recorded, &samples)) continue;
            source = reinterpret_cast<const uint8_t *>(recorded);
            name = "echo";
        } else {
            const auto &clip = clips[static_cast<unsigned>(event)];
            source = clip.start;
            samples = (clip.end - clip.start) / sizeof(int16_t);
            name = clip.name;
        }
        audio_busy.store(true);
        ESP_LOGI(tag, "AUDIO_START: %s samples=%zu", name, samples);
        // Match a quiet recording's level to the small speaker without clipping.
        float gain = 1.0f;
        if (event == BuddyEvent::Echo) {
            int peak = 0;
            for (size_t i = 0; i < samples; ++i) {
                int16_t value;
                std::memcpy(&value, source + i * 2, 2);
                peak = std::max(peak, std::abs(static_cast<int>(value)));
            }
            if (peak > 300) gain = std::clamp(26000.0f / peak, 0.5f, 6.0f);
        }
        bool ok = true;
        for (size_t offset = 0; offset < samples;) {
            const size_t frames = (samples - offset > 256) ? 256 : samples - offset;
            for (size_t i = 0; i < frames; ++i) {
                int16_t sample;
                std::memcpy(&sample, source + (offset + i) * 2, 2);
                sample = static_cast<int16_t>(std::clamp(static_cast<int>(sample * gain), -30000, 30000));
                stereo[2 * i] = stereo[2 * i + 1] = sample;
            }
            if (esp_codec_dev_write(codec, stereo, frames * 4) != ESP_OK) { ok = false; break; }
            offset += frames;
        }
        // Drain the DMA and leave zeros on the output to avoid a held sample.
        std::memset(stereo, 0, sizeof(stereo));
        for (int i = 0; i < 6; ++i)
            if (esp_codec_dev_write(codec, stereo, sizeof(stereo)) != ESP_OK) ok = false;
        vTaskDelay(pdMS_TO_TICKS(110));
        last_audio_end.store(esp_timer_get_time() / 1000);
        audio_busy.store(false);
        if (!ok) healthy.store(false);
        ESP_LOGI(tag, "AUDIO_DONE: %s %s", name, ok ? "PASS" : "FAIL");
        if (event == BuddyEvent::Echo) microphone_playback_done();
        if (event == BuddyEvent::Listen) microphone_recording_cue_done();
    }
}

void motion_task(void *)
{
    MotionEngine engine;
    uint32_t samples = 0, errors = 0;
    TickType_t last_wake = xTaskGetTickCount();
    MotionSample sample{};
    // Average the startup pose; the robot need not start in a particular axis.
    float ax = 0, ay = 0, az = 0;
    unsigned calibration_samples = 0;
    for (;;) {
        const auto acc_result = bmi270_get_acce_data(imu, &sample.ax, &sample.ay, &sample.az);
        const auto gyro_result = bmi270_get_gyro_data(imu, &sample.gx, &sample.gy, &sample.gz);
        sample.ms = esp_timer_get_time() / 1000;
        if (acc_result != ESP_OK || gyro_result != ESP_OK) {
            ++errors;
            if (errors >= 10) { healthy.store(false); ESP_LOGE(tag, "IMU_READ: FAIL"); }
        } else {
            errors = 0;
            ++samples;
            if (calibration_samples < 50) {
                ax += sample.ax; ay += sample.ay; az += sample.az;
                if (++calibration_samples == 50) {
                    auto reference = sample;
                    reference.ax = ax / 50; reference.ay = ay / 50; reference.az = az / 50;
                    engine.reset(reference);
                    ESP_LOGI(tag, "IMU_CALIBRATION: PASS pose=(%.3f,%.3f,%.3f)g", reference.ax, reference.ay, reference.az);
                }
            } else {
                const bool output_suppressed = buddy_audio_busy() || sample.ms - last_audio_end.load() <= 400;
                const bool servo_suppressed = buddy_head_motion_recent();
                // Handling noise at the microphone is not speaker vibration.
                // Keep local IMU feedback while listening; defer audible replies.
                const bool suppress_motion = buddy_ota_busy() || output_suppressed || servo_suppressed;
                const auto event = engine.update(sample, suppress_motion);
                const float dynamic = engine.dynamic_acceleration_g();
                const float rotation = std::sqrt(sample.gx * sample.gx + sample.gy * sample.gy + sample.gz * sample.gz);
                motion_dynamic_g.store(dynamic); motion_rotation_dps.store(rotation);
                motion_peak_dynamic_g.store(std::max(motion_peak_dynamic_g.load(), dynamic));
                motion_peak_rotation_dps.store(std::max(motion_peak_rotation_dps.load(), rotation));
                motion_output_suppressed.store(output_suppressed); motion_servo_suppressed.store(servo_suppressed);
                motion_mic_active.store(microphone_interacting()); motion_samples.store(samples); motion_sample_at.store(sample.ms);
                if (event == BuddyEvent::Move) {
                    motion_moves.fetch_add(1);
                    buddy_idle_activity();
                    buddy_display_pickup();
                } else if (event == BuddyEvent::Shake) motion_shakes.fetch_add(1);
                else if (event == BuddyEvent::Tilt) motion_tilts.fetch_add(1);
                if (event != BuddyEvent::None && !buddy_audio_busy()
                    && !microphone_interacting() && !buddy_head_motion_recent()
                    && sample.ms - last_audio_end.load() > 400) {
                    ESP_LOGI(tag, "MOTION_EVENT: %s", clips[static_cast<unsigned>(event)].name);
                    buddy_ai_motion(event);
                    // A temporary disconnect must not revive old recorded phrases.
                    if (!buddy_ai_configured()) buddy_say(event);
                    else if (!buddy_ai_ready()) buddy_say(BuddyEvent::Chirp);
                }
            }
            if (samples % 250 == 0)
                ESP_LOGI(tag, "IMU_SAMPLE: n=%" PRIu32 " acc=(%.3f,%.3f,%.3f)g gyro=(%.1f,%.1f,%.1f)dps",
                         samples, sample.ax, sample.ay, sample.az, sample.gx, sample.gy, sample.gz);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20));
    }
}

void console_task(void *)
{
    auto *line = static_cast<char *>(heap_caps_calloc(4096, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!line) { healthy.store(false); vTaskDelete(nullptr); return; }
    size_t used = 0;
    bool overflow = false;
    for (;;) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(100)) != 1) continue;
        if (c == '\n' || c == '\r') {
            if (overflow) { overflow = false; used = 0; std::memset(line, 0, 4096); continue; }
            line[used] = '\0';
            if (buddy_remote_command(line)) {
                // Pairing secrets are physical USB input only and are never echoed.
            } else if (buddy_head_command(line)) {
                // Motor commands are applied only by the dedicated servo task.
            } else if (buddy_idle_command(line)) {
                // Local sleep diagnostics never modify network profiles or NVS.
            } else if (buddy_display_command(line)) {
                // Display commands only queue work; the console never owns I2C.
            } else if (buddy_ai_command(line)) {
                // Credentials are consumed locally and never echoed to USB.
            } else if (std::strcmp(line, "demo") == 0) {
                if (microphone_interacting() || buddy_ai_ready()) {
                    ESP_LOGI(tag, "DEMO_REQUEST: BUSY");
                    used = 0;
                    continue;
                }
                ESP_LOGI(tag, "DEMO_START");
                for (unsigned i = 0; i < 5; ++i) {
                    buddy_say(static_cast<BuddyEvent>(i));
                    vTaskDelay(pdMS_TO_TICKS(300));
                    while (buddy_audio_busy()) vTaskDelay(pdMS_TO_TICKS(50));
                    vTaskDelay(pdMS_TO_TICKS(300));
                }
                ESP_LOGI(tag, "DEMO_DONE: PASS");
            } else if (std::strcmp(line, "status") == 0) {
                ESP_LOGI(tag, "BUDDY_STATUS: %s", buddy_healthy() ? "PASS" : "FAIL");
                buddy_display_status();
                buddy_head_status();
                buddy_ai_status();
            } else if (std::strcmp(line, "listen") == 0) {
                ESP_LOGI(tag, "LISTEN_REQUEST: %s", microphone_request_recording() ? "OK" : "BUSY");
            } else if (std::strcmp(line, "mic-test") == 0) {
                ESP_LOGI(tag, "MIC_PROBE_REQUEST: %s", microphone_request_probe() ? "OK" : "BUSY");
            }
            used = 0;
            std::memset(line, 0, 4096);
        } else if (!overflow && used + 1 < 4096) {
            line[used++] = c;
        } else { overflow = true; }
    }
}
} // namespace

bool buddy_motion_selftest_pass() { return motion_selftest_pass.load(); }

BuddyMotionSnapshot buddy_motion_snapshot()
{
    const int64_t sample_at = motion_sample_at.load();
    return {motion_dynamic_g.load(), motion_rotation_dps.load(),
        motion_peak_dynamic_g.load(), motion_peak_rotation_dps.load(),
        motion_output_suppressed.load(), motion_mic_active.load(), motion_servo_suppressed.load(),
        motion_samples.load(), motion_moves.load(), motion_shakes.load(), motion_tilts.load(),
        sample_at ? esp_timer_get_time() / 1000 - sample_at : -1};
}

bool buddy_start()
{
    const bool voice_ok = buddy_voice_self_test();
    ESP_LOGI(tag, "VOICE_EFFECT_TEST: %s (bypass/pitch/silence/bounds/tail)", voice_ok ? "PASS" : "FAIL");
    if (!voice_ok) return false;
    const bool engine_ok = motion_engine_self_test();
    motion_selftest_pass.store(engine_ok);
    ESP_LOGI(tag, "MOTION_ENGINE_TEST: %s (still/move/shake/light movement/isolated bump/tilt/speech/settled)", engine_ok ? "PASS" : "FAIL");
    if (!engine_ok) return false;
    ESP_LOGI(tag, "MOTION_POLICY: shake=%.2fg/%u peaks/%" PRIi64 "ms move=%.2fg/%.0fdps/%" PRIi64 "ms tilt=%.0fdeg/%" PRIi64 "ms",
             motion_settings::shake_high_g, motion_settings::shake_peaks, motion_settings::shake_window_ms,
             motion_settings::move_acceleration_g, motion_settings::move_rotation_dps, motion_settings::move_hold_ms,
             motion_settings::tilt_angle_degrees, motion_settings::tilt_hold_ms);
    buddy_camera_init(); // Optional camera setup must precede the two I2C buses.
    if (!init_imu() || !init_audio()) return false;
    if (!buddy_idle_start()) return false;
    if (!buddy_head_start()) ESP_LOGW(tag, "SERVO_TASK: UNAVAILABLE (other modules retained)");
    if (!buddy_display_start()) ESP_LOGW(tag, "OLED_TASK: UNAVAILABLE (other modules retained)");
    audio_queue = xQueueCreate(1, sizeof(BuddyEvent));
    if (!audio_queue) return false;
    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_config.rx_buffer_size = 4096;
    if (!usb_serial_jtag_is_driver_installed()
        && !check(usb_serial_jtag_driver_install(&usb_config), "USB command console")) return false;
    healthy.store(true);
    // Load the saved AI mode before any automatic sound can be queued.
    if (!buddy_ai_start()) {
        healthy.store(false); ESP_LOGE(tag, "AI_INIT: FAIL"); return false;
    }
    if (!buddy_remote_start()) ESP_LOGW(tag, "REMOTE_OTA: UNAVAILABLE");
    if (!buddy_camera_start()) { healthy.store(false); return false; }
    if (xTaskCreate(audio_task, "buddy_audio", 6144, nullptr, 5, nullptr) != pdPASS
        || xTaskCreate(motion_task, "buddy_motion", 4096, nullptr, 4, nullptr) != pdPASS) {
        healthy.store(false);
        return false;
    }
    if (!microphone_start(codec)) {
        healthy.store(false);
        ESP_LOGE(tag, "MIC_INIT: FAIL");
        return false;
    }
    if (xTaskCreate(console_task, "buddy_console", 6144, nullptr, 3, nullptr) != pdPASS) {
        healthy.store(false); ESP_LOGE(tag, "AI_INIT: FAIL"); return false;
    }
    return true;
}

bool buddy_healthy() { return healthy.load() && microphone_healthy(); }
bool buddy_audio_busy() { return audio_busy.load(); }
void buddy_set_voice_style(VoiceStyle style) { voice_style.store(style); }
VoiceStyle buddy_voice_style() { return voice_style.load(); }
void buddy_say(BuddyEvent event)
{
    if (!buddy_ota_busy() && audio_queue && event != BuddyEvent::None) xQueueOverwrite(audio_queue, &event);
}
