# smooth-ofx 1.6.0 — Rocky Linux 9.5 build handoff

[日本語版](BUILD_HANDOFF_LINUX_ja.md) ・ [BUILDING.md](BUILDING.md)
(reference)

This document is a step-by-step handoff for building, testing, and
packaging `smooth.ofx` 1.6.0 on a Rocky Linux 9.5 host after the
macOS release was finalised. The instructions also work, with minor
path adjustments, on other RHEL-9 derivatives (AlmaLinux 9, Oracle
Linux 9).

**Assumptions**: Rocky Linux 9.5 x86_64, sudo rights, internet
access, ~5 GB free disk (Rust + build artefacts).

---

## 1. Toolchain installation

All install commands below need root or `sudo`.

### 1.1 Base build packages

```bash
sudo dnf install -y gcc gcc-c++ make git cmake
```

Verify:

```bash
gcc --version       # 11.5.0+ (Rocky 9.5 default = 11.5)
cmake --version     # 3.26+   (Rocky 9.5 default = 3.26.5)
git --version
```

If `cmake --version` is below 3.20, enable the CRB (formerly
PowerTools) repository and reinstall:

```bash
sudo dnf config-manager --set-enabled crb
sudo dnf install -y cmake
```

### 1.2 Rust toolchain

Install via rustup as the unprivileged user (the distro's Rust may
lag behind):

```bash
# As the unprivileged user — no sudo
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
source "$HOME/.cargo/env"

cargo --version            # 1.94.0+ expected
rustc --version
rustup target list --installed | grep linux
# → x86_64-unknown-linux-gnu must be present
```

(Future GPU prototype builds with `USE_GPU_CORE=ON` need
`mesa-vulkan-drivers vulkan-loader vulkan-tools` for wgpu's Vulkan
backend; not needed for the production CPU path.)

---

## 2. Clone and submodule init

```bash
mkdir -p ~/src
cd ~/src
git clone https://github.com/<your-fork>/smooth-ofx.git
cd smooth-ofx
git submodule update --init --recursive
```

Confirm `include/openfx/include/ofxImageEffect.h` and
`smooth-ae/rust/smooth_core/Cargo.toml` exist.

---

## 3. Build (production path = `USE_RUST_CORE=ON`, default)

### 3.1 CMake configure

```bash
cmake -S . -B build-linux \
  -DCMAKE_BUILD_TYPE=Release
```

Watch for:

```
-- smooth-ofx build id: 1.6.0+<sha>
-- smooth_core: rust targets = x86_64-unknown-linux-gnu
-- smooth_core: linking .../build-linux/cargo-target/x86_64-unknown-linux-gnu/release/libsmooth_core.a
-- Configuring done
-- Generating done
```

The build-id sha must match `git rev-parse --short HEAD`. If it
shows `+dirty`, commit and re-configure.

### 3.2 Compile

```bash
cmake --build build-linux --config Release
```

Output: `build-linux/smooth.ofx` (~600 KB ELF shared object with
the `.ofx` extension).

```bash
file build-linux/smooth.ofx
# → ELF 64-bit LSB shared object, x86-64, dynamically linked
ldd build-linux/smooth.ofx
# → libstdc++ / libm / libgcc_s / libpthread / libc only
```

### 3.3 Verify with `host_smoke`

```bash
cmake --build build-linux --config Release --target host_smoke
build-linux/host_smoke build-linux/smooth.ofx
```

Expected output:

```
[host-smoke] kOfxActionLoad -> 0
[host-smoke] kOfxActionDescribe -> 0
[host-smoke] kOfxImageEffectActionDescribeInContext -> 0
    param: name=buildInfo      type=OfxParamTypeString label=build
[host-smoke] buildInfo = "1.6.0+<sha> / rust core 0.1.0+<smooth-ae-sha>"
[host-smoke] [ 8bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [16bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [float] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] DONE
```

All three depths must report `562/562/924/2048` — same numbers as
macOS / Windows 1.6.0.

---

## 4. Packaging

### 4.1 OFX bundle layout

```bash
DIST=dist/linux-x86-64
rm -rf "$DIST"
mkdir -p "$DIST/smooth.ofx.bundle/Contents/Linux-x86-64"
cp build-linux/smooth.ofx "$DIST/smooth.ofx.bundle/Contents/Linux-x86-64/"
```

OFX Linux bundle layout:

```
smooth.ofx.bundle/
└── Contents/
    └── Linux-x86-64/
        └── smooth.ofx     (ELF .so)
```

(No `Info.plist`; that file is macOS-only.)

### 4.2 Tarball + SHA-256

```bash
( cd "$DIST" && tar czf ../smooth-1.6.0-linux-x86-64.tar.gz smooth.ofx.bundle )
sha256sum dist/smooth-1.6.0-linux-x86-64.tar.gz | tee dist/smooth-1.6.0-linux-x86-64.tar.gz.sha256
ls -la dist/smooth-1.6.0-linux-x86-64.tar.gz*
```

