# smooth-ofx 1.6.0 — Windows 11 build handoff

[日本語版](BUILD_HANDOFF_WINDOWS_ja.md) ・ [BUILDING.md](BUILDING.md)
(reference)

This document is a step-by-step handoff for building, testing, and
packaging `smooth.ofx` 1.6.0 on a Windows 11 machine after the macOS
release was finalised. It is intentionally more verbose than the
reference [BUILDING.md](BUILDING.md) — copy-paste-able commands and
verification at every step.

**Assumptions**: Windows 11 64-bit, administrator rights, internet
access, ~30 GB free disk (Visual Studio included).

---

## 1. Toolchain installation

### 1.1 Visual Studio 2022 Community (required)

1. Get the installer from
   <https://visualstudio.microsoft.com/downloads/> (Community 2022).
2. In the Visual Studio Installer, pick the **Desktop development
   with C++** workload — that's all you need.
3. Verify the right pane includes at minimum:
   - MSVC v14x — your VS's C++ x64/x86 build tools (VS 2022 = v143,
     VS 2026 = v144)
   - Windows 11 SDK (**10.0.26100 or newer recommended**; 22621 also
     builds, but Rust 1.94's std stdio needs `__imp_NtWriteFile` /
     `__imp_RtlNtStatusToDosError`, which the CMake glue auto-links
     via `ntdll`/`userenv`/`ws2_32`/`dbghelp` — see the CHANGELOG's
     "Windows MSVC fixes" subsection)
   - C++ CMake tools for Windows (CMake 3.27+ bundled)
4. Install. ~10 GB download, ~30 minutes.

### 1.2 Git for Windows (required)

1. Installer from <https://git-scm.com/download/win>.
2. Default options.
3. Verify with Git Bash → `git --version`.

### 1.3 Rust toolchain (required)

PowerShell as **administrator**:

```powershell
Invoke-WebRequest -Uri https://win.rustup.rs/x86_64 -OutFile rustup-init.exe
.\rustup-init.exe -y --default-host x86_64-pc-windows-msvc --default-toolchain stable
```

Open a fresh PowerShell window and verify:

```powershell
cargo --version
rustc --version
rustup target list --installed
```

The list should include `x86_64-pc-windows-msvc`.

---

## 2. Clone and submodule init

PowerShell (no admin needed):

```powershell
mkdir C:\src
cd C:\src
git clone https://github.com/<your-fork>/smooth-ofx.git
cd smooth-ofx
git submodule update --init --recursive
```

Confirm `include\openfx\include\ofxImageEffect.h` and
`smooth-ae\rust\smooth_core\Cargo.toml` exist.

---

## 3. Build (production path = `USE_RUST_CORE=ON`, default)

### 3.1 CMake configure

Pick the generator that matches your installed Visual Studio:

```powershell
# VS 2026 (recommended; verified at commit c23db4c)
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64

# VS 2022 — substitute the generator name
# cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
```

Watch for these lines near the end of configure output:

```
-- smooth-ofx build id: 1.6.0+<sha>
-- smooth_core: rust targets = x86_64-pc-windows-msvc
-- smooth_core: linking ...build-msvc/cargo-target/x86_64-pc-windows-msvc/release/libsmooth_core.lib
-- Configuring done
-- Generating done
```

The build-id sha must match `git rev-parse --short HEAD`. If it
shows `+dirty`, commit your changes and re-configure.

### 3.2 Compile

```powershell
cmake --build build-msvc --config Release
```

Output: `build-msvc\Release\smooth.ofx` (~600 KB DLL with the
`.ofx` extension).

### 3.3 Verify with `host_smoke`

```powershell
cmake --build build-msvc --config Release --target host_smoke
build-msvc\Release\host_smoke.exe build-msvc\Release\smooth.ofx
```

Expected:

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

All three depths must report `pure0=562 pureMax=562 intermediate=924
/ 2048` — same numbers as the macOS shipping build.

---

## 4. Packaging

### 4.1 Build the OFX bundle layout

```powershell
$dist = "dist\windows-x64"
Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$dist\smooth.ofx.bundle\Contents\Win64" | Out-Null
Copy-Item build-msvc\Release\smooth.ofx "$dist\smooth.ofx.bundle\Contents\Win64\"
```

OFX Windows bundle layout:

```
smooth.ofx.bundle\
└── Contents\
    └── Win64\
        └── smooth.ofx     (MSVC DLL)
```

