# Home Assistant integration

## Existing control and photo features

Save a complete home profile using [phone setup](../docs/getting-started.md).
Enter your own HA base URL and long-lived access token. The current client uses
trusted local HTTP; see [the privacy model](../docs/privacy.md). Create/manage a
token in your HA user profile under its security/access-token settings. Never
put a token into this repository, a screenshot or a public issue.

Expose intended entities to Assist and use a supported conversation agent/language
for the firmware's Turkish commands. The robot discovers entity names, rooms and
states; it does not hardcode the prototype owner's light or phone. Controls are
limited to explicit requests and the actions Assist reports as supported.

For photo notifications, register your phone with the official HA Companion app
and grant its notification permission. Choose that phone in Jamiryo setup if
several services exist. The image is uploaded to authenticated local HA media
under `jamiryo`, using a per-device latest filename. Delivery and media access
outside your home depend on your existing HA/private-network configuration.
No new public camera endpoint or router forwarding is required.

## Experimental remote-message dashboard

This section is implemented but still awaiting final device-to-phone acceptance
testing. It uses native HA helpers, a script and dashboard cards; no HACS/custom
component is required. Back up your HA configuration before making your own changes.

### 1. Create three helpers

In HA, open **Settings → Devices & services → Helpers**. Create:

| Type | Entity ID (exact) | Options |
| --- | --- | --- |
| Text | `input_text.jamiryo_message` | Minimum 0, maximum 140, plain text |
| Text | `input_text.jamiryo_inbox` | Minimum 0, maximum 255, plain text |
| Dropdown | `input_select.jamiryo_mode` | `Ask Jamiryo`, `Read aloud at home`, `Send photo` |

Initialize both text helpers to an empty string once. A newly created helper may
initially report `unknown`; do not use that as the message draft. Do not clear
the inbox later while a request is pending. The robot distinguishes helper
existence from its current value when deciding whether the bridge is configured.

### 2. Add the send script

Create a new script, open its YAML editor, and paste `jamiryo_send.yaml` from this
directory. The file is JSON, which is valid YAML. Save it as **Send to Jamiryo**
and ensure its entity ID is exactly `script.jamiryo_send`.

### 3. Create a dedicated dashboard

Create an empty dashboard with URL path **`jamiryo-mesaj`**. In its raw
configuration editor paste `jamiryo_dashboard.yaml`. Its view path intentionally
remains `sohbet` for compatibility with the firmware's notification click target;
the displayed labels are English. Keep normal HA authentication enabled.

Wait for `sensor.jamiryo_messenger` to appear and report a current heartbeat.
The panel treats a heartbeat older than 45 seconds as offline. Enter a short
message, leave the input so HA saves it, choose a mode and press **Send**.
The separate **Send photo** button does not need a message.

### Modes and limits

- **Ask Jamiryo:** text question to the existing Gemini session. The actual
  output transcript is published in the panel and a phone notification is attempted.
- **Read aloud at home:** asks the voice to read the quoted message. Action tools
  are disabled except expression changes. Actual wording/articulation remains
  model output; it is not a deterministic offline TTS engine.
- **Send photo:** direct fresh camera capture/HA upload/phone notification, without
  Gemini. Some explicit Turkish photo-request phrases in ask mode also use this path.

Messages are limited to 140 Unicode characters and the complete compact `J1:`
envelope must fit HA's 255-character text state. Quotes/escaping can make a
nominally short message too large. Requests expire after 120 seconds, with a
small future-clock allowance. A durable last-claimed ID prevents routine replay;
an uncertain action is not automatically retried after restart.

The robot publishes processing state, last reply, error and photo link. The
phone-notification error is separate from a successful panel reply. Ordinary
room speech is not forwarded automatically. A person at home must explicitly
ask, for example, “Telefonuma mesaj gönder: …” to initiate an outgoing message.

HA helpers, recorder history, sensor attributes, media and backups can retain
requests, responses and photos. Review your HA retention policy. Your existing
Tailscale/private HA access can carry this panel remotely; this guide does not
change your router, VPN or HA authentication.

## Host checks

```sh
python scripts/ha_message_bundle.py
python scripts/check_ha_messages.py
```

Run these from the repository root. The template test checks native Jinja value
handling, Unicode/emoji, modes, length rejection, offline/busy checks and pending
request guards without contacting a home or sending any notification.

References: [HA scripts](https://www.home-assistant.io/integrations/script/),
[input text](https://www.home-assistant.io/integrations/input_text/),
[REST API](https://developers.home-assistant.io/docs/api/rest/),
[Companion notifications](https://companion.home-assistant.io/docs/notifications/notifications-basic/).
