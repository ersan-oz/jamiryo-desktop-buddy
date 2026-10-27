# Public snapshot provenance

This repository is a selected source export from the Jamiryo 1.2.9 prototype.
It starts with a new Git history. The original development directory, runtime
state and prior private commits are not imported.

Changes made for `1.2.9-public1`:

- English project, hardware, architecture, setup, privacy and usage documentation.
- Original generated offline tones in place of private speech assets.
- Generalized public character prompt, retaining operational tool instructions.
- No real-person portrait fixtures or automatic portrait downloads. Face
  self-tests still exercise matching/storage boundaries and a blank model frame;
  real-face accuracy needs separate consented local validation.
- Generic synthetic addresses in source self-tests.
- Independent public build/provisioning scripts and Windows pairing registry.
- A malformed NVS message acknowledgement is reset before string operations;
  failed acknowledgement storage remains fail-closed.
- English labels for the optional experimental Home Assistant dashboard.
- Source-only packaging and a publishable-file/history audit.

No generated application/bootloader/model binary, NVS image, private manual,
device log, pairing registry, enrolled person or household configuration is
part of the repository. The product image is the supplied Jamiryo robot image,
with metadata removed. The subsequently supplied head, cover and base STL exports
are in `hardware/stl`: their free-text headers are replaced and their triangle
payloads are preserved. Native editable CAD files are not included.

The public snapshot's local build checks are distinct from the private
prototype's earlier hardware tests. See [status](docs/status.md).
