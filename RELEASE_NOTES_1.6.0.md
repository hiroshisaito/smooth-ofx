# smooth-ofx 1.6.0 — Release Notes

[日本語版](RELEASE_NOTES_1.6.0_ja.md)

**Release date**: 2026-05-05 (initial), refreshed 2026-05-07
**Upstream alignment**: AE-side fork
[hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) v1.6.0
**Output compatibility**: byte-identical to 1.4.0 across all three pixel
depths (verified via `host_smoke` PPM cmp on macOS / Windows)
**Production performance**: 2.5×–3.1× faster than 1.4.0 on multi-core
hosts at 1080p / 4K (8-bit and 32-bit float paths)

---

## Headline changes

- **Algorithm acceleration via Rust core**. The 8-bit and 32-bit float
  paths now run through the shared `smooth_core` crate
  (`smooth-ae/rust/smooth_core` v0.1.0), parallelised with rayon strip
  workers. Output stays byte-identical to 1.4.0; wall-clock drops to
  about a third of baseline on an 8-core macOS host.
- **Build identity surfaced in Effect Controls**. A read-only `build`
  label in the Inspector shows the plugin version + git short SHA
  (with `+dirty` flag) + the Rust core build identity, so operators can
  confirm at a glance which build is loaded during UAT.
- **Cross-platform build documentation**. New top-level
  [BUILDING.md](BUILDING.md) / [BUILDING_ja.md](BUILDING_ja.md) gives
  a single document covering macOS arm64 + x86_64, Windows MSVC, and
  Linux, including signing, validation, and troubleshooting. The
  README's build sections are now a quick-start that links to BUILDING.
- **Cross-platform `host_smoke` harness**. The minimal OFX host that
  drives `smooth.ofx` for byte-equality and benchmark testing is no
  longer Windows-only — the same binary runs on macOS / Linux via
  `dlopen`, with optional bench mode (`SMOOTH_BENCH_SIZE=WxH
  SMOOTH_BENCH_ITERS=N`) and diagnostic modes
  (`SMOOTH_DIAG=transparent` / `range`).

## Performance

Wall-clock measurements on an 8 physical-core x86_64 host comparing the
1.6.0 Rust core build to the 1.4.0 C++-only baseline:

| Image          | 8-bit                              | 32-bit float                       |
|----------------|------------------------------------|------------------------------------|
| 1920 × 1080    | 91.7 ms → **36.2 ms** (2.53×)      | 130.2 ms → **48.5 ms** (2.68×)     |
| 3840 × 2160    | 365.8 ms → **118.8 ms** (3.08×)    | 507.5 ms → **185.9 ms** (2.73×)    |

16-bit integer is unchanged — still on the C++ implementation. The
Rust core uses AE's 0x8000 max-value convention; OFX uses 0xFFFF, so a
direct port would be lossy. The 16-bit path will move to Rust once the
crate gains an OFX-flavour 16bpc max value.

## Bug fixes carried into 1.6.0 from the 1.4.0 hotfix line

- **Black-pixel artefact on lines** in 16-bit / 32-bit float color
  spaces. Root cause: `Link8SquareExecute`'s `int sum_color[4]`
  truncated every float pixel value to zero. Fixed by switching the
  accumulator to `PixelRangeType<PixelType>::type` (unsigned int for
  8/16 int, float for 32-bit float).
- **White-fringe artefact on transparent edges** under DaVinci
  Resolve when the `transparent` option was on. Fixed by
  unpremultiplying input and re-premultiplying output around the
  smoothing pass.
- **Transparent option not detecting white in 16-bit / float color
  spaces**. The white-key check now accepts `0xFFFF` *and* `0x8000`
  for 16-bit (covering both OFX and AE conventions) and uses a
  `|v − 1.0| < 0.005` tolerance for float. Snap-to-1.0 happens for
  the GPU prototype path before the kernel runs.
- **Range slider had no visible effect** at typical slider positions.
  The slider's display max widened from 10 to 100 so the full
  effective threshold is reachable.
- **Build identity field showed the static label twice in the
  Resolve Inspector**. Switched the read-only display from
  `kOfxParamStringIsLabel` to a disabled `kOfxParamStringIsSingleLine`,
  which is the convention other OFX plugins use for build / version
  rows.

## Distribution

