"""Generate native HA configuration. No credentials, plugins or external resources."""
import argparse
import json
import re
import zipfile
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

STATUS = "{% set t = state_attr('sensor.jamiryo_messenger','last_seen') | float(0) %}\n{% set s = states('sensor.jamiryo_messenger') %}\n**Status:** {% if as_timestamp(now()) - t > 45 %}Offline or not ready yet.{% else %}{{ {'ready':'Connected and ready','processing':'Processing...','done':'Done','error':'Request failed'}.get(s,s) }}{% endif %}\n{% set error = state_attr('sensor.jamiryo_messenger','error') %}\n{% if error %}\n{{ {'queue_timeout':'The voice connection was not ready in time. Wait briefly, then send again.',\n'reply_timeout':'The voice reply did not finish. You can send again.',\n'playback_timeout':'Playback did not finish. You can send again.',\n'speech_interrupted':'Reading was interrupted. Send again if you want the complete message.',\n'connection_lost':'The voice connection was lost. Wait until ready, then send again.',\n'missing_audio':'No speech audio was produced. Send the message again.',\n'missing_reply_text':'No written reply was received. Try again when ready.',\n'previous_result_unconfirmed':'The robot restarted during the previous request. It will not replay it automatically.'}.get(error,'The request could not be completed. Try again when the robot is ready.') }}\n{% endif %}\n{% if state_attr('sensor.jamiryo_messenger','notification_error') %}Phone delivery could not be requested. The reply remains below.{% endif %}"

DASHBOARD = {'title': 'Jamiryo',
 'views': [{'title': 'Chat',
            'path': 'sohbet',
            'icon': 'mdi:robot-happy-outline',
            'cards': [{'type': 'vertical-stack',
                       'cards': [{'type': 'markdown',
                                  'content': '# Jamiryo\n'
                                             'Write a message, choose what to do, then press **Send message**.\n'
                                             '\n'
                                             '**Ask Jamiryo:** get an answer.\n'
                                             '\n'
                                             '**Read aloud at home:** play your text through the robot speaker.'},
                                 {'type': 'entities',
                                  'title': 'Your message',
                                  'show_header_toggle': False,
                                  'entities': [{'entity': 'input_text.jamiryo_message',
                                                'name': 'Your message',
                                                'icon': 'mdi:message-text-outline'},
                                               {'entity': 'input_select.jamiryo_mode',
                                                'name': 'What should it do?',
                                                'icon': 'mdi:format-list-bulleted'}]},
                                 {'type': 'button',
                                  'name': 'Send message',
                                  'show_icon': False,
                                  'show_state': False,
                                  'tap_action': {'action': 'perform-action',
                                                 'perform_action': 'script.jamiryo_send'}}]},
                      {'type': 'vertical-stack',
                       'cards': [{'type': 'markdown',
                                  'title': 'Status',
                                  'content': "{% set t = state_attr('sensor.jamiryo_messenger','last_seen') | "
                                             'float(0) %}\n'
                                             "{% set s = states('sensor.jamiryo_messenger') %}\n"
                                             '**Status:** {% if as_timestamp(now()) - t > 45 %}Offline or not '
                                             "ready yet.{% else %}{{ {'ready':'Connected and "
                                             "ready','processing':'Processing...','done':'Done','error':'Request "
                                             "failed'}.get(s,s) }}{% endif %}\n"
                                             "{% set error = state_attr('sensor.jamiryo_messenger','error') %}\n"
                                             '{% if error %}\n'
                                             "{{ {'queue_timeout':'The voice connection was not ready in time. "
                                             "Wait briefly, then send again.',\n"
                                             "'reply_timeout':'The voice reply did not finish. You can send "
                                             "again.',\n"
                                             "'playback_timeout':'Playback did not finish. You can send again.',\n"
                                             "'speech_interrupted':'Reading was interrupted. Send again if you "
                                             "want the complete message.',\n"
                                             "'connection_lost':'The voice connection was lost. Wait until ready, "
                                             "then send again.',\n"
                                             "'missing_audio':'No speech audio was produced. Send the message "
                                             "again.',\n"
                                             "'missing_reply_text':'No written reply was received. Try again when "
                                             "ready.',\n"
                                             "'previous_result_unconfirmed':'The robot restarted during the "
                                             'previous request. It will not replay it '
                                             "automatically.'}.get(error,'The request could not be completed. Try "
                                             "again when the robot is ready.') }}\n"
                                             '{% endif %}\n'
                                             "{% if state_attr('sensor.jamiryo_messenger','notification_error') "
                                             '%}Phone delivery could not be requested. The reply remains below.{% '
                                             'endif %}'},
                                 {'type': 'markdown',
                                  'title': 'Last reply / read-aloud text',
                                  'content': "<p>{{ (state_attr('sensor.jamiryo_messenger','reply') or 'No reply "
                                             "yet.') | e | replace('\\n','<br>') }}</p>\n"
                                             "{% if state_attr('sensor.jamiryo_messenger','reply_source') == "
                                             "'requested_text' %}Audio was played. No speech transcript was "
                                             'returned, so your requested text is shown.{% endif %}'},
                                 {'type': 'button',
                                  'name': 'Request photo',
                                  'show_icon': False,
                                  'show_state': False,
                                  'tap_action': {'action': 'perform-action',
                                                 'perform_action': 'script.jamiryo_send',
                                                 'data': {'mode': 'photo', 'message': ''}}},
                                 {'type': 'markdown',
                                  'content': "{% set image = state_attr('sensor.jamiryo_messenger','image') %}{% "
                                             'if image %}[Open the latest photo]({{ image }}){% endif %}'}]},
                      {'type': 'markdown',
                       'content': 'Up to 140 characters; JSON escaping can reduce the usable length. Wait for the '
                                  'current request to finish. A person at home can explicitly say **Telefonuma '
                                  'mesaj gönder: ...** to send a phone reply. Ordinary room speech is not '
                                  'forwarded.\n'
                                  '\n'
                                  'Photos go to the phone selected in robot setup. Away from home, connect to '
                                  'Home Assistant through your existing private access such as Tailscale.'}]}]}

