"""Generate native HA configuration. No credentials, plugins or external resources."""
import json
from pathlib import Path

MODES = {'Ask Jamiryo': 'ask', 'Read aloud at home': 'say', 'Send photo': 'photo'}


def stop_if(template, explanation):
    return {'if': [{'condition': 'template', 'value_template': template}],
            'then': [{'stop': explanation, 'error': True}]}


SCRIPT = {
    'alias': 'Send to Jamiryo', 'description': 'Send a question, spoken message or single-photo request from a phone.',
    'icon': 'mdi:robot-happy-outline', 'mode': 'single', 'max_exceeded': 'silent',
    'fields': {
        'mode': {'name': 'Action', 'selector': {'select': {'options': ['ask', 'say', 'photo']}}},
        'message': {'name': 'Message', 'selector': {'text': {}}},
    },
    'sequence': [
        {'variables': {
            'send_mode': "{{ mode | default({'Ask Jamiryo':'ask','Read aloud at home':'say','Send photo':'photo'}.get(states('input_select.jamiryo_mode'),'ask')) }}",
            'send_text': "T:{{ message | default(states('input_text.jamiryo_message')) | string | trim }}",
            'request_id': "m{{ utcnow().strftime('%Y%m%d%H%M%S%f') }}",
            'previous': "{{ states('input_text.jamiryo_inbox')[3:] | from_json({}) }}",
        }},
        stop_if("{{ as_timestamp(now()) - (state_attr('sensor.jamiryo_messenger','last_seen') | float(0)) > 45 }}",
                'Jamiryo is offline. Check its power and home Wi-Fi connection.'),
        stop_if("{{ is_state('sensor.jamiryo_messenger','processing') }}", 'Wait for the current request to finish.'),
        stop_if("{{ previous.get('id','') and previous.get('id') != state_attr('sensor.jamiryo_messenger','request_id') and as_timestamp(now()) - (previous.get('at',0) | float(0)) < 120 }}",
                'The previous request has not been received yet; wait briefly.'),
        stop_if("{{ send_mode not in ['ask','say','photo'] or (send_mode != 'photo' and not (send_text[2:] | trim)) }}",
                'Enter a message or select Send photo.'),
        {'variables': {
            'envelope': "J1:{{ {'v':1,'id':request_id | string,'mode':send_mode | string,'text':('' if send_mode == 'photo' else send_text[2:]),'at':as_timestamp(now()) | int} | to_json(ensure_ascii=False) }}",
        }},
        stop_if("{{ envelope | length > 255 }}", 'The message is too long; shorten it and try again.'),
        {'action': 'input_text.set_value', 'target': {'entity_id': 'input_text.jamiryo_inbox'}, 'data': {'value': '{{ envelope }}'}},
    ],
}

STATUS = """{% set t = state_attr('sensor.jamiryo_messenger','last_seen') | float(0) %}
{% set s = states('sensor.jamiryo_messenger') %}
{% if as_timestamp(now()) - t > 45 %}
**Status:** Offline or not ready yet.
{% else %}
**Status:** {{ {'ready':'Connected and ready','processing':'Processing…','done':'Done','error':'Request failed'}.get(s,s) }}
{% endif %}
{% if state_attr('sensor.jamiryo_messenger','error') %}
Error: `{{ state_attr('sensor.jamiryo_messenger','error') }}`
{% endif %}
{% if state_attr('sensor.jamiryo_messenger','notification_error') %}
Phone notification failed: `{{ state_attr('sensor.jamiryo_messenger','notification_error') }}`. The reply is shown below.
{% endif %}"""

DASHBOARD = {
    'title': 'Jamiryo',
    'views': [{'title': 'Chat', 'path': 'sohbet', 'icon': 'mdi:robot-happy-outline', 'cards': [
        {'type': 'markdown', 'content': '# Jamiryo\nTalk to your desktop companion remotely.\n\nAsk a question, read a message aloud at home, or request a fresh camera photo.'},
        {'type': 'entities', 'title': 'Send a message', 'show_header_toggle': False, 'entities': [
            {'entity': 'input_text.jamiryo_message', 'name': 'Your message', 'icon': 'mdi:message-text-outline'},
            {'entity': 'input_select.jamiryo_mode', 'name': 'Action', 'icon': 'mdi:format-list-bulleted'},
            {'entity': 'script.jamiryo_send', 'name': 'Send to Jamiryo', 'action_name': 'Send'},
        ]},
        {'type': 'button', 'name': 'Send photo', 'icon': 'mdi:camera', 'show_state': False,
         'tap_action': {'action': 'perform-action', 'perform_action': 'script.jamiryo_send', 'data': {'mode': 'photo', 'message': ''}}},
        {'type': 'markdown', 'title': 'Connection and request', 'content': STATUS},
        {'type': 'markdown', 'title': 'Last reply', 'content': "<pre style=\"white-space: pre-wrap; overflow-wrap: anywhere;\">{{ (state_attr('sensor.jamiryo_messenger','reply') or 'No reply yet.') | e }}</pre>"},
        {'type': 'markdown', 'content': "{% set image = state_attr('sensor.jamiryo_messenger','image') %}{% if image %}[Open the latest photo]({{ image }}){% endif %}"},
        {'type': 'markdown', 'content': 'Short messages only: up to 140 characters, subject to the 255-character JSON envelope limit. Wait while the robot replies. A person at home can explicitly ask **“Telefonuma mesaj gönder: …”** to send a phone reply. Ordinary room conversations are not forwarded automatically.\n\nPhotos go to the selected HA Companion phone. Your existing private HA access, such as Tailscale, can make this panel available away from home.'},
    ]}],
}


def main():
    root = Path(__file__).resolve().parent.parent / 'home-assistant'
    root.mkdir(exist_ok=True)
    for name, value in [('jamiryo_send.yaml', SCRIPT), ('jamiryo_dashboard.yaml', DASHBOARD)]:
        (root / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print('HA script and dashboard exported (JSON is valid YAML; no secrets).')


if __name__ == '__main__':
    main()
