#include "head_servo.h"
#include "head_motion.h"
#include "head_boot_test.h"
#include "face_follow.h"
#include "head_routine.h"
#include "head_actions.h"
#include "person_seek.h"
#include "buddy.h"
#include "audio/microphone.h"
#include "behavior/idle_sleep.h"
#include "network/gemini_live.h"
#include "network/firmware_update.h"
#include "sensors/camera.h"
#include "sensors/faces.h"
#include <atomic>
#include <cstring>
#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr char tag[] = "buddy_head";
constexpr gpio_num_t signal_pin = GPIO_NUM_2;
std::atomic<bool> available{false}, moving{false};
std::atomic<bool> boot_pending{head_settings::boot_test_enabled};
std::atomic<HeadMode> shown_mode{HeadMode::Off};
std::atomic<int> requested{-1}, shown_pulse{HeadMotion::center_us};
std::atomic<int> test_gaze{100}; // 100 means inactive; active gaze is -12..12.
std::atomic<int64_t> ignore_until{0}, photo_until{0};
std::atomic<unsigned> follow_settings{0}, follow_generation{0}, follow_frames{0};
std::atomic<int64_t> follow_ready_at{0};
std::atomic<int> follow_gaze{100}, follow_x{0};
std::atomic<bool> following{false};
std::atomic<bool> follow_preview{false};
std::atomic<bool> routine_window{false};
std::atomic<RoutinePhase> routine_phase{RoutinePhase::Stopped};
constexpr int turn_request_base = 100;
constexpr int scan_request = 200;
constexpr int object_request = 300;
std::atomic<unsigned> object_token{0};
std::atomic<int> object_target{HeadMotion::center_us};
std::atomic<int64_t> object_until{0};
std::atomic<int64_t> conversation_seen{0}, person_pending_until{0};
std::atomic<unsigned> person_capture_generation{0};
std::atomic<PersonPhase> person_phase{PersonPhase::Idle};
std::atomic<bool> person_selftest{false};
std::atomic<unsigned> person_starts{0}, person_matched{0}, person_not_found{0}, person_canceled{0}, person_frames{0};
std::atomic<bool> scan_active{false}, actions_selftest_passed{false};
std::atomic<unsigned> scan_requests{0}, scans_completed{0}, gesture_moves{0};
std::atomic<int> shown_target{HeadMotion::center_us};
std::atomic<unsigned> voice_requests{0};
std::atomic<int> turn_target{HeadMotion::center_us}, turn_degrees{0};
std::atomic<bool> turn_active{false};
std::atomic<bool> voice_selftest_passed{false};
struct Observation { bool valid; int x; int64_t at; unsigned generation; };
QueueHandle_t observations = nullptr;
QueueHandle_t person_observations = nullptr;
SemaphoreHandle_t settings_mutex = nullptr;
mcpwm_timer_handle_t timer = nullptr;
mcpwm_oper_handle_t oper = nullptr;
mcpwm_cmpr_handle_t comparator = nullptr;
mcpwm_gen_handle_t generator = nullptr;
bool timer_enabled = false, timer_started = false;
int64_t now_ms() { return esp_timer_get_time() / 1000; }

bool check(esp_err_t error, const char *operation)
{
    if (error == ESP_OK) return true;
    ESP_LOGE(tag, "SERVO_DRIVER: FAIL operation=%s error=%s", operation, esp_err_to_name(error));
    return false;
}

void release_driver()
{
    if (generator) mcpwm_generator_set_force_level(generator, 0, true);
    if (timer_started) mcpwm_timer_start_stop(timer, MCPWM_TIMER_STOP_EMPTY);
    if (timer_enabled) mcpwm_timer_disable(timer);
    if (generator) mcpwm_del_generator(generator);
    if (comparator) mcpwm_del_comparator(comparator);
    if (oper) mcpwm_del_operator(oper);
    if (timer) mcpwm_del_timer(timer);
    generator = nullptr; comparator = nullptr; oper = nullptr; timer = nullptr;
    timer_started = timer_enabled = false;
    gpio_reset_pin(signal_pin);
    gpio_set_direction(signal_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(signal_pin, 0);
}

bool init_driver()
{
    gpio_reset_pin(signal_pin);
    gpio_set_direction(signal_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(signal_pin, 0);
    // Camera XCLK already owns LEDC timer/channel 0. MCPWM keeps its clock
    // untouched and gives integer-microsecond pulse widths at 50 Hz.
    mcpwm_timer_config_t config{};
    config.group_id = 0;
    config.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    config.resolution_hz = 1000000;
    config.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    config.period_ticks = 20000;
    if (!check(mcpwm_new_timer(&config, &timer), "timer")) return false;
    mcpwm_operator_config_t operator_config{};
    operator_config.group_id = 0;
    if (!check(mcpwm_new_operator(&operator_config, &oper), "operator")
        || !check(mcpwm_operator_connect_timer(oper, timer), "connect")) return false;
    mcpwm_comparator_config_t compare_config{};
    compare_config.flags.update_cmp_on_tez = true;
    if (!check(mcpwm_new_comparator(oper, &compare_config, &comparator), "comparator")) return false;
    mcpwm_generator_config_t generator_config{};
    generator_config.gen_gpio_num = signal_pin;
    if (!check(mcpwm_new_generator(oper, &generator_config, &generator), "generator")
        || !check(mcpwm_generator_set_force_level(generator, 0, true), "startup-low")
        || !check(mcpwm_comparator_set_compare_value(comparator, HeadMotion::center_us), "center")
        || !check(mcpwm_generator_set_action_on_timer_event(generator,
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)), "rising-edge")
        || !check(mcpwm_generator_set_action_on_compare_event(generator,
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator, MCPWM_GEN_ACTION_LOW)), "falling-edge")
        || !check(mcpwm_timer_enable(timer), "enable")) return false;
    timer_enabled = true;
    if (!check(mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP), "start")) return false;
    timer_started = true;
    return true; // Generator remains forced LOW: no servo pulses at boot.
}

