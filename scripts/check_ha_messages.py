"""Exercise the generated HA script with native Jinja values; no network access."""
import json
from datetime import datetime, timezone
from pathlib import Path
from jinja2 import StrictUndefined
from jinja2.nativetypes import NativeEnvironment
from ha_message_bundle import SCRIPT

NOW = datetime(2026, 10, 26, 12, 0, tzinfo=timezone.utc)


def run(message='', mode='ask', previous='', ack='', age=0, state='ready'):
    values = {'input_text.jamiryo_message': message, 'input_select.jamiryo_mode': 'Ask Jamiryo',
              'input_text.jamiryo_inbox': previous, 'sensor.jamiryo_messenger': state}
    env = NativeEnvironment(undefined=StrictUndefined)
    env.globals.update(states=lambda e: values[e], is_state=lambda e, s: values[e] == s,
                       now=lambda: NOW, utcnow=lambda: NOW,
                       as_timestamp=lambda d: d.timestamp(),
                       state_attr=lambda e, a: {'last_seen': NOW.timestamp() - age, 'request_id': ack}.get(a))
    env.filters['to_json'] = lambda v, ensure_ascii=False: json.dumps(v, ensure_ascii=ensure_ascii, separators=(',', ':'))
    def decode(value, default):
        try:
            return json.loads(value)
        except ValueError:
            return default
    env.filters['from_json'] = decode
    context = {'mode': mode}
    for step in SCRIPT['sequence']:
        if 'variables' in step:
            for k, v in step['variables'].items():
                context[k] = env.from_string(v).render(**context)
        elif 'if' in step:
            if env.from_string(step['if'][0]['value_template']).render(**context):
                raise ValueError(step['then'][0]['stop'])
        else:
            result = env.from_string(step['data']['value']).render(**context)
            assert isinstance(result, str) and result.startswith('J1:') and len(result) <= 255
            return json.loads(result[3:])
    raise AssertionError('No message committed')


def rejected(**kwargs):
    try:
        run(**kwargs)
    except ValueError:
        return True
    return False


def main():
    assert run('Merhaba, nasılsın?')['text'] == 'Merhaba, nasılsın?'
    for words in ['1e3', '00123', 'None', 'True', '1.50', 'Şimdi geliyorum!']:
        assert run(words)['text'] == words  # Numeric-looking messages must not change.
    assert run('Lambayı kapat', 'say')['mode'] == 'say'
    assert run('', 'photo')['text'] == ''
    assert rejected(message='') and rejected(message='hello', age=46) and rejected(message='hello', state='processing')
    assert rejected(message='"' * 140)
    old = {'id': 'm20261026115959000000', 'at': int(NOW.timestamp()) - 1}
    previous = 'J1:' + json.dumps(old)
    assert rejected(message='new', previous=previous, ack='other')
    assert run('new', previous=previous, ack=old['id'])['text'] == 'new'
    assert run('🙂' * 100)['text'] == '🙂' * 100
    out = Path(__file__).resolve().parent.parent / '.local' / 'reports'
    out.mkdir(parents=True, exist_ok=True)
    report = {'pass': True, 'network_requests': 0, 'checks': ['native string preservation', 'Turkish and emoji JSON',
        'say/photo modes', 'empty/offline/busy rejection', 'escaped JSON size bound', 'pending job guard', 'completed job resubmission']}
    (out / 'ha-script-tests.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
