# Optional native HA package

Use this **instead of** creating helpers and the script in the UI. It is not a
one-click integration and does not install the dashboard. The [UI guide](README.md)
does not require editing server configuration or restarting HA.

1. Back up HA configuration. Check [jamiryo_package.yaml](jamiryo_package.yaml):
   it creates two text helpers, one dropdown and `script.jamiryo_send`. Do not
   install it over existing helpers/scripts with those IDs.
2. Put it at `/config/packages/jamiryo.yaml` on your HA server.
3. Merge this into the **existing** `homeassistant:` mapping in `configuration.yaml`:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

   Do not create duplicate `homeassistant:` keys or blindly replace an existing
   packages mapping. This snippet belongs in server configuration, not a dashboard.
4. Run HA's configuration check, resolve errors, then restart HA. This package
   initializes both text helpers to empty on startup, so unsent drafts/inbox
   values are not preserved across that HA restart.
5. Continue with [dashboard and phone setup](README.md#4-add-the-separate-dashboard).

For Turkish use `tr/jamiryo_package.yaml` at the same server path and the Turkish
dashboard; its script is `script.jamiryo_gonder`. Install only one language set.
No YAML file needs a Gemini key, HA token or Wi-Fi password.

Official [HA package documentation](https://www.home-assistant.io/docs/configuration/packages/).
