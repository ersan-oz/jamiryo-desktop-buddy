# Privacy and security model

## Published source

The public export is created from selected source files with a new Git history.
It excludes runtime state, local credentials, device pairing registries, flash
images/backups, logs, recordings, camera captures and enrolled face data. The
product photo has its PNG metadata removed. Real-person portrait fixtures are
not embedded or fetched to build the public version.

Generated offline tones are original mathematical waveforms. The persona is
generalized; an owner's private conversation, household configuration and
personal character requests are not bundled as an example installation.

## Runtime data

| Data | Where it goes / persists |
| --- | --- |
| Wi-Fi, Gemini and HA credentials | Device NVS. This configuration does not enable flash encryption or secure boot. |
| Paired OTA key | Device NVS and a current-user DPAPI-encrypted file on the Windows developer computer. |
| Microphone audio | Sent to Google while the Live session is listening. The app does not continuously archive it to a local recording file. |
| Selected visual frames | Some explicit visual requests and enabled scene observations send images to Google. Local camera buffers are temporary RAM. |
| Face feature vectors | Local NVS. Enrollment is opt-in; there are no pre-enrolled people in the repository. |
| Face labels and explicit notes | Stored locally; selected labels/notes can become cloud conversation context. |
| Requested phone photo | Uploaded to HA's `media/local/jamiryo` area using a device-specific latest filename, then linked by a phone notification. HA and phone caches/backups may retain it. |
| Remote panel requests/replies | HA helpers, sensor attributes, history/backups and notification services can retain them. AI questions/read-aloud text also go to Google. |

No camera streaming endpoint is provided. This does not mean the camera never
contacts a cloud service: enabled visual features send selected images. Disable
the camera/observations when not desired. A physical shutter is an optional
mechanical improvement, not something provided by this repository.

Google and Home Assistant retention/settings are outside the firmware's RAM
cleanup. Consult the [Gemini API terms](https://ai.google.dev/gemini-api/terms),
your project configuration and your HA recorder/media/backup settings.

## Home and network access

Each home profile has its own Wi-Fi connection, Gemini key, HA base URL/token and
phone selection. New homes need their own credentials. Editing a saved home
with a blank secret preserves that home's secret; it is not a request to erase it.

The current HA client accepts a trusted local **HTTP** base URL. Bearer tokens
and request bodies on that path are not transport-encrypted. Use a trusted home
LAN and a dedicated HA account with appropriate permissions; this is a prototype
integration, not a hardened security boundary. Avoid using it for locks, alarms
or other safety-critical actions. Expose intended targets to HA Assist and
verify each actual result.

The setup AP uses a generated password and does not route/NAT into the home LAN.
Its local web interface uses origin/CSRF checks, but is still a local HTTP setup
interface. `home-setup-info` deliberately prints its password on the physical
USB console. Keep that output private.

Local face recognition must not be used as authentication. Do not enroll people
without their agreement. Remote messaging requires authenticated HA access;
ordinary room conversations are not automatically forwarded to a phone.

## Before sharing your own fork

Run `python scripts/check_public_release.py` and inspect its output. The checker
reports file/line/category rather than a matched secret. It catches common
credential forms and forbidden artifacts; it is not proof that arbitrary text
contains no private information. Manually review the final diff and screenshots.

Publish a clean source archive or repository, not your development state folder.
Do not upload `.local`, `.env`, DPAPI files, NVS/flash dumps, face profiles or raw
logs. If a real key has already been published, revoke/rotate it first: deleting
a visible file does not remove copies in Git history, forks or caches. See
[GitHub's sensitive-data guidance](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository).
