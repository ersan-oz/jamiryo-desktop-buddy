"""Pair once over USB; discover and update Jamiryo over the home LAN afterwards."""
import argparse
import hashlib
import hmac
import http.client
import ipaddress
import json
import os
from pathlib import Path
import secrets
import socket
import time

import ai_credentials
from check_firmware_package import check_image

STATE = Path(os.environ.get('JAMIRYO_STATE_DIR',
             str(Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'JamiryoPublic')))
if os.name != 'nt':
    raise SystemExit('Paired OTA tooling currently requires Windows DPAPI; see docs/ota.md.')
REGISTRY = STATE / 'wifi-devices.dpapi'
PORT = 3232


def registry():
    return ai_credentials.load(REGISTRY) if REGISTRY.exists() else {'devices': {}, 'default': None}


def save_registry(data):
    temporary = REGISTRY.with_suffix('.tmp')
    ai_credentials.save(temporary, data)
    temporary.replace(REGISTRY)


def request(host, path, method='GET', body=None, headers=None, timeout=5):
    # Numeric LAN address only: no proxy/environment redirect of pairing signatures.
    address = ipaddress.ip_address(host)
    if address.version != 4 or not address.is_private or address.is_loopback or address.is_link_local:
        raise ValueError('A private IPv4 address on the home network is required')
    connection = http.client.HTTPConnection(host, PORT, timeout=timeout)
    try:
        connection.request(method, path, body=body, headers={'Connection': 'close', **(headers or {})})
        response = connection.getresponse()
        data = response.read(16385)
        if len(data) > 16384:
            raise ValueError('Oversized device response')
        if response.status != 200:
            raise RuntimeError(f'Device HTTP {response.status} during {path}')
        return json.loads(data)
    finally:
        connection.close()


def identity(data, device_id=None):
    return data.get('project') == 'desktop_buddy' and data.get('protocol') == 1 and (
        device_id is None or data.get('device_id') == device_id)


def discover(seconds=3):
    destinations = {'255.255.255.255'}
    # psutil is optional; on this development machine it finds per-adapter broadcasts.
    try:
        import psutil
        for addresses in psutil.net_if_addrs().values():
            for address in addresses:
                if address.family == socket.AF_INET and address.netmask:
                    interface = ipaddress.ip_interface(address.address + '/' + address.netmask)
                    if interface.ip.is_private and not interface.ip.is_loopback and not interface.ip.is_link_local:
                        destinations.add(str(interface.network.broadcast_address))
    except ImportError:
        pass
    found = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(.25)
        for destination in destinations:
            try:
                sock.sendto(b'JAMIRYO_DISCOVER_V1', (destination, 3233))
            except OSError:
                continue
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                payload, peer = sock.recvfrom(2048)
                data = json.loads(payload)
                if identity(data):
                    found[data['device_id']] = {**data, 'host': peer[0]}
            except (OSError, ValueError, KeyError):
                continue
    return list(found.values())


def paired_device(args):
    data = registry()
    device_id = args.device or data.get('default')
    if not device_id or device_id not in data['devices']:
        raise RuntimeError('Pair this computer once with: deploy_wifi.py pair --port COM7')
    return data, device_id, data['devices'][device_id]


def locate(args):
    data, device_id, paired = paired_device(args)
    hosts = [args.host] if args.host else [paired.get('host')]
    for candidate in discover(2):
        if candidate['device_id'] == device_id:
            hosts.append(candidate['host'])
    for host in dict.fromkeys(h for h in hosts if h):
        try:
            status = request(host, '/status')
            if identity(status, device_id):
                paired['host'] = host
                save_registry(data)
                return device_id, paired, host, status
        except (OSError, ValueError, RuntimeError, http.client.HTTPException):
            continue
    raise RuntimeError('Paired Jamiryo was not found. Keep it powered on the same home network; client isolation must be off.')