enum class Activity { Idle, Listen, Think, Speak, Sleep };
Activity activity()
{
    if (buddy_idle_snapshot().stage != BuddySleepStage::Awake) return Activity::Sleep;
    if (buddy_audio_busy()) return Activity::Speak;
    if (microphone_interacting()) return Activity::Listen;
    if (buddy_ai_waiting_reply()) return Activity::Think;
    return Activity::Idle;
}
bool turn_arguments_self_test()
{
    struct Case { const char *json; bool valid; BuddyHeadDirection direction; };
    const Case cases[] = {
        {"{\"direction\":\"left\"}", true, BuddyHeadDirection::Left},
        {"{\"direction\":\"right\"}", true, BuddyHeadDirection::Right},
        {"{\"direction\":\"center\"}", true, BuddyHeadDirection::Center},
        {"{}", false, BuddyHeadDirection::Center},
        {"[]", false, BuddyHeadDirection::Center},
        {"{\"direction\":null}", false, BuddyHeadDirection::Center},
        {"{\"direction\":45}", false, BuddyHeadDirection::Center},
        {"{\"direction\":\"back\"}", false, BuddyHeadDirection::Center},
        {"{\"direction\":\"right\",\"angle\":180}", false, BuddyHeadDirection::Center},
        {"{\"direction\":\"left\",\"direction\":\"right\"}", false, BuddyHeadDirection::Center}
    };
    BuddyHeadDirection direction = BuddyHeadDirection::Center;
    if (buddy_head_parse_turn(nullptr, direction)) return false;
    for (const auto &item : cases) {
        auto *args = cJSON_Parse(item.json);
        if (!args) return false;
        const bool accepted = buddy_head_parse_turn(args, direction); cJSON_Delete(args);
        if (accepted != item.valid || (accepted && direction != item.direction)) return false;
    }
    if (buddy_head_parse_scan(nullptr)) return false;
    for (const char *wire : {"{}", "[]", "{\"angle\":180}", "{\"repeat\":true}"}) {
        auto *args = cJSON_Parse(wire);
        if (!args) return false;
        const bool accepted = buddy_head_parse_scan(args); cJSON_Delete(args);
        if (accepted != !std::strcmp(wire, "{}")) return false;
    }
    return true;
}

