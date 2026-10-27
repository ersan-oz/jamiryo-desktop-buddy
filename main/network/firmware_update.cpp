#include "firmware_update.h"
#include "firmware_contract.h"
#include "setup_network.h"
#include "buddy.h"
#include "audio/microphone.h"
#include "gemini_live.h"
#include "servo/head_servo.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include <algorithm>
#include <atomic>
#include <cstring>

extern "C" {
extern const JamiryoFirmwareContract jamiryo_ota_contract
    __attribute__((section(".rodata_custom_desc"), used, aligned(4))) = {
    "JAMIRYO-OTA-V1", 1, 191296, 1295280,
    {0x31,0x95,0x82,0x6b,0xcc,0xb7,0xaf,0x3a,0x82,0x62,0xb3,0xde,0x18,0x8d,0x21,0x20,
     0xa4,0xc8,0x58,0xbe,0xa9,0x6b,0x9a,0x67,0x08,0xb0,0xf5,0x5b,0xaa,0x97,0xbf,0xc5},
    {0x27,0x04,0xda,0x81,0xba,0x81,0x8b,0x0b,0x41,0xb7,0xc8,0x79,0xe9,0xe5,0xa5,0x25,
     0xa9,0x05,0x5d,0xa4,0x4c,0x62,0x93,0xcb,0x2a,0x9d,0x34,0xb9,0x5d,0xdc,0x03,0xf5}
};
}
static_assert(sizeof(esp_image_header_t) == 24 && sizeof(esp_image_segment_header_t) == 8);
static_assert(sizeof(esp_app_desc_t) == 256 && offsetof(esp_app_desc_t, project_name) == 48);
static_assert(offsetof(esp_image_header_t, chip_id) == 12);

namespace {
constexpr char tag[] = "buddy_ota";
std::atomic<bool> updating{false};
std::atomic<int64_t> restart_at{0};
int64_t now_ms() { return esp_timer_get_time() / 1000; }
esp_err_t message(httpd_req_t *request, const char *status, const char *value)
{
    auto *root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "message", value);
    char *wire = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto result = httpd_resp_send(request, wire ? wire : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(wire); return result;
}
bool model_hash(const char *label, uint32_t length, const uint8_t expected[32])
{
    const auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!partition || partition->size < length) return false;
    psa_hash_operation_t ctx = PSA_HASH_OPERATION_INIT;
    bool ok = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&ctx, PSA_ALG_SHA_256) == PSA_SUCCESS;
    uint8_t block[1024], hash[32];
    for (uint32_t offset = 0; ok && offset < length; offset += sizeof(block)) {
        const size_t count = std::min<size_t>(sizeof(block), length - offset);
        ok = esp_partition_read(partition, offset, block, count) == ESP_OK
            && psa_hash_update(&ctx, block, count) == PSA_SUCCESS;
    }
    size_t hash_size = 0;
    ok = ok && psa_hash_finish(&ctx, hash, sizeof(hash), &hash_size) == PSA_SUCCESS
        && hash_size == sizeof(hash) && !std::memcmp(hash, expected, sizeof(hash));
    psa_hash_abort(&ctx); return ok;
}
bool receive(httpd_req_t *request, uint8_t *bytes, size_t length)
{
    const int64_t deadline = now_ms() + 300000;
    unsigned timeouts = 0;
    for (size_t used = 0; used < length;) {
        if (now_ms() > deadline) return false;
        const int count = httpd_req_recv(request, reinterpret_cast<char *>(bytes + used), length - used);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (count <= 0) return false;
        used += count; timeouts = 0;
    }
    return true;
}
} // namespace

