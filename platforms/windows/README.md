# Windows platform

Windows packaging/distribution artifacts. Currently empty — Windows builds today are pure native executables orchestrated by `scripts/build.ps1` + CMake, no extra project scaffolding needed.

This directory populates when Windows picks up any of:

- Installer config (WiX `.wxs`, Inno Setup `.iss`, or MSIX)
- App manifest (`app.manifest` — DPI awareness, UAC, theme)
- Resource file (`.rc`) + icon (`.ico`) + exe version-info block
- Code-signing / EV certificate configuration
- Microsoft Store (`.appxmanifest`, Store assets)
- Installer UI assets (license text, dialog images, banner)

See [docs/platform.md](../../docs/platform.md) for the broader platform distribution layout.
