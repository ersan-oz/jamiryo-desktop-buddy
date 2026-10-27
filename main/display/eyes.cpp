#include "eyes.h"
#include "face_renderer.h"
#include "photo_effect.h"
#include "buddy.h"
#include "audio/microphone.h"
#include "network/gemini_live.h"
#include "behavior/idle_sleep.h"
#include "servo/head_servo.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr char tag[] = "buddy_eyes";
constexpr int width = 128, height = 64, page_count = height / 8;
std::atomic<bool> enabled{true}, available{false}, rescan{true}, demo{false};
std::atomic<bool> emotion_demo{false}, preview_requested{false};
std::atomic<BuddyEmotion> shown_emotion{BuddyEmotion::Neutral};
std::atomic<FaceActivity> shown_activity{FaceActivity::Idle};
std::atomic<unsigned> address{0}, frames{0}, write_errors{0};
std::atomic<int64_t> photo_focus_at{0}, photo_shutter_at{0};
std::atomic<bool> shown_sleep_marks{false};
constexpr int64_t pickup_duration_ms = 3000;
std::atomic<int64_t> pickup_at{0};
std::atomic<unsigned> pickup_count{0};
std::atomic<bool> shown_pickup{false}, renderer_selftest{false};
i2c_master_bus_handle_t bus = nullptr;
i2c_master_dev_handle_t oled = nullptr;
uint8_t frame[width * page_count]{}, previous[sizeof(frame)]{};
bool previous_valid = false;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

bool commands(const uint8_t *bytes, size_t count)
{
    uint8_t packet[40]{}; // 0x00 selects the command stream.
    if (!oled || count > sizeof(packet) - 1) return false;
    std::memcpy(packet + 1, bytes, count);
    return i2c_master_transmit(oled, packet, count + 1, 40) == ESP_OK;
}

bool discover()
{
    if (oled) { i2c_master_bus_rm_device(oled); oled = nullptr; }
    available.store(false); address.store(0); previous_valid = false;
    for (const uint16_t candidate : {0x3c, 0x3d}) {
        if (i2c_master_probe(bus, candidate, 30) != ESP_OK) continue;
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = candidate;
        config.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus, &config, &oled) != ESP_OK) continue;
        // SH1106 uses page addressing with a two-column RAM offset; an
        // SSD1306 horizontal-addressing frame would be shifted or incomplete.
        const uint8_t init[] = {0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00,
            0x40, 0xad, 0x8b, 0xa1, 0xc8, 0xda, 0x12, 0x81, 0xa0,
            0xd9, 0x1f, 0xdb, 0x40, 0x33, 0xa6, 0xa4};
        if (commands(init, sizeof(init))) {
            vTaskDelay(pdMS_TO_TICKS(100));
            const uint8_t on = 0xaf;
            if (commands(&on, 1)) {
                address.store(candidate); available.store(true);
                ESP_LOGI(tag, "OLED_INIT: READY controller=SH1106 address=0x%02x sda=38 scl=39", candidate);
                return true;
            }
        }
        i2c_master_bus_rm_device(oled); oled = nullptr;
    }
    // An unplugged display is optional and must not stop audio or the camera.
    ESP_LOGI(tag, "OLED_INIT: NOT_CONNECTED (expected 0x3c or 0x3d)");
    return false;
}

void pixel(int x, int y)
{
    if (x >= 0 && x < width && y >= 0 && y < height)
        frame[(y / 8) * width + x] |= static_cast<uint8_t>(1u << (y % 8));
}

FaceActivity current_activity()
{
    if (buddy_audio_busy()) return FaceActivity::Speaking;
    if (microphone_interacting()) return FaceActivity::Listening;
    if (buddy_ai_waiting_reply()) return FaceActivity::Thinking;
    return FaceActivity::Idle;
}