TR_SCRIPT = {'alias': 'Jamiryo gönder',
 'description': 'Telefondan soru, sesli mesaj veya tek fotoğraf isteği gönderir.',
 'icon': 'mdi:robot-happy-outline',
 'mode': 'single',
 'max_exceeded': 'silent',
 'fields': {'mode': {'name': 'İşlem', 'selector': {'select': {'options': ['ask', 'say', 'photo']}}},
            'message': {'name': 'Mesaj', 'selector': {'text': {}}}},
 'sequence': [{'variables': {'send_mode': "{{ mode | default({'Jamiryo’ya sor':'ask','Evde sesli "
                                          "okut':'say','Fotoğraf "
                                          "gönder':'photo'}.get(states('input_select.jamiryo_mode'),'ask')) }}",
                             'send_text': "T:{{ message | default(states('input_text.jamiryo_message')) | string "
                                          '| trim }}',
                             'request_id': "m{{ utcnow().strftime('%Y%m%d%H%M%S%f') }}",
                             'previous': "{{ states('input_text.jamiryo_inbox')[3:] | from_json({}) }}"}},
              {'if': [{'condition': 'template',
                       'value_template': '{{ as_timestamp(now()) - '
                                         "(state_attr('sensor.jamiryo_messenger','last_seen') | float(0)) > 45 "
                                         '}}'}],
               'then': [{'stop': 'Jamiryo çevrimdışı. Robotun ev Wi-Fi’sine bağlı olduğunu kontrol et.',
                         'error': True}]},
              {'if': [{'condition': 'template',
                       'value_template': "{{ is_state('sensor.jamiryo_messenger','processing') }}"}],
               'then': [{'stop': 'Önce devam eden isteğin bitmesini bekle.', 'error': True}]},
              {'if': [{'condition': 'template',
                       'value_template': "{{ previous.get('id','') and previous.get('id') != "
                                         "state_attr('sensor.jamiryo_messenger','request_id') and "
                                         "as_timestamp(now()) - (previous.get('at',0) | float(0)) < 120 }}"}],
               'then': [{'stop': 'Önceki mesaj henüz alınmadı; kısa süre bekle.', 'error': True}]},
              {'if': [{'condition': 'template',
                       'value_template': "{{ send_mode not in ['ask','say','photo'] or (send_mode != 'photo' and "
                                         'not (send_text[2:] | trim)) }}'}],
               'then': [{'stop': 'Bir mesaj yaz veya Fotoğraf gönder işlemini seç.', 'error': True}]},
              {'variables': {'envelope': "J1:{{ {'v':1,'id':request_id | string,'mode':send_mode | "
                                         "string,'text':('' if send_mode == 'photo' else "
                                         "send_text[2:]),'at':as_timestamp(now()) | int} | "
                                         'to_json(ensure_ascii=False) }}'}},
              {'if': [{'condition': 'template', 'value_template': '{{ envelope | length > 255 }}'}],
               'then': [{'stop': 'Mesaj çok uzun; biraz kısaltıp yeniden gönder.', 'error': True}]},
              {'action': 'input_text.set_value',
               'target': {'entity_id': 'input_text.jamiryo_inbox'},
               'data': {'value': '{{ envelope }}'}}]}

