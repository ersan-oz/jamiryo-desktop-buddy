"""Archive only a clean, audited Git commit; exclude state and the .git directory."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    subprocess.run([sys.executable, str(ROOT / 'scripts' / 'check_public_release.py'), '--history'], check=True)
    if subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT).strip():
        raise SystemExit('Commit the reviewed source first; the source archive requires a clean checkout.')
    version = re.search(r'set\(PROJECT_VER "([^"]+)"\)', (ROOT / 'CMakeLists.txt').read_text())[1]
    output = ROOT / 'dist' / ('jamiryo-desktop-buddy-' + version + '-source.zip')
    output.parent.mkdir(exist_ok=True)
    subprocess.run(['git', 'archive', '--format=zip', '--prefix=jamiryo-desktop-buddy/',
                    '--output=' + str(output), 'HEAD'], cwd=ROOT, check=True)
    print('SOURCE_ARCHIVE:', output.name)


if __name__ == '__main__':
    main()
