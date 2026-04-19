# Changelog

[日本語版 CHANGELOG](CHANGELOG_ja.md)

All notable changes to this project will be documented in this file.

The project follows [Semantic Versioning](https://semver.org/). The initial
OFX port inherits the version number (`1.4.0`) of the upstream After Effects
plugin ([loilo-inc/smooth](https://github.com/loilo-inc/smooth)); OFX-port-
specific fixes will advance the patch component.

## 1.4.0 — 2026-04-20

Initial OpenFX port of the **smooth** plugin. Verified on DaVinci Resolve
on both Windows and macOS.

### Added

- OpenFX 1.5.1 plugin implementation (`jp.loilo.smooth`, version 1.4):
  `describe` / `describeInContext` / `createInstance` / `destroyInstance`
  / `render` actions.
- Pixel-depth support for 8-bit integer, 16-bit integer, 16-bit float,
  and 32-bit float RGBA working color spaces.
- Premultiplied-alpha handling: input buffers are unpremultiplied before
  the smoothing pass and re-premultiplied on output, so hosts that pass
  premultiplied pixels (e.g. DaVinci Resolve, Fusion) produce correct
  edges.
- Parameters exposed by the plugin:
  - `range` (double, 0–100, default 1.0)
  - `lineWeight` (double, 0–1, default 0.0)
  - `whiteOption` / transparent (boolean)
- macOS Universal 2 build (`arm64` + `x86_64`), deployment target 11.0.
  Emits a proper `smooth.ofx.bundle` with generated `Info.plist`.
- Windows x64 build via MSVC 2019+ or MSYS2 MinGW-w64.
- Minimal OFX host harness under `tests/host_smoke.cpp` (Windows only)
  for smoke-testing the plugin without a full host.
- Documentation: `README.md` / `README_ja.md`, Apache-2.0 `LICENSE`
  inherited from the upstream project.

### Fixed

- Black-pixel artifact on lines in 16-bit / 32-bit float color spaces.
  The `Link8SquareExecute` accumulator was `int sum_color[4]`, which
  truncated every float pixel value (0..1 range) to zero when summed,
  producing black output. Changed the accumulator type to
  `PixelRangeType<PixelType>::type` (unsigned int for 8/16-bit integer,
  float for 32-bit float).
- White-fringe artifact on transparent edges under DaVinci Resolve
  when the `transparent` option was on. The original algorithm assumed
  straight RGBA; added in-place unpremultiply/premultiply around the
  smoothing pass.

### Known limitations

- Tile rendering is not supported (`kOfxImageEffectPropSupportsTiles = 0`).
- Multi-resolution / proxy rendering is not verified.
- GPU render path (`ofxGPURender.h`) is not implemented — CPU only.
- The macOS release ZIP (`smooth-1.4.0-macos-universal.zip`) is not
  code-signed or notarized; recipients must remove the quarantine
  attribute with `xattr -dr com.apple.quarantine` before loading.

### Acknowledgments

Smoothing algorithms (upMode, downMode, 8link, Lack) are ports of the
original After Effects implementation by Kouji Sugiyama (2004),
open-sourced by LOILO Inc. at
<https://github.com/loilo-inc/smooth>. See [README.md](README.md#acknowledgments)
for details.