Per-architecture single-arch zips, ad hoc signed (no notarization yet —
clear quarantine before loading):

- `dist/smooth-1.6.0-macos-arm64.zip` — Apple Silicon (ad hoc signed)
- `dist/smooth-1.6.0-macos-x86_64.zip` — Intel (ad hoc signed)
- `dist/smooth-1.6.0-windows-x64.zip` — Windows x64 (MSVC, unsigned;
  added 2026-05-08 with build id `1.6.0+c23db4c` after the canonical
  DaVinci Resolve 5-point UAT passed on Windows 11. See
  [BUILD_HANDOFF_WINDOWS.md](BUILD_HANDOFF_WINDOWS.md) for the
  reproducible recipe.)
- Linux build is queued for separate work; see [BUILDING.md](BUILDING.md)
  § 3.5 and [BUILD_HANDOFF_LINUX.md](BUILD_HANDOFF_LINUX.md). § 10 of
  BUILDING sketches a CI matrix.

```bash
# Install (macOS Intel example; replace with -arm64 on Apple Silicon)
sudo bash -c 'rm -rf /Library/OFX/Plugins/smooth.ofx.bundle && cp -R /path/to/smooth.ofx.bundle /Library/OFX/Plugins/ && xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle'
```

Each archive ships `smooth.ofx.bundle` + a per-arch `README.txt` + the
shared `RELEASE-NOTES.txt`. SHA-256 digests are alongside as
`*.zip.sha256`.

## Experimental: GPU prototype (`USE_GPU_CORE`, opt-in)

A `smooth_gpu` crate has been added under `rust/smooth_gpu/` as a
prototype for cross-platform GPU acceleration via [wgpu](https://wgpu.rs).
**Not built by default; not in the distributed binaries.**

- Build with `-DUSE_GPU_CORE=ON -DUSE_RUST_CORE=OFF` (the two are
  mutually exclusive at link time today; see BUILDING § 9 for the
  Rust-runtime duplicate-symbol explanation).
- WGSL kernels for `preprocess` (both AE and OFX byte layouts),
  `mode_flg` detection, and `link8_square` centre handler — each
  verified byte-identical to a CPU reference.
- Hybrid render path: 8bpc preprocess on GPU, the rest of the
  algorithm on the C++ baseline. Toggle on/off at runtime via the
  new `GPU` checkbox in Effect Controls.
- `host_smoke` adds `SMOOTH_BENCH_TOGGLE_GPU=1` for side-by-side
  GPU/CPU bench rows.
- **The prototype is currently *slower* than the shipping
  `USE_RUST_CORE=ON` configuration** — by about 22 ms / frame at
  1080p 8-bit — because the GPU upload/dispatch/readback round-trip
  outweighs the preprocess kernel's compute density on every-frame
  CPU↔GPU buffer hand-off. It is checked in as the architectural
  seam for two follow-up directions:
  - **(i)** make `smooth_core` emit `rlib` so it can be embedded into
    `smooth_gpu` and the link-time mutex disappears; then rayon-CPU
    and GPU kernels can coexist instead of swapping each other out.
  - **(ii)** wire OFX 1.5 `kOfxImageEffectActionRenderGPU` so the
    host hands us a GPU buffer directly and the round-trip cost
    vanishes.

Until one of those lands, the production path stays
`USE_RUST_CORE=ON, USE_GPU_CORE=OFF`.

## Versioning

The OFX port now tracks the AE-side fork's major / minor numbers; we
do not maintain an independent version line. The 1.5.x series was
AE-side only and was skipped by the OFX port; 1.6.0 realigns both
sides. Patch / hotfix releases on top of 1.6.0 will follow the same
upstream tracking convention.

## Acknowledgments

- Original **smooth** plugin: Kouji Sugiyama (杉山浩二, 2004), open-
  sourced by [LOILO Inc.](https://loilo.tv/) at
  <https://github.com/loilo-inc/smooth> under Apache 2.0.
- AE-side modernisation fork
  ([hiroshisaito/smooth](https://github.com/hiroshisaito/smooth)) —
  Rust core, MFR, 32bpc float, build-id UI, and the cross-platform
  testing harness.
- OpenFX SDK 1.5.1 from the
  [Academy Software Foundation](https://www.openeffects.org/).

See [LICENSE](LICENSE) for the full text and [README.md](README.md)
for the project overview.