def pair(args):
    import serial  # Only the one-time pairing operation needs pyserial.
    key = secrets.token_hex(32)
    device = serial.Serial(port=None, baudrate=115200, timeout=.15, write_timeout=3)
    device.dtr = device.rts = False
    device.port = args.port
    info, saved = None, False
    with device:
        device.write(b'ota-info\n')
        end = time.monotonic() + 12
        while time.monotonic() < end:
            line = device.readline().decode('utf-8', 'replace')
            if 'REMOTE_OTA_INFO: ' in line:
                info = json.loads(line.split('REMOTE_OTA_INFO: ', 1)[1])
                break
        if not info or not identity(info):
            raise RuntimeError('USB firmware does not expose remote update pairing yet')
        device.write(('ota-pair ' + key + '\n').encode('ascii'))
        end = time.monotonic() + 10
        while time.monotonic() < end:
            line = device.readline().decode('utf-8', 'replace')
            if 'REMOTE_OTA_PAIR: SAVED' in line:
                saved = True
                break
            if 'REMOTE_OTA_PAIR: REJECTED' in line:
                break
    if not saved:
        raise RuntimeError('Pairing was not saved; keep USB connected and retry')
    data = registry()
    data['devices'][info['device_id']] = {'key': key, 'host': info.get('host', '')}
    data['default'] = info['device_id']
    save_registry(data)
    print(f"PAIRED: {info['device_id']} (key stored with Windows DPAPI; never sent over LAN)")


def upload(args):
    image = args.firmware.read_bytes()
    package = check_image(image)
    device_id, paired, host, status = locate(args)
    if not status.get('paired') or not status.get('ready') or status.get('busy'):
        raise RuntimeError('Jamiryo is not ready for an update; wait for startup or the current upload')
    challenge = request(host, '/challenge')
    nonce = challenge.get('nonce', '')
    if challenge.get('device_id') != device_id or len(nonce) != 64 or any(c not in '0123456789abcdef' for c in nonce):
        raise ValueError('Invalid device challenge')
    digest = hashlib.sha256(image).hexdigest()
    message = f'jamiryo-ota-v1\n{device_id}\n{nonce}\n{digest}'.encode('ascii')
    signature = hmac.new(bytes.fromhex(paired['key']), message, hashlib.sha256).hexdigest()
    print(f"WIFI_UPLOAD: {status['version']} -> {package['version']}, {len(image)} bytes, {host}", flush=True)
    request(host, '/update', 'POST', image, {'Content-Type': 'application/octet-stream',
        'X-Jamiryo-Nonce': nonce, 'X-Jamiryo-SHA256': digest, 'X-Jamiryo-Auth': signature}, timeout=330)
    print('IMAGE_ACCEPTED: waiting for reboot and local health confirmation', flush=True)
    end = time.monotonic() + 150
    expected_digest = image[-32:].hex()
    while time.monotonic() < end:
        time.sleep(3)
        try:
            current = request(host, '/status', timeout=3)
            if identity(current, device_id) and current.get('ready') and not current.get('busy'):
                if current.get('version') == package['version'] and current.get('image_sha256') == expected_digest:
                    print(f"WIFI_UPDATE: PASS version={current['version']} slot={current['slot']} image_sha256=verified")
                    return
        except (OSError, ValueError, RuntimeError, http.client.HTTPException):
            continue
    raise RuntimeError('Upload was accepted but the expected image was not confirmed. Inspect status; do not retry blindly.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    pair_parser = sub.add_parser('pair')
    pair_parser.add_argument('--port', default='COM7')
    sub.add_parser('discover')
    for command in ('status', 'upload'):
        p = sub.add_parser(command)
        p.add_argument('--device')
        p.add_argument('--host')
        if command == 'upload':
            p.add_argument('--firmware', required=True, type=Path)
    args = parser.parse_args()
    if args.command == 'pair':
        pair(args)
    elif args.command == 'discover':
        print(json.dumps(discover(), ensure_ascii=False, indent=2))
    elif args.command == 'status':
        _, _, host, status = locate(args)
        print(json.dumps({'host': host, **status}, ensure_ascii=False, indent=2))
    else:
        upload(args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
        raise SystemExit(str(error))
