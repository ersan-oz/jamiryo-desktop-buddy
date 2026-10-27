# Getting started

## 1. Prepare the development environment

Install [ESP-IDF 6.0.1](https://github.com/espressif/esp-idf/releases/tag/v6.0.1)
and its ESP32-S3 tools using the [official installation guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/get-started/index.html).
Open an ESP-IDF terminal so `IDF_PATH` and its Python environment are available.
Use Python 3.12 for the reference configuration. Git is required for dependency
management. On Windows, a short checkout path helps avoid path-length issues.

Download or clone this repository into a new directory. Install the small host
tool/test dependency set into your chosen Python environment:

```sh
python -m pip install -r requirements-dev.txt
python scripts/build.py
```

The first build downloads managed components and pretrained models from the
Espressif component registry. It does not download people's portrait photos or
call a speech-generation service. `dependencies.lock` is included to retain the
reference versions. Do not change shared models or partitions casually: OTA
checks their exact hashes and layout.

Build output is `.local/build-v2`; validated application packages are in
`.local/artifacts`. Generated audio and `main/assets/personality.txt` are ignored
by Git. You can regenerate them with `python scripts/prepare_assets.py`.

## 2. Install on a new robot over USB

Start with the servo and external 5 V supply disconnected. Power the Atom only
from USB for this installation. Connect the OLED using [the hardware guide](hardware.md).
Do not erase or replace a previously used device's flash without its owner's
backup and a migration plan; NVS, home profiles and face profiles live there.

For a **new** device, run the following from the repository root, replacing
`YOUR_SERIAL_PORT` with its actual serial port:

```sh
idf.py -B .local/build-v2 -p YOUR_SERIAL_PORT flash monitor
```

The normal ESP-IDF `flash` target includes the bootloader, partition table,
initial OTA selection, application and the two shared model partitions. Do not
substitute a standalone application upload for this first installation.
If the board will not enter download mode, follow [M5Stack's board instructions](https://docs.m5stack.com/en/core/AtomS3R-CAM_AI_Chatbot).

Expect hardware/PSRAM, microphone and local startup checks. OLED eyes should
animate if the display is wired correctly. Generated offline tones are short
electronic sounds, not the private prototype's recorded phrases.
Exit the monitor before opening the same serial port in another program.

## 3. Configure voice-only operation

You need a 2.4 GHz Wi-Fi network and your own Gemini API key. Create/manage a
key in [Google AI Studio](https://aistudio.google.com/api-keys). Check the
[current API pricing and quota terms](https://ai.google.dev/gemini-api/docs/pricing)
for your project before use. This project does not provide a shared key.

With the robot still safely USB-powered and the serial monitor closed:

```sh
python scripts/provision.py --port YOUR_SERIAL_PORT
```

Enter the network name and the two hidden secret fields at the local terminal.
The tool sends a bounded configuration over USB and prints only whether the
robot acknowledged saving it. It does not put secrets in command arguments,
source files, an `.env` file or a host log. The firmware stores its configuration
in device NVS; flash encryption is not enabled by these defaults.

This sets the firmware's legacy `free_tier` flag for compatibility. The flag
does **not** enforce Google's billing settings or guarantee zero cost.

The configured native-audio model is `gemini-3.8-live` with the Puck voice.
Availability is account-dependent. If changing models, update the default and
validator in `main/network/gemini_live.cpp`, check Live API compatibility, and
rebuild. Editing a JSON example alone does not bypass the firmware validator.

## 4. Configure a complete home profile from a phone

The existing phone wizard saves a complete home profile: a friendly home name,
Wi-Fi name/password, Gemini API key, Home Assistant base URL and a Home Assistant
long-lived access token. **Home Assistant is required by this wizard's current
profile schema.** Use the voice-only USB path above if you do not use HA.

To open the wizard after voice setup, ask the robot to open home setup, or type
`home-setup` in the USB monitor. A new unconfigured robot also attempts to open
its setup AP after about 30 seconds without a network connection.

The setup Wi-Fi is named `Jamiryo-` plus a device-specific suffix. Its random
password can be displayed deliberately with the USB command `home-setup-info`.
Treat that output as private: do not paste it into an issue or commit it.

Join that network on your phone. Use its captive portal or open
`http://198.18.0.1/` manually. Automatic opening depends on the phone's network
checks and is not guaranteed. The current wizard is in Turkish; the key fields are:

| Wizard label | Meaning |
| --- | --- |
| Ev / profil adı | Home/profile name |
| Wi-Fi adı / SSID | 2.4 GHz network name |
| Wi-Fi şifresi | Network password |
| Gemini API anahtarı | Your home's Gemini key |
| Home Assistant adresi | Base URL, e.g. `http://homeassistant.local:8123` |
| Erişim anahtarı | HA long-lived access token |

Up to four profiles can be stored. A new profile needs its own Gemini key;
leaving an existing profile's secret field blank preserves that home's saved
secret. Selecting a different home selects its credentials as a unit.
See [Home Assistant setup](../home-assistant/README.md).

## 5. Add and calibrate the servo

Disconnect power before rewiring. Use the validated power arrangement from
[hardware.md](hardware.md). First test with the servo unloaded and the head
removed from its horn. Use `servo-center`, then `servo-test` in the USB console
only when the physical setup is appropriate. Fit the horn after centering.
Confirm left/right mounting direction and clearance before enabling automatic
head behavior in setup. The default pulse limits are 1250–1750 µs around 1500 µs;
actual shaft angles vary between servos and mounting arrangements.

## 6. Pair a Windows computer for later Wi-Fi updates

See [OTA](ota.md) for one-time USB pairing. Once paired, ordinary updates use
the home LAN and do not require opening the enclosure. A charged battery or
verified power source is needed throughout an update.

## Troubleshooting

| Symptom | First check |
| --- | --- |
| No OLED | Correct SH1106 module, address 0x3C/0x3D, GPIO38/39 and safe I²C pull-up voltage |
| No voice reply | Your API key/model access, quota, 2.4 GHz Wi-Fi and the robot's power |
| Short resets when the head moves | Servo supply, wiring resistance and peak-current capability |
| Setup page does not open automatically | Connect to the setup AP and enter `http://198.18.0.1/` manually |
| HA control fails | Base URL/token, entity exposure to Assist and exact target name |
| Wi-Fi update cannot find the robot | Power, same LAN, client isolation, then `deploy_wifi.py discover` |

Do not attach raw serial logs, NVS dumps or actual configuration values to a
public issue. Share the error code and a redacted, minimal description instead.
