"""Generate original neutral tones and stage the public personality prompt."""
from pathlib import Path
import math
import struct

ROOT = Path(__file__).resolve().parent.parent


def main():
    assets = ROOT / 'main' / 'assets'
    assets.mkdir(parents=True, exist_ok=True)
    # 16 kHz signed little-endian PCM, mono. No voice recording or cloud request.
    for index, name in enumerate(['hello', 'move', 'shake', 'tilt', 'settled', 'heard', 'listen']):
        samples = []
        frames = 4800
        for i in range(frames):
            t = i / 16000
            envelope = math.sin(math.pi * i / (frames - 1)) ** 2
            frequency = 420 + index * 65 + 180 * t
            samples.append(round(3300 * envelope * math.sin(2 * math.pi * frequency * t)))
        data = struct.pack('<' + 'h' * frames, *samples)
        path = assets / (name + '.pcm')
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
    prompt = (ROOT / 'prompts' / 'gemini_buddy.txt').read_bytes()
    path = assets / 'personality.txt'
    if not path.exists() or path.read_bytes() != prompt:
        path.write_bytes(prompt)
    print('ASSETS: PASS (original synthetic tones; no personal media or portrait fixtures)')


if __name__ == '__main__':
    main()
