# Changelog

## 1.2.20-public1 - 2 November 2026

- Automatic camera attention follows localized movement without requiring a face.
  Adaptive contrast thresholds detect moving hands/objects; face matching remains separate.
- Fixed corrections being interrupted when microphone VAD paused the routine.
  Listening/thinking can coexist with motion attention; actual priorities remain enforced.
- Added completion/cancellation and detector diagnostics to LAN OTA status.
- Preserved self-motion/IMU suppression, bounded eased steps, manual controls,
  photos, enrollment, sleep and A/B update behavior.
- Included the 1.2.15-1.2.17 improvements: varied idle blinks/gaze, transient moods,
  smoother head curves, one online startup greeting and direct HA head buttons.
- Updated English usage, architecture, privacy and bilingual HA installation files.

Physical motion tracking was confirmed on the assembled prototype running 1.2.20.
This public fork retains a generalized persona and generated neutral tones; it is
separately build/host tested and is not flashed by the publication workflow.
No credentials, face profiles, room images, logs or private persona are included.
