# Contributing

Start with a small issue or pull request explaining the behavior being changed.
Use English for public documentation. Turkish runtime strings are expected in
this snapshot; language changes should be intentional and tested across prompts,
local phrases, HA Assist and photo-request parsing.

Before submitting:

```sh
python -m pip install -r requirements-dev.txt
python scripts/check_public_release.py
python -m unittest discover -s tests -v
python scripts/check_ha_messages.py
python scripts/build.py
```

For firmware changes, increment `PROJECT_VER` and describe the actual test
scope. A successful build, queued servo command or accepted notification does
not prove physical motion, audible playback or phone delivery.

Keep NVS, home/face data, A/B slots and shared-model contracts compatible.
Dependency/model changes need deliberate migration review. Avoid blocking work
on the audio/display paths, unbounded cloud buffers, replaying home actions
after reconnect, and casual changes to camera/servo timers.

Never submit actual credentials, private network/device identifiers, raw logs,
portraits, recordings, flash images, NVS backups or pairing registries. Provide
synthetic examples and minimal redacted error codes instead. Read [SECURITY.md](SECURITY.md)
and [privacy.md](docs/privacy.md) before reporting a sensitive problem.