void servo_task(void *)
{
    HeadMotion head;
    FaceFollow follow;
    HeadRoutine routine;
    HeadTurn turn;
    HeadScan scan;
    HeadGestures gestures;
    PersonSeek person;
    unsigned person_serial = 0;
    bool was_person_capture = false;
    HeadBootTest boot_test(head_settings::boot_test_enabled);
    bool pulses = false;
    int written = HeadMotion::center_us;
    Activity previous_activity = Activity::Idle;
    BuddyEmotion previous_emotion = BuddyEmotion::Count;
    int64_t reaction_at = 0, next_move = 0;
    int direction = 1;
    bool follow_paused = true;
    bool was_updating = false, was_window = false, routine_sleeping = false;
    bool turn_photo_paused = false;
    int64_t last_turn_tick = 0;
    if (follow_settings.load() & 1) {
        head.command(HeadMode::Routine, now_ms()); routine.begin(now_ms(), true);
    }
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        const int64_t now = now_ms();
        const HeadMode old_mode = head.mode();
        const bool updating = buddy_ota_busy();
        const int queued = requested.exchange(-1);
        const bool directed_request = queued >= turn_request_base && queued <= turn_request_base + 2;
        const int requested_mode = directed_request ? static_cast<int>(HeadMode::Directed)
            : queued == scan_request ? static_cast<int>(HeadMode::Scan)
            : queued == object_request ? static_cast<int>(HeadMode::Object) : queued;
        const int request = updating && head.mode() != HeadMode::Off
            ? static_cast<int>(HeadMode::Off) : updating ? -1
            : was_updating && queued < 0 && (follow_settings.load() & 1) ? static_cast<int>(HeadMode::Routine) : requested_mode;
        was_updating = updating;
        if (request >= 0) {
            if (person.active()) { person.cancel(); person_canceled.fetch_add(1); }
            person_pending_until.store(0); person_capture_generation.store(0);
            if (request != static_cast<int>(HeadMode::Object)) object_token.store(0);
            // A manual off/center/test/auto request always cancels the pending
            // boot sweep, so it cannot override a later stop command.
            boot_test.cancel();
            head.command(static_cast<HeadMode>(request), now);
            turn.cancel(); scan.cancel(); turn_photo_paused = false; gestures.reset(now);
            if (head.mode() == HeadMode::Scan) scan.begin(now, (follow_settings.load() & 2) != 0);
            if (head.mode() == HeadMode::Directed) {
                const auto direction = static_cast<BuddyHeadDirection>(queued - turn_request_base - 1);
                turn.begin(direction, (follow_settings.load() & 2) != 0, now);
                head.move_to(turn.target(), now, 1000); last_turn_tick = now;
                turn_target.store(turn.target()); turn_degrees.store(static_cast<int>(direction) * 45);
                ESP_LOGI(tag, "SERVO_VOICE_TURN: degrees=%d target_us=%d hold_ms=20000 (shaft position unverified)", static_cast<int>(direction) * 45, turn.target());
            }
            if (head.mode() == HeadMode::Routine) routine.begin(now, false);
            else routine.cancel();
            follow.reset(now); follow_paused = true;
            xQueueReset(observations);
            follow_ready_at.store(now + 900);
            next_move = now + 15000;
            previous_emotion = BuddyEmotion::Count;
            if (head.mode() != HeadMode::Off) ignore_until.store(now + 900);
            ESP_LOGI(tag, "SERVO_MODE: %s", head_mode_name(head.mode()));
        } else if (boot_test.take_due(now)) {
            if (buddy_healthy() && available.load()) {
                head.command(HeadMode::Test, now);
                ignore_until.store(now + 900);
                ESP_LOGI(tag, "SERVO_BOOT_TEST: START uptime_ms=%lld delay_ms=10000", static_cast<long long>(now));
                ESP_LOGI(tag, "SERVO_MODE: test");
            } else {
                ESP_LOGW(tag, "SERVO_BOOT_TEST: SKIPPED device_unhealthy (no later retry)");
            }
        }
        const bool person_hardware = available.load() && buddy_healthy() && !updating
            && (follow_settings.load() & 1) && !follow_preview.load()
            && buddy_camera_enabled() && buddy_faces_tracking_enabled()
            && activity() != Activity::Sleep;
        // Raw RMS noise is not recognized speech. Briefly wait for actual
        // transcription to finish, without letting room noise block forever.
        const bool person_listening = microphone_interacting() && now - conversation_seen.load() < 1200;
        const bool person_quiet = !person_listening && !buddy_audio_busy()
            && now >= photo_until.load() && !buddy_faces_exclusive_capture();
        const int64_t person_pending = person_pending_until.load();
        if (person_pending) {
            const bool ordinary = person_seek::can_start(head.mode(), person.phase());
            if (!person_hardware || !ordinary || now >= person_pending) person_pending_until.store(0);
            else if (person_quiet) {
                person_pending_until.store(0); boot_test.cancel(); routine.cancel(); turn.cancel(); scan.cancel();
                head.command(HeadMode::Person, now); person.begin(now, (follow_settings.load() & 2) != 0);
                follow.reset(now); xQueueReset(observations); xQueueReset(person_observations);
                person_capture_generation.store(0); was_person_capture = false;
                ignore_until.store(now + 900); person_starts.fetch_add(1);
                ESP_LOGI(tag, "CONVERSATION_GAZE: START (local anonymous face search)");
            }
        }
        if (head.mode() == HeadMode::Person) {
            if (!person_hardware || buddy_faces_exclusive_capture()) {
                person.cancel(); person_canceled.fetch_add(1);
            } else {
                Observation observation;
                if (xQueueReceive(person_observations, &observation, 0) == pdTRUE
                    && observation.generation == person_capture_generation.load() && observation.generation) {
                    person.observe(observation.valid, observation.x, observation.at, now); person_frames.fetch_add(1);
                }
                const bool matched_before = person.matched();
                const auto plan = person.update(now, person_quiet, head.update(now), head.moving(),
                    buddy_head_motion_recent(), conversation_seen.load());
                if (!person_quiet) head.freeze(now);
                if (plan.move) head.move_to(plan.target, now, plan.duration_ms);
                if (!matched_before && person.matched()) {
                    person_matched.fetch_add(1);
                    ESP_LOGI(tag, "CONVERSATION_GAZE: ALIGNED (PWM plan, no shaft feedback)");
                }
                if (!person.active() && !person.matched()) person_not_found.fetch_add(1);
            }
            if (!person.active()) {
                const auto resume = (follow_settings.load() & 1) ? HeadMode::Routine : HeadMode::Hold;
                head.command(resume, now); if (resume == HeadMode::Routine) routine.begin(now, false);
                gestures.reset(now);
            }
        }
        const bool person_capture = head.mode() == HeadMode::Person && person.wants_frame()
            && person_quiet && !head.moving() && !buddy_head_motion_recent();
        if (person_capture && !was_person_capture) {
            if (!++person_serial) ++person_serial;
            person_capture_generation.store(person_serial);
        } else if (!person_capture) person_capture_generation.store(0);
        was_person_capture = person_capture; person_phase.store(person.phase());
        if (head.mode() == HeadMode::Object) {
            if (!object_token.load() || (now >= object_until.load() && now >= photo_until.load()) || activity() == Activity::Sleep || updating) {
                object_token.store(0);
                const auto resume = (follow_settings.load() & 1) ? HeadMode::Routine : HeadMode::Hold;
                head.command(resume, now);
                if (resume == HeadMode::Routine) routine.begin(now, false);
                gestures.reset(now);
            } else if (now < photo_until.load()) {
                head.freeze(now);
            } else if (head.target() != object_target.load()) {
                head.move_to(object_target.load(), now, 1100);
            }
        }
        if (head.mode() == HeadMode::Directed) {
            const bool sleep = activity() == Activity::Sleep;
            if (sleep || turn.expired(now)) {
                const int held_target = sleep ? HeadMotion::center_us : turn.target();
                turn.cancel();
                if (follow_settings.load() & 1) {
                    head.command(HeadMode::Routine, now); routine.begin(now, false);
                } else {
                    head.command(HeadMode::Hold, now); head.move_to(held_target, now, 900);
                }
                ESP_LOGI(tag, "SERVO_VOICE_TURN: RELEASE mode=%s", head_mode_name(head.mode()));
            } else if (now < photo_until.load()) {
                head.freeze(now); turn.pause(now - last_turn_tick); turn_photo_paused = true;
            } else if (turn_photo_paused) {
                head.move_to(turn.target(), now, 1000); turn_photo_paused = false;
            }
            last_turn_tick = now;
        }
        if (head.mode() == HeadMode::Scan) {
            const bool sleeping = activity() == Activity::Sleep;
            if (sleeping) scan.cancel();
            const bool allowed = !sleeping && !updating && now >= photo_until.load()
                && !microphone_interacting() && !buddy_faces_exclusive_capture();
            const auto plan = scan.update(now, allowed);
            if (!allowed && !sleeping) head.freeze(now);
            if (plan.move) {
                head.move_to(plan.target, now, plan.duration_ms);
                ESP_LOGI(tag, "SERVO_SCAN: target_us=%d duration_ms=%d", plan.target, plan.duration_ms);
            }
            if (!scan.active()) {
                if (!sleeping) scans_completed.fetch_add(1);
                const auto resume = (follow_settings.load() & 1) ? HeadMode::Routine : HeadMode::Hold;
                head.command(resume, now); if (resume == HeadMode::Routine) routine.begin(now, false);
                gestures.reset(now);
                ESP_LOGI(tag, "SERVO_SCAN: %s", sleeping ? "CANCELED_SLEEP" : "COMPLETE");
            }
        }
        scan_active.store(scan.active()); turn_active.store(turn.active());
        if (head.mode() == HeadMode::Routine) {
            const auto phase = routine.phase();
            const bool quiet = buddy_healthy() && !updating && now >= photo_until.load()
                && activity() == Activity::Idle && !buddy_ai_output_pending() && !buddy_faces_exclusive_capture();
            const auto plan = routine.update(now, quiet);
            if (plan.move) head.move_to(plan.target, now, plan.duration_ms);
            const bool speaking_gesture = routine.phase() == RoutinePhase::Hold && activity() == Activity::Speak
                && now >= photo_until.load() && !buddy_faces_exclusive_capture();
            if (!quiet && activity() != Activity::Sleep && !speaking_gesture) head.freeze(now);
            if (!quiet && activity() == Activity::Sleep && !routine_sleeping)
                head.move_to(HeadMotion::center_us, now, 900);
            routine_sleeping = activity() == Activity::Sleep;
            if (phase != routine.phase()) {
                gestures.reset(now);
                ESP_LOGI(tag, "SERVO_ROUTINE: phase=%s target_us=%d",
                    routine_phase_name(routine.phase()), plan.move ? plan.target : head.pulse());
            }
        }
        routine_phase.store(routine.phase());
        const auto phase = routine.phase();
        boot_pending.store(boot_test.pending() || (head.mode() == HeadMode::Routine
            && phase != RoutinePhase::Hold && phase != RoutinePhase::Track && phase != RoutinePhase::Stopped));
        const bool window = head.mode() == HeadMode::Routine && routine.tracking();
        routine_window.store(window);
        if (window != was_window) {
            follow_generation.fetch_add(1); xQueueReset(observations);
            follow.reset(now); follow_paused = true; was_window = window;
        }
        if (head.mode() == HeadMode::Follow || window) {
            if (!buddy_head_follow_scanning()) {
                if (!follow_paused && activity() == Activity::Sleep)
                    head.move_to(HeadMotion::center_us, now, 900);
                else if (activity() != Activity::Sleep) head.freeze(now);
                follow.reset(now); follow_paused = true;
                xQueueReset(observations); follow_gaze.store(100); following.store(false);
            } else {
                if (follow_paused) { follow.reset(now); follow_paused = false; }
                Observation observation;
                if (xQueueReceive(observations, &observation, 0) == pdTRUE
                    && observation.generation == follow_generation.load()
                    && observation.at <= now && now - observation.at <= 900) {
                    follow.observe(observation.valid, observation.x, observation.at);
                    follow_x.store(observation.valid ? observation.x : 0); ++follow_frames;
                    // An invalid or now-centered measurement must stop the old
                    // correction instead of finishing it with stale coordinates.
                    if (!observation.valid || (follow.active() && !follow.gaze_x())) head.freeze(now);
                }
                const auto plan = follow.update(now, head.update(now), (follow_settings.load() & 2) != 0);
                if (plan.move) {
                    head.move_to(plan.pulse_us, now, 700);
                    ESP_LOGI(tag, "SERVO_FOLLOW: target_us=%d gaze_x=%d face=%d",
                             plan.pulse_us, follow.gaze_x(), follow.active());
                }
                follow_gaze.store(follow.active() ? follow.gaze_x() : 100);
                following.store(follow.active());
            }
        } else { follow_gaze.store(100); following.store(false); }
        if (head.mode() == HeadMode::Routine && routine.phase() == RoutinePhase::Hold) {
            const Activity current = activity();
            const bool allowed = buddy_healthy() && !updating && now >= photo_until.load()
                && !buddy_faces_exclusive_capture()
                && (current == Activity::Speak || (current == Activity::Idle && !buddy_ai_output_pending()));
            const auto plan = gestures.update(now, allowed, current == Activity::Speak,
                head.moving(), (follow_settings.load() & 2) != 0);
            if (plan.move) {
                head.move_to(plan.target, now, plan.duration_ms); gesture_moves.fetch_add(1);
                ESP_LOGI(tag, "SERVO_GESTURE: phase=%s target_us=%d", current == Activity::Speak ? "speaking" : "idle", plan.target);
            }
        }
        if (head.mode() == HeadMode::Auto) {
            const Activity current = activity();
            const BuddyEmotion emotion = buddy_ai_emotion();
            if (current != previous_activity || emotion != previous_emotion) {
                previous_activity = current; previous_emotion = emotion;
                // Let the OLED establish the expression before moving the head.
                reaction_at = now + 160;
            }
            if (now < photo_until.load()) {
                head.freeze(now); next_move = now + 2000;
            } else if (reaction_at && now >= reaction_at) {
                int target = HeadMotion::center_us;
                if (current != Activity::Sleep && current != Activity::Listen) {
                    if (emotion == BuddyEmotion::Shy) target -= 45;
                    else if (emotion == BuddyEmotion::Curious || emotion == BuddyEmotion::Surprised) target += 50;
                    else if (emotion == BuddyEmotion::Happy || emotion == BuddyEmotion::Excited) target += 35 * direction;
                    else if (emotion == BuddyEmotion::Angry) target -= 35 * direction;
                }
                head.move_to(target, now, 900);
                next_move = now + (current == Activity::Speak ? 1600 : 15000);
                reaction_at = 0;
            } else if (!head.moving() && now >= next_move) {
                if (current == Activity::Speak || current == Activity::Idle) {
                    direction = -direction;
                    head.move_to(HeadMotion::center_us + direction * (current == Activity::Speak ? 30 : 45),
                                 now, current == Activity::Speak ? 850 : 1400);
                }
                next_move = now + (current == Activity::Speak ? 1400 : 15000 + esp_random() % 30001);
            }
        }
        const int pulse = head.update(now);
        const bool active = head.mode() != HeadMode::Off && !follow_preview.load()
            && (head.mode() != HeadMode::Routine || routine.output());
        bool ok = true;
        if (active && pulse != written) {
            ok = check(mcpwm_comparator_set_compare_value(comparator, pulse), "pulse");
            written = pulse;
        }
        if (active != pulses) {
            ok = check(mcpwm_generator_set_force_level(generator, active ? -1 : 0, true), "output") && ok;
            pulses = active;
        }
        if (!ok) {
            mcpwm_generator_set_force_level(generator, 0, true);
            head.command(HeadMode::Off, now); turn.cancel(); scan.cancel();
            object_token.store(0);
            person.cancel(); person_phase.store(PersonPhase::Idle); person_capture_generation.store(0);
            turn_active.store(false); scan_active.store(false);
            pulses = false; available.store(false);
        }
        if (head.moving()) ignore_until.store(now + 700);
        test_gaze.store(head.mode() == HeadMode::Test && pulses && ok ? head.test_gaze_x() : 100);
        if (old_mode == HeadMode::Test && head.mode() == HeadMode::Off && request < 0)
            ESP_LOGI(tag, "SERVO_TEST: COMPLETE output=off (physical motion needs observation)");
        shown_mode.store(head.mode()); shown_pulse.store(pulse); shown_target.store(head.target()); moving.store(head.moving());
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
    }
}
} // namespace

