# smooth-ofx

[日本語 README](README_ja.md)

OpenFX port of **smooth** — a pixel-boundary smoothing filter that cleans up
step / staircase patterns, originally released as an After Effects plugin by
LOILO Inc.

This port preserves the original smoothing algorithm and exposes it through
the OpenFX 1.5.1 API, so the same effect can be used in DaVinci Resolve,
Fusion and other OFX-compliant hosts.

## Features

- 8-bit / 16-bit integer and 16-bit / 32-bit float RGBA pixel support
- Premultiplied-alpha correct (auto-converted on input and restored on output)
- Parameters: `range`, `line weight`, `transparent`, plus a read-only
  `build` identity label in the Inspector
- CPU implementation only (Rust core, rayon-parallel)
- No third-party runtime dependencies (OFX headers only); on macOS the
  shipping bundle links only `libc++` and `libSystem`

## Supported platforms

- **macOS 11+** — `arm64` (Apple Silicon) and `x86_64` (Intel) are shipped
  as separate single-architecture builds
- **Windows 10/11 (x64)** — built with Visual Studio 2022/2026 (MSVC); MinGW-w64
  is also supported
- **Linux x86-64** — RHEL-9 family (Rocky Linux 9.5 / AlmaLinux 9 / Oracle
  Linux 9), glibc 2.34+, gcc 11.5+

## Repository layout

| Path | Purpose |
|---|---|
| `smooth/` | OFX plugin source (port target) |
| `include/openfx` | OpenFX SDK (git submodule) |
| `smooth-ae` | Original AE plugin (git submodule, reference only) |
| `tests/` | Minimal OFX host harness for smoke testing |
| `dist/` | Packaging templates (Info.plist, install README) |

### Submodules

| Path | Source | Required for build |
|---|---|---|
| `include/openfx` | [AcademySoftwareFoundation/openfx](https://github.com/AcademySoftwareFoundation/openfx) @ `OFX_Release_1.5.1` | Yes |
| `smooth-ae` | [hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) (maintenance fork of [loilo-inc/smooth](https://github.com/loilo-inc/smooth)) | No — reference only |

After cloning:

```bash
git submodule update --init --recursive
# or, skip the reference-only smooth-ae submodule:
git submodule update --init include/openfx
```

## Building

See **[BUILDING.md](docs/BUILDING.md)** for the full cross-platform build
guide (macOS / Windows / Linux), including Rust core (`smooth_core`)
toolchain setup, CMake options, signing, and validation. Quick path:

```bash
# macOS arm64 (Apple Silicon)
cmake -S . -B build-macos-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos-arm64 --config Release

# Windows MSVC x64 (from a "x64 Native Tools Command Prompt for VS")
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release

# Linux x86_64
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

The `USE_RUST_CORE` CMake option (default ON since 1.6.0) wires the
shared `smooth_core` Rust crate into the build for the 8-bit and
32-bit float code paths. Pass `-DUSE_RUST_CORE=OFF` to skip it
(useful for environments without a Rust toolchain or for the MSYS2
MinGW dev path).

## Installation

Copy the built bundle / DLL to the host's OFX plugin directory.

- **macOS**: `/Library/OFX/Plugins/smooth.ofx.bundle/`
- **Windows**: `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\Contents\Win64\smooth.ofx`
- **Linux**: `/usr/OFX/Plugins/smooth.ofx.bundle/Contents/Linux-x86-64/smooth.ofx` (or the host's dedicated plugin directory, e.g. Resolve's `/var/BlackmagicDesign/DaVinci Resolve/Support/OFX/Plugins/`)

Restart the OFX host. The effect appears under **Effects → Filters → Smooth**.

Unsigned macOS builds may be quarantined by Gatekeeper; clear the attribute
with:

```bash
sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle
```

## License

This project is licensed under the **Apache License, Version 2.0**,
inherited from the original
[loilo-inc/smooth](https://github.com/loilo-inc/smooth) project.
See [LICENSE](LICENSE) for the full text.

## Acknowledgments

- **Original plugin** — The smooth After Effects plugin was created by
  Kouji Sugiyama (杉山浩二) in 2004 and later open-sourced by
  [LOILO Inc.](https://loilo.tv/) at
  <https://github.com/loilo-inc/smooth> under the Apache 2.0 license.
  All smoothing algorithms (upMode, downMode, 8link, Lack) in this
  repository are direct ports of that implementation.
- **OpenFX SDK** — [Academy Software Foundation](https://www.openeffects.org/)
  for the OpenFX 1.5.1 specification and reference headers.

This port contributes: the OFX plugin boundary (describe / render / clips
/ parameters), higher bit-depth support (16-bit integer, 16/32-bit float),
premultiplied-alpha handling required by OFX hosts, and a minimal host
harness for smoke testing.
