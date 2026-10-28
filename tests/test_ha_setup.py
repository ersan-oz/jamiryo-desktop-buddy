"""Installation artifacts must agree across UI/package and language choices."""
import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
import zipfile
from jinja2 import StrictUndefined
from jinja2.nativetypes import NativeEnvironment
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'scripts'))
from ha_message_bundle import SCRIPT, DASHBOARD, MODES, TR_SCRIPT, TR_DASHBOARD, package


def actions(node):
    if isinstance(node, dict):
        if 'perform_action' in node:
            yield node['perform_action']
        for value in node.values():
            yield from actions(value)
    elif isinstance(node, list):
        for value in node:
            yield from actions(value)


class HomeSetupTests(unittest.TestCase):
    def test_language_sets_and_package_match(self):
        for folder, script, dashboard, modes, script_id in [
            (ROOT/'home-assistant', SCRIPT, DASHBOARD, MODES, 'jamiryo_send'),
            (ROOT/'home-assistant/tr', TR_SCRIPT, TR_DASHBOARD,
             {'Jamiryo’ya sor':'ask','Evde sesli okut':'say','Fotoğraf gönder':'photo'}, 'jamiryo_gonder')]:
            self.assertEqual(json.loads((folder/'jamiryo_send.yaml').read_text(encoding='utf-8')), script)
            self.assertEqual(json.loads((folder/'jamiryo_dashboard.yaml').read_text(encoding='utf-8')), dashboard)
            p = json.loads((folder/'jamiryo_package.yaml').read_text(encoding='utf-8'))
            self.assertEqual(p, package(script, modes, script_id))
            self.assertEqual(set(p), {'input_text','input_select','script'})
            self.assertEqual(p['input_text']['jamiryo_message']['max'],140)
            self.assertEqual(p['input_text']['jamiryo_inbox']['max'],255)
            self.assertEqual(p['input_select']['jamiryo_mode']['options'],list(modes))
            self.assertEqual(set(actions(dashboard)), {'script.'+script_id})
            self.assertEqual(dashboard['views'][0]['path'],'sohbet')
            draft_group=dashboard['views'][0]['cards'][0]['cards']
            self.assertEqual(len(draft_group[1]['entities']),2)
            self.assertEqual(draft_group[2]['type'],'button')

    def test_status_and_reply_source_are_explained(self):
        for board in [DASHBOARD,TR_DASHBOARD]:
            wire=json.dumps(board,ensure_ascii=False)
            for field in ['queue_timeout','speech_interrupted','requested_text']:
                self.assertIn(field,wire)
            self.assertNotIn('<pre',wire)

    def test_exact_dropdown_labels_select_wire_modes(self):
        env=NativeEnvironment(undefined=StrictUndefined)
        for script, labels in [(SCRIPT,MODES),(TR_SCRIPT,{'Jamiryo’ya sor':'ask','Evde sesli okut':'say','Fotoğraf gönder':'photo'})]:
            code=script['sequence'][0]['variables']['send_mode']
            for label, expected in labels.items():
                self.assertEqual(env.from_string(code).render(states=lambda _:label),expected)

    def test_bundle_has_only_installation_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'setup.zip'
            subprocess.run([sys.executable,str(ROOT/'scripts/ha_message_bundle.py'),'--bundle',str(path)],check=True,capture_output=True)
            with zipfile.ZipFile(path) as z:
                self.assertEqual(len(z.namelist()),10)
                self.assertTrue(all(not n.startswith('/') and '..' not in Path(n).parts for n in z.namelist()))
                self.assertFalse(any(n.endswith(('.dpapi','.bin','.pdf','.log')) for n in z.namelist()))
                text=z.read('home-assistant/README.md').decode('utf-8')
                self.assertIn('https://github.com/ersan-oz/jamiryo-desktop-buddy/blob/main/docs/getting-started.md',text)


if __name__=='__main__':
    unittest.main()
