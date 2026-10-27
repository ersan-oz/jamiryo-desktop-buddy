"""Meaningful boundaries: sensitive payloads, media metadata and local doc links."""
from pathlib import Path
import re
import struct
import sys
import unittest
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
from check_public_release import scan_text, audit_blob


class PublicReleaseTests(unittest.TestCase):
    def test_credential_patterns_without_exposing_values(self):
        for secret in ['AIza' + 'x' * 35, 'ghp_' + 'a' * 36,
                       'eyJ' + 'a' * 15 + '.' + 'b' * 15 + '.' + 'c' * 15,
                       '-----BEGIN ' + 'PRIVATE KEY-----']:
            found = scan_text('header\n' + secret)
            self.assertTrue(found)
            self.assertEqual(found[0][0], 2)
            self.assertNotIn(secret, repr(found))

    def test_normal_code_is_not_a_credential(self):
        self.assertFalse(scan_text('const char *token; config["api_key"] = getpass();'))

    def test_personal_paths_are_detected(self):
        for path in ['C:' + '/Users/' + 'example/project', '/' + 'home/' + 'example/project']:
            self.assertTrue(scan_text(path))

    def test_backups_and_unapproved_images_rejected(self):
        for name in ['flash.bin', 'pairing.dpapi', 'recording.wav', '.env', 'face.jpg']:
            self.assertTrue(audit_blob(name, b'private example'))
        self.assertTrue(audit_blob('other.png', b'not an approved product image'))

    def test_product_image_has_no_private_metadata(self):
        self.assertFalse(audit_blob('docs/assets/jamiryo.png', (ROOT / 'docs/assets/jamiryo.png').read_bytes()))
        data = b'\x89PNG\r\n\x1a\n' + struct.pack('>I', 4) + b'tEXt' + b'test' + b'\x00' * 4
        self.assertTrue(audit_blob('docs/assets/jamiryo.png', data))

    def test_local_document_links_exist(self):
        checked = 0
        for path in ROOT.rglob('*.md'):
            if any(part in {'.git', '.local', 'managed_components'} for part in path.relative_to(ROOT).parts):
                continue
            text = path.read_text(encoding='utf-8')
            targets = re.findall(r'\]\(([^)]+)\)', text) + re.findall(r'src="([^"]+)"', text)
            for target in targets:
                if target.startswith(('http:', 'https:', '#', 'mailto:')):
                    continue
                relative = unquote(target.split('#')[0])
                if relative:
                    self.assertTrue((path.parent / relative).is_file(), f'Broken link in {path.name}: {target}')
                    checked += 1
        self.assertGreater(checked, 15)


if __name__ == '__main__':
    unittest.main()
