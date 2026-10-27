#include <cinttypes>
#include <cstddef>
#include <cstdint>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "buddy.h"
#include "network/firmware_update.h"

namespace {
constexpr char kTag[] = "buddy";
constexpr std::size_t kExpectedMemoryBytes = 8 * 1024 * 1024;
constexpr std::size_t kTestBytes = 64 * 1024;

// The IDF boot test also checks PSRAM. This verifies an explicitly allocated
// application buffer with address-dependent values and alternating bit patterns.
bool verify_psram_buffer()
{
    auto *buffer = static_cast<std::uint32_t *>(
        heap_caps_malloc(kTestBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        ESP_LOGE(kTag, "PSRAM buffer test: FAIL (64 KiB allocation failed)");
        return false;
    }

    constexpr std::uint32_t patterns[] = {0x00000000, 0x55555555, 0xAAAAAAAA};
    for (const auto pattern : patterns) {
        for (std::size_t i = 0; i < kTestBytes / sizeof(*buffer); ++i) {
            buffer[i] = static_cast<std::uint32_t>(i) ^ pattern;
        }
        for (std::size_t i = 0; i < kTestBytes / sizeof(*buffer); ++i) {
            if (buffer[i] != (static_cast<std::uint32_t>(i) ^ pattern)) {
                ESP_LOGE(kTag, "PSRAM buffer test: FAIL (word %zu)", i);
                heap_caps_free(buffer);
                return false;
            }
        }
    }

    heap_caps_free(buffer);
    ESP_LOGI(kTag, "PSRAM buffer test: PASS (64 KiB, 3 patterns)");
    return true;
}

bool report_hardware()
{
    const auto *app = esp_app_get_description();
    esp_chip_info_t chip{};
    esp_chip_info(&chip);

    ESP_LOGI(kTag, "Mini Desktop Buddy - stage 1 hardware check");
    ESP_LOGI(kTag, "Firmware: %s %s | ESP-IDF: %s", app->project_name,
             app->version, esp_get_idf_version());
    ESP_LOGI(kTag, "Chip: %s | cores: %u | revision: %u.%u", CONFIG_IDF_TARGET,
             static_cast<unsigned>(chip.cores), chip.revision / 100,
             chip.revision % 100);
    ESP_LOGI(kTag, "Reset reason: %d", static_cast<int>(esp_reset_reason()));

    std::uint32_t flash_bytes = 0;
    const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_bytes);
    const bool flash_ok = flash_result == ESP_OK && flash_bytes == kExpectedMemoryBytes;
    ESP_LOGI(kTag, "Flash: %" PRIu32 " bytes | %s", flash_bytes,
             flash_ok ? "PASS" : "FAIL");
    if (flash_result != ESP_OK) {
        ESP_LOGE(kTag, "Flash query failed: %s", esp_err_to_name(flash_result));
    }

    const std::size_t psram_bytes = esp_psram_get_size();
    const bool psram_ready = esp_psram_is_initialized()
                             && psram_bytes == kExpectedMemoryBytes;
    ESP_LOGI(kTag, "PSRAM: %zu bytes | %s", psram_bytes,
             psram_ready ? "PASS" : "FAIL");
    ESP_LOGI(kTag, "Free heap: internal=%zu bytes | PSRAM=%zu bytes",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    const bool buffer_ok = psram_ready && verify_psram_buffer();
    return chip.model == CHIP_ESP32S3 && chip.cores == 2
           && flash_ok && psram_ready && buffer_ok;
}
} // namespace

extern "C" void app_main()
{
    // Give the host time to reconnect to the USB console after a reset.
    vTaskDelay(pdMS_TO_TICKS(2000));
    const bool hardware_ok = report_hardware();
    ESP_LOGI(kTag, "BUDDY_STAGE1: %s", hardware_ok ? "PASS" : "FAIL");
    const bool buddy_ok = hardware_ok && buddy_ota_models_valid() && buddy_start();
    ESP_LOGI(kTag, "BUDDY_STAGE2: %s", buddy_ok ? "PASS" : "FAIL");
    buddy_ota_confirm_boot(buddy_ok);

    for (;;) {
        buddy_ota_tick();
        ESP_LOGI(kTag, "BUDDY_HEARTBEAT uptime=%" PRIi64 "s status=%s free_internal=%zu free_psram=%zu",
                 esp_timer_get_time() / 1000000, hardware_ok && buddy_healthy() ? "PASS" : "FAIL",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
