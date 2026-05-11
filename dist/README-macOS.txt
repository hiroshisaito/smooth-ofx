smooth OFX plugin (macOS Universal) — install guide
=====================================================

** LEGACY 1.4.0 UNIVERSAL BUILD — kept for archival reference only **

This guide ships inside `smooth-1.4.0-macos-universal.zip` (still in
`dist/` for archival). The 1.6.0 line ships as per-architecture
single-arch zips instead — see `README-macOS-arm64.txt` (Apple Silicon)
or `README-macOS-x86_64.txt` (Intel) for current install instructions.

Version: 1.4.0
Architectures: arm64 + x86_64 (Apple Silicon / Intel)
Deployment target: macOS 11.0+
Signed/Notarized: NO (unsigned build for internal/test distribution)

Install
-------

1. Unzip smooth-1.4.0-macos-universal.zip. You should get a directory
   named "smooth.ofx.bundle".

2. Copy it to the OFX plugins directory:

       /Library/OFX/Plugins/

   (Admin rights required. Use Finder's "Authenticate" dialog, or run
   `sudo cp -R smooth.ofx.bundle /Library/OFX/Plugins/` in Terminal.)

3. This build is UNSIGNED. macOS Gatekeeper will likely block it.
   Remove the quarantine attribute so Resolve can load it:

       sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle

4. Restart DaVinci Resolve. The plugin appears under
   Effects -> Filters -> "Smooth".

Uninstall
---------

       sudo rm -rf /Library/OFX/Plugins/smooth.ofx.bundle

Notes
-----

- CPU-only.
- Tile / multi-resolution not supported.
- Works in 8-bit int, 16-bit int, 16-bit float, and 32-bit float
  working color spaces.
- For redistribution outside the team, re-sign with a Developer ID
  Application certificate and notarize via `notarytool`.
