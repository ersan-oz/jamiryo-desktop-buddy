# Using Jamiryo

## Daily use

Switch on the completed robot using its verified normal power arrangement.
Allow it to connect to Wi-Fi and Gemini before trying voice conversation. OLED
eyes and local movement reactions are independent of online AI availability.
Automatic head motion depends on its saved setting, mounting direction and
current activity. Keep fingers, cables and objects clear of the rotating head.

The default persona is Turkish, curious and mildly grumpy. Replies are short.
These are example requests; they are not guaranteed fixed keywords:

| Try saying | Intended action |
| --- | --- |
| “Merhaba Jamiryo, nasılsın?” | Start a short conversation |
| “Sağa döner misin?” / “Sola döner misin?” | Request a bounded right/left head target |
| “Önüne bak.” | Return toward the center |
| “Bir etrafı kontrol etsene.” | Look-around sweep and settle |
| “Ne görüyorsun?” | Obtain a current image for a visual answer |
| “Bardak nerede, ona bakar mısın?” | Search stopped head views and aim approximately toward the object |
| “Fotoğraf çekip telefonuma gönder.” | Fresh photo plus OLED shutter effect and HA phone notification |
| “Salon lambasını kapat.” | Explicit HA Assist action, if that actual target is exposed and resolvable |
| “Ev kurulumunu aç.” | Open the setup Wi-Fi through the available tool |

Commands are interpreted by Gemini and validated locally. The robot should
explain unavailable/ambiguous results rather than claim success from a spoken
answer alone. Confirm the physical result of home controls and motor direction.

## Expressions and physical interaction

Happy, angry, shy and surprised expressions accompany suitable replies. Eyes
blink and change gaze during idle time. Being picked up/moved can show local
`@ @` eyes for about three seconds. Shake, tilt and pickup are approximate IMU
classifications; sensor thresholds cannot measure exact lift height.

After a period without meaningful interaction, eyes become drowsy, close and
show `Zzz`. Speech or meaningful motion can wake this visual behavior. It is
not battery-saving deep sleep and does not require an exact wake word.

## Memory and faces

Explicitly ask Jamiryo to remember or forget a simple fact/preference. Do not
use it as the only copy of important information. Notes are bounded persistent
context, not model training. A new cloud session may lose immediate conversation
history even while explicit notes remain.

Only enroll consenting people. Follow the robot's guided face-enrollment tool
or the corresponding supported USB commands with a user-chosen label. Keep the
face front-lit and visible. A familiar label can trigger an occasional greeting;
false matches and missed faces remain possible. Do not rely on it for security.

## Home Assistant phone features

Install the official HA Companion app, register it with your home, and enable
the intended notification permissions. Select its notification service in the
robot's setup page if more than one phone is available. The photo path uploads
to authenticated HA media. Notification acceptance does not guarantee immediate
phone delivery; remote image loading also needs working HA network access.

An optional experimental HA dashboard can send a text question, read-aloud
message or photo request from outside the home through your existing private
HA access such as Tailscale. Set it up with [the HA guide](../home-assistant/README.md).
It has not yet completed final device-to-phone acceptance testing.

## Moving to another home

Open setup, add or select a home profile, and save that home's Wi-Fi, Gemini
and HA details. Each profile has independent credentials and phone selection.
Do not give a new owner your pairing registry, flash backup or old personal
profiles. A complete ownership-transfer/reset workflow is not provided here;
plan that separately and revoke credentials that should no longer be usable.

## Charging and updating

Use the charging input of the actual compatible battery board. Unless that
board explicitly supports load sharing, turn the robot off while charging.
The firmware has no battery-percentage reading or low-battery alert in this
snapshot. A charger's full-charge LED is not a state-of-charge sensor for the ESP32.

Keep adequate stable power during [Wi-Fi updates](ota.md). An accepted upload
still needs boot/image verification. Preserve the last known-good package.
