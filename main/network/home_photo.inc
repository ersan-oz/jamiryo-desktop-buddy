// Included inside home_assistant.cpp's private namespace: credentials stay local.
struct PhoneRecord { char name[48]{}, url[192]{}, service[buddy_notify::name_size]{}; };
buddy_notify::Targets phone_cache{};
PhoneRecord phone_cache_home{};
unsigned phone_cache_epoch = 0;
int64_t phone_cache_at = 0;
bool phones_loading = false;
char phones_error[48]{};

bool phone_slot(const Profile &profile, char (&key)[16])
{
    bool found = false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    for (unsigned i = 0; i < database->count; ++i)
        if (same(profile.name, database->profiles[i].name) && same(profile.url, database->profiles[i].url)) {
            std::snprintf(key, sizeof(key), "phone%u", i); found = true; break;
        }
    xSemaphoreGive(mutex); return found;
}
bool valid_phone_record(const PhoneRecord &record, const Profile &profile)
{
    return std::memchr(record.name, 0, sizeof(record.name)) && std::memchr(record.url, 0, sizeof(record.url))
        && std::memchr(record.service, 0, sizeof(record.service)) && same(record.name, profile.name)
        && same(record.url, profile.url) && buddy_notify::valid_target(record.service);
}
bool phone_record(const Profile &profile, PhoneRecord &record, const char *save = nullptr)
{
    char key[16]; nvs_handle_t handle;
    if (!phone_slot(profile, key) || (save && !buddy_notify::valid_target(save))) return false;
    if (nvs_open_from_partition(partition, "phone_targets", save ? NVS_READWRITE : NVS_READONLY, &handle) != ESP_OK) return false;
    bool ok;
    if (save) {
        record = PhoneRecord{}; std::strcpy(record.name, profile.name); std::strcpy(record.url, profile.url); std::strcpy(record.service, save);
        ok = nvs_set_blob(handle, key, &record, sizeof(record)) == ESP_OK && nvs_commit(handle) == ESP_OK;
    } else {
        size_t size = sizeof(record);
        ok = nvs_get_blob(handle, key, &record, &size) == ESP_OK && size == sizeof(record) && valid_phone_record(record, profile);
        if (!ok) record = PhoneRecord{};
    }
    nvs_close(handle); return ok;
}
bool contains_phone(const buddy_notify::Targets &targets, const char *service)
{
    for (unsigned i = 0; i < targets.count; ++i) if (same(targets.names[i], service)) return true;
    return false;
}
esp_err_t phone_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0)
        static_cast<buddy_notify::Scanner *>(event->user_data)->feed(static_cast<const char *>(event->data), event->data_len);
    return ESP_OK;
}
bool discover_phones(const Profile &profile, buddy_notify::Targets &targets, const char *&error)
{
    auto *scanner = static_cast<buddy_notify::Scanner *>(heap_caps_malloc(sizeof(buddy_notify::Scanner), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scanner) { error = "out_of_memory"; return false; }
    new (scanner) buddy_notify::Scanner;
    char url[256]; std::snprintf(url, sizeof(url), "%s/api/services", profile.url);
    esp_http_client_config_t settings{}; settings.url = url; settings.timeout_ms = 5000;
    settings.disable_auto_redirect = true; settings.buffer_size = 512; settings.buffer_size_tx = 1536;
    settings.event_handler = phone_event; settings.user_data = scanner;
    auto client = esp_http_client_init(&settings);
    if (!client) error = "out_of_memory";
    else {
        char authorization[1040]; std::snprintf(authorization, sizeof(authorization), "Bearer %s", profile.token);
        const bool configured = esp_http_client_set_header(client, "Authorization", authorization) == ESP_OK
            && esp_http_client_set_header(client, "Accept", "application/json") == ESP_OK;
        std::memset(authorization, 0, sizeof(authorization));
        const auto outcome = configured ? esp_http_client_perform(client) : ESP_FAIL;
        const int status = esp_http_client_get_status_code(client);
        if (status == 401 || status == 403) error = "unauthorized";
        else if (outcome != ESP_OK) error = "connection_failed";
        else if (status != 200) error = "http_error";
        else if (!scanner->complete()) error = "invalid_service_catalog";
        else targets = scanner->result;
        esp_http_client_cleanup(client);
    }
    heap_caps_free(scanner); return !error;
}
void refresh_phones(const Profile &profile, const Request &request, const char *&error)
{
    auto *targets = static_cast<buddy_notify::Targets *>(heap_caps_calloc(1, sizeof(buddy_notify::Targets), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!targets) error = "out_of_memory";
    else discover_phones(profile, *targets, error);
    xSemaphoreTake(mutex, portMAX_DELAY);
    phones_loading = false;
    if (request.epoch == epoch.load()) {
        phone_cache = targets ? *targets : buddy_notify::Targets{};
        std::strcpy(phone_cache_home.name, profile.name); std::strcpy(phone_cache_home.url, profile.url);
        phone_cache_epoch = request.epoch; phone_cache_at = now_ms();
        std::snprintf(phones_error, sizeof(phones_error), "%s", error ? error : "");
        ESP_LOGI(tag, "HOME_PHONES: count=%u error=%s", phone_cache.count, error ? error : "none");
        for (unsigned i = 0; i < phone_cache.count; ++i) ESP_LOGI(tag, "HOME_PHONE_TARGET: %s", phone_cache.names[i]);
    }
    xSemaphoreGive(mutex); heap_caps_free(targets);
}
bool save_cached_phone(const char *service)
{
    if (!buddy_notify::valid_target(service)) return false;
    Profile profile; PhoneRecord record;
    bool ok = active_profile(profile);
    if (ok) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        ok = phone_cache_epoch == epoch.load() && now_ms() - phone_cache_at < 60000 && !phones_error[0]
            && same(profile.name, phone_cache_home.name) && same(profile.url, phone_cache_home.url) && contains_phone(phone_cache, service);
        xSemaphoreGive(mutex);
        if (ok) ok = phone_record(profile, record, service);
    }
    std::memset(&profile, 0, sizeof(profile)); return ok;
}
bool photo_current(const Request &request)
{
    return (request.remote || request.epoch == epoch.load()) && !restart_at.load() && buddy_camera_enabled();
}
void send_photo(const Profile &profile, const Request &request, cJSON *result, const char *&error)
{
    if (!buddy_camera_enabled()) { error = "camera_disabled"; return; }
    PhoneRecord phone;
    auto *targets = static_cast<buddy_notify::Targets *>(heap_caps_calloc(1, sizeof(buddy_notify::Targets), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!targets) { error = "out_of_memory"; return; }
    bool ok = discover_phones(profile, *targets, error);
    if (ok && !phone_record(profile, phone)) {
        if (targets->count == 1) ok = phone_record(profile, phone, targets->names[0]);
        else { error = targets->count ? "choose_phone_in_setup" : "phone_not_registered"; ok = false; }
        if (!ok && !error) error = "phone_save_failed";
    }
    if (ok && !contains_phone(*targets, phone.service)) { ok = false; error = "selected_phone_unavailable"; }
    heap_caps_free(targets);
    if (!ok) return;
    if (!photo_current(request)) { error = "request_cancelled"; return; }
    BuddyVisionFrame frame;
    if (!buddy_camera_take_photo(frame)) { error = "camera_unavailable"; return; }
    const size_t bytes = frame.bytes;
    const bool valid = bytes >= 4 && bytes <= 48 * 1024 && frame.jpeg[0] == 0xff && frame.jpeg[1] == 0xd8
        && frame.jpeg[bytes - 2] == 0xff && frame.jpeg[bytes - 1] == 0xd9 && now_ms() - frame.captured_ms < 6000;
    char filename[40], expected[128], boundary[48], header[512], footer[80], content_type[100];
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    std::snprintf(filename, sizeof(filename), "latest-%02X%02X%02X.jpg", mac[3], mac[4], mac[5]);
    std::snprintf(expected, sizeof(expected), "media-source://media_source/local/jamiryo/%s", filename);
    std::snprintf(boundary, sizeof(boundary), "Jamiryo%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    const int head_size = std::snprintf(header, sizeof(header), "--%s\r\nContent-Disposition: form-data; name=\"media_content_id\"\r\n\r\nmedia-source://media_source/local/jamiryo\r\n--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: image/jpeg\r\n\r\n", boundary, boundary, filename);
    const int tail_size = std::snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);
    std::snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    const size_t size = head_size + bytes + tail_size;
    char *wire = valid && head_size > 0 && head_size < int(sizeof(header)) && tail_size > 0 && tail_size < int(sizeof(footer))
        ? static_cast<char *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr;
    if (wire) {
        std::memcpy(wire, header, head_size); std::memcpy(wire + head_size, frame.jpeg, bytes); std::memcpy(wire + head_size + bytes, footer, tail_size);
    }
    std::memset(frame.jpeg, 0, bytes); heap_caps_free(frame.jpeg);
    if (!wire) { error = valid ? "out_of_memory" : "invalid_photo"; return; }
    cJSON *uploaded = photo_current(request) ? http(profile, "/api/media_source/local_source/upload", wire, error, content_type, size) : nullptr;
    std::memset(wire, 0, size); heap_caps_free(wire);
    if (!uploaded) { if (!error) error = "request_cancelled"; return; }
    ok = same(text(uploaded, "media_content_id"), expected); cJSON_Delete(uploaded);
    if (!ok) { error = "invalid_upload_response"; return; }
    if (!photo_current(request)) { error = "request_cancelled"; return; }
    char image[128], path[160];
    std::snprintf(image, sizeof(image), "/media/local/jamiryo/%s?v=%08lx", filename, (unsigned long)esp_random());
    std::snprintf(path, sizeof(path), "/api/services/notify/%s", phone.service);
    auto *payload = cJSON_CreateObject(); cJSON_AddStringToObject(payload, "title", "Jamiryo");
    cJSON_AddStringToObject(payload, "message", "Fotoğrafı çektim, ellam.");
    auto *data = cJSON_AddObjectToObject(payload, "data"); cJSON_AddStringToObject(data, "image", image);
    cJSON_AddStringToObject(data, "clickAction", image); cJSON_AddStringToObject(data, "tag", "jamiryo_photo");
    cJSON_AddStringToObject(data, "channel", "Jamiryo fotoğrafları");
    wire = cJSON_PrintUnformatted(payload); cJSON_Delete(payload);
    cJSON *reply = wire ? http(profile, path, wire, error) : nullptr; cJSON_free(wire);
    if (!reply) { if (!error) error = "out_of_memory"; return; }
    if (!cJSON_IsArray(reply)) error = "invalid_notify_response";
    else {
        ++photos_sent;
        cJSON_AddStringToObject(result, "image_url", image);
        cJSON_ReplaceItemInObject(result, "applied", cJSON_CreateBool(true));
        cJSON_AddBoolToObject(result, "notification_requested", true); cJSON_AddNumberToObject(result, "image_bytes", bytes);
        cJSON_AddStringToObject(result, "message", "Home Assistant accepted the photo notification. Phone delivery/display has not been independently confirmed.");
        ESP_LOGI(tag, "HOME_PHOTO: uploaded=%u notification_accepted=1 (one latest authenticated media file)", unsigned(bytes));
    }
    cJSON_Delete(reply);
}
bool notification_test()
{
    bool ok = buddy_notify::valid_target("mobile_app_sm_g780g") && !buddy_notify::valid_target("mobile_app_")
        && !buddy_notify::valid_target("mobile_app_x/../shell") && !buddy_notify::valid_target("notify.mobile_app_phone");
    constexpr char fixture[] = "[{\"domain\":\"other\",\"services\":{\"mobile_app_wrong\":{}}},{\"services\":{\"mobile_app_sm_g780g\":{\"description\":\"quoted \\\"brace } text\",\"fields\":{\"mobile_app_nested\":{}}},\"send_message\":{}},\"domain\":\"notify\"}]";
    auto *scanner = static_cast<buddy_notify::Scanner *>(heap_caps_malloc(sizeof(buddy_notify::Scanner), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scanner) return false;
    for (size_t chunk : {size_t(1), size_t(7), size_t(511)}) {
        new (scanner) buddy_notify::Scanner;
        for (size_t i = 0; i < sizeof(fixture) - 1; i += chunk) scanner->feed(fixture + i, std::min(chunk, sizeof(fixture) - 1 - i));
        ok &= scanner->complete() && scanner->result.count == 1 && same(scanner->result.names[0], "mobile_app_sm_g780g");
    }
    new (scanner) buddy_notify::Scanner; scanner->feed(fixture, sizeof(fixture) - 2); ok &= !scanner->complete();
    heap_caps_free(scanner);
    // Startup runs on IDF's small main stack; keep the profile fixture in PSRAM.
    auto *p = static_cast<Profile *>(heap_caps_calloc(1, sizeof(Profile), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!p) return false;
    std::strcpy(p->name, "Ev"); std::strcpy(p->url, "http://10.0.0.10:8123");
    PhoneRecord record; std::strcpy(record.name, p->name); std::strcpy(record.url, p->url); std::strcpy(record.service, "mobile_app_phone");
    ok &= valid_phone_record(record, *p); std::strcpy(p->url, "http://10.0.0.1:8123"); ok &= !valid_phone_record(record, *p);
    std::memset(record.service, 'a', sizeof(record.service)); ok &= !valid_phone_record(record, *p);
    heap_caps_free(p);
    return ok;
}
