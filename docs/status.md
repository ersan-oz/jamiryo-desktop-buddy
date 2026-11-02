# Validation, limitations and roadmap

Public snapshot: **1.2.20-public1**, prepared 2 November 2026.

## What the labels mean

“Implemented” means there is an actual code path. “Prototype tested” describes
earlier observations on the assembled robot. Neither proves that a new public
build, different breakout module, different supply or changed cloud account has
passed a complete hardware acceptance test.

| Area | Status |
| --- | --- |
| OLED blinks, emotions, pickup and photo effects | Implemented; previously confirmed on the prototype |
| Gemini voice, memory and HA controls/photo notifications | Implemented; previously exercised on the prototype |
| Bounded head routines, motion attention and object search | Implemented; approximate movement and vision, requiring mounting calibration |
| Paired A/B LAN OTA | Implemented; prior prototype updates confirmed; public fork is not deployed by this publication task |
| HA remote message/read-aloud/photo dashboard | Prototype series 1.2.11-1.2.14 exercised question/reply delivery, audible read-aloud and photos; public fork is separately build/host tested |
| Public source-only build and privacy checks | See the local validation record below and the actual GitHub Actions results |
| Battery level/percentage | Not implemented |
| Exact local “Jamiryo” wake word / deep low-power standby | Not implemented |
| Capacitive touch, mobile base, docking | Not implemented |
| Printable enclosure STL | Head, cover and base supplied; see [mesh and print notes](../hardware/README.md) |
| Editable enclosure CAD / verified slicer profile | Not supplied |

## Public validation record

Local validation completed on 2 November 2026:

- Clean-source ESP-IDF 6.0.1 build for ESP32-S3: **PASS**. A short Windows build
  directory was used to avoid Windows toolchain path-length limits.
- OTA package validation: **PASS** for image checksum/SHA-256, damaged image
  rejection, both app-slot sizes, preserved NVS offsets and shared model hashes.
- Public-file audit: **PASS**. Matched secret values are never written to its output.
- Host release/privacy/document-link/STL tests: **14 passed**.
- HA native-Jinja protocol/template groups: **9 passed**, with no network requests.

The application is 2,422,896 bytes and fits both `0x2E0000` application slots.
The machine-readable record is [validation.json](validation.json). This is a
build/host validation record, not a new device deployment. GitHub Actions is
provided separately; consult the actual run results rather than treating the
workflow definition as proof of a successful remote run.

No public-fork device flash, new physical servo test or actual home action was performed
for this source refresh. Earlier prototype acceptance results are recorded separately;
no private room photo, home screenshot, credential or user message is published.

## Motion-attention acceptance

Prototype firmware 1.2.20 was uploaded by paired Wi-Fi OTA, confirmed its expected
image and boot, and the user confirmed physical tracking of movement. Live
diagnostics showed completed corrections without mid-ramp cancellation during
the test. The regression also simulates microphone activity during an eased
turn. This observation belongs to the assembled prototype, not a new flash of
the public fork. No private captures, labels or device identifiers are included.

The current public source includes separate local motion attention and face
recognition, a one-time online greeting, varied idle behavior and direct HA
head controls. The former automatic conversation-start face search is no longer
armed. See [motion tracking](motion-tracking.md) for gates and limitations.

## Practical limits

The robot needs internet/cloud service for conversation and visual interpretation.
Local reactions continue without it. Reconnection can lose immediate conversation
context; explicit stored notes are a separate limited memory mechanism.

Motion attention can follow a moving hand/object without detecting a face,
but a TV, curtain or other movement can also attract it. The tracker itself
sends no cloud image. A still target supplies no new movement. The current
face model is experimental and can miss or confuse people. Object
search chooses a stopped head view, not an accurate angle or a 3D object coordinate.
There is no wheel drive or mechanical feedback. Sleep eyes do not establish an
energy budget; battery runtime and supply peak capability require measurements.

## Potential next work

- Exercise the optional native package on a separate clean HA test server; current checks validate artifact consistency, not a fresh server installation.
- Measure power/runtime and design validated battery-voltage sensing.
- Train and evaluate a consented local wake-word model before claiming it works.
- Improve English runtime/UI coverage and configurable character/voice settings.
- Add dimensioned parametric CAD, assembly drawings and verified slicer profiles.
- Add repeatable consented vision and acoustic acceptance procedures.

## New-home installation artifacts

The illustrated guide offers a UI method and an optional native package. English
and Turkish sets use matching script IDs and mode labels. Host checks exercise
those mappings and the allowlisted portable bundle. Saving a robot profile does
not create HA helpers or dashboards. Package loading/restart and actual outside-
home networking still need validation in the recipient installation.
