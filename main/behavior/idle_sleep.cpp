#include "idle_sleep.h"
#include "buddy.h"
#include "audio/microphone.h"
#include "network/gemini_live.h"
#include <cinttypes>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr char tag[] = "buddy_idle";
IdleSleepEngine engine;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
int64_t now_ms() { return esp_timer_get_time() / 1000; }
}

bool buddy_idle_start()
{
    const bool passed = idle_sleep_self_test();
    ESP_LOGI(tag, "IDLE_SLEEP_TEST: %s (timing/wake/once-only/offline/demo-expiry)", passed ? "PASS" : "FAIL");
    buddy_idle_activity();
    ESP_LOGI(tag, "IDLE_SLEEP_POLICY: drowsy_s=120 asleep_s=180 mumble_s=300 mic=always_listening");
    return passed;
}
void buddy_idle_activity()
{
    portENTER_CRITICAL(&lock);
    engine.activity(now_ms());
    portEXIT_CRITICAL(&lock);
}
BuddySleepSnapshot buddy_idle_snapshot()
{
    portENTER_CRITICAL(&lock);
    const auto result = engine.snapshot(now_ms());
    portEXIT_CRITICAL(&lock);
    return result;
}
bool buddy_idle_take_mumble()
{
    portENTER_CRITICAL(&lock);
    const bool taken = engine.take_mumble(now_ms());
    portEXIT_CRITICAL(&lock);
    return taken;
}
void buddy_idle_status()
{
    const auto value = buddy_idle_snapshot();
    ESP_LOGI(tag, "IDLE_SLEEP_STATUS: stage=%s idle_ms=%" PRId64 " mumble_due=%d mumble_used=%d demo=%d",
             buddy_sleep_name(value.stage), value.idle_ms, value.mumble_due, value.mumble_used, value.demo);
}
bool buddy_idle_command(const char *line)
{
    if (std::strcmp(line, "sleep-status") == 0) buddy_idle_status();
    else if (std::strcmp(line, "sleep-selftest") == 0) {
        ESP_LOGI(tag, "IDLE_SLEEP_TEST: %s", idle_sleep_self_test() ? "PASS" : "FAIL");
    } else if (std::strcmp(line, "sleep-demo") == 0) {
        if (buddy_audio_busy() || microphone_interacting() || buddy_ai_output_pending()) {
            ESP_LOGI(tag, "IDLE_SLEEP_DEMO: BUSY"); return true;
        }
        portENTER_CRITICAL(&lock);
        engine.demo(now_ms());
        portEXIT_CRITICAL(&lock);
        ESP_LOGI(tag, "IDLE_SLEEP_DEMO: START drowsy_s=3 asleep_s=7 mumble_s=11 duration_s=20");
    } else if (std::strcmp(line, "sleep-wake") == 0) {
        buddy_idle_activity(); buddy_idle_status();
    } else return false;
    return true;
}
