# Building smooth-ofx

[日本語版 BUILDING](BUILDING_ja.md)

This guide covers building smooth-ofx 1.6.0+ on macOS, Windows, and
Linux, including the Rust core (`smooth_core`) integration that runs
the 8-bit and 32-bit float code paths in parallel via rayon.

For a release-quality build, follow the platform-specific recipe in
**Section 3**. For a quick smoke test or development iteration with
the C++ baseline only, pass `-DUSE_RUST_CORE=OFF`.

## 1. Toolchain prerequisites

| Component | macOS                              | Windows (production)        | Windows (dev)                 | Linux                          |
|-----------|------------------------------------|-----------------------------|-------------------------------|--------------------------------|
| C++       | Xcode CLT or Xcode (AppleClang)    | MSVC 2019+ (VS 17 / 18)     | MSYS2 MinGW-w64 g++ 12+       | gcc 9+ or clang 12+            |
| CMake     | 3.20+                              | 3.20+                       | 3.20+                         | 3.20+                          |
| Generator | Unix Makefiles / Ninja             | "Visual Studio 17/18 …"     | Ninja or MSYS Makefiles       | Unix Makefiles / Ninja         |
| Rust      | rustup (stable)                    | rustup (stable + MSVC)      | n/a (use `-DUSE_RUST_CORE=OFF`) | rustup (stable) or distro Rust |
| Std lib   | libc++                             | MSVC CRT                    | MSYS libstdc++                | libstdc++                      |

### Rust target triples this build expects

CMake selects the triple based on the platform and (on macOS) on
`CMAKE_OSX_ARCHITECTURES`:

| Platform              | Rust target triple              |
|-----------------------|---------------------------------|
| macOS arm64           | `aarch64-apple-darwin`          |
| macOS x86_64          | `x86_64-apple-darwin`           |
| Windows x64 (MSVC)    | `x86_64-pc-windows-msvc`        |
| Linux x86_64          | `x86_64-unknown-linux-gnu`      |

Install with rustup:

```bash
# macOS
rustup target add aarch64-apple-darwin x86_64-apple-darwin

# Windows (PowerShell or cmd)
rustup target add x86_64-pc-windows-msvc

# Linux
rustup target add x86_64-unknown-linux-gnu
```

`rust-toolchain.toml` in `smooth-ae/rust/smooth_core/` already pins the
channel to `stable`, so no manual channel pin is needed.

## 2. One-time clone setup (all platforms)

```bash
git clone https://github.com/<your-fork>/smooth-ofx.git
cd smooth-ofx

# OFX SDK is required; smooth-ae carries the Rust crate so it is also required
# when USE_RUST_CORE is on (default).
git submodule update --init --recursive
```

If you plan to skip the Rust core (`-DUSE_RUST_CORE=OFF`), the
`smooth-ae` submodule is technically optional, but recommended for
historical reference.

## 3. Platform-specific build

### 3.1 macOS — per-architecture release builds (recommended)

Distribution zips are shipped as separate single-arch builds. Build
each target by switching `CMAKE_OSX_ARCHITECTURES`:

```bash
# Apple Silicon (arm64)
cmake -S . -B build-macos-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos-arm64 --config Release

# Intel (x86_64)
cmake -S . -B build-macos-x86_64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos-x86_64 --config Release
```

Output:
`build-macos-<arch>/smooth.ofx.bundle/Contents/MacOS/smooth.ofx`

A Universal 2 bundle is also possible if you prefer one fat binary:

```bash
cmake -S . -B build-macos-universal \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos-universal --config Release
```

### 3.2 macOS — code signing for distribution

Ad-hoc sign for internal/test distribution, full Developer ID + notary
for public distribution:

```bash
# Ad hoc (test distribution)
codesign -fs - --deep --options runtime build-macos-arm64/smooth.ofx.bundle
xattr -cr build-macos-arm64/smooth.ofx.bundle

# Public distribution (requires Developer ID Application certificate)
codesign --deep --options runtime --timestamp \
  --sign "Developer ID Application: Your Name (TEAMID)" \
  build-macos-arm64/smooth.ofx.bundle

# Notarize (requires Apple ID with app-specific password set up via notarytool)
ditto -c -k --keepParent build-macos-arm64/smooth.ofx.bundle smooth-arm64.zip
xcrun notarytool submit smooth-arm64.zip --apple-id <id> --password <app-pass> \
  --team-id TEAMID --wait
xcrun stapler staple build-macos-arm64/smooth.ofx.bundle
```

