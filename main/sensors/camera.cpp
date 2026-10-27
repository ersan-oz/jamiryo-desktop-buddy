#include "camera.h"
#include "image_rotation.h"
#include "scene_change.h"
#include "faces.h"
#include "display/eyes.h"
#include "buddy.h"
#include "audio/microphone.h"
#include "network/gemini_live.h"
#include "network/firmware_update.h"
#include "servo/head_servo.h"
#include "servo/object_view.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cinttypes>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "cam_hal.h"
extern "C" {
#include "sccb.h"
}
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"

namespace {
constexpr char tag[] = "buddy_camera";
constexpr size_t jpeg_capacity = 48 * 1024;
std::atomic<bool> available{false}, enabled{true}, observing{true}, requested{false};
TaskHandle_t task = nullptr;
std::atomic<unsigned> captured{0}, selected{0};
std::atomic<bool> orientation_verified{false}, rotation_selftest{false};
std::atomic<unsigned> rotated_frames{0}, rotation_max_us{0};
constexpr unsigned rotation_degrees = 180;
std::atomic<unsigned> photo_ticket{0}, photo_serial{0}, photos{0};
QueueHandle_t photo_requests = nullptr, photo_results = nullptr;
SemaphoreHandle_t photo_lock = nullptr;
struct PhotoResult { unsigned ticket = 0; BuddyVisionFrame frame{}; };
uint8_t *jpeg_buffer = nullptr;
struct SearchJob { unsigned id, view; };
QueueHandle_t search_jobs = nullptr, search_results = nullptr;
std::atomic<unsigned> search_id{0};
uint8_t *panorama = nullptr; // Only the camera task reads/writes/frees this buffer.
unsigned panorama_id = 0, panorama_mask = 0;
struct JpegTarget { uint8_t *data; size_t size = 0; };

int64_t now_ms() { return esp_timer_get_time() / 1000; }
size_t jpeg_write(void *arg, size_t index, const void *data, size_t size)
{
    auto &target = *static_cast<JpegTarget *>(arg);
    if (index > jpeg_capacity || size > jpeg_capacity - index) return 0;
    std::memcpy(target.data + index, data, size);
    target.size = index + size;
    return size;
}
void settings(bool save)
{
    nvs_handle_t handle;
    if (nvs_open("buddy_camera", save ? NVS_READWRITE : NVS_READONLY, &handle) != ESP_OK) return;
    if (save) {
        nvs_set_u8(handle, "enabled", enabled.load());
        nvs_set_u8(handle, "observe", observing.load()); nvs_commit(handle);
    } else {
        uint8_t value;
        if (nvs_get_u8(handle, "enabled", &value) == ESP_OK) enabled.store(value != 0);
        if (nvs_get_u8(handle, "observe", &value) == ESP_OK) observing.store(value != 0);
    }
    nvs_close(handle);
}
bool frame_valid(const camera_fb_t *frame)
{
    return frame && frame->format == PIXFORMAT_RGB565 && frame->width == 320
        && frame->height == 240 && frame->len >= 320 * 240 * 2;
}
camera_fb_t *fresh_frame()
{
    // After a snapshot, stop VSYNC interrupts as well as DMA, before returning
    // the single framebuffer. The next start therefore captures a new image.
    cam_start();
    auto *frame = esp_camera_fb_get();
    cam_stop();
    if (frame_valid(frame)) {
        const int64_t began = esp_timer_get_time();
        if (!buddy_rotate_rgb565_180(frame->buf, frame->len, frame->width, frame->height)) {
            esp_camera_fb_return(frame); return nullptr;
        }
        const unsigned elapsed = static_cast<unsigned>(esp_timer_get_time() - began);
        rotation_max_us.store(std::max(rotation_max_us.load(), elapsed));
        rotated_frames.fetch_add(1); orientation_verified.store(true);
    }
    return frame;
}
void clear_panorama()
{
    if (panorama) { std::memset(panorama, 0, object_view::panorama_bytes); heap_caps_free(panorama); }
    panorama = nullptr; panorama_id = panorama_mask = 0;
}
void capture_search(const SearchJob &job)
{
    BuddySearchCapture result; result.search_id = job.id; result.view = job.view;
    if (panorama_id != job.id) clear_panorama();
    if (job.view == 1 && !panorama) {
        panorama = static_cast<uint8_t *>(heap_caps_calloc(1, object_view::panorama_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        panorama_id = job.id;
    }
    const unsigned expected_mask = (1u << (job.view - 1)) - 1;
    if (panorama && panorama_mask == expected_mask && buddy_camera_enabled()
        && buddy_head_object_settled(job.id) && !buddy_ota_busy()) {
        camera_fb_t *frame = fresh_frame();
        if (frame_valid(frame) && search_id.load() == job.id && buddy_head_object_settled(job.id)) {
            ++captured;
            result.ok = object_view::copy_panel(panorama, object_view::panorama_bytes, frame->buf, frame->len, job.view);
            if (result.ok) panorama_mask |= 1u << (job.view - 1);
        }
        if (frame) esp_camera_fb_return(frame);
    }
    if (result.ok && job.view == object_view::count) {
        result.ok = false;
        for (uint8_t quality : {uint8_t(55), uint8_t(35)}) {
            JpegTarget target{jpeg_buffer};
            const bool converted = fmt2jpg_cb(panorama, object_view::panorama_bytes,
                object_view::width, object_view::height * object_view::count, PIXFORMAT_RGB565, quality, jpeg_write, &target);
            if (converted && target.size >= 4 && target.size <= jpeg_capacity
                && target.data[0] == 0xff && target.data[1] == 0xd8
                && target.data[target.size - 2] == 0xff && target.data[target.size - 1] == 0xd9) {
                result.frame.jpeg = static_cast<uint8_t *>(heap_caps_malloc(target.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (result.frame.jpeg) {
                    std::memcpy(result.frame.jpeg, jpeg_buffer, target.size);
                    result.frame.bytes = target.size; result.frame.captured_ms = now_ms();
                    result.frame.requested = true; result.ok = true;
                }
            }
            std::memset(jpeg_buffer, 0, jpeg_capacity);
            if (result.ok) break;
        }
    }
    if (job.view == object_view::count || !result.ok) clear_panorama();
    if (search_id.load() == job.id && xQueueSend(search_results, &result, 0) == pdTRUE) return;
    if (result.frame.jpeg) { std::memset(result.frame.jpeg, 0, result.frame.bytes); heap_caps_free(result.frame.jpeg); }
}
void finish_photo(unsigned ticket, const uint8_t *jpeg, size_t bytes, int64_t time)
{
    if (!ticket || photo_ticket.load() != ticket) return;
    PhotoResult result; result.ticket = ticket; result.frame.captured_ms = time;
    if (jpeg && bytes && buddy_camera_enabled()) {
        result.frame.jpeg = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (result.frame.jpeg) { std::memcpy(result.frame.jpeg, jpeg, bytes); result.frame.bytes = bytes; }
    }
    if (photo_ticket.load() == ticket && xQueueSend(photo_results, &result, 0) == pdTRUE) return;
    if (result.frame.jpeg) { std::memset(result.frame.jpeg, 0, result.frame.bytes); heap_caps_free(result.frame.jpeg); }
}
BuddySceneChange::Grid grid_from(const camera_fb_t &frame)
{
    BuddySceneChange::Grid grid{};
    for (unsigned y = 0; y < 24; ++y) for (unsigned x = 0; x < 32; ++x) {
        unsigned sum = 0;
        for (unsigned dy : {2u, 7u}) for (unsigned dx : {2u, 7u}) {
            const size_t offset = ((y * 10 + dy) * frame.width + x * 10 + dx) * 2;
            const uint16_t pixel = (uint16_t(frame.buf[offset]) << 8) | frame.buf[offset + 1];
            const unsigned r = ((pixel >> 11) & 31) * 255 / 31;
            const unsigned g = ((pixel >> 5) & 63) * 255 / 63;
            const unsigned b = (pixel & 31) * 255 / 31;
            sum += (77 * r + 150 * g + 29 * b) >> 8;
        }
        grid[y * 32 + x] = sum / 4;
    }
    return grid;
}
void camera_task(void *)
{
    BuddySceneChange scene;
    int64_t next_scan = now_ms() + 3000;
    unsigned warmup = 0;
    for (;;) {
        if (panorama && (panorama_id != search_id.load() || !buddy_camera_enabled() || buddy_ota_busy())) clear_panorama();
        if (buddy_ota_busy()) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        buddy_faces_tick();
        SearchJob job{};
        if (xQueueReceive(search_jobs, &job, 0) == pdTRUE && job.id == search_id.load()) {
            capture_search(job); scene.reset(); warmup = 0;
        }
        if (search_id.load()) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); continue; }
        const bool explicit_request = requested.exchange(false);
        unsigned photo = 0;
        if (xQueueReceive(photo_requests, &photo, 0) == pdTRUE && photo != photo_ticket.load()) photo = 0;
        const int64_t now = now_ms();
        const unsigned person_generation = !photo && !explicit_request && buddy_faces_wants_person_frame(now)
            ? buddy_head_person_generation() : 0;
        const bool idle = !buddy_audio_busy() && !microphone_interacting() && !buddy_ai_output_pending();
        const bool face_scan = !person_generation && !photo && buddy_faces_wants_frame(now, idle);
        const bool track_scan = !person_generation && !photo && buddy_faces_wants_tracking_frame(now);
        const bool scan = !photo && buddy_camera_observing() && now >= next_scan
            && !buddy_faces_exclusive_capture()
            && buddy_ai_scene_allowed() && !buddy_audio_busy() && !microphone_interacting();
        if (!buddy_camera_enabled()) {
            scene.reset(); warmup = 0;
            if (explicit_request) buddy_ai_offer_vision(BuddyVisionFrame{nullptr, 0, now, true, 0});
            finish_photo(photo, nullptr, 0, now);
        } else if (explicit_request || scan || face_scan || track_scan || person_generation || photo) {
            next_scan = now + 1500;
            const int64_t started = now_ms();
            if (photo) buddy_display_photo_begin();
            camera_fb_t *frame = fresh_frame();
            if (!frame_valid(frame)) {
                if (frame) esp_camera_fb_return(frame);
                ESP_LOGW(tag, "CAMERA_CAPTURE: UNAVAILABLE");
                if (explicit_request) buddy_ai_offer_vision(BuddyVisionFrame{nullptr, 0, now, true, 0});
                if (person_generation) buddy_head_person_observe(false, 0, started, person_generation);
                finish_photo(photo, nullptr, 0, now);
                if (photo) buddy_display_photo_end(false);
            } else {
                ++captured;
                const auto grid = grid_from(*frame);
                if (buddy_head_motion_recent()) { scene.reset(); warmup = 0; }
                const unsigned score = scene.update(grid);
                if (warmup < 3) ++warmup;
                const bool scene_event = scan && score && warmup >= 3 && !buddy_head_motion_recent();
                if (person_generation && person_generation == buddy_head_person_generation() && !buddy_audio_busy())
                    buddy_faces_process(frame->buf, frame->width, frame->height, false, person_generation, started);
                else if (face_scan || track_scan) buddy_faces_process(frame->buf, frame->width, frame->height, face_scan);
                if (explicit_request || scene_event || photo) {
                    JpegTarget target{jpeg_buffer};
                    const bool converted = frame2jpg_cb(frame, 65, jpeg_write, &target);
                    const bool jpeg_ok = converted && target.size >= 4 && target.size <= jpeg_capacity
                        && target.data[0] == 0xff && target.data[1] == 0xd8
                        && target.data[target.size - 2] == 0xff && target.data[target.size - 1] == 0xd9;
                    const int64_t captured_ms = frame->timestamp.tv_sec * int64_t(1000)
                        + frame->timestamp.tv_usec / 1000;
                    if (photo) buddy_display_photo_end(jpeg_ok && buddy_camera_enabled() && photo_ticket.load() == photo);
                    finish_photo(photo, jpeg_ok ? target.data : nullptr, jpeg_ok ? target.size : 0, captured_ms);
                    if (photo) ESP_LOGI(tag, "CAMERA_PHOTO: %s jpeg=%zu capture_ms=%" PRId64, jpeg_ok ? "CAPTURED" : "FAILED", jpeg_ok ? target.size : 0, now_ms() - started);
                    if ((explicit_request || scene_event) && jpeg_ok && buddy_camera_enabled()) {
                        auto *copy = static_cast<uint8_t *>(heap_caps_malloc(target.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                        if (copy) {
                            std::memcpy(copy, target.data, target.size);
                            if (buddy_ai_offer_vision(BuddyVisionFrame{copy, target.size, captured_ms, explicit_request, score})) {
                                ++selected;
                                ESP_LOGI(tag, "CAMERA_FRAME: %s jpeg=%zu capture_ms=%" PRId64 " change=%u",
                                         explicit_request ? "requested" : "scene_change", target.size, now_ms() - started, score);
                            } else heap_caps_free(copy);
                        } else if (explicit_request) buddy_ai_offer_vision(BuddyVisionFrame{nullptr, 0, now, true, 0});
                    } else if (explicit_request) buddy_ai_offer_vision(BuddyVisionFrame{nullptr, 0, now, true, 0});
                    std::memset(jpeg_buffer, 0, target.size);
                }
                esp_camera_fb_return(frame);
            }
        }
        // Notifications expedite a requested snapshot; otherwise poll locally.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }
}
} // namespace

bool buddy_camera_init()
{
    const bool rotation_ok = buddy_rotation_self_test(); rotation_selftest.store(rotation_ok);
    ESP_LOGI(tag, "CAMERA_ROTATION_TEST: %s (180deg/pixel-order/colors/bounds/odd-size)", rotation_ok ? "PASS" : "FAIL");
    if (!rotation_ok) return false;
    const bool scene_ok = buddy_scene_self_test();
    ESP_LOGI(tag, "CAMERA_SCENE_TEST: %s (exposure/noise/foreground/stable)", scene_ok ? "PASS" : "FAIL");
    if (!scene_ok) return false;
    gpio_config_t power{}; power.pin_bit_mask = 1ULL << 18; power.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&power) != ESP_OK || gpio_set_level(GPIO_NUM_18, 0) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(500));
    camera_config_t config{};
    config.pin_pwdn = -1; config.pin_reset = -1;
    config.pin_xclk = 21; config.pin_sccb_sda = 12; config.pin_sccb_scl = 9;
    config.pin_d0 = 3; config.pin_d1 = 42; config.pin_d2 = 46; config.pin_d3 = 48;
    config.pin_d4 = 4; config.pin_d5 = 17; config.pin_d6 = 11; config.pin_d7 = 13;
    config.pin_vsync = 10; config.pin_href = 14; config.pin_pclk = 40;
    config.xclk_freq_hz = 20000000; config.ledc_timer = LEDC_TIMER_0; config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565; config.frame_size = FRAMESIZE_QVGA;
    config.fb_count = 1; config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    const esp_err_t result = esp_camera_init(&config);
    if (result != ESP_OK) {
        ESP_LOGW(tag, "CAMERA_INIT: UNAVAILABLE error=%s", esp_err_to_name(result));
        return false;
    }
    auto *sensor = esp_camera_sensor_get();
    const bool identity_ok = sensor && sensor->id.PID == GC0308_PID;
    // Sensor settings/AEC operate autonomously after setup. No sensor register
    // access is allowed after releasing SCCB; IMU/audio now reuse both ports.
    const int released = SCCB_Deinit();
    auto *frame = identity_ok && released == ESP_OK ? fresh_frame() : nullptr;
    const bool image_ok = frame_valid(frame);
    if (frame) esp_camera_fb_return(frame);
    if (!image_ok) {
        esp_camera_deinit(); ESP_LOGW(tag, "CAMERA_INIT: UNAVAILABLE frame/pid/bus"); return false;
    }
    jpeg_buffer = static_cast<uint8_t *>(heap_caps_malloc(jpeg_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpeg_buffer) { esp_camera_deinit(); return false; }
    available.store(true);
    ESP_LOGI(tag, "CAMERA_INIT: PASS GC0308 320x240 RGB565 SCCB_RELEASED SNAPSHOT_ONLY (no stream server)");
    return true;
}
bool buddy_camera_start()
{
    settings(false);
    if (!available.load()) return true; // Voice and IMU keep working without camera.
    photo_requests = xQueueCreate(1, sizeof(unsigned)); photo_results = xQueueCreate(1, sizeof(PhotoResult)); photo_lock = xSemaphoreCreateMutex();
    search_jobs = xQueueCreate(1, sizeof(SearchJob)); search_results = xQueueCreate(1, sizeof(BuddySearchCapture));
    if (!photo_requests || !photo_results || !photo_lock || !search_jobs || !search_results) return false;
    return xTaskCreate(camera_task, "buddy_camera", 12288, nullptr, 2, &task) == pdPASS;
}
bool buddy_camera_available() { return available.load(); }
bool buddy_camera_enabled() { return available.load() && enabled.load(); }
bool buddy_camera_observing() { return buddy_camera_enabled() && observing.load(); }
BuddyCameraSnapshot buddy_camera_snapshot()
{
    return {buddy_camera_available(), buddy_camera_enabled(), orientation_verified.load(),
        rotation_selftest.load(), rotation_degrees, captured.load(), photos.load(), rotated_frames.load(), rotation_max_us.load()};
}
bool buddy_camera_request()
{
    if (!task || !buddy_camera_enabled() || search_id.load()) return false;
    requested.store(true); xTaskNotifyGive(task); return true;
}
bool buddy_camera_search_request(unsigned id, unsigned view)
{
    if (!task || !id || !object_view::valid(view, 500) || !buddy_camera_enabled() || photo_ticket.load()
        || buddy_faces_exclusive_capture() || buddy_ota_busy()) return false;
    if (view == 1) search_id.store(id);
    if (search_id.load() != id) return false;
    const SearchJob job{id, view};
    if (xQueueSend(search_jobs, &job, 0) != pdTRUE) return false;
    xTaskNotifyGive(task); return true;
}
bool buddy_camera_search_result(BuddySearchCapture &result)
{
    return search_results && xQueueReceive(search_results, &result, 0) == pdTRUE;
}
void buddy_camera_search_cancel(unsigned id)
{
    unsigned expected = id;
    if (id && search_id.compare_exchange_strong(expected, 0) && task) xTaskNotifyGive(task);
}
bool buddy_camera_take_photo(BuddyVisionFrame &frame, uint32_t timeout_ms)
{
    frame = BuddyVisionFrame{};
    if (!task || !photo_lock || !buddy_camera_enabled() || search_id.load() || xSemaphoreTake(photo_lock, 0) != pdTRUE) return false;
    PhotoResult result;
    while (xQueueReceive(photo_results, &result, 0) == pdTRUE) {
        if (result.frame.jpeg) { std::memset(result.frame.jpeg, 0, result.frame.bytes); heap_caps_free(result.frame.jpeg); }
    }
    unsigned stale; while (xQueueReceive(photo_requests, &stale, 0) == pdTRUE) {}
    const unsigned ticket = photo_serial.fetch_add(1) + 1;
    photo_ticket.store(ticket);
    bool ok = xQueueSend(photo_requests, &ticket, 0) == pdTRUE;
    if (ok) {
        xTaskNotifyGive(task);
        const int64_t deadline = now_ms() + timeout_ms;
        ok = false;
        for (;;) {
            const int64_t remaining = deadline - now_ms();
            if (remaining <= 0 || xQueueReceive(photo_results, &result, std::max<TickType_t>(1, pdMS_TO_TICKS(remaining))) != pdTRUE) break;
            if (result.ticket == ticket && result.frame.jpeg && buddy_camera_enabled()) { frame = result.frame; ok = true; ++photos; break; }
            if (result.frame.jpeg) { std::memset(result.frame.jpeg, 0, result.frame.bytes); heap_caps_free(result.frame.jpeg); }
            if (result.ticket == ticket) break;
        }
    }
    photo_ticket.store(0); xSemaphoreGive(photo_lock); return ok;
}
void buddy_camera_status()
{
    ESP_LOGI(tag, "CAMERA_STATUS: available=%d enabled=%d observe=%d captured=%u selected=%u photos=%u",
             buddy_camera_available(), buddy_camera_enabled(), buddy_camera_observing(), captured.load(), selected.load(), photos.load());
    ESP_LOGI(tag, "CAMERA_ORIENTATION_STATUS: rotation=%u verified=%d rotated_frames=%u max_us=%u",
             rotation_degrees, orientation_verified.load(), rotated_frames.load(), rotation_max_us.load());
}
bool buddy_camera_command(const char *line)
{
    bool matched = true;
    if (!std::strcmp(line, "camera-off")) enabled.store(false);
    else if (!std::strcmp(line, "camera-on")) enabled.store(true);
    else if (!std::strcmp(line, "camera-observe-off")) observing.store(false);
    else if (!std::strcmp(line, "camera-observe-on")) { enabled.store(true); observing.store(true); }
    else if (!std::strcmp(line, "camera-status")) { buddy_camera_status(); return true; }
    else matched = false;
    if (matched) { settings(true); if (task) xTaskNotifyGive(task); buddy_camera_status(); }
    return matched;
}
