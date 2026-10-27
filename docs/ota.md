# Wi-Fi updates

## One-time setup

Install the complete layout on a new robot using [USB setup](getting-started.md).
Before connecting USB, isolate external positive power and servo power. Do not
assume the enclosure switch alone isolates the boost supply.

The provided paired OTA client currently requires **Windows**, Python and DPAPI.
Install `requirements-dev.txt`, close the serial monitor and run:

```powershell
python scripts/deploy_wifi.py pair --port YOUR_SERIAL_PORT
```

This creates a random 256-bit device-pairing key. It sends it through the local
USB console once, stores it in robot NVS, and protects the Windows copy for the
current user with DPAPI. Do not share the registry or a complete flash/NVS dump.
The public tool defaults to `%LOCALAPPDATA%/JamiryoPublic/wifi-devices.dpapi`,
separate from private development installations. `JAMIRYO_STATE_DIR` can select
another private registry directory. DPAPI protects the host file, not robot flash.

## Routine update

Power the robot from its normal verified supply and keep it on the same home
LAN as the computer. Ethernet-to-Wi-Fi is supported if the router permits local
clients to communicate. Then, in an ESP-IDF terminal:

```powershell
./scripts/update_wifi.ps1
```

This builds this public checkout, validates the package, discovers the paired
device, uploads through its authenticated endpoint and waits for its exact image
hash/version plus boot-ready confirmation. Changing a home profile does not
automatically revoke the independent device/computer pairing.

For a package built with a custom `--state` directory:

```powershell
./scripts/update_wifi.ps1 -SkipBuild -Firmware PATH_TO_VALIDATED_APPLICATION_BIN
```

Do not flash this source snapshot over a different flash/model layout. The
contract intentionally rejects such a package. Shared model changes require
a separately planned installation/migration; they are not routine app OTA.

## Diagnostics

```powershell
python scripts/deploy_wifi.py discover
python scripts/deploy_wifi.py status
```

Discovery uses UDP 3233; the paired LAN update/status service uses TCP 3232.
It starts after initial boot work, so allow roughly 30 seconds before diagnosing
a newly powered device. The setup AP also provides an update form while active.
Manual setup address: `http://198.18.0.1/`.

Check power first, especially after extended battery use. Then check the LAN,
firewall/client isolation and current status. An upload being accepted is not
proof of successful deployment. If the script cannot confirm the expected
image, stop and inspect status; do not repeatedly upload blindly.

## What authentication does and does not provide

The client obtains a short-lived, single-use nonce. An HMAC-SHA256 signature
binds the nonce, device identity and full file digest. The server checks the
received body, application checksum/SHA-256, target and layout/model contract
before selecting it for boot. Browser-origin requests are rejected by the LAN
endpoint. The key itself is not sent over the LAN.

The update transport is **HTTP on a trusted LAN**, not TLS. HMAC authenticates
the update; it does not encrypt the firmware, replace secure boot, or make the
public status metadata private. No cloud relay or router port forwarding is
needed. Do not expose these ports to the internet.

Preserve the last known-good package and a private backup of your own device
data. A/B rollback helps with a failed new app, but it is not protection against
every flash, hardware or power failure.
