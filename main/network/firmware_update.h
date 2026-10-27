#pragma once
#include "esp_http_server.h"

// Only authenticated setup-AP or paired LAN wrappers may call upload.
esp_err_t buddy_ota_status(httpd_req_t *request);
esp_err_t buddy_ota_upload(httpd_req_t *request, const uint8_t *expected_sha256 = nullptr);
bool buddy_ota_busy();
bool buddy_ota_models_valid();
void buddy_ota_confirm_boot(bool core_ok);
void buddy_ota_tick();
// USB commissioning diagnostics use the same authenticated HTTP upload path.
bool buddy_ota_http_test(const char *csrf, bool install);