(No `Info.plist` is needed for Windows bundles — that file is macOS-
only.)

### 4.2 Zip + SHA-256

```powershell
Compress-Archive -Path "$dist\smooth.ofx.bundle" -DestinationPath "dist\smooth-1.6.0-windows-x64.zip" -Force
Get-FileHash -Algorithm SHA256 dist\smooth-1.6.0-windows-x64.zip | Format-List | Out-File -FilePath dist\smooth-1.6.0-windows-x64.zip.sha256 -Encoding ASCII
Get-Content dist\smooth-1.6.0-windows-x64.zip.sha256
```

### 4.3 Authenticode signing (skip for internal testing)

Internal / test distributions can ship unsigned. Windows SmartScreen
may warn on first load — users can override via "More info → Run".

For public distribution you'll need an Authenticode (Developer ID
equivalent) certificate:

```powershell
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /f your-cert.pfx /p <password> build-msvc\Release\smooth.ofx
signtool verify /pa /v build-msvc\Release\smooth.ofx
```

---

## 5. Install and UAT

### 5.1 Install into the OFX plugin directory

PowerShell **as administrator**:

```powershell
$plug = "C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle"
if (Test-Path $plug) { Remove-Item -Recurse -Force $plug }
Copy-Item -Recurse "$dist\smooth.ofx.bundle" "C:\Program Files\Common Files\OFX\Plugins\"
```

Quit DaVinci Resolve completely, then restart.

### 5.2 UAT (canonical 5-point check)

Same canonical test that ran on macOS:

| # | Check | Expectation |
|---|-------|-------------|
| 1 | Inspector "build" field | `1.6.0+<sha> / rust core 0.1.0+<smooth-ae-sha>` (no `+dirty`, not `cpp core`) |
| 2 | Apply Smooth on 8 / 16-bit int comp | Anti-aliased step edges, no black pixels on lines |
| 3 | **32-bit float comp + transparent ON** (key) | **No black pixels on lines** + white (1,1,1) becomes transparent + no crashes |
| 4 | 32-bit float comp + transparent OFF | Same output as 8/16bpc, white pixels stay opaque |
| 5 | 4K (3840×2160 or larger) render speed | Comparable to macOS 1.6.0 (~120 ms/frame at 4K 8bpc) |

All five PASS → **Windows 1.6.0 build complete**.

---

## 6. Build report-back template (back to macOS team)

```text
Windows 1.6.0 build report
==========================
Date:         YYYY-MM-DD HH:MM TZ
Host:         Windows 11 (build xxx), x86_64
SDK:          Windows 11 SDK 10.0.xxxxx
Compiler:     MSVC v14x (VS 2022 17.x.x or VS 2026 18.x.x)
Rust:         rustc 1.x.y
Build ID:     1.6.0+<sha>
host_smoke:   3 paths PASS (562/562/924)
zip:          dist\smooth-1.6.0-windows-x64.zip
sha256:       <hash>
UAT:          5/5 PASS  (or NG details)
Notes:        <if any>
```

---

## 7. Troubleshooting

### `cmake -G "Visual Studio 17 2022"` says "generator not found"
→ The C++ CMake tools VS workload component is missing. Re-run the
  Visual Studio Installer.

### `cargo: command not found` after rustup install
→ Open a fresh PowerShell window. `cargo` and `rustc` are added to
  PATH only for new sessions.

### `cargo build` complains "linker `link.exe` not found"
→ Rust can't see MSVC's linker from a plain PowerShell. Run from
  the **"x64 Native Tools Command Prompt for VS 2022"** instead
  (Start menu → search).

### Submodule update fails with network errors
→ Behind a corporate proxy? `git config --global http.proxy
  http://proxy:port` (ask IT).

### Resolve doesn't list Smooth
1. Check the bundle is at
   `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\Contents\Win64\smooth.ofx`.
2. Fully quit Resolve and restart.
3. If Explorer's preview holds the file, sign out and back in to free
   the lock, then retry the copy.

### host_smoke doesn't print `pure0=562`
→ Output diverges from macOS 1.6.0 → real algorithm/ABI mismatch.
  Share the stack trace and CMake configure output with the macOS
  team.

---

## 8. Pointers

- [BUILDING.md](BUILDING.md) — cross-platform reference
- [CHANGELOG.md](CHANGELOG.md) — what changed in 1.6.0
- [RELEASE_NOTES_1.6.0.md](RELEASE_NOTES_1.6.0.md) — release notes
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) — license inventory