bool renderer_check()
{
    // Distinct visible expressions and clipping must survive every render.
    struct GuardedFrame { uint8_t before = 0x5a; uint8_t data[1024]; uint8_t after = 0xa5; } test;
    uint32_t hashes[buddy_emotion_count]{};
    for (unsigned index = 0; index < buddy_emotion_count; ++index) {
        const auto emotion = static_cast<BuddyEmotion>(index);
        render_face(test.data, face_pose(emotion), 0, emotion == BuddyEmotion::Shy ? 4 : 0,
                    1.0f, FaceActivity::Idle, 0);
        unsigned lit = 0; uint32_t hash = 2166136261u;
        for (uint8_t byte : test.data) {
            lit += __builtin_popcount(static_cast<unsigned>(byte));
            hash = (hash ^ byte) * 16777619u;
        }
        if (test.before != 0x5a || test.after != 0xa5 || lit < 20 || lit > 3000) return false;
        for (unsigned previous_index = 0; previous_index < index; ++previous_index)
            if (hashes[previous_index] == hash) return false;
        hashes[index] = hash;
        render_face(test.data, face_pose(emotion), 200, -200, 0.0f, FaceActivity::Speaking, 200);
        if (test.before != 0x5a || test.after != 0xa5) return false;
    }
    auto asleep = face_pose(BuddyEmotion::Sleepy);
    asleep.left_height = asleep.right_height = 2; asleep.corner = 1;
    for (int64_t phase : {0, 1000, 2000}) {
        render_face(test.data, asleep, 0, 4, 1, FaceActivity::Idle, phase, true);
        unsigned marks = 0;
        for (unsigned offset = 0; offset < 3 * 128; ++offset) marks += __builtin_popcount(static_cast<unsigned>(test.data[offset]));
        if (marks < 30 || test.before != 0x5a || test.after != 0xa5) return false;
    }
    for (int64_t phase : {0, 160, 480, 1500, 2960}) {
        render_pickup_face(test.data, phase);
        unsigned left = 0, right = 0; uint32_t hash = 2166136261u;
        for (unsigned offset = 0; offset < 1024; ++offset) {
            const unsigned lit = __builtin_popcount(static_cast<unsigned>(test.data[offset]));
            if (offset % 128 < 64) left += lit; else right += lit;
            hash = (hash ^ test.data[offset]) * 16777619u;
        }
        if (left < 200 || right != left || test.before != 0x5a || test.after != 0xa5) return false;
        for (uint32_t other : hashes) if (other == hash) return false;
    }
    return true;
}

