# Security

This is a hobby prototype, not a security-reviewed appliance. The present public
snapshot is the supported review target; there is no promise of a response SLA.

Do not include a real key, token, password, private address/identifier, face
profile, raw capture or flash dump in a public issue. If GitHub private
vulnerability reporting is available for this repository, use it for details
that could expose a user or enable misuse. Otherwise open a minimal issue
requesting a private contact route without disclosing the exploit or user data.

If a credential is exposed, revoke/rotate it at its provider. Remove it from
the repository/history using GitHub's documented procedure; assume that copies
may already exist. Do not rely on `.gitignore` to remove previously tracked data.

The local publish checker is a useful guard, not a complete secret detector.
Review diffs and commit history manually before publishing. Runtime trust and
transport limitations are documented in [privacy.md](docs/privacy.md) and
[ota.md](docs/ota.md).