bool buddy_ota_busy() { return updating.load(); }
bool buddy_ota_models_valid()
{
    uint8_t prefix[firmware_prefix_size]{};
    prefix[0] = 0xe9; prefix[1] = 3; prefix[12] = 9;
    const uint32_t app_magic = ESP_APP_DESC_MAGIC_WORD;
    std::memcpy(prefix + 32, &app_magic, sizeof(app_magic));
    std::memcpy(prefix + 80, "desktop_buddy", sizeof("desktop_buddy"));
    std::memcpy(prefix + firmware_contract_offset, &jamiryo_ota_contract, sizeof(jamiryo_ota_contract));
    bool guard_ok = jamiryo_firmware_compatible(prefix, sizeof(prefix), jamiryo_ota_contract)
        && !jamiryo_firmware_compatible(prefix, sizeof(prefix) - 1, jamiryo_ota_contract);
    for (const unsigned offset : {0U, 12U, 32U, 80U, 288U, 304U, 316U, 379U}) {
        prefix[offset] ^= 0x40;
        guard_ok &= !jamiryo_firmware_compatible(prefix, sizeof(prefix), jamiryo_ota_contract);
        prefix[offset] ^= 0x40;
    }
    ESP_LOGI(tag, "OTA_GUARD_TEST: %s (short/target/project/layout/models)", guard_ok ? "PASS" : "FAIL");
    const bool ok = guard_ok && model_hash("human_face_det", jamiryo_ota_contract.detect_bytes, jamiryo_ota_contract.detect_sha256)
        && model_hash("human_face_feat", jamiryo_ota_contract.feature_bytes, jamiryo_ota_contract.feature_sha256);
    ESP_LOGI(tag, "OTA_MODELS: %s", ok ? "PASS" : "FAIL"); return ok;
}
void buddy_ota_confirm_boot(bool core_ok)
{
    esp_ota_img_states_t state;
    const auto *running = esp_ota_get_running_partition();
    const auto state_result = esp_ota_get_state_partition(running, &state);
    const bool initial = state_result == ESP_ERR_NOT_FOUND || (state_result == ESP_OK && state == ESP_OTA_IMG_UNDEFINED);
    if (!initial && (state_result != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY)) return;
    // Local hardware health confirms the update; an unavailable router, Gemini
    // or Home Assistant must never invalidate otherwise working firmware.
    for (unsigned i = 0; core_ok && i < 50; ++i) {
        vTaskDelay(pdMS_TO_TICKS(200)); core_ok = buddy_healthy();
    }
    // A blank otadata partition needs a first VALID entry, so the very first
    // Wi-Fi update already has a confirmed previous image to roll back to.
    if (initial && core_ok && esp_ota_set_boot_partition(running) != ESP_OK) core_ok = false;
    if (core_ok && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(tag, "OTA_BOOT: CONFIRMED slot=%s initial=%d", running->label, initial);
    else {
        if (initial) { ESP_LOGE(tag, "OTA_BOOT: INITIAL_HEALTH_FAILED"); return; }
        ESP_LOGE(tag, "OTA_BOOT: ROLLBACK local_health_failed");
        const auto error = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(tag, "OTA_BOOT: rollback unavailable %s", esp_err_to_name(error));
    }
}
void buddy_ota_tick()
{
    if (restart_at.load() && now_ms() >= restart_at.load()) esp_restart();
}
esp_err_t buddy_ota_status(httpd_req_t *request)
{
    const auto *running = esp_ota_get_running_partition();
    const auto *target = esp_ota_get_next_update_partition(nullptr);
    auto *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "slot", running ? running->label : "unknown");
    cJSON_AddNumberToObject(root, "max_bytes", target ? target->size : 0);
    cJSON_AddBoolToObject(root, "busy", updating.load());
    esp_ota_img_states_t state;
    const auto state_result = esp_ota_get_state_partition(running, &state);
    cJSON_AddBoolToObject(root, "ready", state_result == ESP_OK && state == ESP_OTA_IMG_VALID);
    char *wire = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const auto result = httpd_resp_send(request, wire ? wire : "{}", HTTPD_RESP_USE_STRLEN);
    cJSON_free(wire); return result;
}
esp_err_t buddy_ota_upload(httpd_req_t *request, const uint8_t *expected_sha256)
{
    const auto *target = esp_ota_get_next_update_partition(nullptr);
    const auto *running = esp_ota_get_running_partition();
    if (!target || !running || target->address == running->address)
        return message(request, "503 Service Unavailable", "Güncelleme alanı hazır değil.");
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_VALID)
        return message(request, "409 Conflict", "Jamiryo açılış kontrolünü yapıyor. Birkaç saniye sonra tekrar dene.");
    if (request->content_len < firmware_prefix_size || request->content_len > target->size)
        return message(request, "400 Bad Request", "Dosya boyutu uygun değil. Jamiryo için hazırlanan .bin dosyasını seç.");
    uint8_t buffer[4096];
    if (!receive(request, buffer, firmware_prefix_size)) return ESP_FAIL;
    if (!jamiryo_firmware_compatible(buffer, firmware_prefix_size, jamiryo_ota_contract))
        return message(request, "400 Bad Request", "Bu dosya Jamiryo’nun bu sürümüyle uyumlu değil. Güncel .bin dosyasını seç.");
    bool expected = false;
    if (!updating.compare_exchange_strong(expected, true))
        return message(request, "409 Conflict", "Bir güncelleme zaten sürüyor.");
    // The servo task observes OTA busy and stops PWM without changing saved settings.
    const int64_t quiet_deadline = now_ms() + 10000;
    while (now_ms() < quiet_deadline && (buddy_ai_ready() || buddy_audio_busy() || microphone_interacting()))
        vTaskDelay(pdMS_TO_TICKS(100));
    if (buddy_audio_busy() || microphone_interacting()) {
        updating.store(false);
        return message(request, "409 Conflict", "Jamiryo sustuktan sonra tekrar dene.");
    }
    esp_ota_handle_t handle = 0;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    bool hash_ok = !expected_sha256 || psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
    esp_err_t error = esp_ota_begin(target, request->content_len, &handle);
    if (!hash_ok) error = ESP_FAIL;
    size_t used = firmware_prefix_size;
    if (error == ESP_OK && expected_sha256 && psa_hash_update(&hash, buffer, used) != PSA_SUCCESS) error = ESP_FAIL;
    if (error == ESP_OK) error = esp_ota_write(handle, buffer, used);
    const int64_t upload_deadline = now_ms() + 300000;
    while (error == ESP_OK && used < request->content_len) {
        const size_t count = std::min<size_t>(sizeof(buffer), request->content_len - used);
        if (now_ms() > upload_deadline || !receive(request, buffer, count)) { error = ESP_ERR_TIMEOUT; break; }
        if (expected_sha256 && psa_hash_update(&hash, buffer, count) != PSA_SUCCESS) { error = ESP_FAIL; break; }
        error = esp_ota_write(handle, buffer, count); used += count;
    }
    if (error == ESP_OK && expected_sha256) {
        uint8_t actual[32]; size_t size = 0; unsigned difference = 0;
        if (psa_hash_finish(&hash, actual, sizeof(actual), &size) != PSA_SUCCESS || size != 32) error = ESP_FAIL;
        else { for (unsigned i=0;i<32;++i) difference |= actual[i] ^ expected_sha256[i]; if (difference) error=ESP_ERR_INVALID_CRC; }
    }
    psa_hash_abort(&hash);
    if (error == ESP_OK) { error = esp_ota_end(handle); handle = 0; }
    if (handle) esp_ota_abort(handle);
    if (error == ESP_OK) error = esp_ota_set_boot_partition(target);
    if (error != ESP_OK) {
        ESP_LOGW(tag, "OTA_UPLOAD: REJECTED bytes=%zu error=%s", used, esp_err_to_name(error));
        updating.store(false);
        return message(request, "400 Bad Request", "Güncelleme tamamlanamadı. Mevcut yazılım korundu; dosyayı ve Wi-Fi bağlantısını kontrol edip tekrar dene.");
    }
    ESP_LOGI(tag, "OTA_UPLOAD: VERIFIED bytes=%zu next=%s", used, target->label);
    const auto result = message(request, "200 OK", "Güncelleme yüklendi. Jamiryo yeniden başlıyor; yaklaşık 20 saniye sonra hazır olacak. Ev ayarların ve hafızan korundu.");
    restart_at.store(now_ms() + 1500); return result;
}