void emit_preview(const char *name, FaceActivity activity)
{
    for (unsigned page = 0; page < page_count; ++page) {
        char hex[width * 2 + 1];
        constexpr char digits[] = "0123456789abcdef";
        for (int x = 0; x < width; ++x) {
            const auto byte = frame[page * width + x];
            hex[x * 2] = digits[byte >> 4]; hex[x * 2 + 1] = digits[byte & 15];
        }
        hex[width * 2] = 0;
        ESP_LOGI(tag, "OLED_PREVIEW: %s %s page=%u data=%s", name, face_activity_name(activity), page, hex);
    }
}
void preview_frames()
{
    for (unsigned index = 0; index < buddy_emotion_count + 3; ++index) {
        const auto emotion = index < buddy_emotion_count ? static_cast<BuddyEmotion>(index) : BuddyEmotion::Neutral;
        const auto activity = index < buddy_emotion_count ? FaceActivity::Idle :
            static_cast<FaceActivity>(index - buddy_emotion_count + 1);
        const int gx = activity == FaceActivity::Thinking ? -5 : 0;
        const int gy = emotion == BuddyEmotion::Shy ? 4 : activity == FaceActivity::Thinking ? -4 : 0;
        render_face(frame, face_pose(emotion, activity), gx, gy, 1.0f, activity, 100);
        emit_preview(buddy_emotion_name(emotion), activity);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    auto asleep = face_pose(BuddyEmotion::Sleepy);
    asleep.left_height = asleep.right_height = 2; asleep.corner = 1;
    render_face(frame, asleep, 0, 4, 1, FaceActivity::Idle, 1000, true);
    emit_preview("asleep", FaceActivity::Idle);
    render_pickup_face(frame, 0);
    emit_preview("pickup_at_eyes", FaceActivity::Idle);
    ESP_LOGI(tag, "OLED_PREVIEW: COMPLETE");
}

bool refresh()
{
    for (unsigned page = 0; page < page_count; ++page) {
        const auto offset = page * width;
        if (previous_valid && std::memcmp(frame + offset, previous + offset, width) == 0) continue;
        const uint8_t location[] = {static_cast<uint8_t>(0xb0 + page), 0x02, 0x10};
        if (!commands(location, sizeof(location))) return false;
        uint8_t data[width + 1]; data[0] = 0x40;
        std::memcpy(data + 1, frame + offset, width);
        if (i2c_master_transmit(oled, data, sizeof(data), 40) != ESP_OK) return false;
    }
    std::memcpy(previous, frame, sizeof(frame)); previous_valid = true;
    frames.fetch_add(1);
    return true;
}

void eyes_task(void *)
{
    int64_t blink_at = now_ms() + 3500, blink_started = -1;
    int64_t look_at = now_ms() + 2000, demo_until = 0;
    float gaze_x = 0, gaze_y = 0;
    int target_x = 0, target_y = 0;
    int64_t emotions_until = 0;
    FacePose pose;
    BuddyEmotion previous_emotion = BuddyEmotion::Count;
    FaceActivity previous_activity = FaceActivity::Idle;
    BuddySleepStage previous_sleep = BuddySleepStage::Awake;
    const bool renderer_ok = renderer_check();
    renderer_selftest.store(renderer_ok);
    ESP_LOGI(tag, "OLED_RENDER_TEST: %s (10 expressions, clipped gaze/blink, floating Zzz, pickup @ @)", renderer_ok ? "PASS" : "FAIL");
    if (!renderer_ok) { vTaskDelete(nullptr); return; }
    bool blanked = false;
    bool photo_was_active = false;
    bool servo_was_active = false;
    int servo_direction = 99;
    int64_t shown_shutter = 0;
    int64_t retry_at = 0;
    for (;;) {
        if (enabled.load() && (rescan.exchange(false)
            || (!available.load() && now_ms() >= retry_at))) {
            const bool connected = discover();
            retry_at = connected ? 0 : now_ms() + 5000;
            blanked = false;
        }
        if (!enabled.load()) {
            if (available.load() && !blanked) {
                const uint8_t off = 0xae;
                blanked = commands(&off, 1);
            }
            vTaskDelay(pdMS_TO_TICKS(100)); continue;
        }
        if (!available.load()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        const int64_t now = now_ms();
        const auto sleep = buddy_idle_snapshot();
        if (sleep.stage != previous_sleep) {
            ESP_LOGI(tag, "OLED_SLEEP: %s idle_ms=%lld", buddy_sleep_name(sleep.stage), static_cast<long long>(sleep.idle_ms));
            if (sleep.stage == BuddySleepStage::Awake) { blink_started = -1; gaze_x = gaze_y = 0; }
            previous_sleep = sleep.stage;
        }
        if (preview_requested.exchange(false)) preview_frames();
        if (emotion_demo.exchange(false)) {
            emotions_until = now + 12000; demo_until = 0;
            ESP_LOGI(tag, "OLED_EMOTION_DEMO: START (happy, angry, shy, surprised; 3s each)");
        }
        if (demo.exchange(false)) {
            demo_until = now + 8000;
            emotions_until = 0;
            blink_started = -1; blink_at = now + 3000; look_at = now + 2300;
            ESP_LOGI(tag, "OLED_TEST: START (grid 2s, then eyes/blink/look)");
        }
        std::memset(frame, 0, sizeof(frame));
        bool sleep_marks = false;
        const int64_t focus = photo_focus_at.load(), shutter = photo_shutter_at.load();
        const bool photo_active = (shutter && now >= shutter && now - shutter < 650)
            || (focus && !shutter && now >= focus && now - focus < 2000);
        const int64_t picked_up = pickup_at.load();
        const bool pickup_active = picked_up && now >= picked_up && now - picked_up < pickup_duration_ms;
        bool pickup_drawn = false;
        int servo_gaze = 0;
        const bool servo_test = buddy_head_test_gaze(servo_gaze);
        if (servo_was_active && !servo_test) {
            gaze_x = gaze_y = 0; target_x = target_y = 0;
            blink_started = -1; blink_at = now + 2500; look_at = now + 2000;
            ESP_LOGI(tag, "OLED_SERVO_TEST: DONE (returned to live face)");
        }
        if (photo_was_active && !photo_active) {
            blink_started = -1; blink_at = now + 2500; gaze_x = gaze_y = 0;
            ESP_LOGI(tag, "OLED_PHOTO: DONE (returned to live face)");
        }
        if (servo_test) {
            // A diagnostic overlay stays visible during AI speech and blinking.
            // The motor already eases the pulse; applying a second filter here
            // would make the eyes visibly lag behind the command being tested.
            const int direction = servo_gaze < 0 ? -1 : servo_gaze > 0 ? 1 : 0;
            if (!servo_was_active || direction != servo_direction)
                ESP_LOGI(tag, "OLED_SERVO_TEST: %s gaze_x=%d", direction < 0 ? "LEFT" : direction > 0 ? "RIGHT" : "CENTER", servo_gaze);
            servo_direction = direction;
            pose = face_pose(BuddyEmotion::Neutral);
            gaze_x = servo_gaze; gaze_y = 0;
            blink_started = -1; blink_at = now + 2500;
            render_face(frame, pose, servo_gaze, 0, 1.0f, FaceActivity::Idle, now, false);
        } else if (photo_active) {
            render_photo_face(frame, focus ? now - focus : 0, shutter ? now - shutter : -1);
            if (!photo_was_active) ESP_LOGI(tag, "OLED_PHOTO: FOCUS");
            if (shutter && shutter != shown_shutter) {
                shown_shutter = shutter; ESP_LOGI(tag, "OLED_PHOTO: SHUTTER_FLASH");
            }
        } else if (pickup_active) {
            // Local IMU feedback takes priority over cloud emotion and blinks.
            render_pickup_face(frame, now - picked_up);
            pickup_drawn = true;
            pose = face_pose(BuddyEmotion::Neutral);
            gaze_x = gaze_y = 0; target_x = target_y = 0;
            blink_started = -1; blink_at = now + 2500; look_at = now + 2000;
        } else if (demo_until && demo_until - now > 6000) {
            // Four borders and all eight page bands expose addressing errors.
            for (int x = 0; x < width; ++x) { pixel(x, 0); pixel(x, height - 1); }
            for (int y = 0; y < height; ++y) { pixel(0, y); pixel(width - 1, y); }
            for (int x = 16; x < width; x += 16)
                for (int y = 0; y < height; y += 2) pixel(x, y);
            for (int y = 8; y < height; y += 8)
                for (int x = 0; x < width; x += 2) pixel(x, y);
        } else {
            BuddyEmotion emotion = buddy_ai_emotion();
            FaceActivity activity = current_activity();
            const bool live_face = !emotions_until || now >= emotions_until;
            const bool sleepy = sleep.stage != BuddySleepStage::Awake && live_face;
            // Short noise can trigger cloud VAD without being real interaction.
            // Keep closed eyes until local sustained speech actually wakes us.
            if (sleepy && sleep.stage == BuddySleepStage::Asleep && activity != FaceActivity::Speaking)
                activity = FaceActivity::Idle;
            const bool sleeping = sleepy && sleep.stage == BuddySleepStage::Asleep && activity == FaceActivity::Idle;
            if (sleepy) emotion = BuddyEmotion::Sleepy;
            if (emotions_until && now < emotions_until) {
                constexpr BuddyEmotion sequence[] = {BuddyEmotion::Happy, BuddyEmotion::Angry, BuddyEmotion::Shy, BuddyEmotion::Surprised};
                emotion = sequence[(now - (emotions_until - 12000)) / 3000];
                activity = FaceActivity::Idle;
            }
            if (emotion != previous_emotion || activity != previous_activity) {
                shown_emotion.store(emotion); shown_activity.store(activity);
                ESP_LOGI(tag, "OLED_EXPRESSION: emotion=%s phase=%s source=%s", buddy_emotion_name(emotion),
                         face_activity_name(activity), emotions_until && now < emotions_until ? "demo" : "live");
                previous_emotion = emotion; previous_activity = activity;
            }
            if (now >= look_at) {
                target_x = static_cast<int>(esp_random() % 13) - 6;
                target_y = static_cast<int>(esp_random() % 7) - 3;
                look_at = now + 2000 + esp_random() % 4000;
            }
            int gx_target = target_x, gy_target = target_y;
            if (emotion == BuddyEmotion::Shy) { gx_target /= 3; gy_target = 4; }
            if (emotion == BuddyEmotion::Sleepy) { gx_target = 0; gy_target = 4; }
            if (activity == FaceActivity::Listening || activity == FaceActivity::Speaking) { gx_target = gy_target = 0; }
            if (activity == FaceActivity::Thinking) { gx_target = (now / 1400) % 2 ? -5 : 5; gy_target = -4; }
            gaze_x += 0.2f * (gx_target - gaze_x); gaze_y += 0.2f * (gy_target - gaze_y);
            auto target_pose = face_pose(emotion, activity);
            if (sleeping) {
                target_pose.left_height = target_pose.right_height = 2;
                target_pose.corner = 1;
                // Closed eyes rise/fall by one pixel with slow breathing.
                gaze_x = 0;
                gaze_y = 4 + std::sin(now * 0.0012f);
            }
            pose.approach(target_pose, sleepy ? 0.055f : 0.22f);
            if (now >= blink_at) {
                blink_started = now;
                blink_at = now + 3000 + esp_random() % 5000;
            }
            float openness = 1.0f;
            if (!sleeping && blink_started >= 0 && now - blink_started < 200)
                openness = std::abs(static_cast<float>(now - blink_started) - 100.0f) / 100.0f;
            const int gx = static_cast<int>(std::lround(gaze_x)), gy = static_cast<int>(std::lround(gaze_y));
            sleep_marks = sleeping && pose.left_height <= 3.0f && pose.right_height <= 3.0f;
            render_face(frame, pose, gx, gy, openness, activity, now, sleep_marks);
        }
        if (shown_sleep_marks.exchange(sleep_marks) != sleep_marks)
            ESP_LOGI(tag, "OLED_SLEEP_MARKS: %s", sleep_marks ? "Zzz_ON" : "OFF");
        photo_was_active = photo_active;
        servo_was_active = servo_test;
        if (!refresh()) {
            pickup_drawn = false;
            write_errors.fetch_add(1); available.store(false); previous_valid = false;
            retry_at = now_ms() + 5000;
            demo_until = 0;
            emotions_until = 0;
            ESP_LOGW(tag, "OLED_WRITE: ERROR (audio bus retained; retry in 5s)");
        } else if (demo_until && now >= demo_until) {
            demo_until = 0;
            ESP_LOGI(tag, "OLED_TEST: TRANSPORT_OK (visual confirmation still required)");
        }
        if (available.load() && emotions_until && now >= emotions_until) {
            emotions_until = 0;
            ESP_LOGI(tag, "OLED_EMOTION_DEMO: TRANSPORT_OK (returned to live emotion)");
        }
        if (shown_pickup.exchange(pickup_drawn) != pickup_drawn)
            ESP_LOGI(tag, "OLED_PICKUP: %s", pickup_drawn ? "AT_EYES" : "DONE");
        // Low priority and bounded page writes keep I2S/audio tasks responsive.
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
} // namespace

bool buddy_display_start()
{
    if (i2c_master_get_bus_handle(I2C_NUM_1, &bus) != ESP_OK) return false;
    return xTaskCreate(eyes_task, "buddy_eyes", 4096, nullptr, 1, nullptr) == pdPASS;
}

void buddy_display_photo_begin()
{
    buddy_idle_activity();
    buddy_head_photo_hold();
    photo_shutter_at.store(0); photo_focus_at.store(now_ms());
}
void buddy_display_photo_end(bool captured)
{
    if (captured) photo_shutter_at.store(now_ms());
    else { photo_focus_at.store(0); photo_shutter_at.store(0); }
}

void buddy_display_pickup()
{
    pickup_at.store(now_ms());
    pickup_count.fetch_add(1);
}

BuddyDisplaySnapshot buddy_display_snapshot()
{
    const int64_t last = pickup_at.load();
    return {available.load(), renderer_selftest.load(),
        enabled.load() && available.load() && shown_pickup.load(),
        frames.load(), write_errors.load(), pickup_count.load(),
        last ? now_ms() - last : -1};
}

void buddy_display_status()
{
    ESP_LOGI(tag, "OLED_STATUS: enabled=%d available=%d address=0x%02x frames=%u errors=%u",
        enabled.load(), available.load(), address.load(), frames.load(), write_errors.load());
    ESP_LOGI(tag, "OLED_FACE_STATUS: emotion=%s phase=%s", buddy_emotion_name(shown_emotion.load()), face_activity_name(shown_activity.load()));
    ESP_LOGI(tag, "OLED_SLEEP_MARKS_STATUS: visible=%d", shown_sleep_marks.load());
}

bool buddy_display_command(const char *line)
{
    if (std::strcmp(line, "display-status") == 0) buddy_display_status();
    else if (std::strcmp(line, "display-off") == 0) { enabled.store(false); buddy_display_status(); }
    else if (std::strcmp(line, "display-on") == 0 || std::strcmp(line, "display-test") == 0) {
        if (std::strcmp(line, "display-test") == 0) demo.store(true);
        enabled.store(true); rescan.store(true);
        ESP_LOGI(tag, "OLED_REQUEST: QUEUED");
    } else if (std::strcmp(line, "display-emotions") == 0) {
        emotion_demo.store(true); enabled.store(true); rescan.store(true);
        ESP_LOGI(tag, "OLED_REQUEST: EMOTIONS_QUEUED");
    } else if (std::strcmp(line, "display-preview") == 0) {
        preview_requested.store(true);
        ESP_LOGI(tag, "OLED_REQUEST: PREVIEW_QUEUED");
    } else return false;
    return true;
}
