# Direct head buttons

Firmware 1.2.17 and later supports **Left / Center / Right** directly through HA.
The current dashboard places the row immediately below **Request photo**.
No draft message or Gemini call is needed. The requested direction holds for
about 20 seconds, then the saved automatic mode can resume.

For an existing installation, back up your dedicated Jamiryo dashboard and send
script. Replace both with the matching updated files. English uses
`script.jamiryo_send`; [Turkish](tr/README.md) uses `script.jamiryo_gonder`.
Package installations must update their package script rather than create a
second UI script. Helpers and the three ordinary dropdown options stay the same.

For a customized dashboard, insert [this row](jamiryo_head_buttons.yaml) below
its photo button, and still update the send script. The [Turkish row](tr/jamiryo_head_buttons.yaml)
uses the Turkish script ID. Do not replace unrelated home dashboard content.

The wire mode is `head`, with only `left`, `center` or `right` as text. The robot
rejects arbitrary angles and commands older than 15 seconds. Requests are
acknowledged before execution and do not replay after a reconnect. The buttons
check the robot's `head_controls` and `head_available` attributes first.

Test each direction on the assembled robot and verify mounting direction in
robot setup. Confirmation means the local command was queued, not that a shaft
encoder measured the angle. Keep the rotating area clear.
