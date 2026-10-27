# Validation, limitations and roadmap

Public snapshot: **1.2.9-public1**, prepared 27 October 2026.

## What the labels mean

“Implemented” means there is an actual code path. “Prototype tested” describes
earlier observations on the assembled robot. Neither proves that a new public
build, different breakout module, different supply or changed cloud account has
passed a complete hardware acceptance test.

| Area | Status |
| --- | --- |
| OLED blinks, emotions, pickup and photo effects | Implemented; previously confirmed on the prototype |
| Gemini voice, memory and HA controls/photo notifications | Implemented; previously exercised on the prototype |
| Bounded head routines, conversation gaze and object search | Implemented; approximate movement and vision, requiring mounting calibration |
| Paired A/B LAN OTA | Implemented; prior prototype updates confirmed; public fork is not deployed by this publication task |
| HA remote message/read-aloud/photo dashboard | Experimental. Host protocol/template tests exist; final device-to-phone acceptance is pending |
| Public source-only build and privacy checks | See the local validation record below and the actual GitHub Actions results |
| Battery level/percentage | Not implemented |
| Exact local “Jamiryo” wake word / deep low-power standby | Not implemented |
| Capacitive touch, mobile base, docking | Not implemented |
| Printable enclosure STL | Head, cover and base supplied; see [mesh and print notes](../hardware/README.md) |
| Editable enclosure CAD / verified slicer profile | Not supplied |

## Public validation record

Local validation completed on 27 October 2026:

- Clean-source ESP-IDF 6.0.1 build for ESP32-S3: **PASS**. A short Windows build
  directory was used to avoid Windows toolchain path-length limits.
- OTA package validation: **PASS** for image checksum/SHA-256, damaged image
  rejection, both app-slot sizes, preserved NVS offsets and shared model hashes.
- Public-file audit: **PASS**. Matched secret values are never written to its output.
- Host release/privacy/document-link/STL tests: **10 passed**.
- HA native-Jinja protocol/template groups: **7 passed**, with no network requests.

The application is 2,409,840 bytes and fits both `0x2E0000` application slots.
The machine-readable record is [validation.json](validation.json). This is a
build/host validation record, not a new device deployment. GitHub Actions is
provided separately; consult the actual run results rather than treating the
workflow definition as proof of a successful remote run.

No device flash, new physical servo test, private room photo or actual home
action was performed to prepare this public repository.

## Practical limits

The robot needs internet/cloud service for conversation and visual interpretation.
Local reactions continue without it. Reconnection can lose immediate conversation
context; explicit stored notes are a separate limited memory mechanism.

The current face model is experimental and can miss or confuse people. Object
search chooses a stopped head view, not an accurate angle or a 3D object coordinate.
There is no wheel drive or mechanical feedback. Sleep eyes do not establish an
energy budget; battery runtime and supply peak capability require measurements.

## Potential next work

- Finish HA remote-message, read-aloud, repeated-request and photo delivery tests.
- Measure power/runtime and design validated battery-voltage sensing.
- Train and evaluate a consented local wake-word model before claiming it works.
- Improve English runtime/UI coverage and configurable character/voice settings.
- Add dimensioned parametric CAD, assembly drawings and verified slicer profiles.
- Add repeatable consented vision and acoustic acceptance procedures.
