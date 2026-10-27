#include "faces.h"
#include "face_matching.h"
#include "face_greeting.h"
#include "camera.h"
#include "buddy.h"
#include "audio/microphone.h"
#include "network/gemini_live.h"
#include "servo/head_servo.h"
#include "servo/person_seek.h"
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <new>
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

// Public snapshot does not embed portrait test fixtures.

namespace {
using namespace buddy_faces;
constexpr char tag[] = "buddy_faces", partition[] = "face_store", space[] = "profiles";
enum class Action { Enroll, Forget, Identify, List, Test };
struct Request { Action action; unsigned epoch = 0; char id[128]{}, name[name_size]{}; };
Database *database = nullptr, *working = nullptr;
Person *enrollment = nullptr;
SemaphoreHandle_t mutex = nullptr;
QueueHandle_t requests = nullptr, events = nullptr;
BuddyMemoryReply reply_sink = nullptr;
HumanFaceDetect *detector = nullptr;
HumanFaceFeat *extractor = nullptr;
std::atomic<bool> storage_ready{false}, enabled{true}, model_ready{false}, enrolling{false};
std::atomic<unsigned> epoch{0}, scans{0}, errors{0}, writes{0}, saved_count{0}, enrollment_samples{0};
int64_t next_scan = 0, enrollment_deadline = 0, identify_deadline = 0;
int64_t next_tracking_scan = 0;
Request identify_request{};
std::atomic<bool> identify_waiting{false};
unsigned last_count = 0, stable_count = 0;
int candidate = -1;
GreetingCadence<capacity> greetings;
// Guarded by the database mutex, read by the network task before speaking.
char confirmed_name[name_size]{};
int64_t confirmed_at = 0;
int64_t last_frame = 0, last_known = 0;
float last_similarity = 0;
char last_name[name_size]{};
const char *last_state = "not_scanned";

int64_t now_ms() { return esp_timer_get_time() / 1000; }
bool equal(const char *a, const char *b) { return a && !std::strcmp(a, b); }
const char *string(const cJSON *args, const char *key)
{
    const auto *item = cJSON_GetObjectItemCaseSensitive(args, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}
void snapshot(Database &target)
{
    xSemaphoreTake(mutex, portMAX_DELAY); target = *database; xSemaphoreGive(mutex);
}
esp_err_t save(const char *ns, const Database &db)
{
    if (!valid_database(db)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open_from_partition(partition, ns, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "faces", &db, sizeof(db));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle); return result;
}
esp_err_t load(const char *ns, Database &db)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open_from_partition(partition, ns, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    size_t size = sizeof(db);
    result = nvs_get_blob(handle, "faces", &db, &size); nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result == ESP_OK && (size != sizeof(db) || !valid_database(db))) return ESP_ERR_INVALID_ARG;
    return result;
}
bool commit()
{
    const auto result = save(space, *working);
    if (result != ESP_OK) { ++errors; ESP_LOGW(tag, "FACE_STORAGE: WRITE_ERROR code=%s", esp_err_to_name(result)); return false; }
    xSemaphoreTake(mutex, portMAX_DELAY); *database = *working;
    confirmed_name[0] = 0; confirmed_at = 0; xSemaphoreGive(mutex);
    saved_count.store(working->count); ++writes; candidate = -1; stable_count = 0;
    greetings.reset_profiles();
    return true;
}
void reply(const Request &request, cJSON *result)
{
    if (!request.id[0] || request.epoch != epoch.load() || !buddy_ai_ready()) { cJSON_Delete(result); return; }
    const char *name = request.action == Action::Enroll ? "enroll_face" : request.action == Action::Forget ? "forget_face" : request.action == Action::List ? "list_registered_people" : "identify_person";
    cJSON *root = cJSON_CreateObject();
    auto *responses = cJSON_AddArrayToObject(cJSON_AddObjectToObject(root, "toolResponse"), "functionResponses");
    auto *response = cJSON_CreateObject(); cJSON_AddItemToArray(responses, response);
    cJSON_AddStringToObject(response, "id", request.id); cJSON_AddStringToObject(response, "name", name);
    cJSON_AddItemToObject(response, "response", result);
    const bool queued = reply_sink(root);
    ESP_LOGI(tag, "FACE_TOOL_REPLY: tool=%s queued=%d (identity withheld)", name, queued);
}
bool emit(BuddyFaceEvent::Type type, const char *name, const char *reason = "")
{
    BuddyFaceEvent event{}; event.type = type; event.time_ms = now_ms();
    if (name) std::snprintf(event.name, sizeof(event.name), "%s", name);
    std::snprintf(event.reason, sizeof(event.reason), "%s", reason);
    if (xQueueSend(events, &event, 0) == pdTRUE) return true;
    BuddyFaceEvent old; xQueueReceive(events, &old, 0);
    return xQueueSend(events, &event, 0) == pdTRUE;
}
void clear_enrollment()
{
    if (enrollment) std::memset(static_cast<void *>(enrollment), 0, sizeof(Person));
    enrolling.store(false); enrollment_samples.store(0);
}
void enrollment_failed(const char *reason)
{
    emit(BuddyFaceEvent::Type::EnrollmentFailed, enrollment->name, reason);
    ESP_LOGI(tag, "FACE_ENROLL: STOP reason=%s (name withheld)", reason); clear_enrollment();
}
void finish_enrollment()
{
    snapshot(*working); int index = find(*working, enrollment->name);
    if (index < 0 && working->count < capacity) index = working->count++;
    if (index < 0) { enrollment_failed("face_capacity_full"); return; }
    working->people[index] = *enrollment;
    if (!commit()) { enrollment_failed("flash_write_failed"); return; }
    emit(BuddyFaceEvent::Type::Enrolled, enrollment->name);
    ESP_LOGI(tag, "FACE_ENROLL: SAVED profiles=%u (name/features withheld)", saved_count.load());
    clear_enrollment();
}
bool detection_model()
{
    if (!detector) {
        detector = new (std::nothrow) HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, false);
        // Broaden only the coarse proposal stage for the low-resolution camera.
        // The refinement, tracking confidence and identity checks stay intact.
        if (detector) detector->set_score_thr(0.30f, 0);
    }
    return detector != nullptr;
}
bool models()
{
    if (model_ready.load()) return true;
    if (!storage_ready.load()) return false;
    const int64_t started = now_ms();
    detection_model();
    if (!extractor) extractor = new (std::nothrow) HumanFaceFeat(HumanFaceFeat::MFN_S8_V1, false);
    if (!detector || !extractor || extractor->get_feat_len() != dimensions) {
        ++errors; ESP_LOGW(tag, "FACE_MODELS: UNAVAILABLE"); return false;
    }
    model_ready.store(true);
    ESP_LOGI(tag, "FACE_MODELS: READY MFN_S8_V1 dimensions=%u load_ms=%" PRId64 " free_internal=%zu free_psram=%zu",
             dimensions, now_ms() - started, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}
bool self_test()
{
    void *test_storage = heap_caps_malloc(sizeof(Database), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *test = static_cast<Database *>(test_storage);
    auto *readback = static_cast<Database *>(heap_caps_malloc(sizeof(Database), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *feature = static_cast<float *>(heap_caps_calloc(dimensions, sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!test || !readback || !feature) { heap_caps_free(test); heap_caps_free(readback); heap_caps_free(feature); return false; }
    new (test) Database{};
    test->count = capacity;
    for (unsigned i = 0; i < capacity; ++i) {
        auto &person = test->people[i]; std::snprintf(person.name, name_size, "Test %u", i);
        person.samples = samples_per_person;
        for (auto &sample : person.features) sample[i] = 1;
    }
    feature[0] = 1;
    bool ok = valid_database(*test) && match(*test, feature).index == 0;
    feature[0] = 0; feature[10] = 1; ok &= match(*test, feature).index == -1;
    feature[10] = 0; feature[0] = feature[1] = std::sqrt(0.5f); ok &= match(*test, feature).index == -1;
    feature[0] = 2; ok &= !valid_feature(feature);
    ok &= !valid_name("bad\nname") && valid_name("Örnek") && !valid_name("all");
    ok &= save("test", *test) == ESP_OK;
    new (readback) Database{};
    ok &= load("test", *readback) == ESP_OK && !std::memcmp(test, readback, sizeof(Database));
    ok &= forget(*readback, "Test 0") && readback->count == capacity - 1 && find(*readback, "Test 1") == 0;
    ok &= forget(*readback, "all") && readback->count == 0 && !forget(*readback, "missing");
    nvs_handle_t handle;
    if (nvs_open_from_partition(partition, "test", NVS_READWRITE, &handle) == ESP_OK) {
        const auto erased = nvs_erase_key(handle, "faces");
        ok &= erased == ESP_OK && nvs_commit(handle) == ESP_OK; nvs_close(handle);
    } else ok = false;
    if (models()) {
        auto *blank = static_cast<uint8_t *>(heap_caps_calloc(320 * 240, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (blank) {
            dl::image::img_t image{blank, 320, 240, dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
            ok &= detector->run(image).empty(); heap_caps_free(blank);
            // Real-face accuracy requires a consented, device-local test.
        } else ok = false;
    } else ok = false;
    std::memset(static_cast<void *>(test), 0, sizeof(Database)); std::memset(static_cast<void *>(readback), 0, sizeof(Database));
    heap_caps_free(test); heap_caps_free(readback); heap_caps_free(feature); return ok;
}
void finish_identify(const char *error = nullptr)
{
    if (!identify_waiting) return;
    auto *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(result, "available", !error);
    cJSON_AddStringToObject(result, "status", error ? error : last_state);
    cJSON_AddNumberToObject(result, "face_count", error ? 0 : last_count);
    if (!error && equal(last_state, "known")) cJSON_AddStringToObject(result, "name", last_name);
    cJSON_AddNumberToObject(result, "age_ms", error ? 0 : now_ms() - last_frame);
    reply(identify_request, result); identify_waiting = false;
}
void declaration(cJSON *array, const char *name, const char *description, bool named)
{
    auto *function = cJSON_CreateObject(); cJSON_AddItemToArray(array, function);
    cJSON_AddStringToObject(function, "name", name); cJSON_AddStringToObject(function, "behavior", "BLOCKING");
    cJSON_AddStringToObject(function, "description", description);
    auto *parameters = cJSON_AddObjectToObject(function, "parameters"); cJSON_AddStringToObject(parameters, "type", "OBJECT");
    auto *properties = cJSON_AddObjectToObject(parameters, "properties");
    if (named) {
        auto *property = cJSON_AddObjectToObject(properties, "name"); cJSON_AddStringToObject(property, "type", "STRING");
        cJSON_AddStringToObject(property, "description", "The person's explicitly supplied name, maximum 47 UTF-8 bytes, letters/spaces/digits/hyphen/apostrophe only. For forgetting all face profiles, use all.");
        auto *required = cJSON_AddArrayToObject(parameters, "required"); cJSON_AddItemToArray(required, cJSON_CreateString("name"));
    }
}
} // namespace

bool buddy_faces_start(BuddyMemoryReply sink)
{
    reply_sink = sink; mutex = xSemaphoreCreateMutex(); requests = xQueueCreate(4, sizeof(Request)); events = xQueueCreate(4, sizeof(BuddyFaceEvent));
    void *a = heap_caps_malloc(sizeof(Database), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *b = heap_caps_malloc(sizeof(Database), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    void *c = heap_caps_malloc(sizeof(Person), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a || !b || !c || !mutex || !requests || !events) { heap_caps_free(a); heap_caps_free(b); heap_caps_free(c); return false; }
    database = new (a) Database{}; working = new (b) Database{}; enrollment = new (c) Person{};
    esp_err_t result = nvs_flash_init_partition(partition);
    if (result == ESP_OK) result = load(space, *database);
    storage_ready.store(result == ESP_OK); saved_count.store(result == ESP_OK ? database->count : 0);
    nvs_handle_t handle;
    if (result == ESP_OK && nvs_open_from_partition(partition, space, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t value; if (nvs_get_u8(handle, "enabled", &value) == ESP_OK) enabled.store(value != 0); nvs_close(handle);
    }
    ESP_LOGI(tag, "FACE_INIT: %s profiles=%u capacity=%u code=%s (names/features withheld)", result == ESP_OK ? "READY" : "STORAGE_ERROR", saved_count.load(), capacity, esp_err_to_name(result));
    ESP_LOGI(tag, "FACE_GREETING_TEST: %s (per-person/global cooldown, repeat, profile reset, clock rollback)", greeting_cadence_self_test() ? "PASS" : "FAIL");
    return true; // Camera/audio still work if face storage cannot be opened.
}
void buddy_faces_tick()
{
    if (!requests) return;
    const int64_t now = now_ms();
    if (enrolling.load() && (now > enrollment_deadline || !buddy_camera_enabled() || !enabled.load()))
        enrollment_failed(!buddy_camera_enabled() || !enabled.load() ? "camera_or_faces_disabled" : "no_consistent_single_face_timeout");
    if (enrolling.load() && enrollment_samples.load() == samples_per_person
        && !buddy_audio_busy() && !microphone_interacting()) finish_enrollment();
    if (identify_waiting && (now > identify_deadline || !buddy_camera_enabled() || !enabled.load())) finish_identify("camera_disabled_or_timeout");
    Request request;
    if (xQueueReceive(requests, &request, 0) != pdTRUE) return;
    if (request.epoch != epoch.load() && request.id[0]) return;
    if (request.action == Action::Test) {
        if (buddy_audio_busy() || microphone_interacting()) { xQueueSendToFront(requests, &request, 0); return; }
        const bool ok = storage_ready.load() && self_test();
        ESP_LOGI(tag, "FACE_SELFTEST: %s (known/unknown/ambiguity, UTF8, max capacity, NVS/cleanup, model blank frame)", ok ? "PASS" : "FAIL"); return;
    }
    auto *result = cJSON_CreateObject(); const char *error = nullptr;
    bool applied = false, changed = false;
    if (!storage_ready.load()) error = "storage_unavailable";
    else if (request.action == Action::List) {
        snapshot(*working); auto *names = cJSON_AddArrayToObject(result, "names");
        for (unsigned i = 0; i < working->count; ++i) cJSON_AddItemToArray(names, cJSON_CreateString(working->people[i].name));
        applied = true;
    } else if (request.action == Action::Forget) {
        if (buddy_audio_busy()) { cJSON_Delete(result); xQueueSendToFront(requests, &request, 0); return; }
        if (enrolling.load()) { clear_enrollment(); ESP_LOGI(tag, "FACE_ENROLL: CANCELLED_BY_FORGET"); }
        snapshot(*working); changed = forget(*working, request.name);
        applied = !changed || commit();
        if (!applied) error = "flash_write_failed";
        last_name[0] = 0; last_state = "not_scanned";
        if (applied) xQueueReset(events);
    } else if (!buddy_camera_enabled() || !enabled.load()) error = "camera_or_faces_disabled";
    else if (request.action == Action::Enroll) {
        snapshot(*working);
        if (enrolling.load()) error = "enrollment_busy";
        else if (find(*working, request.name) < 0 && working->count == capacity) error = "face_capacity_full";
        else {
            clear_enrollment(); std::strcpy(enrollment->name, request.name);
            enrolling.store(true); enrollment_deadline = now + 30000; next_scan = now + 1500; applied = true;
            ESP_LOGI(tag, "FACE_ENROLL: START samples_required=%u (name withheld)", samples_per_person);
        }
        cJSON_AddBoolToObject(result, "started", applied); cJSON_AddBoolToObject(result, "saved", false);
    } else {
        if (enrolling.load()) error = "enrollment_in_progress";
        else if (identify_waiting) error = "identify_busy";
        else { identify_request = request; identify_waiting = true; identify_deadline = now + 10000; next_scan = now; cJSON_Delete(result); return; }
    }
    cJSON_AddBoolToObject(result, "applied", applied); cJSON_AddBoolToObject(result, "deleted", applied && changed);
    cJSON_AddNumberToObject(result, "profile_count", saved_count.load());
    if (error) cJSON_AddStringToObject(result, "error", error);
    reply(request, result);
    if (request.action == Action::Forget) ESP_LOGI(tag, "FACE_FORGET: applied=%d changed=%d profiles=%u (names withheld)", applied, applied && changed, saved_count.load());
}
bool buddy_faces_wants_frame(int64_t now, bool otherwise_idle)
{
    if (!storage_ready.load() || !enabled.load() || !buddy_camera_enabled() || now < next_scan || buddy_audio_busy() || microphone_interacting()) return false;
    if (enrolling.load()) return enrollment_samples.load() < samples_per_person;
    return identify_waiting || otherwise_idle;
}
bool buddy_faces_exclusive_capture() { return enrolling.load() || identify_waiting; }
bool buddy_faces_tracking_enabled() { return enabled.load(); }
bool buddy_faces_wants_person_frame(int64_t now)
{
    return enabled.load() && now >= next_tracking_scan && buddy_head_person_generation()
        && !buddy_faces_exclusive_capture() && !buddy_audio_busy();
}
bool buddy_faces_wants_tracking_frame(int64_t now)
{
    return enabled.load() && now >= next_tracking_scan && buddy_head_follow_scanning();
}
void buddy_faces_process(uint8_t *pixels, unsigned width, unsigned height, bool recognize,
                         unsigned person_generation, int64_t captured_ms)
{
    const unsigned generation = buddy_head_follow_generation();
    if (!pixels || width != 320 || height != 240 || !detection_model()) {
        if (person_generation) buddy_head_person_observe(false, 0, captured_ms, person_generation);
        if (recognize) finish_identify("model_unavailable");
        next_tracking_scan = now_ms() + 1000; return;
    }
    const int64_t started = now_ms();
    if (recognize) next_scan = started + (enrolling.load() ? 1500 : 3000);
    dl::image::img_t image{pixels, static_cast<uint16_t>(width), static_cast<uint16_t>(height), dl::image::DL_IMAGE_PIX_TYPE_RGB565BE};
    auto &detected = detector->run(image);
    // Reuse this detection for tracking. Recognition and embedding extraction
    // keep their original slow schedule; extra follow frames never identify.
    next_tracking_scan = now_ms() + 500;
    if (person_generation) {
        const dl::detect::result_t *best = nullptr;
        int best_area = 0;
        for (const auto &face : detected) {
            if (face.box.size() != 4 || !person_seek::face_box(face.box[0], face.box[1], face.box[2], face.box[3], face.score, width, height)) continue;
            const int area = (face.box[2] - face.box[0]) * (face.box[3] - face.box[1]);
            if (area > best_area) { best = &face; best_area = area; }
        }
        const int x = best ? (best->box[0] + best->box[2] - static_cast<int>(width)) * 1000 / static_cast<int>(width) : 0;
        buddy_head_person_observe(best != nullptr, x, captured_ms, person_generation);
        return; // Anonymous detection: no embedding, matching, enrollment or greeting.
    }
    const auto *tracked = detected.size() == 1 ? &detected.front() : nullptr;
    const bool track_valid = tracked && tracked->box.size() == 4 && tracked->score >= 0.65f
        && tracked->box[0] >= 0 && tracked->box[1] >= 0
        && tracked->box[2] <= static_cast<int>(width) && tracked->box[3] <= static_cast<int>(height)
        && tracked->box[2] - tracked->box[0] >= 35 && tracked->box[3] - tracked->box[1] >= 35;
    const int x_milli = track_valid ? (tracked->box[0] + tracked->box[2] - static_cast<int>(width)) * 1000 / static_cast<int>(width) : 0;
    buddy_head_follow_observe(track_valid, x_milli, now_ms(), generation);
    if (!recognize) {
        ESP_LOGI(tag, "FACE_TRACK_SCAN: valid=%d faces=%u x_milli=%d score=%.3f elapsed_ms=%lld",
                 track_valid, static_cast<unsigned>(detected.size()), x_milli, tracked ? tracked->score : 0,
                 static_cast<long long>(now_ms() - started));
        return;
    }
    int seen_index = -1;
    ++scans; last_count = detected.size(); last_frame = now_ms(); last_name[0] = 0; last_similarity = 0;
    last_state = detected.empty() ? "no_face" : detected.size() > 1 ? "multiple_faces" : "unknown";
    const bool single = detected.size() == 1;
    const auto *face = single ? &detected.front() : nullptr;
    const bool adequate = face && face->box.size() == 4 && face->keypoint.size() == 10
        && face->score >= 0.65f && face->box[2] - face->box[0] >= (enrolling.load() ? 65 : 45)
        && face->box[3] - face->box[1] >= (enrolling.load() ? 65 : 45);
    if (adequate && (enrolling.load() || saved_count.load() > 0) && !buddy_audio_busy() && !microphone_interacting()) {
        dl::TensorBase *feature = models() ? extractor->run(image, face->keypoint) : nullptr;
        const float *values = feature && feature->get_size() == dimensions ? feature->get_element_ptr<float>() : nullptr;
        if (valid_feature(values)) {
            if (enrolling.load() && enrollment->samples < samples_per_person) {
                const bool consistent = !enrollment->samples || similarity(enrollment->features[0], values) >= match_threshold;
                if (consistent) {
                    std::memcpy(enrollment->features[enrollment->samples++], values, sizeof(Feature));
                    enrollment_samples.store(enrollment->samples);
                    ESP_LOGI(tag, "FACE_ENROLL: SAMPLE %u/%u detect_score=%.2f", enrollment->samples, samples_per_person, face->score);
                    // The next tick commits only when audio and speech are idle.
                } else ESP_LOGI(tag, "FACE_ENROLL: WAIT reason=different_face_or_angle");
            } else {
                snapshot(*working); const auto result = match(*working, values); last_similarity = result.best;
                if (result.index >= 0) {
                    if (candidate == result.index && started - last_known < 8000) ++stable_count;
                    else { candidate = result.index; stable_count = 1; }
                    last_known = started;
                    if (stable_count >= 2) {
                        last_state = "known"; std::strcpy(last_name, working->people[result.index].name);
                        seen_index = result.index;
                    } else if (identify_waiting) next_scan = now_ms() + 800;
                } else { candidate = -1; stable_count = 0; }
            }
        } else { ++errors; last_state = "bad_feature"; }
    } else { candidate = -1; stable_count = 0; if (!detected.empty() && !adequate && single) last_state = "face_too_small_or_unclear"; }
    xSemaphoreTake(mutex, portMAX_DELAY);
    std::snprintf(confirmed_name, sizeof(confirmed_name), "%s", equal(last_state, "known") ? last_name : "");
    confirmed_at = now_ms(); xSemaphoreGive(mutex);
    if (seen_index >= 0 && !identify_waiting.load() && buddy_ai_ready() && greetings.due(seen_index, started)
        && emit(BuddyFaceEvent::Type::Seen, last_name, "occasional_name_address")) {
        greetings.record(seen_index, started);
        ESP_LOGI(tag, "FACE_GREETING: QUEUED (identity withheld; same-person cooldown=300s)");
    }
    ESP_LOGI(tag, "FACE_SCAN: faces=%u state=%s similarity=%.3f elapsed_ms=%" PRId64 " (identity withheld)", last_count, last_state, last_similarity, now_ms() - started);
    if (identify_waiting && !(candidate >= 0 && stable_count == 1)) finish_identify();
}
bool buddy_faces_take_event(BuddyFaceEvent &event) { return events && xQueueReceive(events, &event, 0) == pdTRUE; }
bool buddy_faces_has_profile(const char *name)
{
    if (!storage_ready.load()) return false;
    xSemaphoreTake(mutex, portMAX_DELAY); const bool found = find(*database, name) >= 0; xSemaphoreGive(mutex); return found;
}
bool buddy_faces_seen_current(const BuddyFaceEvent &event)
{
    if (!storage_ready.load() || !enabled.load() || !buddy_camera_enabled() || buddy_faces_exclusive_capture()) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int64_t age = now_ms() - confirmed_at;
    const bool current = confirmed_name[0] && equal(confirmed_name, event.name)
        && age >= 0 && age <= 5000 && find(*database, event.name) >= 0;
    xSemaphoreGive(mutex); return current;
}
bool buddy_faces_is_tool(const char *name) { return equal(name, "enroll_face") || equal(name, "forget_face") || equal(name, "identify_person") || equal(name, "list_registered_people"); }
bool buddy_faces_queue_tool(const char *name, const char *id, const cJSON *args)
{
    if (!requests || !id || !*id || std::strlen(id) >= sizeof(Request::id)) return false;
    Request request{}; request.epoch = epoch.load(); std::strcpy(request.id, id);
    request.action = equal(name, "enroll_face") ? Action::Enroll : equal(name, "forget_face") ? Action::Forget : equal(name, "list_registered_people") ? Action::List : Action::Identify;
    if (request.action == Action::Enroll || request.action == Action::Forget) {
        const char *person = string(args, "name");
        if (!valid_name(person) && !(request.action == Action::Forget && equal(person, "all"))) return false;
        std::strcpy(request.name, person);
    }
    return xQueueSend(requests, &request, 0) == pdTRUE;
}
void buddy_faces_add_tools(cJSON *declarations)
{
    declaration(declarations, "enroll_face", "Only on an explicit user request, begin registering their face under the supplied name. started=true only starts a 30-second window, it is NOT a successful save. Ask them to face the camera alone at 30-60cm, in good light, and move their head slightly. Three consistent samples are required. Wait for face_enrollment_complete device event before claiming a saved face. Reusing a name replaces its samples only after a successful save. Capacity five people; no automatic enrollment.", true);
    declaration(declarations, "forget_face", "On an explicit request to forget a face, remove that named profile, or all for all registered faces. This removes face templates/name associations, not personal text notes or network settings. Wait for applied/deleted result before acknowledging.", true);
    declaration(declarations, "identify_person", "Capture a fresh camera frame and perform LOCAL matching against explicitly enrolled faces. Use for 'Ben kimim?', 'Beni taniyor musun?' or who is in front of you. Only status=known authorizes using the returned name. unknown, multiple_faces, no_face and unclear results must not be assigned an identity from a raw image, old face events or personal memory. No image needs to be sent to Gemini for this tool.", false);
    declaration(declarations, "list_registered_people", "Read the current names of explicitly registered face profiles. Use for 'Kimi taniyorsun?' and to find an exact name before forgetting a profile. This list does not say who is currently in front of the camera; use identify_person for that.", false);
}
void buddy_faces_cancel_session() { epoch.fetch_add(1); }
void buddy_faces_status()
{
    ESP_LOGI(tag, "FACE_STATUS: storage=%d enabled=%d models=%d profiles=%u capacity=%u enrolling=%d samples=%u scans=%u writes=%u errors=%u (names/features withheld)",
             storage_ready.load(), enabled.load(), model_ready.load(), saved_count.load(), capacity, enrolling.load(), enrollment_samples.load(), scans.load(), writes.load(), errors.load());
}
bool buddy_faces_command(const char *line)
{
    if (equal(line, "face-status")) { buddy_faces_status(); return true; }
    if (equal(line, "face-off") || equal(line, "face-on")) {
        enabled.store(equal(line, "face-on"));
        nvs_handle_t handle;
        if (storage_ready.load() && nvs_open_from_partition(partition, space, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, "enabled", enabled.load()); nvs_commit(handle); nvs_close(handle);
        }
        buddy_faces_status(); return true;
    }
    Request request{}; request.epoch = epoch.load();
    if (!std::strncmp(line, "face-enroll ", 12) || !std::strncmp(line, "face-forget ", 12)) {
        request.action = !std::strncmp(line, "face-enroll ", 12) ? Action::Enroll : Action::Forget;
        const char *name = line + 12;
        if (!valid_name(name) && !(request.action == Action::Forget && equal(name, "all"))) { ESP_LOGI(tag, "FACE_REQUEST: INVALID_NAME"); return true; }
        std::strcpy(request.name, name);
    } else if (equal(line, "face-scan")) request.action = Action::Identify;
    else if (equal(line, "face-selftest")) request.action = Action::Test;
    else return false;
    ESP_LOGI(tag, "FACE_REQUEST: %s", requests && xQueueSend(requests, &request, 0) == pdTRUE ? "QUEUED" : "BUSY"); return true;
}
