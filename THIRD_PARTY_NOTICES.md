# Third-party notices

The project's MIT license covers its original source, documentation and supplied
product image. It does not relicense third-party libraries, hardware designs,
trademarks, cloud services or pretrained model assets.

Dependencies are fetched by ESP-IDF Component Manager rather than vendored here.
`dependencies.lock` records the reference resolution and content hashes. Consult
the fetched component's license files and upstream notices before redistributing
a firmware binary or modified dependency.

| Component / service | Reference | Upstream |
| --- | --- | --- |
| ESP-IDF | 6.0.1 | [Espressif ESP-IDF](https://github.com/espressif/esp-idf) |
| BMI270 wrapper | 1.1.0 | [Espressif BSP](https://github.com/espressif/esp-bsp) |
| ES8311 codec abstraction | 1.6.2 | [esp_codec_dev](https://github.com/espressif/esp-adf/tree/master/components/esp_codec_dev) |
| WebSocket client | 1.5.0 | [esp-protocols](https://github.com/espressif/esp-protocols) |
| Camera driver | 2.1.8 | [esp32-camera](https://github.com/espressif/esp32-camera) |
| cJSON wrapper/library | Lock-file version | [idf-extra-components](https://github.com/espressif/idf-extra-components) |
| Human face recognition / detection | 0.3.2 / 0.4.2 | [ESP-DL](https://github.com/espressif/esp-dl) |
| ESP-DL and pretrained model packages | 3.3.13 reference resolution | [ESP-DL components and model notices](https://github.com/espressif/esp-dl) |
| Jinja2 / pyserial (host tools) | requirements-dev.txt | [Jinja](https://github.com/pallets/jinja), [pyserial](https://github.com/pyserial/pyserial) |

Gemini, Google, Home Assistant, ESP32, M5Stack and other names belong to their
respective owners. This project is independent and does not imply endorsement.
Google API usage and model access follow the [Gemini API terms](https://ai.google.dev/gemini-api/terms).
The firmware uses its own generic character rather than distributing an actor's
voice recording or a fictional character asset.
