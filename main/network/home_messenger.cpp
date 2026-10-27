#include "home_messenger.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
enum class Phase { Idle, Queued, Active, Finishing, Failed };
struct Data {
    BuddyMessageRequest request;
    BuddyMessageResult result;
    Phase phase = Phase::Idle;
    int64_t since = 0;
    unsigned accepted = 0, completed = 0, failures = 0;
};
Data *data = nullptr;
SemaphoreHandle_t lock = nullptr;
bool tested = false;
int64_t now_ms() { return esp_timer_get_time() / 1000; }
const char *field(const cJSON *o, const char *name)
{
    const auto *v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}
bool same(const char *a, const char *b) { return a && b && !std::strcmp(a, b); }
bool valid_text(const char *p)
{
    if (!p || std::strlen(p) > 560) return false;
    unsigned chars = 0;
    const auto *s = reinterpret_cast<const unsigned char *>(p);
    while (*s) {
        uint32_t cp; unsigned n;
        if (*s < 0x80) { cp = *s; n = 1; }
        else if (*s >= 0xc2 && *s <= 0xdf) { cp = *s & 31; n = 2; }
        else if (*s >= 0xe0 && *s <= 0xef) { cp = *s & 15; n = 3; }
        else if (*s >= 0xf0 && *s <= 0xf4) { cp = *s & 7; n = 4; }
        else return false;
        for (unsigned i = 1; i < n; ++i) {
            if ((s[i] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (s[i] & 63);
        }
        if ((n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) || cp > 0x10ffff
            || (cp >= 0xd800 && cp <= 0xdfff) || (cp < 32 && cp != 9 && cp != 10) || cp == 127) return false;
        if (++chars > 140) return false;
        s += n;
    }
    return true;
}
bool photo_words(const char *input)
{
    char out[192]{}; size_t used = 0;
    for (const auto *s = reinterpret_cast<const unsigned char *>(input); *s && used + 1 < sizeof(out); ++s) {
        unsigned char c = *s;
        if (c == 0xc4 && (s[1] == 0x9f || s[1] == 0x9e)) { c = 'g'; ++s; }
        else if (c == 0xc4 && s[1] == 0xb1) { c = 'i'; ++s; }
        else if (c == 0xc4 && s[1] == 0xb0) { c = 'i'; ++s; }
        else if (c == 0xc3 && (s[1] == 0xb6 || s[1] == 0x96)) { c = 'o'; ++s; }
        else if (c == 0xc3 && (s[1] == 0xbc || s[1] == 0x9c)) { c = 'u'; ++s; }
        else if (c == 0xc3 && (s[1] == 0xa7 || s[1] == 0x87)) { c = 'c'; ++s; }
        else if (c >= 0x80) return false;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c == '.' || c == '!' || c == '?' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ' && (!used || out[used - 1] == ' ')) continue;
        out[used++] = char(c);
    }
    while (used && out[used - 1] == ' ') out[--used] = 0;
    for (const char *words : {"fotograf gonder", "fotograf cek", "fotograf cekip gonder", "fotograf cekip telefonuma gonder",
                              "bana fotograf gonder", "evden fotograf gonder", "foto gonder"})
        if (same(out, words)) return true;
    return false;
}
void fail_locked(const char *reason)
{
    std::snprintf(data->result.error, sizeof(data->result.error), "%s", reason);
    data->phase = Phase::Failed; ++data->failures;
}
bool self_test()
{
    auto *request = static_cast<BuddyMessageRequest *>(heap_caps_calloc(1, sizeof(BuddyMessageRequest), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!request) return false;
    const char *cases[] = {
        "{\"v\":1,\"id\":\"20261026000123456789\",\"mode\":\"ask\",\"text\":\"Merhaba\",\"at\":1000}",
        "{\"v\":1,\"id\":\"20261026000123456789\",\"mode\":\"photo\",\"text\":\"\",\"at\":1000}",
        "{\"v\":1,\"id\":\"bad\",\"mode\":\"ask\",\"text\":\"Merhaba\",\"at\":1000}",
        "{\"v\":1,\"id\":\"20261026000123456789\",\"mode\":\"ask\",\"text\":true,\"at\":1000}",
        "{\"v\":1,\"id\":\"20261026000123456789\",\"mode\":\"shell\",\"text\":\"Merhaba\",\"at\":1000}",
        "{\"v\":1,\"id\":\"20261026000123456789\",\"mode\":\"ask\",\"text\":\"\",\"at\":1000}"
    };
    bool ok = true;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        auto *o = cJSON_Parse(cases[i]); ok &= buddy_message_decode(o, 1010, *request) == (i < 2);
        if (i == 0) { ok &= !buddy_message_decode(o, 1200, *request); ok &= !buddy_message_decode(o, 980, *request); }
        cJSON_Delete(o);
    }
    ok &= photo_words("Fotoğraf gönder!") && photo_words("FOTOĞRAF GÖNDER")
        && !photo_words("fotoğraf gönderme") && !photo_words("fotoğraf nedir")
        && valid_text("Şimdi nasılsın?") && !valid_text("bad\rtext") && !valid_text("\xc0\x80");
    auto *result = static_cast<BuddyMessageResult *>(heap_caps_calloc(1, sizeof(BuddyMessageResult), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (result) {
        std::strcpy(request->id, "20261026000123456789"); std::strcpy(request->text, "fixture"); request->mode = BuddyMessageMode::Say;
        ok &= buddy_messenger_queue(*request) && !buddy_messenger_queue(*request) && buddy_messenger_take(*request)
            && buddy_messenger_reading() && buddy_messenger_generating();
        buddy_messenger_text("Şimdi "); buddy_messenger_text("hazır."); buddy_messenger_audio(); buddy_messenger_complete();
        ok &= !buddy_messenger_result(*result, false) && buddy_messenger_result(*result, true)
            && same(result->text, "Şimdi hazır.") && result->audio && !result->error[0];
        buddy_messenger_release();
        data->accepted = data->completed = data->failures = 0;
        std::memset(result, 0, sizeof(*result)); heap_caps_free(result);
    } else ok = false;
    std::memset(request, 0, sizeof(*request)); heap_caps_free(request); return ok;
}
}

bool buddy_message_decode(const cJSON *o, double server_now, BuddyMessageRequest &r)
{
    const char *id = field(o, "id"), *mode = field(o, "mode"), *words = field(o, "text");
    const auto *v = cJSON_GetObjectItemCaseSensitive(o, "v"), *at = cJSON_GetObjectItemCaseSensitive(o, "at");
    const bool prefixed = id && id[0] == 'm' && std::strlen(id) == 21;
    if (!cJSON_IsObject(o) || cJSON_GetArraySize(o) != 5 || !cJSON_IsNumber(v) || v->valuedouble != 1
        || !id || (!prefixed && std::strlen(id) != 20) || !mode || !valid_text(words) || !cJSON_IsNumber(at)
        || !std::isfinite(at->valuedouble) || !std::isfinite(server_now) || std::floor(at->valuedouble) != at->valuedouble
        || server_now - at->valuedouble > 120 || server_now - at->valuedouble < -5) return false;
    for (const char *p = id + (prefixed ? 1 : 0); *p; ++p) if (*p < '0' || *p > '9') return false;
    if (!same(mode, "ask") && !same(mode, "say") && !same(mode, "photo")) return false;
    if (!same(mode, "photo") && !*words) return false;
    std::strcpy(r.id, id); std::strcpy(r.text, words);
    r.mode = same(mode, "photo") || (same(mode, "ask") && photo_words(words)) ? BuddyMessageMode::Photo
        : same(mode, "say") ? BuddyMessageMode::Say : BuddyMessageMode::Ask;
    return true;
}
bool buddy_messenger_start()
{
    lock = xSemaphoreCreateMutex();
    void *p = heap_caps_calloc(1, sizeof(Data), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p || !lock) return false;
    data = new (p) Data; tested = self_test(); return tested;
}
bool buddy_messenger_queue(const BuddyMessageRequest &r)
{
    if (!data || !lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY); const bool ok = data->phase == Phase::Idle;
    if (ok) {
        data->request = r; std::memset(&data->result, 0, sizeof(data->result)); std::strcpy(data->result.id, r.id);
        data->phase = Phase::Queued; data->since = now_ms(); ++data->accepted;
    }
    xSemaphoreGive(lock); return ok;
}
bool buddy_messenger_take(BuddyMessageRequest &r)
{
    if (!data) return false;
    xSemaphoreTake(lock, portMAX_DELAY); const bool ok = data->phase == Phase::Queued;
    if (ok) { r = data->request; data->phase = Phase::Active; data->since = now_ms(); }
    xSemaphoreGive(lock); return ok;
}
bool buddy_messenger_generating()
{
    if (!data) return false;
    xSemaphoreTake(lock, portMAX_DELAY); bool value = data->phase == Phase::Active; xSemaphoreGive(lock); return value;
}
bool buddy_messenger_reading()
{
    if (!data) return false;
    xSemaphoreTake(lock, portMAX_DELAY); bool value = data->phase == Phase::Active && data->request.mode == BuddyMessageMode::Say;
    xSemaphoreGive(lock); return value;
}
void buddy_messenger_text(const char *text)
{
    if (!data || !text) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (data->phase == Phase::Active) {
        size_t used = std::strlen(data->result.text), size = std::strlen(text), take = std::min(size, sizeof(data->result.text) - used - 1);
        while (take && (static_cast<unsigned char>(text[take]) & 0xc0) == 0x80) --take;
        std::memcpy(data->result.text + used, text, take); data->result.text[used + take] = 0;
        data->result.truncated |= take != size;
    }
    xSemaphoreGive(lock);
}
void buddy_messenger_audio()
{
    if (!data) return;
    xSemaphoreTake(lock, portMAX_DELAY); if (data->phase == Phase::Active) data->result.audio = true; xSemaphoreGive(lock);
}
void buddy_messenger_complete()
{
    if (!data) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (data->phase == Phase::Active) {
        data->phase = Phase::Finishing;
        if (!data->result.text[0]) fail_locked("missing_reply_text");
    }
    xSemaphoreGive(lock);
}
void buddy_messenger_fail(const char *reason)
{
    if (!data) return;
    xSemaphoreTake(lock, portMAX_DELAY); if (data->phase == Phase::Active) fail_locked(reason); xSemaphoreGive(lock);
}
bool buddy_messenger_result(BuddyMessageResult &r, bool output_idle)
{
    if (!data) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    if ((data->phase == Phase::Queued && now_ms() - data->since > 30000)
        || ((data->phase == Phase::Active || data->phase == Phase::Finishing) && now_ms() - data->since > 90000)) fail_locked("reply_timeout");
    bool done = data->phase == Phase::Failed || (data->phase == Phase::Finishing && output_idle);
    if (done) r = data->result;
    xSemaphoreGive(lock); return done;
}
void buddy_messenger_release()
{
    if (!data) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (data->phase == Phase::Finishing) ++data->completed;
    std::memset(&data->request, 0, sizeof(data->request)); std::memset(&data->result, 0, sizeof(data->result));
    data->phase = Phase::Idle; xSemaphoreGive(lock);
}
BuddyMessengerSnapshot buddy_messenger_snapshot()
{
    BuddyMessengerSnapshot r{data != nullptr, tested, "idle", 0, 0, 0}; if (!data) return r;
    xSemaphoreTake(lock, portMAX_DELAY);
    const char *names[] = {"idle", "queued", "active", "finishing", "failed"}; r.phase = names[unsigned(data->phase)];
    r.accepted = data->accepted; r.completed = data->completed; r.failures = data->failures;
    xSemaphoreGive(lock); return r;
}
