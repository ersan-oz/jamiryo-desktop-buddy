// Included after home_photo.inc: only the existing home worker performs HTTP.
struct MessageAck {
    uint32_t magic = 0x4a4d5331, phase = 0;
    char name[48]{}, url[192]{}, id[33]{};
};
struct HomeMessages {
    Profile home;
    MessageAck ack;
    BuddyMessageRequest request;
    BuddyMessageResult result;
    char slot[16]{}, current_id[33]{}, reply[2049]{}, error[48]{}, image[160]{};
    const char *state = "ready";
    const char *reply_source = "status";
    int64_t poll_at = 0, publish_at = 0, server_ms = 0;
    double server_now = 0;
    bool bound = false, ack_ok = false, active = false, configured = false;
};
HomeMessages *messages = nullptr;

bool message_slot(const Profile &profile, char (&key)[16])
{
    bool found = false; xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < database->count; ++i)
        if (same(profile.name, database->profiles[i].name) && same(profile.url, database->profiles[i].url)) {
            std::snprintf(key, sizeof(key), "last%u", i); found = true; break;
        }
    xSemaphoreGive(mutex); return found;
}
bool message_ack(bool save)
{
    auto &m = *messages; nvs_handle_t handle;
    const auto opened = nvs_open_from_partition(partition, "messages", save ? NVS_READWRITE : NVS_READONLY, &handle);
    if (!save && opened == ESP_ERR_NVS_NOT_FOUND) return true;
    if (opened != ESP_OK) return false;
    bool ok;
    if (save) ok = nvs_set_blob(handle, m.slot, &m.ack, sizeof(m.ack)) == ESP_OK && nvs_commit(handle) == ESP_OK;
    else {
        size_t size = sizeof(m.ack); const auto e = nvs_get_blob(handle, m.slot, &m.ack, &size);
        ok = e == ESP_ERR_NVS_NOT_FOUND || (e == ESP_OK && size == sizeof(m.ack) && m.ack.magic == 0x4a4d5331
            && m.ack.phase <= 3 && std::memchr(m.ack.name, 0, sizeof(m.ack.name)) && std::memchr(m.ack.url, 0, sizeof(m.ack.url))
            && std::memchr(m.ack.id, 0, sizeof(m.ack.id)) && same(m.ack.name, m.home.name));
    }
    nvs_close(handle); return ok;
}
bool message_phone(const Profile &profile, PhoneRecord &phone, const char *&error)
{
    auto *targets = static_cast<buddy_notify::Targets *>(heap_caps_calloc(1, sizeof(buddy_notify::Targets), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!targets) { error = "out_of_memory"; return false; }
    bool ok = discover_phones(profile, *targets, error);
    if (ok && !phone_record(profile, phone)) {
        if (targets->count == 1) ok = phone_record(profile, phone, targets->names[0]);
        else { error = targets->count ? "choose_phone_in_setup" : "phone_not_registered"; ok = false; }
    }
    if (ok && !contains_phone(*targets, phone.service)) { error = "selected_phone_unavailable"; ok = false; }
    heap_caps_free(targets); return ok;
}
bool message_notify(const Profile &profile, const char *words, const char *&error)
{
    PhoneRecord phone; if (!message_phone(profile, phone, error)) return false;
    char path[160]; std::snprintf(path, sizeof(path), "/api/services/notify/%s", phone.service);
    auto *payload = cJSON_CreateObject(); cJSON_AddStringToObject(payload, "title", "Jamiryo");
    cJSON_AddStringToObject(payload, "message", words);
    auto *data = cJSON_AddObjectToObject(payload, "data");
    cJSON_AddStringToObject(data, "clickAction", "/jamiryo-mesaj/sohbet");
    char *wire = cJSON_PrintUnformatted(payload); cJSON_Delete(payload);
    auto *response = wire ? http(profile, path, wire, error) : nullptr;
    if (wire) { std::memset(wire, 0, std::strlen(wire)); cJSON_free(wire); }
    const bool ok = cJSON_IsArray(response); cJSON_Delete(response);
    if (!ok && !error) error = "notification_failed";
    return ok;
}
bool message_publish(const char *notification_error = nullptr)
{
    auto &m = *messages;
    if (m.server_ms) m.server_now += double(now_ms() - m.server_ms) / 1000;
    m.server_ms = now_ms();
    auto *payload = cJSON_CreateObject(); cJSON_AddStringToObject(payload, "state", m.state);
    auto *attrs = cJSON_AddObjectToObject(payload, "attributes");
    cJSON_AddStringToObject(attrs, "friendly_name", "Jamiryo mesajlaşma");
    cJSON_AddNumberToObject(attrs, "protocol", 1); cJSON_AddNumberToObject(attrs, "last_seen", m.server_now);
    cJSON_AddStringToObject(attrs, "request_id", m.current_id); cJSON_AddStringToObject(attrs, "reply", m.reply);
    cJSON_AddStringToObject(attrs, "reply_source", m.reply_source);
    cJSON_AddStringToObject(attrs, "error", m.error); cJSON_AddStringToObject(attrs, "image", m.image);
    cJSON_AddStringToObject(attrs, "notification_error", notification_error ? notification_error : "");
    cJSON_AddStringToObject(attrs, "firmware", esp_app_get_description()->version);
    char *wire = cJSON_PrintUnformatted(payload); cJSON_Delete(payload); const char *error = nullptr;
    auto *response = wire ? http(m.home, "/api/states/sensor.jamiryo_messenger", wire, error) : nullptr;
    if (wire) { std::memset(wire, 0, std::strlen(wire)); cJSON_free(wire); }
    const bool ok = response != nullptr; cJSON_Delete(response);
    m.publish_at = now_ms() + (ok ? 20000 : 5000); return ok;
}
void message_done(const char *error, const char *reply, const char *image = "", bool notify = false)
{
    auto &m = *messages; m.active = false; m.state = error ? "error" : "done";
    if (error) m.reply_source = "status";
    std::snprintf(m.error, sizeof(m.error), "%s", error ? error : "");
    std::snprintf(m.reply, sizeof(m.reply), "%s", reply ? reply : "");
    std::snprintf(m.image, sizeof(m.image), "%s", image);
    m.ack.phase = error ? 3 : 2;
    if (!message_ack(true)) { m.state = "error"; std::strcpy(m.error, "completion_not_saved"); }
    const char *phone_error = nullptr;
    if (notify && !error && m.reply[0]) message_notify(m.home, m.reply, phone_error);
    message_publish(phone_error);
}
void message_tick()
{
    if (!messages || buddy_ota_busy() || restart_at.load() || !storage_ready) return;
    auto &m = *messages;
    if (m.active) {
        if (buddy_messenger_result(m.result, !buddy_ai_output_pending() && !buddy_audio_busy())) {
            m.reply_source = m.result.transcript ? "model_transcript" : "requested_text";
            message_done(m.result.error[0] ? m.result.error : nullptr, m.result.text, "", true);
            std::memset(&m.result, 0, sizeof(m.result)); buddy_messenger_release();
        } else if (now_ms() >= m.publish_at) message_publish();
        return;
    }
    if (now_ms() < m.poll_at) return;
    m.poll_at = now_ms() + 3000;
    // This PSRAM profile snapshot avoids adding a 1.8 KiB local to the worker.
    if (!active_profile(m.home)) { m.poll_at = now_ms() + 30000; return; }
    if (!m.bound) {
        if (!message_slot(m.home, m.slot)) return;
        m.ack_ok = message_ack(false); m.bound = true;
        if (!m.ack_ok) m.ack = MessageAck{};
        std::strcpy(m.current_id, m.ack.id);
        if (!m.ack_ok) { m.state = "error"; std::strcpy(m.error, "message_storage_unavailable"); }
        else if (m.ack.phase == 1) { m.state = "error"; std::strcpy(m.error, "previous_result_unconfirmed"); }
    }
    const char *error = nullptr;
    constexpr char query[] = "{\"template\":\"{% set exists = states.input_text | selectattr('entity_id','eq','input_text.jamiryo_inbox') | list | count %}{{ {'request': states('input_text.jamiryo_inbox'), 'now': as_timestamp(now()) | int, 'configured': exists > 0} | to_json }}\"}";
    auto *root = http(m.home, "/api/template", query, error);
    if (!root) { m.poll_at = now_ms() + 15000; return; }
    const auto *clock = cJSON_GetObjectItemCaseSensitive(root, "now"); const char *wire = text(root, "request");
    if (!cJSON_IsNumber(clock) || !std::isfinite(clock->valuedouble) || !wire || std::strlen(wire) > 2048) {
        cJSON_Delete(root); m.poll_at = now_ms() + 30000; return;
    }
    m.server_now = clock->valuedouble;
    m.server_ms = now_ms();
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "configured"))) {
        cJSON_Delete(root); m.configured = false; m.poll_at = now_ms() + 30000; return;
    }
    m.configured = true;
    auto *job = !std::strncmp(wire, "J1:", 3) && !std::strstr(wire, "\\u0000") ? cJSON_Parse(wire + 3) : nullptr;
    const bool valid = buddy_message_decode(job, m.server_now, m.request);
    cJSON_Delete(job); cJSON_Delete(root);
    if (!valid || same(m.request.id, m.ack.id)) {
        if (now_ms() >= m.publish_at) message_publish();
        return;
    }
    if (!m.ack_ok || buddy_audio_busy() || microphone_interacting() || buddy_ai_output_pending()) return;
    // Claim durably BEFORE any speech/photo/action. Reconnects never replay it.
    std::strcpy(m.ack.name, m.home.name); std::strcpy(m.ack.url, m.home.url);
    std::strcpy(m.ack.id, m.request.id); m.ack.phase = 1;
    std::strcpy(m.current_id, m.request.id); m.error[0] = m.reply[0] = m.image[0] = 0; m.state = "processing";
    m.reply_source = "status";
    if (!message_ack(true)) { m.ack_ok = false; m.state = "error"; std::strcpy(m.error, "claim_not_saved"); message_publish(); return; }
    if (!message_publish()) { message_done("status_unreachable", "İstek başlatılamadı; yeniden gönder."); return; }
    if (m.request.mode == BuddyMessageMode::Photo) {
        Request photo; photo.action = Action::Photo; photo.epoch = epoch.load(); photo.created = now_ms(); photo.remote = true;
        auto *result = cJSON_CreateObject(); cJSON_AddBoolToObject(result, "applied", false);
        send_photo(m.home, photo, result, error);
        const char *image = text(result, "image_url");
        message_done(error, error ? "Fotoğraf gönderilemedi; hata bilgisini kontrol et."
            : "Fotoğraf Home Assistant'a yüklendi ve telefon bildirimi kabul edildi.", image ? image : "");
        cJSON_Delete(result);
    } else if (!buddy_messenger_queue(m.request)) message_done("message_busy", "Jamiryo meşgul; yeniden dene.");
    else m.active = true;
    std::memset(&m.request, 0, sizeof(m.request));
}
