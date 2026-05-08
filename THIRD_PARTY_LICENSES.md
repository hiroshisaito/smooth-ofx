# Third-Party Licenses and Notices

This file lists the third-party components that are linked into, or required
to build, the smooth-ofx OpenFX plugin binaries.

The smooth-ofx project itself is distributed under the **Apache License,
Version 2.0**, inherited from the upstream LOILO smooth After Effects plugin
(<https://github.com/loilo-inc/smooth>) via the AE-side maintenance fork
(<https://github.com/hiroshisaito/smooth>). See `LICENSE` for the project
license text. Include both `LICENSE` and this file with source and binary
redistributions.

This inventory was verified on 2026-05-07 from:

- `include/openfx/LICENSE.md` (OpenFX SDK 1.5.1 submodule)
- The CPU Rust path inventory at `smooth-ae/THIRD_PARTY_LICENSES.md`
  (kept in sync with `smooth-ae/rust/smooth_core/Cargo.lock`)
- `cargo metadata --locked --manifest-path rust/smooth_gpu/Cargo.toml`
- `cargo tree --locked --manifest-path rust/smooth_gpu/Cargo.toml
   --target aarch64-apple-darwin --edges normal,no-proc-macro`
- `cargo tree --locked --manifest-path rust/smooth_gpu/Cargo.toml
   --target x86_64-apple-darwin --edges normal,no-proc-macro`

All four shipping targets — macOS arm64, macOS x86_64, Windows x64, and
Linux x86-64 — link the same `smooth_core` Rust crate set (Cargo.lock is
shared across targets). The macOS / Windows / Linux platform-specific
runtime libraries (system `libc++` / `libSystem` on macOS, MSVC CRT +
`ntdll` / `userenv` / `ws2_32` / `dbghelp` on Windows, glibc + libstdc++
on Linux) are part of the host OS, not redistributed by this project.

## Apache-2.0 Compatibility Summary

All third-party components linked into the smooth-ofx plugin binaries are
licensed under permissive terms:
- **MIT**, **Apache-2.0**, **BSD-3-Clause**, **ISC**, **Zlib**, **CC0-1.0**,
  **Unlicense**, or dual/multi-license expressions composed of those.
- Build-time-only dependencies (proc macros, build scripts) carry the same
  set, plus `Unicode-3.0` for the `unicode-ident` Unicode data tables.
- **No GPL, LGPL, AGPL, or MPL dependency was found** in the runtime,
  build-time, or transitive dependency set.

Based on this dependency set, smooth-ofx can be distributed as an Apache-2.0
project, provided the third-party copyright and license notices in this file
are preserved.

The shipping production binaries (`USE_RUST_CORE=ON`, `USE_GPU_CORE=OFF` —
the default) and the optional GPU prototype binaries (`USE_GPU_CORE=ON`)
carry different dep sets; both are documented below.

## 1. OpenFX SDK (header-only, both build configurations)

The OpenFX 1.5.1 SDK is included as the submodule `include/openfx`. Only the
header files (`include/openfx/include/*.h`) are required to compile a smooth-
ofx plugin; no OpenFX library or DLL is linked or shipped. Inline functions
and macros from the headers may end up in the compiled plugin binary, so the
BSD-3-Clause notice below MUST be preserved when distributing source or
binary builds.

| Component | Version | License |
| --- | --- | --- |
| OpenFX SDK | 1.5.1 (submodule pin) | BSD-3-Clause |

```
BSD 3-Clause License

Copyright (c) 2025, OpenFX and contributors to the OpenFX project

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## 2. CPU Rust Core — `smooth_core` (shipping default, `USE_RUST_CORE=ON`)

These crates are linked into the production plugin binaries on every
shipping target — `dist/smooth-1.6.0-macos-{arm64,x86_64}.zip`,
`dist/smooth-1.6.0-windows-x64.zip`, and
`dist/smooth-1.6.0-linux-x86-64.tar.gz`. License expressions are
taken from each Cargo package's metadata. This list mirrors the upstream
`smooth-ae/THIRD_PARTY_LICENSES.md`; refer to that file for the proc-macro
and build-script crates that are not linked into the plugin binary.

| Package | Version | License | Notice holder / project |
| --- | --- | --- | --- |
| crossbeam-deque | 0.8.6  | MIT OR Apache-2.0 | crossbeam-rs project |
| crossbeam-epoch | 0.9.18 | MIT OR Apache-2.0 | crossbeam-rs project |
| crossbeam-utils | 0.8.21 | MIT OR Apache-2.0 | crossbeam-rs project |
| either          | 1.15.0 | MIT OR Apache-2.0 | bluss |
| rayon           | 1.12.0 | MIT OR Apache-2.0 | rayon-rs project |
| rayon-core      | 1.13.0 | MIT OR Apache-2.0 | rayon-rs project |

## 3. Optional GPU Prototype — `smooth_gpu` (opt-in, `USE_GPU_CORE=ON`)

The `smooth_gpu` crate (`rust/smooth_gpu/`) is **not built by default and not
included in the shipping `smooth-1.6.0-macos-*.zip` binaries**. It is only
relevant when an end user (or a downstream packager) builds with
`-DUSE_GPU_CORE=ON`. The list below is the full runtime dependency set
linked into a `USE_GPU_CORE=ON` macOS plugin binary.

The list was generated from
`cargo tree --locked --target aarch64-apple-darwin --edges normal,no-proc-macro`;
the x86_64 tree is identical. Future Windows / Linux GPU-prototype builds
will introduce additional crates (e.g. `ash`, `windows-sys`,
`gpu-allocator`); those will be appended when those targets are exercised.

| Package | Version | License | Notice holder / project |
| --- | --- | --- | --- |
| arrayvec            | 0.7.6        | MIT OR Apache-2.0          | bluss |
| bit-set             | 0.8.0        | Apache-2.0 OR MIT          | contain-rs |
| bit-vec             | 0.8.0        | Apache-2.0 OR MIT          | contain-rs |
| bitflags            | 1.3.2        | MIT OR Apache-2.0          | bitflags-rs |
| bitflags            | 2.11.1       | MIT OR Apache-2.0          | bitflags-rs |
| block               | 0.1.6        | MIT                        | Steven Sheldon |
| bytemuck            | 1.25.0       | Zlib OR Apache-2.0 OR MIT  | Lokathor |
| cfg-if              | 1.0.4        | MIT OR Apache-2.0          | rust-lang/cfg-if |
| codespan-reporting  | 0.11.1       | Apache-2.0                 | brendanzab |
| core-foundation     | 0.9.4        | MIT OR Apache-2.0          | servo/core-foundation-rs |
| core-foundation-sys | 0.8.7        | MIT OR Apache-2.0          | servo/core-foundation-rs |
| core-graphics-types | 0.1.3        | MIT OR Apache-2.0          | servo/core-foundation-rs |
| equivalent          | 1.0.2        | Apache-2.0 OR MIT          | indexmap-rs |
| foreign-types       | 0.5.0        | MIT/Apache-2.0             | sfackler |
| foreign-types-shared| 0.3.1        | MIT/Apache-2.0             | sfackler |
| hashbrown           | 0.17.0       | MIT OR Apache-2.0          | rust-lang/hashbrown |
| hexf-parse          | 0.2.1        | CC0-1.0                    | lifthrasiir |
| indexmap            | 2.14.0       | Apache-2.0 OR MIT          | indexmap-rs |
| libc                | 0.2.186      | MIT OR Apache-2.0          | rust-lang/libc |
| libloading          | 0.8.9        | ISC                        | nagisa |
| lock_api            | 0.4.14       | MIT OR Apache-2.0          | Amanieu/parking_lot |
| log                 | 0.4.29       | MIT OR Apache-2.0          | rust-lang/log |
| malloc_buf          | 0.0.6        | MIT                        | Steven Sheldon |
| metal               | 0.29.0       | MIT OR Apache-2.0          | gfx-rs |
| naga                | 23.1.0       | MIT OR Apache-2.0          | gfx-rs/wgpu |
| objc                | 0.2.7        | MIT                        | Steven Sheldon |
| once_cell           | 1.21.4       | MIT OR Apache-2.0          | matklad |
| parking_lot         | 0.12.5       | MIT OR Apache-2.0          | Amanieu/parking_lot |
| parking_lot_core    | 0.9.12       | MIT OR Apache-2.0          | Amanieu/parking_lot |
| pollster            | 0.4.0        | Apache-2.0/MIT             | zesterer |
| profiling           | 1.0.18       | MIT OR Apache-2.0          | aclysma |
| raw-window-handle   | 0.6.2        | MIT OR Apache-2.0 OR Zlib  | rust-windowing |
| rustc-hash          | 1.1.0        | Apache-2.0/MIT             | rust-lang/rustc-hash |
| scopeguard          | 1.2.0        | MIT OR Apache-2.0          | bluss |
| smallvec            | 1.15.1       | MIT OR Apache-2.0          | servo/rust-smallvec |
| static_assertions   | 1.1.0        | MIT OR Apache-2.0          | nvzqz |
| termcolor           | 1.4.1        | Unlicense OR MIT           | BurntSushi |
| thiserror           | 1.0.69       | MIT OR Apache-2.0          | dtolnay |
| unicode-width       | 0.1.14       | MIT OR Apache-2.0          | unicode-rs |
| unicode-xid         | 0.2.6        | MIT OR Apache-2.0          | unicode-rs |
| wgpu                | 23.0.1       | MIT OR Apache-2.0          | gfx-rs/wgpu |
| wgpu-core           | 23.0.1       | MIT OR Apache-2.0          | gfx-rs/wgpu |
| wgpu-hal            | 23.0.1       | MIT OR Apache-2.0          | gfx-rs/wgpu |
| wgpu-types          | 23.0.0       | MIT OR Apache-2.0          | gfx-rs/wgpu |

## 4. SDKs, Toolchains, Test Dependencies, and Local Tools

Apple Xcode and the macOS SDK, Microsoft Visual Studio and the Windows SDK,
the DaVinci Resolve OpenFX host, and the Rust toolchain (`cargo`, `rustc`,
target standard libraries) are SDK / toolchain dependencies governed by
their respective vendor or upstream terms. They are required at build time
or at runtime via the OS / host application; smooth-ofx source and binary
distributions must not redistribute those SDKs.

The OFX host smoke harness (`tests/host_smoke.cpp`) is shipped only with
the source tree and is not part of the plugin binary distribution.

If Cargo dependencies are vendored, or if a new platform target is added
(Windows MSVC, Linux x86_64, etc.), regenerate this file from the
target-specific `cargo metadata` / `cargo tree` output before release.

## License Text Appendices

The full text of the Apache License, Version 2.0 is in the project-root
`LICENSE` file.

For each non-Apache permissive license that appears above, the canonical
notice text is reproduced below. Where a package's license expression
includes `MIT`, the applicable copyright holders are the notice holders,
package authors, or copyright notices included in the corresponding package
source; preserve those notices alongside the MIT permission notice.

### MIT Permission Notice

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ISC License (libloading)

```
Copyright © Simonas Kazlauskas

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
```

### zlib License (bytemuck, raw-window-handle)

```
This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

3. This notice may not be removed or altered from any source distribution.
```

### Unlicense (termcolor when used under Unlicense)

```
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute
this software, either in source code form or as a compiled binary, for any
purpose, commercial or non-commercial, and by any means.

In jurisdictions that recognize copyright laws, the author or authors of
this software dedicate any and all copyright interest in the software to
the public domain. We make this dedication for the benefit of the public
at large and to the detriment of our heirs and successors. We intend this
dedication to be an overt act of relinquishment in perpetuity of all
present and future rights to this software under copyright law.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

For more information, please refer to <http://unlicense.org/>
```

### CC0 1.0 Universal (hexf-parse)

```
Creative Commons Legal Code

CC0 1.0 Universal

CREATIVE COMMONS CORPORATION IS NOT A LAW FIRM AND DOES NOT PROVIDE LEGAL
SERVICES. DISTRIBUTION OF THIS DOCUMENT DOES NOT CREATE AN ATTORNEY-CLIENT
RELATIONSHIP. CREATIVE COMMONS PROVIDES THIS INFORMATION ON AN "AS-IS" BASIS.
CREATIVE COMMONS MAKES NO WARRANTIES REGARDING THE USE OF THIS DOCUMENT OR
THE INFORMATION OR WORKS PROVIDED HEREUNDER, AND DISCLAIMS LIABILITY FOR
DAMAGES RESULTING FROM THE USE OF THIS DOCUMENT OR THE INFORMATION OR WORKS
PROVIDED HEREUNDER.

The person who associated a work with this deed has dedicated the work to
the public domain by waiving all of his or her rights to the work worldwide
under copyright law, including all related and neighboring rights, to the
extent allowed by law. You can copy, modify, distribute and perform the
work, even for commercial purposes, all without asking permission.

For the full text of CC0 1.0 Universal, see
<https://creativecommons.org/publicdomain/zero/1.0/legalcode>.
```

### Apache-2.0

For packages whose license expression includes Apache-2.0, see the Apache
License, Version 2.0 text in the project-root `LICENSE`.
