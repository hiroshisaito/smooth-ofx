smooth OFX plugin (macOS arm64 / Apple Silicon) — install guide
===============================================================

Version: 1.4.0
Architecture: arm64 (Apple Silicon only — M1 / M2 / M3 / M4 …)
Deployment target: macOS 11.0+
Signed/Notarized: Ad hoc signed, NOT notarized (internal/test distribution)

This archive contains ONLY the arm64 build. Intel (x86_64) Macs must use
smooth-1.4.0-macos-x86_64.zip instead.


Install
-------

1. Unzip smooth-1.4.0-macos-arm64.zip. You should get a directory named
   "smooth.ofx.bundle".

2. Copy it to the OFX plugins directory:

       /Library/OFX/Plugins/

   (Admin rights required. Use Finder's "Authenticate" dialog, or run
   `sudo cp -R smooth.ofx.bundle /Library/OFX/Plugins/` in Terminal.)

3. This build is AD HOC SIGNED only (no Developer ID). macOS Gatekeeper
   may still block it on first load. Remove the quarantine attribute so
   Resolve can load it:

       sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle

4. Restart DaVinci Resolve. The plugin appears under
   Effects -> Filters -> "Smooth".


Verify the architecture
-----------------------

       lipo -info /Library/OFX/Plugins/smooth.ofx.bundle/Contents/MacOS/smooth.ofx

Expected output:
       Non-fat file: ... is architecture: arm64


Uninstall
---------

       sudo rm -rf /Library/OFX/Plugins/smooth.ofx.bundle


Notes
-----

- CPU-only (no GPU render path).
- Tile / multi-resolution not supported.
- Works in 8-bit int, 16-bit int, 16-bit float, and 32-bit float
  working color spaces.
- For public redistribution, re-sign with a Developer ID Application
  certificate and notarize via `notarytool`.
