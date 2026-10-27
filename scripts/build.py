"""Build this checkout in isolated storage, validate and export an OTA image."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', type=Path, default=ROOT / '.local')
    args = parser.parse_args()
    state = args.state.resolve()
    state.mkdir(parents=True, exist_ok=True)
    idf = Path(os.environ.get('IDF_PATH', '')) / 'tools' / 'idf.py'
    if not idf.is_file():
        raise SystemExit('Open an ESP-IDF 6.0.1 terminal first (IDF_PATH is not configured).')
    subprocess.run([sys.executable, str(ROOT / 'scripts' / 'prepare_assets.py')], check=True)
    subprocess.run([sys.executable, str(idf), '-B', str(state / 'build-v2'),
                    '-D', 'SDKCONFIG=' + str(state / 'sdkconfig'),
                    '-D', 'IDF_TARGET=esp32s3', 'build'], cwd=ROOT, check=True)
    subprocess.run([sys.executable, str(ROOT / 'scripts' / 'check_firmware_package.py'),
                    '--state', str(state)], check=True)


if __name__ == '__main__':
    main()