TR_DASHBOARD = {'title': 'Jamiryo',
 'views': [{'title': 'Sohbet',
            'path': 'sohbet',
            'icon': 'mdi:robot-happy-outline',
            'cards': [{'type': 'vertical-stack',
                       'cards': [{'type': 'markdown',
                                  'content': '# Jamiryo\n'
                                             'Metnini yaz, ne yapacağını seç ve **Mesajı gönder** düğmesine bas.\n'
                                             '\n'
                                             '**Jamiryo’ya sor:** Cevap verir.\n'
                                             '\n'
                                             '**Evde sesli okut:** Yazdığın metni evde hoparlörden okur.'},
                                 {'type': 'entities',
                                  'title': 'Mesajın',
                                  'show_header_toggle': False,
                                  'entities': [{'entity': 'input_text.jamiryo_message',
                                                'name': 'Mesajın',
                                                'icon': 'mdi:message-text-outline'},
                                               {'entity': 'input_select.jamiryo_mode',
                                                'name': 'Ne yapsın?',
                                                'icon': 'mdi:format-list-bulleted'}]},
                                 {'type': 'button',
                                  'name': 'Mesajı gönder',
                                  'icon': 'mdi:send',
                                  'show_icon': False,
                                  'show_state': False,
                                  'tap_action': {'action': 'perform-action',
                                                 'perform_action': 'script.jamiryo_gonder'}}]},
                      {'type': 'vertical-stack',
                       'cards': [{'type': 'markdown',
                                  'title': 'Durum',
                                  'content': "{% set t = state_attr('sensor.jamiryo_messenger','last_seen') | "
                                             'float(0) %}\n'
                                             "{% set s = states('sensor.jamiryo_messenger') %}\n"
                                             '{% if as_timestamp(now()) - t > 45 %}\n'
                                             '**Durum:** Çevrimdışı veya henüz hazır değil.\n'
                                             '{% else %}\n'
                                             "**Durum:** {{ {'ready':'Bağlı ve "
                                             "hazır','processing':'İşleniyor…','done':'Tamamlandı','error':'İşlem "
                                             "tamamlanamadı'}.get(s,s) }}\n"
                                             '{% endif %}\n'
                                             "{% if state_attr('sensor.jamiryo_messenger','error') %}\n"
                                             "{% set error = state_attr('sensor.jamiryo_messenger','error') %}\n"
                                             "{{ {'queue_timeout':'Ses bağlantısı zamanında hazır olmadı. Biraz "
                                             "bekleyip mesajı yeniden gönder.',\n"
                                             "    'reply_timeout':'Sesli yanıt tamamlanamadı. Mesajı yeniden "
                                             "gönderebilirsin.',\n"
                                             "    'playback_timeout':'Hoparlörde oynatma tamamlanamadı. Mesajı "
                                             "yeniden gönderebilirsin.',\n"
                                             "    'speech_interrupted':'Sesli okuma yarıda kesildi. Tamamını "
                                             "okutmak için yeniden gönder.',\n"
                                             "    'connection_lost':'Ses bağlantısı koptu. Bağlantı hazır olunca "
                                             "yeniden gönder.',\n"
                                             "    'missing_audio':'Konuşma sesi üretilemedi. Mesajı yeniden "
                                             "gönder.',\n"
                                             "    'missing_reply_text':'Yanıt alınamadı. Mesajı yeniden "
                                             "gönder.',\n"
                                             "    'previous_result_unconfirmed':'Önceki işlem sırasında robot "
                                             'yeniden başladı. Mesaj kendiliğinden tekrarlanmaz; gerekiyorsa '
                                             "yeniden gönder.'}.get(error,'İşlem tamamlanamadı. Robot hazır "
                                             "olduğunda yeniden dene.') }}\n"
                                             '{% endif %}\n'
                                             "{% if state_attr('sensor.jamiryo_messenger','notification_error') "
                                             '%}\n'
                                             'Telefon bildirimi gönderilemedi: `{{ '
                                             "state_attr('sensor.jamiryo_messenger','notification_error') }}`. "
                                             'Yanıt aşağıdadır.\n'
                                             '{% endif %}'},
                                 {'type': 'markdown',
                                  'title': 'Son cevap / okunan metin',
                                  'content': "<p>{{ (state_attr('sensor.jamiryo_messenger','reply') or 'Henüz bir "
                                             "cevap yok.') | e | replace('\\n','<br>') }}</p>\n"
                                             "{% if state_attr('sensor.jamiryo_messenger','reply_source') == "
                                             "'requested_text' %}Ses oynatıldı. Konuşma dökümü alınamadığı için "
                                             'gönderdiğin metin gösteriliyor.{% endif %}'},
                                 {'type': 'button',
                                  'name': 'Fotoğraf iste',
                                  'icon': 'mdi:camera',
                                  'show_icon': False,
                                  'show_state': False,
                                  'tap_action': {'action': 'perform-action',
                                                 'perform_action': 'script.jamiryo_gonder',
                                                 'data': {'mode': 'photo', 'message': ''}}},
                                 {'type': 'markdown',
                                  'content': "{% set image = state_attr('sensor.jamiryo_messenger','image') %}{% "
                                             'if image %}[Son fotoğrafı aç]({{ image }}){% endif %}'}]},
                      {'type': 'markdown',
                       'content': 'Kısa mesajlar için: en fazla 140 karakter. Robot cevap verirken bekle. Evdeki '
                                  'kişi yanıt göndermek için **“Telefonuma mesaj gönder: …”** diyebilir. Normal '
                                  'oda konuşmaları telefona otomatik aktarılmaz.\n'
                                  '\n'
                                  'Fotoğraf, seçili telefonuna Home Assistant bildirimiyle gönderilir. Tailscale '
                                  'açıkken bu paneli ev dışında da kullanabilirsin.'}]}]}