bool buddy_head_start()
{
    const bool passed = head_motion_self_test();
    ESP_LOGI(tag, "SERVO_ENGINE_TEST: %s (bounds/easing/retarget/off/test-cancel)", passed ? "PASS" : "FAIL");
    const bool boot_passed = head_boot_test_self_test();
    ESP_LOGI(tag, "SERVO_BOOT_ENGINE_TEST: %s (10000ms/once/cancel/no-repeat/eye-sync)", boot_passed ? "PASS" : "FAIL");
    const bool follow_passed = face_follow_self_test();
    ESP_LOGI(tag, "SERVO_FOLLOW_ENGINE_TEST: %s (delay/deadband/bounds/reverse/loss/stale/reset)", follow_passed ? "PASS" : "FAIL");
    const bool routine_passed = head_routine_self_test();
    ESP_LOGI(tag, "SERVO_ROUTINE_TEST: %s (once-per-boot/sweep/center/pause/periodic-window/cancel)", routine_passed ? "PASS" : "FAIL");
    const bool turn_passed = head_turn_self_test() && turn_arguments_self_test();
    const bool actions_passed = head_actions_self_test(); actions_selftest_passed.store(actions_passed);
    const bool person_passed = person_seek_self_test() && person_seek_pose_test(); person_selftest.store(person_passed);
    ESP_LOGI(tag, "CONVERSATION_GAZE_TEST: %s (cadence/fresh stable faces/pause/reverse/bounds/expiry/cancel)", person_passed ? "PASS" : "FAIL");
    ESP_LOGI(tag, "SERVO_ACTIONS_TEST: %s (scan/reverse/pause/cancel/idle/speech/pacing)", actions_passed ? "PASS" : "FAIL");
    voice_selftest_passed.store(turn_passed);
    ESP_LOGI(tag, "SERVO_VOICE_TEST: %s (left/right/center/reverse/hold/retarget/cancel/invalid-args)", turn_passed ? "PASS" : "FAIL");
    if (!passed || !boot_passed || !follow_passed || !routine_passed || !turn_passed || !actions_passed || !person_passed) return false;
    observations = xQueueCreate(1, sizeof(Observation));
    person_observations = xQueueCreate(1, sizeof(Observation));
    settings_mutex = xSemaphoreCreateMutex();
    if (!observations || !person_observations || !settings_mutex) return false;
    // Head startup precedes the network module that historically initialized
    // default NVS. Initialize here too so saved follow settings load on boot.
    // Never erase shared credentials or memories if initialization fails.
    const esp_err_t storage = nvs_flash_init();
    if (storage != ESP_OK)
        ESP_LOGW(tag, "SERVO_SETTINGS: storage unavailable (%s); default off", esp_err_to_name(storage));
    nvs_handle_t handle;
    if (storage == ESP_OK && nvs_open("buddy_head", NVS_READWRITE, &handle) == ESP_OK) {
        uint8_t value;
        if (nvs_get_u8(handle, "follow", &value) == ESP_OK && value <= 3) follow_settings.store(value);
        // This robot's servo is mounted in the opposite orientation. Apply this
        // explicit mounting correction once, retaining the automatic on/off bit.
        // Later changes in the setup page must survive subsequent firmware boots.
        uint8_t mounting_revision = 0;
        if (nvs_get_u8(handle, "mount_rev", &mounting_revision) == ESP_ERR_NVS_NOT_FOUND) {
            const unsigned corrected = follow_settings.load() | 2u;
            const bool saved = nvs_set_u8(handle, "follow", corrected) == ESP_OK
                && nvs_set_u8(handle, "mount_rev", 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
            if (saved) {
                follow_settings.store(corrected);
                ESP_LOGI(tag, "SERVO_MOUNTING: reverse=1 SAVED automatic=%d", (corrected & 1u) != 0);
            } else ESP_LOGW(tag, "SERVO_MOUNTING: correction not saved");
        }
        nvs_close(handle);
    }
    if (!init_driver()) { release_driver(); return false; }
    if (xTaskCreate(servo_task, "buddy_head", 3072, nullptr, 2, nullptr) != pdPASS) {
        release_driver(); return false;
    }
    available.store(true);
    ESP_LOGI(tag, "SERVO_INIT: PASS gpio=2 frequency=50Hz limits=1250..1750us boot=%s",
             follow_settings.load() & 1 ? "scan-then-periodic-follow" : head_settings::boot_test_enabled ? "delayed-test" : "off");
    if (head_settings::boot_test_enabled)
        ESP_LOGI(tag, "SERVO_BOOT_TEST: ARMED due_uptime_ms=10000 once_per_boot");
    return true;
}

bool buddy_head_motion_recent() { return now_ms() < ignore_until.load(); }
bool buddy_head_parse_turn(const cJSON *args, BuddyHeadDirection &direction)
{
    if (!cJSON_IsObject(args) || cJSON_GetArraySize(args) != 1) return false;
    const auto *value = cJSON_GetObjectItemCaseSensitive(args, "direction");
    if (!cJSON_IsString(value)) return false;
    if (!std::strcmp(value->valuestring, "left")) direction = BuddyHeadDirection::Left;
    else if (!std::strcmp(value->valuestring, "right")) direction = BuddyHeadDirection::Right;
    else if (!std::strcmp(value->valuestring, "center")) direction = BuddyHeadDirection::Center;
    else return false;
    return true;
}
bool buddy_head_turn(BuddyHeadDirection direction)
{
    if (!valid_head_direction(direction) || !available.load() || !buddy_healthy()
        || buddy_ota_busy() || now_ms() < photo_until.load() || buddy_faces_exclusive_capture()) return false;
    buddy_idle_activity();
    object_token.store(0);
    follow_preview.store(false); follow_generation.fetch_add(1);
    requested.store(turn_request_base + static_cast<int>(direction) + 1);
    voice_requests.fetch_add(1); return true;
}
bool buddy_head_parse_scan(const cJSON *args) { return cJSON_IsObject(args) && cJSON_GetArraySize(args) == 0; }
bool buddy_head_scan()
{
    if (!available.load() || !buddy_healthy() || buddy_ota_busy() || scan_active.load()
        || requested.load() == scan_request || now_ms() < photo_until.load() || buddy_faces_exclusive_capture()) return false;
    buddy_idle_activity(); follow_preview.store(false); follow_generation.fetch_add(1);
    object_token.store(0);
    requested.store(scan_request); scan_requests.fetch_add(1); return true;
}
BuddyHeadSnapshot buddy_head_snapshot()
{
    return {available.load(), moving.load(), turn_active.load() || scan_active.load(), voice_selftest_passed.load(), shown_pulse.load(), shown_target.load(),
        turn_degrees.load(), voice_requests.load(), head_mode_name(shown_mode.load()), scan_active.load() || requested.load() == scan_request, actions_selftest_passed.load(),
        scan_requests.load(), scans_completed.load(), gesture_moves.load(), routine_phase_name(routine_phase.load())};
}
bool buddy_head_test_gaze(int &gaze_x)
{
    const int gaze = test_gaze.load();
    if (gaze == 100) return false;
    gaze_x = gaze;
    return true;
}
void buddy_head_photo_hold() { photo_until.store(now_ms() + 2000); }
bool buddy_head_object_begin(unsigned token)
{
    if (!token || !available.load() || !buddy_healthy() || buddy_ota_busy()
        || now_ms() < photo_until.load() || buddy_faces_exclusive_capture()) return false;
    buddy_idle_activity(); follow_preview.store(false); follow_generation.fetch_add(1);
    object_target.store(HeadMotion::center_us); object_until.store(now_ms() + 45000);
    object_token.store(token); requested.store(object_request); return true;
}
bool buddy_head_object_current(unsigned token)
{
    return token && object_token.load() == token && now_ms() < object_until.load()
        && available.load() && !buddy_ota_busy();
}
bool buddy_head_object_move(unsigned token, int pulse)
{
    if (!buddy_head_object_current(token) || pulse < HeadMotion::minimum_us || pulse > HeadMotion::maximum_us) return false;
    object_target.store(pulse); return true;
}
bool buddy_head_object_settled(unsigned token)
{
    return buddy_head_object_current(token) && shown_mode.load() == HeadMode::Object
        && !moving.load() && !buddy_head_motion_recent() && shown_pulse.load() == object_target.load();
}
bool buddy_head_object_hold(unsigned token)
{
    if (!buddy_head_object_settled(token)) return false;
    object_until.store(now_ms() + 20000); return true;
}
void buddy_head_object_cancel(unsigned token)
{
    unsigned expected = token;
    if (token) object_token.compare_exchange_strong(expected, 0);
}
void buddy_head_note_conversation()
{
    const int64_t now = now_ms(), previous = conversation_seen.exchange(now);
    if (person_seek::new_conversation(now, previous)) person_pending_until.store(now + person_seek::pending_ms);
    else {
        // Long opening sentences extend an existing opportunity. A manual
        // cancellation sets it to zero and must never be revived by a late chunk.
        int64_t pending = person_pending_until.load();
        if (pending) person_pending_until.compare_exchange_strong(pending, now + person_seek::pending_ms);
    }
}
unsigned buddy_head_person_generation() { return person_capture_generation.load(); }
void buddy_head_person_observe(bool valid, int x_milli, int64_t at, unsigned generation)
{
    if (!person_observations || !generation || generation != person_capture_generation.load()) return;
    const Observation observation{valid, x_milli, at, generation};
    xQueueOverwrite(person_observations, &observation);
}
BuddyPersonSnapshot buddy_head_person_snapshot()
{
    const auto phase = person_phase.load();
    return {phase != PersonPhase::Idle, person_selftest.load(), person_phase_name(phase), person_starts.load(),
        person_matched.load(), person_not_found.load(), person_canceled.load(), person_frames.load()};
}
BuddyHeadFollowSettings buddy_head_follow_settings()
{
    const unsigned value = follow_settings.load();
    return {(value & 1) != 0, (value & 2) != 0};
}
bool buddy_head_set_follow(bool enabled, bool reverse)
{
    if (!available.load() || !settings_mutex || buddy_ota_busy()
        || xSemaphoreTake(settings_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    const unsigned value = (enabled ? 1u : 0u) | (reverse ? 2u : 0u);
    bool ok = true;
    if (value != follow_settings.load()) {
        nvs_handle_t handle;
        ok = nvs_open("buddy_head", NVS_READWRITE, &handle) == ESP_OK;
        if (ok) {
            ok = nvs_set_u8(handle, "follow", value) == ESP_OK && nvs_commit(handle) == ESP_OK;
            nvs_close(handle);
        }
    }
    if (ok) {
        follow_preview.store(false);
        follow_settings.store(value); follow_generation.fetch_add(1);
        requested.store(static_cast<int>(enabled ? HeadMode::Routine : HeadMode::Off));
        ESP_LOGI(tag, "SERVO_FOLLOW_SETTINGS: enabled=%d reverse=%d SAVED", enabled, reverse);
    }
    xSemaphoreGive(settings_mutex);
    return ok;
}
bool buddy_head_follow_scanning()
{
    return available.load() && (shown_mode.load() == HeadMode::Follow || routine_window.load())
        && !buddy_ota_busy() && buddy_healthy() && buddy_camera_enabled()
        && now_ms() >= follow_ready_at.load() && now_ms() >= photo_until.load()
        && buddy_idle_snapshot().stage == BuddySleepStage::Awake
        && !buddy_faces_exclusive_capture()
        && !buddy_audio_busy() && !microphone_interacting() && !buddy_ai_output_pending();
}
unsigned buddy_head_follow_generation() { return follow_generation.load(); }
void buddy_head_follow_observe(bool valid, int x_milli, int64_t at, unsigned generation)
{
    if (!observations || !buddy_head_follow_scanning() || generation != follow_generation.load()) return;
    const Observation observation{valid, x_milli, at, generation};
    xQueueOverwrite(observations, &observation);
}
bool buddy_head_follow_gaze(int &gaze_x)
{
    const int gaze = follow_gaze.load();
    if (gaze == 100) return false;
    gaze_x = gaze;
    return true;
}
void buddy_head_status()
{
    ESP_LOGI(tag, "SERVO_STATUS: available=%d mode=%s gpio=2 pulse_us=%d moving=%d limits=1250..1750 boot=%s boot_pending=%d",
             available.load(), head_mode_name(shown_mode.load()), shown_pulse.load(), moving.load(),
             follow_settings.load() & 1 ? "scan-then-periodic-follow" : head_settings::boot_test_enabled ? "delayed-test" : "off", boot_pending.load());
    ESP_LOGI(tag, "SERVO_ROUTINE_STATUS: phase=%s interval_ms=30000 window_ms=12000 nominal_scan_degrees=90",
             routine_phase_name(routine_phase.load()));
    ESP_LOGI(tag, "SERVO_VOICE_STATUS: active=%d target_us=%d degrees=%d requests=%u",
             turn_active.load(), turn_target.load(), turn_degrees.load(), voice_requests.load());
    const auto settings = buddy_head_follow_settings();
    ESP_LOGI(tag, "SERVO_FOLLOW_STATUS: enabled=%d reverse=%d scanning=%d following=%d frames=%u x_milli=%d gaze_x=%d preview=%d",
             settings.enabled, settings.reverse, buddy_head_follow_scanning(), following.load(),
             follow_frames.load(), follow_x.load(), follow_gaze.load(), follow_preview.load());
    ESP_LOGI(tag, "SERVO_FOLLOW_GATES: healthy=%d ota=%d camera=%d delay=%d photo=%d awake=%d faces_busy=%d audio=%d mic=%d ai_pending=%d",
             buddy_healthy(), buddy_ota_busy(), buddy_camera_enabled(), now_ms() < follow_ready_at.load(),
             now_ms() < photo_until.load(), buddy_idle_snapshot().stage == BuddySleepStage::Awake,
             buddy_faces_exclusive_capture(), buddy_audio_busy(), microphone_interacting(), buddy_ai_output_pending());
}
bool buddy_head_command(const char *line)
{
    if (std::strcmp(line, "servo-status") == 0) buddy_head_status();
    else if (std::strcmp(line, "servo-selftest") == 0) {
        ESP_LOGI(tag, "SERVO_ENGINE_TEST: %s (no physical movement)", head_motion_self_test() ? "PASS" : "FAIL");
        ESP_LOGI(tag, "SERVO_BOOT_ENGINE_TEST: %s (no physical movement)", head_boot_test_self_test() ? "PASS" : "FAIL");
        ESP_LOGI(tag, "SERVO_FOLLOW_ENGINE_TEST: %s (no physical movement)", face_follow_self_test() ? "PASS" : "FAIL");
        ESP_LOGI(tag, "SERVO_ROUTINE_TEST: %s (no physical movement)", head_routine_self_test() ? "PASS" : "FAIL");
        ESP_LOGI(tag, "SERVO_VOICE_TEST: %s (no physical movement)", head_turn_self_test() && turn_arguments_self_test() ? "PASS" : "FAIL");
    }
    else if (std::strcmp(line, "servo-track-preview") == 0) {
        if (!available.load() || buddy_ota_busy()) ESP_LOGW(tag, "SERVO_FOLLOW_PREVIEW: UNAVAILABLE");
        else {
            follow_preview.store(true); follow_generation.fetch_add(1);
            requested.store(static_cast<int>(HeadMode::Follow));
            ESP_LOGI(tag, "SERVO_FOLLOW_PREVIEW: START (PWM disabled, settings unchanged)");
        }
    }
    else if (std::strcmp(line, "servo-track-on") == 0 || std::strcmp(line, "servo-track-off") == 0
             || std::strcmp(line, "servo-track-reverse") == 0 || std::strcmp(line, "servo-track-normal") == 0) {
        const auto settings = buddy_head_follow_settings();
        const bool enabled = std::strcmp(line, "servo-track-on") == 0 ? true :
            std::strcmp(line, "servo-track-off") == 0 ? false : settings.enabled;
        const bool reverse = std::strcmp(line, "servo-track-reverse") == 0 ? true :
            std::strcmp(line, "servo-track-normal") == 0 ? false : settings.reverse;
        ESP_LOGI(tag, "SERVO_FOLLOW_REQUEST: %s", buddy_head_set_follow(enabled, reverse) ? "SAVED" : "FAILED");
    }
    else {
        HeadMode mode;
        if (std::strcmp(line, "servo-off") == 0) mode = HeadMode::Off;
        else if (std::strcmp(line, "servo-center") == 0) mode = HeadMode::Hold;
        else if (std::strcmp(line, "servo-test") == 0) mode = HeadMode::Test;
        else if (std::strcmp(line, "servo-on") == 0) mode = HeadMode::Auto;
        else return false;
        if (!available.load()) ESP_LOGW(tag, "SERVO_REQUEST: UNAVAILABLE");
        else {
            const auto settings = buddy_head_follow_settings();
            if (settings.enabled && !buddy_head_set_follow(false, settings.reverse))
                ESP_LOGW(tag, "SERVO_FOLLOW_SETTINGS: disable not saved (manual stop still applied)");
            follow_preview.store(false); follow_generation.fetch_add(1);
            requested.store(static_cast<int>(mode));
            ESP_LOGI(tag, "SERVO_REQUEST: %s QUEUED", head_mode_name(mode));
        }
    }
    return true;
}
