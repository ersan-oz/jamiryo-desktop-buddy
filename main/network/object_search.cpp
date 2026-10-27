#include "object_search.h"
#include "servo/object_view.h"
#include "servo/head_servo.h"
#include "sensors/faces.h"
#include "firmware_update.h"
#include "buddy.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace {
enum class Phase { Idle, Queued, Moving, Capture, Select, ChoiceQueued, Aiming };
std::atomic<Phase> phase{Phase::Idle};
std::atomic<bool> tests_passed{false};
std::atomic<unsigned> searches{0}, panoramas{0}, aimed{0}, misses{0}, failures{0};
struct Request { char id[128]{}; unsigned token = 0, view = 0; int x = 500; bool find = false, found = false; };
QueueHandle_t requests = nullptr;
BuddyObjectControl send_control = nullptr;
BuddyObjectImage send_image = nullptr;
Request pending{};
unsigned serial = 0, token = 0, view = 0;
int64_t deadline = 0, image_at = 0;
bool reverse = false;
const cJSON *field(const cJSON *o, const char *n) { return cJSON_GetObjectItemCaseSensitive(o, n); }
bool same(const char *a, const char *b) { return a && !std::strcmp(a, b); }
int64_t now_ms() { return esp_timer_get_time() / 1000; }
bool integer(const cJSON *v, unsigned low, unsigned high)
{
    return cJSON_IsNumber(v) && std::isfinite(v->valuedouble) && v->valuedouble >= low
        && v->valuedouble <= high && v->valuedouble == std::floor(v->valuedouble);
}
bool parse(const char *name, const cJSON *args, Request &out)
{
    if (!cJSON_IsObject(args)) return false;
    if (same(name, "find_object")) {
        const auto *target = field(args, "target");
        if (cJSON_GetArraySize(args) != 1 || !cJSON_IsString(target) || !target->valuestring[0]
            || std::strlen(target->valuestring) > 95) return false;
        bool meaningful = false;
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(target->valuestring); *p; ++p) {
            if (*p < 32 || *p == 127) return false;
            meaningful |= *p > 32;
        }
        out.find = true; return meaningful; // Target remains only in the model's turn, never stored/logged here.
    }
    if (!same(name, "face_object") || !integer(field(args, "search_id"), 1, 0x7fffffff)) return false;
    const auto *found = field(args, "found");
    if (!cJSON_IsBool(found)) return false;
    out.token = static_cast<unsigned>(field(args, "search_id")->valuedouble);
    out.found = cJSON_IsTrue(found);
    if (!out.found) return cJSON_GetArraySize(args) == 2;
    if (cJSON_GetArraySize(args) != 4 || !integer(field(args, "view"), 1, 3)
        || !integer(field(args, "x_milli"), 0, 1000)) return false;
    out.view = field(args, "view")->valueint; out.x = field(args, "x_milli")->valueint; return true;
}
bool argument_tests()
{
    struct Case { const char *tool, *args; bool valid; };
    const Case cases[] = {
        {"find_object", "{\"target\":\"bardak\"}", true},
        {"find_object", "{\"target\":\"\"}", false}, {"find_object", "{\"target\":\"  \"}", false},
        {"find_object", "{\"target\":\"x\",\"target\":\"y\"}", false},
        {"find_object", "{\"target\":3}", false}, {"find_object", "[]", false},
        {"face_object", "{\"search_id\":1,\"found\":false}", true},
        {"face_object", "{\"search_id\":1,\"found\":true,\"view\":3,\"x_milli\":0}", true},
        {"face_object", "{\"search_id\":1,\"found\":true,\"view\":1,\"x_milli\":1000}", true},
        {"face_object", "{\"search_id\":0,\"found\":false}", false},
        {"face_object", "{\"search_id\":1.5,\"found\":false}", false},
        {"face_object", "{\"search_id\":1,\"found\":\"true\"}", false},
        {"face_object", "{\"search_id\":1,\"found\":true,\"view\":4,\"x_milli\":500}", false},
        {"face_object", "{\"search_id\":1,\"found\":true,\"view\":2,\"x_milli\":1001}", false},
        {"face_object", "{\"search_id\":1,\"found\":true,\"view\":2,\"x_milli\":3.2}", false},
        {"face_object", "{\"search_id\":1,\"found\":false,\"view\":1}", false},
        {"face_object", "{\"search_id\":1,\"found\":true}", false}
    };
    Request request;
    if (parse("find_object", nullptr, request)) return false;
    for (const auto &test : cases) {
        auto *args = cJSON_Parse(test.args); const bool valid = parse(test.tool, args, request); cJSON_Delete(args);
        if (valid != test.valid) return false;
    }
    return object_view::self_test();
}
bool panel_tests()
{
    constexpr size_t guard = 16;
    auto *out = static_cast<uint8_t *>(heap_caps_malloc(object_view::panorama_bytes + guard * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *in = static_cast<uint8_t *>(heap_caps_malloc(object_view::panel_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    bool ok = out && in;
    if (ok) {
        for (size_t i = 0; i < object_view::panel_bytes; ++i) in[i] = (i * 37 + i / 640) % 251;
        for (unsigned panel = 1; panel <= 3 && ok; ++panel) {
            std::memset(out, 0xa5, object_view::panorama_bytes + guard * 2);
            ok &= object_view::copy_panel(out + guard, object_view::panorama_bytes, in, object_view::panel_bytes, panel);
            for (size_t i = 0; i < object_view::panorama_bytes && ok; ++i) {
                const size_t local = i % object_view::panel_bytes;
                const unsigned x = (local / 2) % 320, y = (local / 2) / 320;
                const bool selected = i / object_view::panel_bytes == panel - 1;
                const bool badge = x >= 2 && x < 19 && y >= 2 && y < 25;
                if (!selected || !badge) ok &= out[guard + i] == (selected ? in[local] : 0xa5);
            }
            for (size_t i = 0; i < guard; ++i)
                ok &= out[i] == 0xa5 && out[guard + object_view::panorama_bytes + i] == 0xa5;
        }
        ok &= !object_view::copy_panel(out + guard, object_view::panorama_bytes - 1, in, object_view::panel_bytes, 1)
            && !object_view::copy_panel(out + guard, object_view::panorama_bytes, in, object_view::panel_bytes - 1, 1)
            && !object_view::copy_panel(out + guard, object_view::panorama_bytes, in, object_view::panel_bytes, 4);
    }
    if (out) { std::memset(out, 0, object_view::panorama_bytes + guard * 2); heap_caps_free(out); }
    if (in) { std::memset(in, 0, object_view::panel_bytes); heap_caps_free(in); }
    return ok;
}
void release(BuddyVisionFrame &frame)
{
    if (frame.jpeg) { std::memset(frame.jpeg, 0, frame.bytes); heap_caps_free(frame.jpeg); } frame = {};
}
void reset(bool keep_head = false)
{
    buddy_camera_search_cancel(token);
    if (!keep_head) buddy_head_object_cancel(token);
    pending = {}; token = view = 0; deadline = image_at = 0;
    phase.store(Phase::Idle);
}
cJSON *response(const Request &request, cJSON *&result)
{
    auto *root = cJSON_CreateObject();
    auto *array = cJSON_AddArrayToObject(cJSON_AddObjectToObject(root, "toolResponse"), "functionResponses");
    auto *item = cJSON_CreateObject(); cJSON_AddItemToArray(array, item);
    cJSON_AddStringToObject(item, "id", request.id);
    cJSON_AddStringToObject(item, "name", request.find ? "find_object" : "face_object");
    result = cJSON_AddObjectToObject(item, "response"); return root;
}
void error(const char *reason)
{
    failures.fetch_add(1);
    if (pending.id[0]) {
        cJSON *result; auto *root = response(pending, result);
        cJSON_AddBoolToObject(result, pending.find ? "image_sent" : "aimed", false);
        cJSON_AddStringToObject(result, "error", reason); send_control(root);
    }
    ESP_LOGI("buddy_object", "OBJECT_SEARCH: failed reason=%s (no image/target logged)", reason);
    reset();
}
} // namespace

bool buddy_object_start(BuddyObjectControl control, BuddyObjectImage image)
{
    tests_passed.store(argument_tests() && panel_tests());
    ESP_LOGI("buddy_object", "OBJECT_ENGINE_TEST: %s (arguments/panel pixels/guards/reverse/bounds/stale evidence)", tests_passed.load() ? "PASS" : "FAIL");
    if (!tests_passed.load() || !control || !image) return false;
    send_control = control; send_image = image;
    requests = xQueueCreate(1, sizeof(Request)); return requests != nullptr;
}
bool buddy_object_is_tool(const char *name) { return same(name, "find_object") || same(name, "face_object"); }
bool buddy_object_busy() { return phase.load() != Phase::Idle; }
bool buddy_object_queue_tool(const char *name, const char *id, const cJSON *args)
{
    Request request;
    if (!requests || !id || !*id || std::strlen(id) >= sizeof(request.id) || !parse(name, args, request)
        || !buddy_camera_enabled() || !buddy_head_snapshot().available || buddy_faces_exclusive_capture() || buddy_ota_busy()) return false;
    Phase expected = request.find ? Phase::Idle : Phase::Select;
    if (!phase.compare_exchange_strong(expected, request.find ? Phase::Queued : Phase::ChoiceQueued)) return false;
    std::strcpy(request.id, id);
    if (xQueueSend(requests, &request, 0) == pdTRUE) return true;
    phase.store(expected); return false;
}
void buddy_object_tick()
{
    const int64_t now = now_ms();
    Request request;
    if (xQueueReceive(requests, &request, 0) == pdTRUE) {
        if (request.find) {
            pending = request; token = serial = serial % 0x7fffffff + 1;
            searches.fetch_add(1); view = 1; reverse = buddy_head_follow_settings().reverse;
            deadline = now + 18000; phase.store(Phase::Moving);
            if (!buddy_head_object_begin(token) || !buddy_head_object_move(token, object_view::target(view, 500, reverse)))
                error("servo_unavailable_or_busy");
        } else {
            pending = request;
            if (!object_view::evidence_current(token, request.token, image_at, now, deadline))
                error("stale_or_wrong_search");
            else if (!request.found) {
                cJSON *result; auto *root = response(request, result);
                cJSON_AddBoolToObject(result, "found", false); cJSON_AddBoolToObject(result, "aimed", false);
                send_control(root); misses.fetch_add(1); reset();
            } else if (!buddy_head_object_move(token, object_view::target(request.view, request.x, reverse)))
                error("servo_override_or_unavailable");
            else { phase.store(Phase::Aiming); deadline = now + 5000; }
        }
    }
    if (token && buddy_object_busy() && (!buddy_camera_enabled() || !buddy_head_object_current(token)
        || buddy_faces_exclusive_capture() || buddy_ota_busy())) error("search_interrupted_or_camera_disabled");
    if (deadline && buddy_object_busy() && now >= deadline) error("search_timeout");
    if (phase.load() == Phase::Moving && buddy_head_object_settled(token)) {
        if (buddy_camera_search_request(token, view)) phase.store(Phase::Capture);
        else error("camera_unavailable_or_busy");
    }
    BuddySearchCapture capture;
    while (buddy_camera_search_result(capture)) {
        if (phase.load() == Phase::Capture && capture.search_id == token && capture.view == view) {
            if (!capture.ok) error("capture_failed");
            else if (view < object_view::count) {
                ++view;
                if (buddy_head_object_move(token, object_view::target(view, 500, reverse))) phase.store(Phase::Moving);
                else error("servo_override_or_unavailable");
            } else if (!send_image(capture.frame)) error("image_delivery_failed");
            else {
                cJSON *result; auto *root = response(pending, result);
                cJSON_AddBoolToObject(result, "image_sent", true); cJSON_AddNumberToObject(result, "search_id", token);
                cJSON_AddStringToObject(result, "layout", "Three separate views stacked VERTICALLY: badge 1 TOP=left; 2 MIDDLE=center; 3 BOTTOM=right. Each panel is 320x240. Not a continuous panorama.");
                cJSON_AddStringToObject(result, "next_action", "Inspect only this image for the requested object. Call face_object with this search_id, found, and if found the best view 1..3 and the object horizontal center x_milli 0..1000 WITHIN THAT PANEL. If absent/unclear use found=false. Do not infer a view from text or older images; do not use turn_head.");
                cJSON_AddBoolToObject(result, "physical_motion_verified", false);
                buddy_camera_search_cancel(token);
                buddy_head_object_move(token, HeadMotion::center_us);
                image_at = now_ms(); deadline = image_at + 20000; pending = {};
                phase.store(Phase::Select); panoramas.fetch_add(1);
                if (!send_control(root)) error("tool_response_failed");
            }
        }
        release(capture.frame);
    }
    if (phase.load() == Phase::Aiming && buddy_head_object_settled(token)) {
        if (!buddy_head_object_hold(token)) { error("servo_override_or_unavailable"); return; }
        cJSON *result; auto *root = response(pending, result);
        cJSON_AddBoolToObject(result, "aimed", true); cJSON_AddBoolToObject(result, "found", true);
        cJSON_AddNumberToObject(result, "hold_ms", 20000);
        cJSON_AddBoolToObject(result, "physical_motion_verified", false);
        if (send_control(root)) aimed.fetch_add(1); else failures.fetch_add(1);
        reset(true);
    }
}
void buddy_object_cancel_session()
{
    reset(); Request request; while (requests && xQueueReceive(requests, &request, 0) == pdTRUE) {}
    BuddySearchCapture result;
    while (buddy_camera_search_result(result)) release(result.frame);
}
BuddyObjectSnapshot buddy_object_snapshot()
{
    const auto current = phase.load();
    const char *names[] = {"idle", "queued", "moving", "capture", "select", "choice_queued", "aiming"};
    return {current != Phase::Idle, tests_passed.load(), names[static_cast<unsigned>(current)],
        searches.load(), panoramas.load(), aimed.load(), misses.load(), failures.load()};
}
void buddy_object_add_tools(cJSON *declarations)
{
    auto add = [declarations](const char *name, const char *description) {
        auto *f = cJSON_CreateObject(); cJSON_AddItemToArray(declarations, f);
        cJSON_AddStringToObject(f, "name", name); cJSON_AddStringToObject(f, "behavior", "BLOCKING");
        cJSON_AddStringToObject(f, "description", description);
        auto *p = cJSON_AddObjectToObject(f, "parameters"); cJSON_AddStringToObject(p, "type", "OBJECT"); return p;
    };
    auto *p = add("find_object", "Physically search for a particular nearby OBJECT the user asks about: 'bardak nerede', 'etraftaki kumandayi bul', 'sagindaki nesne ne', 'su siseye baksana' or equivalent natural wording. Sweeps to left/center/right with a fresh stationary image at EACH stop. Returns ONE numbered image with three separate vertically stacked views and a search_id. Then MUST call face_object to face the visible object or report found=false. One search only, never idle or a home-control request. No images are stored. For a generic 'ne goruyorsun' without a target use look_around.");
    auto *properties = cJSON_AddObjectToObject(p, "properties"); auto *target = cJSON_AddObjectToObject(properties, "target");
    cJSON_AddStringToObject(target, "type", "STRING"); cJSON_AddStringToObject(target, "description", "Short object label from the user's actual request, at most 95 UTF-8 bytes; no extra instructions.");
    cJSON_AddItemToArray(cJSON_AddArrayToObject(p, "required"), cJSON_CreateString("target"));
    p = add("face_object", "Complete find_object using ONLY its freshly delivered numbered image. Must use the returned search_id. If the object is clearly visible, choose its best view (1 top/left, 2 middle/center, 3 bottom/right) and horizontal center x_milli within that panel (0 left edge, 500 center, 1000 right edge). If absent, ambiguous or unclear set found=false and omit view/x_milli. Wait for aimed=true before saying you turned toward it. No shaft feedback or guaranteed exact alignment. Never guess an unseen object.");
    properties = cJSON_AddObjectToObject(p, "properties");
    for (const char *name : {"search_id", "view", "x_milli"}) {
        auto *v = cJSON_AddObjectToObject(properties, name); cJSON_AddStringToObject(v, "type", "INTEGER");
    }
    cJSON_AddStringToObject(cJSON_AddObjectToObject(properties, "found"), "type", "BOOLEAN");
    auto *required = cJSON_AddArrayToObject(p, "required");
    for (const char *name : {"search_id", "found"}) cJSON_AddItemToArray(required, cJSON_CreateString(name));
}
