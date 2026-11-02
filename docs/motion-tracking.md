# Local motion attention — 1.2.20

Automatic head attention now uses localized camera movement rather than requiring
a detected face. Moving a hand or object in view can attract Jamiryo's head even
when no face is visible. Saved face profiles and occasional name greetings still
use the existing, separate face recognition flow.

The camera worker compares a 32 × 24 luminance grid with a 200 ms minimum pause
between captures (actual cadence also includes capture and recognition work) while the
robot is awake and stationary and its speaker is quiet. Listening/thinking do not
prevent motion attention. It compensates for a global exposure shift,
adapts the pixel threshold to residual noise, rejects small disconnected noise
and broad scene changes, and requires a localized
region to move in two consecutive intervals. Comparable movement at opposite
edges is treated as ambiguous. These grids stay in RAM; the tracker itself makes
no cloud requests and records no images.

Eyes lead each head correction by 180 ms. Corrections outside the center deadband
use 35–100 µs of servo pulse width, eased over 850 ms and clamped to the existing 1250–1750 µs
range. The mounting direction setting is retained. After movement stops, the
controller holds its last position rather than repeatedly returning to center.
Normal idle behavior may resume after eight seconds without a correction.

1.2.20 fixes a live integration failure: raw microphone VAD (including room or
servo noise) used to pause the routine's Track phase and freeze each correction
mid-ramp. Hold and Track now both keep an attention window while listening;
the routine no longer freezes a correction owned by motion attention. Playback
pauses new samples but allows an already started 850 ms correction to finish.
Manual commands, photos, sleep, enrollment and updates still cancel it. An
integration regression simulates microphone activity halfway through a real eased
HeadMotion ramp and requires the target pulse to be reached.

Manual direction commands (including Home Assistant buttons), object searches,
photos, enrollment, sleep and updates take priority. Frames are ignored while the
head is moving and for its settling interval. The comparison baseline resets after
these interruptions, a manual-command generation change, a long frame gap or
physical movement detected by the IMU. Routine camera activity does not reset
the inactivity/sleep timer. The former automatic conversation-start face sweep is
no longer armed; movement supplies the default attention target instead.

This is approximate motion attention, not object identification or continuous
tracking during a turn. A moving television image, curtain or other prominent
movement can attract attention. A still person or object does not provide a new
motion target. Physical shaft direction and accuracy still require a live test.

## Verification

The detector's constexpr tests exercise left/right movement, uniform exposure
changes, noise, a single appearance followed by stillness, duplicate timestamps,
broad changes, resets and long gaps. Controller tests cover eye lead, deadband,
single-use observations, reverse mounting, bounds, cancellation and holding after
target loss. Routine tests ensure a conversation pause does not recenter the head.

The LAN OTA server's `/status` endpoint (port 3232) exposes `motion_tracking` with detector/controller self-tests,
sampling state, sleep stage, pause reason, processed frames, candidate/confirmed
targets, adaptive threshold, changed/region cells, frame gap, accepted observations,
requested corrections, rejected broad changes and baseline resets. The low-contrast
regression test covers a moving edge that the original fixed threshold rejected.
`completed` and `canceled` distinguish full PWM ramps from interrupted requests.
Counters verify software decisions;
they do not measure shaft position.

For a live test, first speak to Jamiryo to wake it if it has been idle. Leave it stationary
and quiet after the reply, then slowly move a hand or
object left and right within camera view at about 30–60 cm. Verify that confirmed
targets and corrections increase and that the head turns toward the movement.
After the object stops, verify it does not keep correcting from its own turn.
