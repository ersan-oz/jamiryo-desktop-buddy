#include "remote_update.h"
#include "firmware_update.h"
#include "servo/head_servo.h"
#include "buddy.h"
#include "display/eyes.h"
#include "sensors/camera.h"
#include "gemini_live.h"
#include "home_assistant.h"
#include "home_messenger.h"
#include "conversation_policy.h"
#include "object_search.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <initializer_list>

namespace {
constexpr char tag[] = "buddy_remote";
constexpr unsigned http_port = 3232, discovery_port = 3233;
constexpr char probe[] = "JAMIRYO_DISCOVER_V1";
httpd_handle_t server = nullptr;
SemaphoreHandle_t mutex = nullptr;
uint8_t pairing_key[32]{};
bool paired = false;
char device_id[32]{}, nonce[65]{};
int64_t nonce_at = 0, retry_at = 0;
std::atomic<bool> connected{false};
int64_t now_ms() { return esp_timer_get_time() / 1000; }
void hex(const uint8_t *data, size_t size, char *out)
{
    for (size_t i = 0; i < size; ++i) std::snprintf(out + 2 * i, 3, "%02x", data[i]);
}
bool unhex(const char *value, uint8_t *out)
{
    if (!value || std::strlen(value) != 64) return false;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned byte = 0;
        for (unsigned j = 0; j < 2; ++j) {
            const char c = value[2*i+j];
            if (c >= '0' && c <= '9') byte = byte * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') byte = byte * 16 + c - 'a' + 10;
            else return false;
        }
        out[i] = byte;
    }
    return true;
}
bool station(esp_netif_ip_info_t &info)
{
    auto *net = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return connected.load() && net && esp_netif_get_ip_info(net, &info) == ESP_OK && info.ip.addr;
}
bool local_peer(uint32_t peer, const esp_netif_ip_info_t &info)
{
    return peer && (peer & info.netmask.addr) == (info.ip.addr & info.netmask.addr);
}
uint32_t ipv4(const sockaddr_storage &address)
{
    if (address.ss_family == AF_INET) return reinterpret_cast<const sockaddr_in *>(&address)->sin_addr.s_addr;
    if (address.ss_family == AF_INET6) {
        const auto &ip = reinterpret_cast<const sockaddr_in6 *>(&address)->sin6_addr;
        uint32_t result = 0;
        if (IN6_IS_ADDR_V4MAPPED(&ip)) std::memcpy(&result, ip.s6_addr + 12, 4);
        return result;
    }
    return 0;
}
bool lan_request(httpd_req_t *request)
{
    esp_netif_ip_info_t info{}; sockaddr_storage local{}, peer{};
    socklen_t size = sizeof(local); const int socket = httpd_req_to_sockfd(request);
    if (!station(info) || getsockname(socket, reinterpret_cast<sockaddr *>(&local), &size)) return false;
    size = sizeof(peer);
    return !getpeername(socket, reinterpret_cast<sockaddr *>(&peer), &size)
        && ipv4(local) == info.ip.addr && local_peer(ipv4(peer), info);
}
esp_err_t deny(httpd_req_t *request, httpd_err_code_t code, const char *message)
{
    httpd_resp_set_hdr(request, "Connection", "close");
    const auto result = httpd_resp_send_err(request, code, message);
    httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request)); return result;
}
cJSON *identity()
{
    auto *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "project", "desktop_buddy");
    cJSON_AddNumberToObject(root, "protocol", 1);
    cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddNumberToObject(root, "port", http_port);
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    xSemaphoreTake(mutex, portMAX_DELAY); cJSON_AddBoolToObject(root, "paired", paired); xSemaphoreGive(mutex);
    return root;
}
esp_err_t json(httpd_req_t *request, cJSON *root)
{
    char *wire = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto result = httpd_resp_send(request, wire ? wire : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(wire); return result;
}
esp_err_t status(httpd_req_t *request)
{
    if (!lan_request(request)) return deny(request, HTTPD_403_FORBIDDEN, "Local station network only.");
    auto *root = identity(); const auto *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state{}; uint8_t digest[32]{}; char digest_hex[65]{};
    const bool ready = running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_VALID;
    cJSON_AddBoolToObject(root, "ready", ready); cJSON_AddBoolToObject(root, "busy", buddy_ota_busy());
    cJSON_AddStringToObject(root, "slot", running ? running->label : "unknown");
    cJSON_AddNumberToObject(root, "uptime_ms", now_ms());
    cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason());
    const auto head = buddy_head_snapshot(); auto *motor = cJSON_AddObjectToObject(root, "head");
    cJSON_AddBoolToObject(motor, "available", head.available); cJSON_AddBoolToObject(motor, "moving", head.moving);
    cJSON_AddBoolToObject(motor, "manual_active", head.manual); cJSON_AddStringToObject(motor, "mode", head.mode);
    cJSON_AddNumberToObject(motor, "pulse_us", head.pulse_us); cJSON_AddNumberToObject(motor, "target_us", head.target_us);
    cJSON_AddNumberToObject(motor, "requested_degrees", head.requested_degrees);
    cJSON_AddNumberToObject(motor, "voice_requests", head.voice_requests);
    cJSON_AddBoolToObject(motor, "voice_selftest_pass", head.voice_selftest);
    cJSON_AddBoolToObject(motor, "actions_selftest_pass", head.actions_selftest);
    cJSON_AddBoolToObject(motor, "scan_active", head.scan_active);
    cJSON_AddNumberToObject(motor, "scan_requests", head.scan_requests);
    cJSON_AddNumberToObject(motor, "scans_completed", head.scans_completed);
    cJSON_AddNumberToObject(motor, "gesture_moves", head.gesture_moves);
    cJSON_AddStringToObject(motor, "routine_phase", head.routine_phase);
    const auto head_settings = buddy_head_follow_settings();
    cJSON_AddBoolToObject(motor, "automatic_enabled", head_settings.enabled);
    cJSON_AddBoolToObject(motor, "reverse", head_settings.reverse);
    const auto object = buddy_object_snapshot(); auto *search = cJSON_AddObjectToObject(root, "object_search");
    cJSON_AddBoolToObject(search, "active", object.active); cJSON_AddBoolToObject(search, "selftest_pass", object.selftest);
    cJSON_AddStringToObject(search, "state", object.state);
    cJSON_AddNumberToObject(search, "searches", object.searches); cJSON_AddNumberToObject(search, "panoramas", object.panoramas);
    cJSON_AddNumberToObject(search, "aimed", object.aimed); cJSON_AddNumberToObject(search, "misses", object.misses);
    cJSON_AddNumberToObject(search, "failures", object.failures);
    const auto person = buddy_head_person_snapshot(); auto *gaze = cJSON_AddObjectToObject(root, "conversation_gaze");
    cJSON_AddBoolToObject(gaze, "active", person.active); cJSON_AddBoolToObject(gaze, "selftest_pass", person.selftest);
    cJSON_AddStringToObject(gaze, "phase", person.phase);
    cJSON_AddNumberToObject(gaze, "starts", person.starts); cJSON_AddNumberToObject(gaze, "matched", person.matched);
    cJSON_AddNumberToObject(gaze, "not_found", person.not_found); cJSON_AddNumberToObject(gaze, "canceled", person.canceled);
    cJSON_AddNumberToObject(gaze, "frames", person.frames);
    auto *motion = cJSON_AddObjectToObject(root, "motion");
    cJSON_AddBoolToObject(motion, "selftest_pass", buddy_motion_selftest_pass());
    cJSON_AddNumberToObject(motion, "move_acceleration_g", motion_settings::move_acceleration_g);
    cJSON_AddNumberToObject(motion, "move_rotation_dps", motion_settings::move_rotation_dps);
    cJSON_AddNumberToObject(motion, "move_hold_ms", motion_settings::move_hold_ms);
    const auto sensed = buddy_motion_snapshot();
    cJSON_AddNumberToObject(motion, "dynamic_g", sensed.dynamic_g);
    cJSON_AddNumberToObject(motion, "rotation_dps", sensed.rotation_dps);
    cJSON_AddNumberToObject(motion, "peak_dynamic_g", sensed.peak_dynamic_g);
    cJSON_AddNumberToObject(motion, "peak_rotation_dps", sensed.peak_rotation_dps);
    cJSON_AddBoolToObject(motion, "output_suppressed", sensed.output_suppressed);
    cJSON_AddBoolToObject(motion, "microphone_active", sensed.microphone_active);
    cJSON_AddBoolToObject(motion, "servo_suppressed", sensed.servo_suppressed);
    cJSON_AddNumberToObject(motion, "samples", sensed.samples);
    cJSON_AddNumberToObject(motion, "moves", sensed.moves);
    cJSON_AddNumberToObject(motion, "shakes", sensed.shakes);
    cJSON_AddNumberToObject(motion, "tilts", sensed.tilts);
    cJSON_AddNumberToObject(motion, "sample_age_ms", sensed.sample_age_ms);
    const auto face = buddy_display_snapshot(); auto *display = cJSON_AddObjectToObject(root, "display");
    cJSON_AddBoolToObject(display, "available", face.available);
    cJSON_AddBoolToObject(display, "renderer_selftest_pass", face.renderer_selftest);
    cJSON_AddBoolToObject(display, "pickup_visible", face.pickup_visible);
    cJSON_AddNumberToObject(display, "frames", face.frames);
    cJSON_AddNumberToObject(display, "write_errors", face.write_errors);
    cJSON_AddNumberToObject(display, "pickups", face.pickups);
    cJSON_AddNumberToObject(display, "last_pickup_age_ms", face.last_pickup_age_ms);
    const auto camera = buddy_camera_snapshot(); auto *view = cJSON_AddObjectToObject(root, "camera");
    cJSON_AddBoolToObject(view, "available", camera.available);
    cJSON_AddBoolToObject(view, "enabled", camera.enabled);
    cJSON_AddNumberToObject(view, "rotation_degrees", camera.rotation_degrees);
    cJSON_AddBoolToObject(view, "orientation_verified", camera.orientation_verified);
    cJSON_AddBoolToObject(view, "rotation_selftest_pass", camera.rotation_selftest);
    cJSON_AddNumberToObject(view, "rotated_frames", camera.rotated_frames);
    cJSON_AddNumberToObject(view, "rotation_max_us", camera.rotation_max_us);
    cJSON_AddNumberToObject(view, "captured", camera.captured);
    cJSON_AddNumberToObject(view, "photos", camera.photos);
    const auto home = buddy_home_snapshot(); auto *homes = cJSON_AddObjectToObject(root, "home_profiles");
    cJSON_AddBoolToObject(homes, "storage_ready", home.storage_ready);
    cJSON_AddBoolToObject(homes, "selftest_pass", home.selftest);
    cJSON_AddBoolToObject(homes, "gemini_configured", home.gemini_configured);
    cJSON_AddNumberToObject(homes, "schema", home.schema); cJSON_AddNumberToObject(homes, "count", home.profiles);
    cJSON_AddNumberToObject(homes, "worker_stack_min_bytes", home.worker_stack_min_bytes);
    const auto messages = buddy_messenger_snapshot(); auto *messenger = cJSON_AddObjectToObject(root, "messenger");
    cJSON_AddBoolToObject(messenger, "available", messages.available); cJSON_AddBoolToObject(messenger, "selftest_pass", messages.selftest);
    cJSON_AddStringToObject(messenger, "phase", messages.phase); cJSON_AddNumberToObject(messenger, "accepted", messages.accepted);
    cJSON_AddNumberToObject(messenger, "completed", messages.completed); cJSON_AddNumberToObject(messenger, "failures", messages.failures);
    cJSON_AddNumberToObject(messenger, "phase_age_ms", messages.phase_age_ms);
    cJSON_AddNumberToObject(messenger, "failure_age_ms", messages.failure_age_ms);
    cJSON_AddStringToObject(messenger, "last_failure", messages.last_failure);
    cJSON_AddNumberToObject(messenger, "empty_boundaries", messages.empty_boundaries);
    const auto ai = buddy_ai_snapshot(); auto *chat = cJSON_AddObjectToObject(root, "conversation");
    cJSON_AddBoolToObject(chat, "ready", ai.ready);
    cJSON_AddBoolToObject(chat, "guard_active", ai.conversation_guard);
    cJSON_AddBoolToObject(chat, "resume_available", ai.resume_available);
    cJSON_AddBoolToObject(chat, "selftest_pass", ai.conversation_selftest);
    cJSON_AddNumberToObject(chat, "quiet_ms", conversation_policy::autonomous_quiet_ms);
    cJSON_AddNumberToObject(chat, "sessions", ai.sessions);
    cJSON_AddNumberToObject(chat, "recoveries", ai.recoveries);
    cJSON_AddNumberToObject(chat, "last_input_age_ms", ai.last_input_age_ms);
    cJSON_AddNumberToObject(chat, "last_reply_age_ms", ai.last_reply_age_ms);
    cJSON_AddStringToObject(chat, "state", ai.state);
    cJSON_AddStringToObject(chat, "socket_stage", ai.socket_stage);
    cJSON_AddNumberToObject(chat, "socket_error", ai.socket_error);
    cJSON_AddNumberToObject(chat, "failures", ai.failures);
    cJSON_AddNumberToObject(chat, "retry_ms", ai.retry_ms);
    cJSON_AddNumberToObject(chat, "internal_free", ai.internal_free);
    cJSON_AddNumberToObject(chat, "internal_largest", ai.internal_largest);
    cJSON_AddNumberToObject(chat, "failure_free", ai.failure_free);
    cJSON_AddNumberToObject(chat, "failure_largest", ai.failure_largest);
    cJSON_AddNumberToObject(chat, "confirmed_inputs", ai.confirmed_inputs);
    cJSON_AddNumberToObject(chat, "ignored_vad_turns", ai.ignored_vad_turns);
    cJSON_AddNumberToObject(chat, "context_trigger_tokens", conversation_policy::context_trigger_tokens);
    cJSON_AddNumberToObject(chat, "context_target_tokens", conversation_policy::context_target_tokens);
    if (running && esp_partition_get_sha256(running, digest) == ESP_OK) {
        hex(digest, sizeof(digest), digest_hex); cJSON_AddStringToObject(root, "image_sha256", digest_hex);
    }
    return json(request, root);
}
esp_err_t challenge(httpd_req_t *request)
{
    if (!lan_request(request)) return deny(request, HTTPD_403_FORBIDDEN, "Local station network only.");
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!paired) { xSemaphoreGive(mutex); return deny(request, HTTPD_403_FORBIDDEN, "Pair once over USB."); }
    uint8_t random[32]; esp_fill_random(random, sizeof(random)); hex(random, sizeof(random), nonce); nonce_at = now_ms();
    auto *root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "device_id", device_id);
    cJSON_AddStringToObject(root, "nonce", nonce); cJSON_AddNumberToObject(root, "expires_seconds", 60);
    xSemaphoreGive(mutex); return json(request, root);
}
bool authenticate(httpd_req_t *request, uint8_t body_digest[32])
{
    char supplied_nonce[65]{}, supplied_digest[65]{}, supplied_auth[65]{};
    uint8_t auth[32]{}, key[32]{};
    if (httpd_req_get_hdr_value_len(request, "Origin")) return false;
    if (httpd_req_get_hdr_value_str(request, "X-Jamiryo-Nonce", supplied_nonce, sizeof(supplied_nonce)) != ESP_OK
        || httpd_req_get_hdr_value_str(request, "X-Jamiryo-SHA256", supplied_digest, sizeof(supplied_digest)) != ESP_OK
        || httpd_req_get_hdr_value_str(request, "X-Jamiryo-Auth", supplied_auth, sizeof(supplied_auth)) != ESP_OK
        || !unhex(supplied_digest, body_digest) || !unhex(supplied_auth, auth)) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool valid = paired && nonce[0] && !std::strcmp(supplied_nonce, nonce)
        && now_ms() >= nonce_at && now_ms() - nonce_at <= 60000;
    if (valid) std::memcpy(key, pairing_key, sizeof(key));
    xSemaphoreGive(mutex);
    if (!valid) return false;
    char wire[256]; const int length = std::snprintf(wire, sizeof(wire), "jamiryo-ota-v1\n%s\n%s\n%s", device_id, supplied_nonce, supplied_digest);
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_HMAC); psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attrs, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_key_id_t id{}; uint8_t expected[32]{}; size_t count = 0;
    bool ok = psa_import_key(&attrs, key, sizeof(key), &id) == PSA_SUCCESS;
    if (ok) ok = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), reinterpret_cast<const uint8_t *>(wire), length, expected, sizeof(expected), &count) == PSA_SUCCESS && count == 32;
    if (id) psa_destroy_key(id);
    std::memset(key, 0, sizeof(key));
    unsigned difference = 0; for (unsigned i = 0; i < 32; ++i) difference |= expected[i] ^ auth[i];
    if (!ok || difference) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    ok = nonce[0] && !std::strcmp(nonce, supplied_nonce); if (ok) nonce[0] = 0;
    xSemaphoreGive(mutex); return ok;
}
esp_err_t upload(httpd_req_t *request)
{
    uint8_t expected[32];
    if (!lan_request(request) || !authenticate(request, expected)) return deny(request, HTTPD_403_FORBIDDEN, "Invalid update authentication.");
    return buddy_ota_upload(request, expected);
}
void discovery_task(void *)
{
    for (;;) {
        if (!connected.load()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        const int socket = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(discovery_port); address.sin_addr.s_addr = INADDR_ANY;
        timeval timeout{1,0};
        if (socket < 0 || bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
            if (socket >= 0) close(socket);
            vTaskDelay(pdMS_TO_TICKS(3000)); continue;
        }
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        for (;;) {
            char buffer[64]; sockaddr_in peer{}; socklen_t size = sizeof(peer);
            const int count = recvfrom(socket, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr *>(&peer), &size);
            if (count < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            esp_netif_ip_info_t info{};
            if (count != sizeof(probe)-1 || std::memcmp(buffer, probe, count) || !station(info) || !local_peer(peer.sin_addr.s_addr, info)) continue;
            auto *root = identity(); char *wire = cJSON_PrintUnformatted(root); cJSON_Delete(root);
            if (wire) { sendto(socket, wire, std::strlen(wire), 0, reinterpret_cast<sockaddr *>(&peer), size); cJSON_free(wire); }
        }
    }
}
} // namespace

bool buddy_remote_start()
{
    mutex = xSemaphoreCreateMutex(); if (!mutex || psa_crypto_init() != PSA_SUCCESS) return false;
    uint8_t mac[6]; if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
    std::snprintf(device_id, sizeof(device_id), "jamiryo-%02x%02x%02x%02x%02x%02x", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    nvs_handle_t handle;
    if (nvs_open("buddy_update", NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(pairing_key); paired = nvs_get_blob(handle, "key", pairing_key, &size) == ESP_OK && size == sizeof(pairing_key);
        nvs_close(handle);
    }
    return xTaskCreate(discovery_task, "buddy_discover", 3072, nullptr, 1, nullptr) == pdPASS;
}
void buddy_remote_tick(bool wifi_connected)
{
    connected.store(wifi_connected);
    if (!mutex || server || !wifi_connected || now_ms() < 20000 || now_ms() < retry_at) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = http_port; config.ctrl_port = 32320; config.stack_size = 10240;
    config.task_priority = 2; config.max_uri_handlers = 3; config.max_open_sockets = 2;
    config.lru_purge_enable = true; config.recv_wait_timeout = 10;
    if (httpd_start(&server, &config) != ESP_OK) { server = nullptr; retry_at = now_ms() + 10000; return; }
    for (const auto &route : {httpd_uri_t{.uri="/status",.method=HTTP_GET,.handler=status,.user_ctx=nullptr},
                            httpd_uri_t{.uri="/challenge",.method=HTTP_GET,.handler=challenge,.user_ctx=nullptr},
                            httpd_uri_t{.uri="/update",.method=HTTP_POST,.handler=upload,.user_ctx=nullptr}}) {
        if (httpd_register_uri_handler(server, &route) != ESP_OK) { httpd_stop(server); server = nullptr; retry_at = now_ms()+10000; return; }
    }
    ESP_LOGI(tag, "REMOTE_OTA: READY port=%u discovery=%u (key withheld)", http_port, discovery_port);
}
bool buddy_remote_command(const char *line)
{
    if (!mutex) return false;
    if (!std::strcmp(line, "ota-info")) {
        auto *root = identity(); esp_netif_ip_info_t info{}; char host[16]{};
        if (station(info)) std::snprintf(host, sizeof(host), IPSTR, IP2STR(&info.ip));
        cJSON_AddStringToObject(root, "host", host); char *wire = cJSON_PrintUnformatted(root); cJSON_Delete(root);
        ESP_LOGI(tag, "REMOTE_OTA_INFO: %s", wire ? wire : "{}"); cJSON_free(wire); return true;
    }
    if (std::strncmp(line, "ota-pair ", 9)) return false;
    uint8_t key[32]{}; bool ok = unhex(line + 9, key) && !buddy_ota_busy();
    nvs_handle_t handle;
    if (ok) {
        ok = nvs_open("buddy_update", NVS_READWRITE, &handle) == ESP_OK;
        if (ok) { ok = nvs_set_blob(handle, "key", key, sizeof(key)) == ESP_OK && nvs_commit(handle) == ESP_OK; nvs_close(handle); }
    }
    if (ok) { xSemaphoreTake(mutex, portMAX_DELAY); std::memcpy(pairing_key,key,sizeof(key)); paired=true; nonce[0]=0; xSemaphoreGive(mutex); }
    std::memset(key,0,sizeof(key)); ESP_LOGI(tag,"REMOTE_OTA_PAIR: %s (key withheld)",ok ? "SAVED" : "REJECTED"); return true;
}
