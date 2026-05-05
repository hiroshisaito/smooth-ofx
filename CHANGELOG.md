# Changelog

[日本語版 CHANGELOG](CHANGELOG_ja.md)

All notable changes to this project will be documented in this file.

The project follows [Semantic Versioning](https://semver.org/) and tracks
the upstream AE-side fork
[hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) for major /
minor numbers — the OFX port does not maintain an independent version line.
The 1.5.x series was AE-side only and was skipped by the OFX port; 1.6.0
realigns both sides.

## 1.6.0 — 2026-05-05

Aligns the OFX port with the AE-side fork
[hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) v1.6.0.
Output is byte-identical to 1.4.0 for all three pixel depths; the change
is internal (Rust-core algorithm) plus build-info UI.

### Added

- `smooth_core` Rust crate (vendored from `smooth-ae/rust/smooth_core`,
  v0.1.0) is now used for the 8-bit and 32-bit float code paths.
  rayon strip-parallel internally; output verified byte-identical to
  the 1.4.0 C++ implementation via host_smoke + PPM cmp.
- New CMake option `USE_RUST_CORE` (default OFF) enables the Rust-core
  build. Distributed macOS zips are built with `USE_RUST_CORE=ON`.
- Read-only `build` parameter in Effect Controls showing the plugin
  version, OFX-port git SHA (with `+dirty` flag), and the Rust core
  build identity. Lets UAT confirm at a glance which build is loaded.
- Cross-platform `host_smoke` (was Windows-only); now runs on macOS /
  Linux via `dlopen`, with an optional bench mode triggered by
  `SMOOTH_BENCH_SIZE=WxH SMOOTH_BENCH_ITERS=N`.

### Performance

Wall-clock measurements on an 8-core host, comparing Rust-core build
to the C++-only 1.4.0 baseline:

| Image          | 8-bit                | 32-bit float         |
|----------------|----------------------|----------------------|
| 1920 x 1080    | 91.7 ms -> 36.2 ms (2.53x) | 130.2 ms -> 48.5 ms (2.68x) |
| 3840 x 2160    | 365.8 ms -> 118.8 ms (3.08x) | 507.5 ms -> 185.9 ms (2.73x) |

16-bit integer is unchanged (still C++ — OFX uses 0xFFFF max where
`smooth_core::Pixel16` uses 0x8000). Will move to Rust once the crate
gets an OFX-flavour 16bpc max value.

### Notes

- Bundle size grew (~134 KB -> ~607 KB on arm64) because rayon and
  its dependencies are statically linked into the plugin. No new
  runtime dependencies; `otool -L` still shows only `libc++` /
  `libSystem`.
- Distribution zips renamed from `smooth-1.4.0-*` to `smooth-1.6.0-*`.

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
- macOS builds for `arm64` (Apple Silicon) and `x86_64` (Intel),
  deployment target 11.0. Release artifacts are shipped as separate
  single-architecture zips (`smooth-1.4.0-macos-arm64.zip` and
  `smooth-1.4.0-macos-x86_64.zip`); each emits a proper
  `smooth.ofx.bundle` with generated `Info.plist` and is ad hoc signed.
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
- The macOS release zips (`smooth-1.4.0-macos-arm64.zip` and
  `smooth-1.4.0-macos-x86_64.zip`) are **ad hoc signed** but **not
  notarized** with a Developer ID. Gatekeeper may still block first
  load; recipients must remove the quarantine attribute with
  `xattr -dr com.apple.quarantine` before loading.

### Acknowledgments

Smoothing algorithms (upMode, downMode, 8link, Lack) are ports of the
original After Effects implementation by Kouji Sugiyama (2004),
open-sourced by LOILO Inc. at
<https://github.com/loilo-inc/smooth>. See [README.md](README.md#acknowledgments)
for details.
