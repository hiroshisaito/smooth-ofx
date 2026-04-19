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
- Parameters: `range`, `line weight`, `transparent`
- CPU only (no GPU render path)
- No third-party runtime dependencies (OFX headers only)

## Supported platforms

- **macOS 11+** — Universal 2 (`arm64` + `x86_64`)
- **Windows 10+ (x64)** — MSVC 2019+ or MSYS2 MinGW-w64

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
| `smooth-ae` | [loilo-inc/smooth](https://github.com/loilo-inc/smooth) | No — reference only |

After cloning:

```bash
git submodule update --init --recursive
# or, skip the reference-only smooth-ae submodule:
git submodule update --init include/openfx
```

## Building

### macOS (Universal)

```bash
cmake -S . -B build-macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DBUILD_TESTS=OFF
cmake --build build-macos --config Release
```

The bundle is emitted at
`build-macos/smooth.ofx.bundle/Contents/MacOS/smooth.ofx`.

### Windows

```bash
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

The DLL is emitted at `build-msvc/Release/smooth.ofx`.

## Installation

Copy the built bundle / DLL to the host's OFX plugin directory.

- **macOS**: `/Library/OFX/Plugins/smooth.ofx.bundle/`
- **Windows**: `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\Contents\Win64\smooth.ofx`

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
