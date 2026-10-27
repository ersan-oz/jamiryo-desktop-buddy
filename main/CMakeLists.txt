idf_component_register(
    SRCS "main.cpp" "buddy.cpp" "audio/microphone.cpp" "network/gemini_live.cpp" "network/object_search.cpp" "network/home_assistant.cpp" "network/home_messenger.cpp" "network/captive_dns.cpp" "network/firmware_update.cpp" "network/remote_update.cpp" "sensors/camera.cpp" "sensors/faces.cpp" "display/eyes.cpp" "memory/user_memory.cpp" "behavior/idle_sleep.cpp" "servo/head_servo.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_app_format esp_hw_support esp_system esp_psram heap
                  app_update spi_flash esp_timer freertos log esp_driver_i2c esp_driver_i2s esp_driver_mcpwm
                  esp_driver_usb_serial_jtag bmi270 esp_codec_dev esp_websocket_client
                  esp_wifi esp_event esp_netif nvs_flash cjson mbedtls esp_ringbuf esp32-camera esp_driver_gpio human_face_recognition esp_http_client esp_http_server
    EMBED_FILES "assets/hello.pcm" "assets/move.pcm" "assets/shake.pcm"
                "assets/tilt.pcm" "assets/settled.pcm" "assets/heard.pcm" "assets/listen.pcm"
    EMBED_TXTFILES "assets/personality.txt" "assets/home_setup.html"
)
# SCCB is only used during camera setup, before IMU/audio claim the two I2C buses.
# The pinned driver's teardown releases that temporary bus without stopping DMA.
idf_component_get_property(buddy_camera_dir espressif__esp32-camera COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE "${buddy_camera_dir}/driver/private_include")
# Audio resampling and pitch processing must run faster than realtime even
# when the surrounding IDF build uses its default debug optimization.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u jamiryo_ota_contract")
