"""Audit publishable Git files without printing matched private values."""
import argparse
from pathlib import Path
import re
import struct
import subprocess

ROOT = Path(__file__).resolve().parent.parent
RULES = {
    'Google API key': re.compile(r'(?:AIza|AQ\.)[A-Za-z0-9_-]{20,}'),
    'GitHub token': re.compile(r'(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,})'),
    'JWT credential': re.compile(r'eyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{10,}'),
    'private key': re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    'personal absolute path': re.compile(r'(?i)(?:[A-Z]:[/\\]+Users[/\\]+[^/\\\s"\']+|[/]home[/][^/\s"\']+)'),
    'device MAC': re.compile(r'(?i)\b(?:[0-9a-f]{2}:){5}[0-9a-f]{2}\b'),
    'credential literal': re.compile(r'''(?i)\b(?:api_key|password|token|ssid)\s*(?:=|:)\s*["'][^"'\n]{8,}["']'''),
}
FORBIDDEN = {'.dpapi', '.bin', '.elf', '.map', '.espdl', '.nvs', '.pcm', '.wav', '.mp3', '.jpg', '.jpeg', '.log', '.pem', '.key'}


def scan_text(text):
    return [(text.count('\n', 0, m.start()) + 1, category)
            for category, pattern in RULES.items() for m in pattern.finditer(text)]


def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT)


def files():
    names = git('ls-files', '-z', '--cached', '--others', '--exclude-standard').decode('utf-8').split('\0')
    return sorted(set(n for n in names if n))


def audit_blob(name, data):
    findings = []
    path = Path(name)
    if path.suffix.lower() in FORBIDDEN or path.name in {'credentials.json', 'wifi-devices.dpapi'} or path.name.startswith('.env'):
        findings.append((0, 'forbidden artifact'))
    if path.suffix == '.png':
        if name != 'docs/assets/jamiryo.png' or data[:8] != b'\x89PNG\r\n\x1a\n':
            return findings + [(0, 'unapproved image')]
        offset = 8
        while offset < len(data):
            if offset + 12 > len(data):
                return findings + [(0, 'invalid PNG')]
            size = struct.unpack_from('>I', data, offset)[0]
            kind = data[offset+4:offset+8]
            if kind not in {b'IHDR', b'PLTE', b'IDAT', b'IEND', b'tRNS'}:
                findings.append((0, 'PNG metadata / unreviewed chunk'))
            offset += size + 12
        if offset != len(data):
            findings.append((0, 'invalid PNG length'))
        return findings
    try:
        text = data.decode('utf-8-sig')
    except UnicodeError:
        return findings + [(0, 'unreviewed binary')]
    return findings + scan_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--history', action='store_true')
    args = parser.parse_args()
    findings = []
    selected = files()
    for name in selected:
        p = ROOT / name
        if p.is_symlink() or not p.resolve().is_relative_to(ROOT.resolve()):
            findings.append((name, 0, 'unexpected symlink / path')); continue
        for line, category in audit_blob(name, p.read_bytes()):
            findings.append((name, line, category))
    if args.history:
        for entry in git('rev-list', '--objects', '--all').decode('utf-8').splitlines():
            oid, _, name = entry.partition(' ')
            if not name or git('cat-file', '-t', oid).strip() != b'blob':
                continue
            for line, category in audit_blob(name, git('cat-file', 'blob', oid)):
                findings.append(('history:' + name, line, category))
    for name, line, category in sorted(set(findings)):
        print(f'{name}:{line}: {category} [value withheld]')
    if findings:
        raise SystemExit('PUBLIC_AUDIT: FAIL; review findings before publishing.')
    print(f'PUBLIC_AUDIT: PASS ({len(selected)} publishable files; matched values never logged)')


if __name__ == '__main__':
    main()