(For zip: `( cd "$DIST" && zip -r ../smooth-1.6.0-linux-x86-64.zip smooth.ofx.bundle )`.)

### 4.3 Signing (typically not required on Linux)

Linux OFX hosts (DaVinci Resolve Studio Linux, Natron, etc.) don't
require code signing. Distribute the tarball as-is.

---

## 5. Install and UAT

### 5.1 Copy into the OFX plugin directory

OFX standard system-wide path:

```bash
sudo rm -rf /usr/OFX/Plugins/smooth.ofx.bundle
sudo mkdir -p /usr/OFX/Plugins
sudo cp -R "$DIST/smooth.ofx.bundle" /usr/OFX/Plugins/
```

For DaVinci Resolve Studio Linux, the Resolve-specific path may
also need the bundle:

```bash
sudo cp -R "$DIST/smooth.ofx.bundle" "/var/BlackmagicDesign/DaVinci Resolve/Support/OFX/Plugins/"
```

(Resolve's preferred plugin search path varies by version and
install method. `/usr/OFX/Plugins/` is the OFX standard;
Resolve may pick the bundle from its own Support tree first.)

### 5.2 UAT (canonical 5-point check, Resolve Linux Studio assumed)

Restart Resolve fully. Run the same canonical test as macOS / Windows:

| # | Check | Expectation |
|---|-------|-------------|
| 1 | Inspector "build" field | `1.6.0+<sha> / rust core 0.1.0+<smooth-ae-sha>` (no `+dirty`, not `cpp core`) |
| 2 | Apply Smooth on 8 / 16-bit int comp | Anti-aliased step edges, no black pixels on lines |
| 3 | **32-bit float comp + transparent ON** (key) | **No black pixels on lines** + white (1,1,1) becomes transparent + no crashes |
| 4 | 32-bit float comp + transparent OFF | Same output as 8/16bpc, white pixels stay opaque |
| 5 | 4K (3840×2160 or larger) render speed | Comparable to macOS 1.6.0 |

All five PASS → **Linux 1.6.0 build complete**.

(If Resolve Linux is not at hand, [Natron](https://natrongithub.github.io/)
is a free OFX host suitable for the same checks.)

---

## 6. Build report-back template (back to macOS team)

```text
Linux 1.6.0 build report
==========================
Date:         YYYY-MM-DD HH:MM TZ
Host:         Rocky Linux 9.5 (kernel 5.14.x), x86_64
Compiler:     gcc x.y.z
Rust:         rustc 1.x.y
CMake:        3.x.y
Build ID:     1.6.0+<sha>
host_smoke:   3 paths PASS (562/562/924)
tarball:      dist/smooth-1.6.0-linux-x86-64.tar.gz
sha256:       <hash>
UAT:          5/5 PASS  (or NG details)
OFX host:     DaVinci Resolve Studio x.y / Natron / etc.
Notes:        <if any>
```

---

## 7. Troubleshooting

### `cmake --version` < 3.20
→ Enable CRB and reinstall (§ 1.1).

### `cargo: command not found` in a new shell
→ `~/.cargo/env` not sourced from `~/.bashrc`:
```bash
echo 'source "$HOME/.cargo/env"' >> ~/.bashrc
source ~/.bashrc
```

### CMake configure errors: `OFX SDK headers not found`
→ Submodule not initialised:
```bash
git submodule update --init --recursive
```

### `cargo build` runs for a long time
→ The shipping path (`USE_RUST_CORE=ON`) compiles rayon and its
  transitive deps crate-by-crate on the first run (1–2 minutes).
  Adding `USE_GPU_CORE=ON` pulls the full wgpu tree on top of that
  (4–5 minutes). Subsequent builds are seconds.

### Resolve / Natron doesn't show Smooth
1. Bundle present at `/usr/OFX/Plugins/smooth.ofx.bundle/Contents/Linux-x86-64/smooth.ofx`?
2. Permissions: `ls -l` shows `-rwxr-xr-x` (executable bit set)?
3. Restart the host fully.
4. SELinux enforcing? Check `getenforce`; if needed,
   `sudo restorecon -Rv /usr/OFX/Plugins/smooth.ofx.bundle`.

### `ldd` reports `not found` for some library
→ glibc / libstdc++ mismatch. Building on Rocky 9.5 (glibc 2.34)
  produces binaries that should run on any RHEL-9-class host with
  glibc 2.34+. For older targets, rebuild on that target.

### host_smoke doesn't print `pure0=562`
→ Output diverges from macOS / Windows 1.6.0 → real algorithm/ABI
  mismatch. Share `uname -a` and CMake configure output with the
  macOS team.

---

## 8. Pointers

- [BUILDING.md](BUILDING.md) — cross-platform reference
- [CHANGELOG.md](CHANGELOG.md) — what changed in 1.6.0
- [RELEASE_NOTES_1.6.0.md](RELEASE_NOTES_1.6.0.md) — release notes
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) — license inventory