Distribution zip layout:

```
smooth-1.6.0-macos-arm64.zip
├── smooth.ofx.bundle/
│   └── Contents/
│       ├── _CodeSignature/CodeResources
│       ├── Info.plist                       (auto-generated from dist/Info.plist.in)
│       └── MacOS/smooth.ofx                 (Mach-O thin per arch)
├── README.txt                               (dist/README-macOS-<arch>.txt)
└── RELEASE-NOTES.txt                        (dist/RELEASE-NOTES.txt)
```

For a step-by-step Windows handoff (Visual Studio install through
UAT) see [BUILD_HANDOFF_WINDOWS.md](BUILD_HANDOFF_WINDOWS.md). For the
Rocky Linux 9.5 equivalent see
[BUILD_HANDOFF_LINUX.md](BUILD_HANDOFF_LINUX.md). The sections below
remain the concise cross-platform reference.

### 3.3 Windows — MSVC release build (recommended for distribution)

Resolve and most other commercial OFX hosts ship MSVC-compiled binaries,
so production OFX plugins must match the MSVC ABI. Use Visual Studio
2019 or later.

```cmd
:: From a "x64 Native Tools Command Prompt for VS"
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64
cmake --build build-msvc --config Release
```

(Replace the generator name with whichever VS version you have, e.g.
`"Visual Studio 17 2022"`.)

Output: `build-msvc/Release/smooth.ofx`

This is a DLL with `.ofx` extension. For distribution, embed it in the
OFX bundle layout:

```
smooth-1.6.0-windows-x64.zip
└── smooth.ofx.bundle/
    └── Contents/
        ├── Info.plist                  (Windows hosts ignore but include for symmetry)
        └── Win64/smooth.ofx            (MSVC DLL)
```

You can use `cmake --install` (it stages directly to the
`OFX_INSTALL_DIR`-derived layout) or zip it manually.

For Authenticode signing (enterprise distribution), use
`signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /n "Your Cert" smooth.ofx`.

### 3.4 Windows — MSYS2 MinGW-w64 dev build (faster iteration, not for shipping)

MSYS2 MinGW-w64 is convenient for fast edit/build cycles but produces
binaries with the MinGW C++ ABI, which is **not compatible** with
Resolve / Fusion / etc. Treat MinGW builds as smoke tests only.

The MinGW path also does not currently link against the Rust core (the
CMake glue picks the MSVC Rust target unconditionally). Pass
`-DUSE_RUST_CORE=OFF` for MinGW:

```bash
# From an MSYS2 MINGW64 shell
cmake -S . -B build-mingw -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_RUST_CORE=OFF
cmake --build build-mingw
```

Output: `build-mingw/smooth.ofx`

You can run the Windows-only `host_smoke` test from MinGW:

```bash
build-mingw/host_smoke.exe build-mingw/smooth.ofx
```

### 3.5 Linux — release build

```bash
cmake -S . -B build-linux \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

Output: `build-linux/smooth.ofx`

Distribution layout:

```
smooth.ofx.bundle/
└── Contents/
    ├── Info.plist
    └── Linux-x86-64/smooth.ofx
