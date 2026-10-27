"""Validate the built OTA image, shared models and preserved NVS layout; export it."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import struct

CONTRACT_OFFSET = 288
CONTRACT_SIZE = 92


def check_image(data):
    if len(data) < CONTRACT_OFFSET + CONTRACT_SIZE or data[0] != 0xE9 or not 1 <= data[1] <= 16:
        raise ValueError("Invalid ESP application image")
    if struct.unpack_from('<H', data, 12)[0] != 9 or struct.unpack_from('<I', data, 32)[0] != 0xABCD5432:
        raise ValueError("Image must be an ESP32-S3 application")
    if data[80:112] != b'desktop_buddy'.ljust(32, b'\0'):
        raise ValueError("Wrong firmware project")
    magic, layout, det_size, feat_size, det_sha, feat_sha = struct.unpack_from('<16sIII32s32s', data, CONTRACT_OFFSET)
    if magic != b'JAMIRYO-OTA-V1'.ljust(16, b'\0') or layout != 1:
        raise ValueError("Factory or incompatible flash layout")
    checksum, offset = 0xEF, 24
    for _ in range(data[1]):
        _, length = struct.unpack_from('<II', data, offset)
        offset += 8
        if offset + length >= len(data):
            raise ValueError("Truncated image segment")
        for byte in data[offset:offset + length]:
            checksum ^= byte
        offset += length
    checksum_offset = ((offset + 16) // 16) * 16 - 1
    if checksum_offset >= len(data) or data[checksum_offset] != checksum:
        raise ValueError("Image checksum mismatch")
    digest_offset = checksum_offset + 1
    if data[23] != 1 or len(data) != digest_offset + 32 or hashlib.sha256(data[:digest_offset]).digest() != data[digest_offset:]:
        raise ValueError("Image SHA-256 mismatch")
    return {
        'version': data[48:80].split(b'\0', 1)[0].decode('ascii'),
        'layout': layout,
        'detect_bytes': det_size, 'feature_bytes': feat_size,
        'detect_sha256': det_sha.hex(), 'feature_sha256': feat_sha.hex(),
        'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state', required=True, type=Path)
    args = parser.parse_args()
    build = args.state / 'build-v2'
    image = (build / 'desktop_buddy.bin').read_bytes()
    info = check_image(image)
    # These negative cases use the actual exported image, not a mock upload.
    bad = bytearray(image); bad[-1] ^= 1
    for data in (image[:379], image[:-32], bad):
        try:
            check_image(data)
        except (ValueError, struct.error):
            pass
        else:
            raise AssertionError('Damaged firmware was accepted')
    legacy = args.state / 'backups' / 'before-ota-0.9.8' / 'desktop_buddy.bin'
    if legacy.exists():
        try:
            check_image(legacy.read_bytes())
        except ValueError:
            pass
        else:
            raise AssertionError('Legacy factory firmware was accepted')
    for stem, key in (('human_face_detect', 'detect'), ('human_face_feat', 'feature')):
        model = (build / 'espdl_models' / (stem + '.espdl')).read_bytes()
        if len(model) != info[key + '_bytes'] or hashlib.sha256(model).hexdigest() != info[key + '_sha256']:
            raise ValueError('Shared model changed; layout contract and USB bootstrap need review')
    project = Path(__file__).resolve().parent.parent
    partitions = {}
    for row in csv.reader(line for line in (project / 'partitions.csv').read_text().splitlines() if line.strip() and not line.startswith('#')):
        name, _, _, start, size = [s.strip() for s in row[:5]]
        length = int(size[:-1]) * 1024 if size.endswith('K') else int(size, 0)
        partitions[name] = (int(start, 0), length)
    for name, expected in {'nvs': (0x9000, 0x6000), 'face_store': (0x710000, 0x20000), 'home_store': (0x730000, 0x20000)}.items():
        if partitions[name] != expected:
            raise ValueError('Saved data partition moved: ' + name)
    ordered = sorted(partitions.values())
    if any(start + length > next_start for (start, length), (next_start, _) in zip(ordered, ordered[1:])) or ordered[-1][0] + ordered[-1][1] > 8 * 1024 * 1024:
        raise ValueError('Overlapping or oversized flash layout')
    if any(len(image) > partitions[name][1] for name in ('ota_0', 'ota_1')):
        raise ValueError('Firmware does not fit both update slots')
    target = args.state / 'artifacts'
    target.mkdir(parents=True, exist_ok=True)
    output = target / ('Jamiryo-' + info['version'] + '.bin')
    output.write_bytes(image)
    output.with_suffix('.json').write_text(json.dumps(info, indent=2) + '\n')
    print('OTA_PACKAGE: PASS (image checksum/SHA-256, damaged and legacy rejection, models, A/B slots, saved data offsets)')
    print('Firmware:', output.resolve())
    print(json.dumps({'version': info['version'], 'bytes': info['bytes'], 'sha256': info['sha256']}))


if __name__ == '__main__':
    main()