def package(script, modes, script_id):
    # Native HA package, optional alternative to creating helpers in the UI.
    return {'input_text': {
        'jamiryo_message': {'name':'Jamiryo message','min':0,'max':140,'initial':'','mode':'text'},
        'jamiryo_inbox': {'name':'Jamiryo inbox','min':0,'max':255,'initial':'','mode':'text'}},
        'input_select': {'jamiryo_mode': {'name':'Jamiryo mode','options':list(modes)}},
        'script': {script_id: script}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, help='Optional portable setup ZIP output')
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent / 'home-assistant'
    for folder, script, dashboard, modes, sid in [
        (root, SCRIPT, DASHBOARD, MODES, 'jamiryo_send'),
        (root/'tr', TR_SCRIPT, TR_DASHBOARD,
         {'Jamiryo’ya sor':'ask','Evde sesli okut':'say','Fotoğraf gönder':'photo'}, 'jamiryo_gonder')]:
        folder.mkdir(parents=True, exist_ok=True)
        for name, value in [('jamiryo_send.yaml',script),('jamiryo_dashboard.yaml',dashboard),
                            ('jamiryo_package.yaml',package(script,modes,sid))]:
            (folder/name).write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    if args.bundle:
        args.bundle.parent.mkdir(parents=True,exist_ok=True)
        with zipfile.ZipFile(args.bundle,'w',zipfile.ZIP_DEFLATED) as z:
            allowed = ['README.md','package-install.md','jamiryo_send.yaml','jamiryo_dashboard.yaml',
                       'jamiryo_package.yaml','tr/README.md','tr/jamiryo_send.yaml',
                       'tr/jamiryo_dashboard.yaml','tr/jamiryo_package.yaml']
            included = {'home-assistant/'+name for name in allowed}
            included.add('docs/assets/home-assistant-setup.svg')
            for name in allowed:
                p = root/name
                wire = p.read_text(encoding='utf-8')
                if p.suffix == '.md':
                    def link(match):
                        target=match.group(1)
                        if target.startswith(('http:','https:','#','mailto:')): return match.group(0)
                        path, _, anchor=target.partition('#')
                        resolved=(p.parent/path).resolve().relative_to(root.parent.resolve()).as_posix()
                        if resolved in included: return match.group(0)
                        return ']('+'https://github.com/ersan-oz/jamiryo-desktop-buddy/blob/main/'+resolved+('#'+anchor if anchor else '')+')'
                    wire=re.sub(r'\]\(([^)]+)\)',link,wire)
                z.writestr('home-assistant/'+name,wire)
            z.write(root.parent/'docs/assets/home-assistant-setup.svg','docs/assets/home-assistant-setup.svg')
    print('English and Turkish HA templates generated; no credentials or network requests.')


if __name__ == '__main__':
    main()