```

Linux OFX hosts (Natron, Resolve Linux Studio) typically don't require
signing. Ship the bundle as a tarball or distro package.

## 4. CMake options reference

| Option            | Default | Effect                                                              |
|-------------------|---------|---------------------------------------------------------------------|
| `USE_RUST_CORE`   | `ON`    | Build & link `smooth_core` Rust crate (8-bit and 32-bit float paths). Off → C++ baseline only. |
| `BUILD_TESTS`     | `ON`    | Build the cross-platform `host_smoke` harness in `tests/`.          |
| `OFX_INSTALL_DIR` | OS-specific | Where `cmake --install` puts the bundle. Defaults: `/Library/OFX/Plugins` (macOS), `C:/Program Files/Common Files/OFX/Plugins` (Windows), `/usr/OFX/Plugins` (Linux). |

Other commonly-used CMake variables:
- `CMAKE_BUILD_TYPE` — `Release` for production, `RelWithDebInfo` for
  profiling.
- `CMAKE_OSX_ARCHITECTURES` — `arm64` / `x86_64` / `arm64;x86_64` (mac only).
- `CMAKE_OSX_DEPLOYMENT_TARGET` — `11.0` minimum.

## 5. Validation: `host_smoke`

`host_smoke` is a minimal OFX host harness that loads `smooth.ofx`,
runs `setHost → onLoad → describe → describeInContext → createInstance
→ render` for 8-bit, 16-bit, and 32-bit float, and prints aggregate
pixel statistics. Expected output for the synthetic 64×32 stripe test:

```
[host-smoke] [ 8bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [16bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [float] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] buildInfo = "1.6.0+<sha> / rust core 0.1.0+<smooth_core-sha>"
```

Usage:

```bash
# macOS
build-macos-arm64/host_smoke build-macos-arm64/smooth.ofx.bundle/Contents/MacOS/smooth.ofx

# Windows MSVC
build-msvc\Release\host_smoke.exe build-msvc\Release\smooth.ofx

# Linux
build-linux/host_smoke build-linux/smooth.ofx
```

### Diagnostic modes

```bash
# Bench: time render() on a parametric image size
SMOOTH_BENCH_SIZE=1920x1080 SMOOTH_BENCH_ITERS=20 host_smoke <smooth.ofx>

# Verify white-key tolerance (transparent option) across pixel layouts
SMOOTH_DIAG=transparent host_smoke <smooth.ofx>

# Sweep the range parameter to see threshold engagement
SMOOTH_DIAG=range host_smoke <smooth.ofx>
```

## 6. Build identity in the UI

Every build embeds an identity string surfaced as a read-only field in
the Effect Controls / Inspector under "build", e.g.:

```
1.6.0+c812c03 / rust core 0.1.0+a566908
```

Components:
- `1.6.0` — plugin version (from `project(smooth-ofx VERSION ...)`)
- `c812c03` — short git SHA of the smooth-ofx repo at configure time
- `+dirty` — appended if the working tree had uncommitted changes
- `cpp core` or `rust core <semver>+<smooth_core-sha>[+dirty]` — the
  algorithm path being used.

Use the build label during UAT to confirm the right binary is loaded.

## 7. Installation paths per platform

| Platform    | System-wide                                                                                    | User-local                              |
|-------------|------------------------------------------------------------------------------------------------|-----------------------------------------|
| macOS       | `/Library/OFX/Plugins/smooth.ofx.bundle/`                                                      | `~/Library/OFX/Plugins/smooth.ofx.bundle/` (host-dependent) |
| Windows     | `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\`                                 | `%APPDATA%\OFX\Plugins\smooth.ofx.bundle\` (host-dependent) |
| Linux       | `/usr/OFX/Plugins/smooth.ofx.bundle/` or `/opt/OFX/Plugins/smooth.ofx.bundle/`                 | `~/.OFX/Plugins/smooth.ofx.bundle/`     |

`cmake --install` writes to the `OFX_INSTALL_DIR` cache variable; the
default per platform matches the system-wide row.

## 8. Troubleshooting

**`smooth_core Cargo.toml not found at …`**
Run `git submodule update --init smooth-ae` (the Rust crate lives there).

**`cargo not found in PATH`**
Install Rust via [rustup.rs](https://rustup.rs), or pass
`-DUSE_RUST_CORE=OFF` to skip Rust integration.

**Linking error about ABI mismatch on Windows**
You're mixing MinGW C++ with the MSVC Rust target. Either build the
whole stack with MSVC, or use `-DUSE_RUST_CORE=OFF` for MinGW.

**macOS: `lipo: can't open input file`**
The cargo step probably didn't produce a static lib for one of the
arches. Re-configure with the full per-arch CMake invocation; check
that `rustup target list --installed` includes both
`aarch64-apple-darwin` and `x86_64-apple-darwin`.

**Resolve doesn't show the plugin**
1. Confirm bundle ID is `jp.loilo.smooth` and not duplicated by another
   plugin.
2. Clear quarantine: `sudo xattr -dr com.apple.quarantine
   /Library/OFX/Plugins/smooth.ofx.bundle`.
3. Restart Resolve fully (it caches plugin metadata).

**`host_smoke` reports `dlopen error=…` on macOS**
Architecture mismatch between the harness and the plugin. Build both
with the same `CMAKE_OSX_ARCHITECTURES`.

## 9. GPU core (`smooth_gpu`) — cross-platform notes

The GPU acceleration path is being prototyped via the `wgpu` crate
(`rust/smooth_gpu/`), still in Phase A scaffolding as of 2026-05-06.
The crate is **not yet wired into the OFX build**; `USE_RUST_CORE`
remains the only Rust path that ships in 1.6.0. These notes capture
things to watch when the Windows / Linux machines pick up the work:

- **Backends selected at build time**: wgpu 23 default features pull in
  Metal (macOS), DX12 (Windows), and Vulkan (Linux/Windows) backends
  into one staticlib. Selection happens at runtime via
  `wgpu::Backends::PRIMARY`.
- **Static lib size**: `libsmooth_gpu.a` measures ≈ 9.3 MB on macOS
  arm64 (vs ≈ 5.7 MB for `libsmooth_core.a`). Expect the per-arch OFX
  bundle to grow by roughly 4–5 MB once `smooth_gpu` is linked in.
- **Linux runtime**: the OFX bundle does **not** ship a Vulkan
  loader/driver. The host system needs a Vulkan ICD (e.g.
  `mesa-vulkan-drivers`, `vulkan-tools`) installed for `smooth_gpu`
  to acquire an adapter. Document the install-time prerequisite in
  `dist/README-linux-*.txt` when the Linux distribution path is built.
- **Windows runtime**: DX12 is provided by the OS (10+); no extra
  install on the user's side. wgpu can fall back to Vulkan if a
  Vulkan driver is also present, but this should not be relied on.
- **Rust target triples for `smooth_gpu`** match `smooth_core`'s — see
  the table at the top of this file. CI / cross-builds need
  `rustup target add` for each target you build on a given host.
- **Headless CI**: `smooth_gpu`'s integration test
  (`init_can_acquire_a_device_on_this_host`) actually spins up a GPU
  device. On Linux runners without a usable GPU, set
  `SMOOTH_GPU_SKIP_INIT_TEST=1` to skip it; the unit tests for
  `smooth_gpu_version()` and `smooth_gpu_build_id()` always run.
- **MinGW path**: same caveat as for `smooth_core` — wgpu links MSVC
  symbols on Windows, so the MSYS2 MinGW dev path can't link
  `smooth_gpu` either. Plan to gate it off when MSVC is not detected,
  same way `USE_RUST_CORE` is treated.
- **`USE_RUST_CORE` and `USE_GPU_CORE` are mutually exclusive**:
  both staticlibs embed the Rust runtime (`std::panicking::EMPTY_PANIC`,
  `_rust_eh_personality`, …). Linking them side-by-side fails at the
  C++ link step with duplicate-symbol errors. CMake refuses the
  combination with `FATAL_ERROR`. `smooth_core`'s `crate-type` is
  `staticlib`-only, so it cannot be embedded as a Rust path-dep into
  `smooth_gpu` to dedup the runtime — modifying that would change
  the AE-side submodule. Until that lands upstream the runtime story
  for `USE_GPU_CORE=ON` is: GPU path on the new prototype, CPU
  fallback through the existing C++ implementations (the same code
  that ships when `USE_RUST_CORE=OFF`).

When a real algorithm kernel lands, this section will get a "GPU build
matrix" entry; until then, treat it as advisory.

## 10. Cross-platform CI sketch (future work)

A minimal GitHub Actions matrix covering all three platforms:

```yaml
strategy:
  matrix:
    include:
      - os: macos-14         # arm64
        cmake-arch: arm64
      - os: macos-13         # x86_64
        cmake-arch: x86_64
      - os: windows-2022
        cmake-arch: x64
      - os: ubuntu-22.04
        cmake-arch: ""
```

Steps per matrix entry: install Rust (`actions-rs/toolchain`) →
`git submodule update --init --recursive` → CMake configure with the
right flags → build → run `host_smoke` → upload the bundle/zip as
a build artefact.

This is not implemented yet; documented here as a starting point.
