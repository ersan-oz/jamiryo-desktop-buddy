"""Provision voice-only Wi-Fi/Gemini over a physically connected USB console."""
import argparse
import getpass
import json
import time


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    args = parser.parse_args()
    config = {'provider': 'gemini', 'model': 'gemini-3.8-live', 'voice': 'Puck', 'free_tier': True}
    config['ssid'] = input('2.4 GHz Wi-Fi name: ')
    config['password'] = getpass.getpass('Wi-Fi password (hidden): ')
    config['api_key'] = getpass.getpass('Your Gemini API key (hidden): ').strip()
    if not 1 <= len(config['ssid'].encode()) <= 32 or len(config['password'].encode()) > 63:
        raise SystemExit('Invalid Wi-Fi field length.')
    if config['password'] and len(config['password'].encode()) < 8:
        raise SystemExit('Wi-Fi password must be empty or at least 8 bytes.')
    if not 20 <= len(config['api_key']) <= 512 or not all(33 <= ord(c) < 127 for c in config['api_key']):
        raise SystemExit('Invalid API key format.')
    wire = ('configure ' + json.dumps(config, ensure_ascii=False, separators=(',', ':')) + '\n').encode()
    if len(wire) >= 4096:
        raise SystemExit('Provisioning request is too large.')
    device = serial.Serial(port=None, baudrate=115200, timeout=.3, write_timeout=5)
    device.dtr = device.rts = False
    device.port = args.port
    with device:
        device.reset_input_buffer()
        for start in range(0, len(wire), 128):
            device.write(wire[start:start+128]); device.flush(); time.sleep(.025)
        end = time.monotonic() + 15
        while time.monotonic() < end:
            line = device.readline().decode('utf-8', 'replace')
            # Never echo raw serial output, config, credentials or setup passwords.
            if 'AI_CONFIG: SAVED' in line:
                print('Configuration saved on the robot. No credentials saved on this computer.')
                return
            if 'AI_CONFIG: REJECTED' in line:
                raise SystemExit('Robot rejected the configuration; check field values and firmware.')
    raise SystemExit('No save acknowledgement. Configuration is unconfirmed; inspect the USB connection.')


if __name__ == '__main__':
    main()