bool buddy_ota_http_test(const char *csrf, bool install)
{
    const auto *running = esp_ota_get_running_partition();
    esp_partition_pos_t position{running->address, running->size};
    esp_image_metadata_t metadata{};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK) return false;
    uint8_t block[1024];
    if (esp_partition_read(running, 0, block, firmware_prefix_size) != ESP_OK) return false;
    auto request = [&](const uint8_t *data, size_t count, const char *token, const char *origin, bool abort) {
        esp_http_client_config_t config{};
        config.url = BUDDY_SETUP_URL "api/firmware"; config.timeout_ms = 45000;
        auto *client = esp_http_client_init(&config);
        if (!client) return false;
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        esp_http_client_set_header(client, "Origin", origin);
        esp_http_client_set_header(client, "X-Jamiryo-Token", token);
        bool ok = esp_http_client_open(client, abort ? 4096 : count) == ESP_OK;
        if (ok) ok = esp_http_client_write(client, reinterpret_cast<const char *>(data), count) == static_cast<int>(count);
        if (abort) {
            esp_http_client_close(client); esp_http_client_cleanup(client);
            vTaskDelay(pdMS_TO_TICKS(500)); return ok && !buddy_ota_busy();
        }
        if (ok) ok = esp_http_client_fetch_headers(client) >= 0;
        const int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ok && status == (std::strcmp(token, csrf) || std::strcmp(origin, BUDDY_SETUP_ORIGIN) ? 403 : 400);
    };
    bool ok = request(block, firmware_prefix_size, "invalid-token", BUDDY_SETUP_ORIGIN, false)
        && request(block, firmware_prefix_size, csrf, "http://foreign.invalid", false);
    block[12] ^= 1;
    ok = ok && request(block, firmware_prefix_size, csrf, BUDDY_SETUP_ORIGIN, false);
    block[12] ^= 1;
    ok = ok && request(block, firmware_prefix_size, csrf, BUDDY_SETUP_ORIGIN, false)
        && request(block, firmware_prefix_size, csrf, BUDDY_SETUP_ORIGIN, true)
        && esp_ota_get_boot_partition()->address == running->address;
    ESP_LOGI(tag, "OTA_HTTP_GUARD_TEST: %s (auth/origin/chip/incomplete/abort/current_boot_unchanged)", ok ? "PASS" : "FAIL");
    if (!ok || !install) return ok;
    esp_http_client_config_t config{};
    config.url = BUDDY_SETUP_URL "api/firmware"; config.timeout_ms = 120000;
    auto *client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "Origin", BUDDY_SETUP_ORIGIN);
    esp_http_client_set_header(client, "X-Jamiryo-Token", csrf);
    ok = esp_http_client_open(client, metadata.image_len) == ESP_OK;
    for (size_t offset = 0; ok && offset < metadata.image_len;) {
        const size_t count = std::min<size_t>(sizeof(block), metadata.image_len - offset);
        ok = esp_partition_read(running, offset, block, count) == ESP_OK;
        for (size_t sent = 0; ok && sent < count;) {
            const int written = esp_http_client_write(client, reinterpret_cast<char *>(block + sent), count - sent);
            ok = written > 0;
            if (ok) sent += written;
        }
        offset += count;
    }
    if (ok) ok = esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200;
    esp_http_client_close(client); esp_http_client_cleanup(client);
    ESP_LOGI(tag, "OTA_HTTP_INSTALL_TEST: %s bytes=%u", ok ? "PASS" : "FAIL", static_cast<unsigned>(metadata.image_len));
    return ok;
}
