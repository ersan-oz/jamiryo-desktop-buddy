#include "presence.h"
#include "buddy.h"
#include "audio/microphone.h"
#include "network/gemini_live.h"
#include "network/firmware_update.h"
#include "display/eyes.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
PresenceEngine engine;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
bool selftest = false;
int64_t now_ms() { return esp_timer_get_time() / 1000; }
}
bool buddy_presence_start()
{
    selftest = presence_self_test();
    engine.begin(now_ms(), esp_random());
    ESP_LOGI("buddy_presence", "PRESENCE_TEST: %s (mood decay/blinks/sleep/no replay/bounds/priorities)", selftest ? "PASS" : "FAIL");
    return selftest;
}
PresenceSnapshot buddy_presence_snapshot()
{
    // Take other subsystems' snapshots before our short critical section.
    const auto sleep = buddy_idle_snapshot();
    const auto phase = buddy_audio_busy() ? PresencePhase::Speaking
        : microphone_interacting() ? PresencePhase::Listening
        : buddy_ai_waiting_reply() ? PresencePhase::Thinking : PresencePhase::Idle;
    const bool blocked = buddy_ota_busy() || buddy_display_snapshot().pickup_visible
        || (phase == PresencePhase::Idle && buddy_ai_output_pending());
    portENTER_CRITICAL(&lock);
    const auto value = engine.update(now_ms(), phase, sleep.stage, blocked);
    portEXIT_CRITICAL(&lock);
    return value;
}
void buddy_presence_engage()
{
    portENTER_CRITICAL(&lock); engine.engage(now_ms()); portEXIT_CRITICAL(&lock);
}
void buddy_presence_emotion(BuddyEmotion emotion)
{
    portENTER_CRITICAL(&lock); engine.emotion(emotion, now_ms()); portEXIT_CRITICAL(&lock);
}
bool buddy_presence_selftest_pass() { return selftest; }
