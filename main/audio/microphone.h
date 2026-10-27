#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_codec_dev.h"

bool microphone_start(esp_codec_dev_handle_t codec);
bool microphone_healthy();
bool microphone_interacting();
bool microphone_request_recording();
bool microphone_request_probe();
bool microphone_take_recording(const int16_t **data, std::size_t *frames);
void microphone_playback_done();
void microphone_recording_cue_done();
