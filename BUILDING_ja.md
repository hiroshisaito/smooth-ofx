# smooth-ofx ビルドガイド

[English BUILDING](BUILDING.md)

このガイドは smooth-ofx 1.6.0 以降を macOS / Windows / Linux でビルド
するための手順をまとめたものです。8-bit と 32-bit float の経路を
rayon で並列化する Rust core (`smooth_core`) 統合を含みます。

リリース品質のビルドはセクション 3 のプラットフォーム別レシピに
従ってください。Rust ツールチェーンが無い環境やスモーク用途では
`-DUSE_RUST_CORE=OFF` で C++ ベースラインのみのビルドに切り替え
られます。

## 1. ツールチェーン要件

| 項目 | macOS | Windows (本番) | Windows (開発) | Linux |
|---|---|---|---|---|
| C++ コンパイラ | Xcode CLT または Xcode (AppleClang) | MSVC 2019 以降 (VS 17 / 18) | MSYS2 MinGW-w64 g++ 12+ | gcc 9+ または clang 12+ |
| CMake | 3.20 以上 | 3.20 以上 | 3.20 以上 | 3.20 以上 |
| Generator | Unix Makefiles / Ninja | "Visual Studio 17/18 …" | Ninja または MSYS Makefiles | Unix Makefiles / Ninja |
| Rust | rustup (stable) | rustup (stable + MSVC) | 不要 (`-DUSE_RUST_CORE=OFF` 推奨) | rustup (stable) または ディストリビューション Rust |
| C++ 標準ライブラリ | libc++ | MSVC CRT | MSYS libstdc++ | libstdc++ |

### 想定する Rust ターゲットトリプル

CMake はプラットフォームと (macOS の場合) `CMAKE_OSX_ARCHITECTURES`
からトリプルを自動選択します:

| プラットフォーム | Rust ターゲット |
|---|---|
| macOS arm64 | `aarch64-apple-darwin` |
| macOS x86_64 | `x86_64-apple-darwin` |
| Windows x64 (MSVC) | `x86_64-pc-windows-msvc` |
| Linux x86_64 | `x86_64-unknown-linux-gnu` |

rustup でインストール:

```bash
# macOS
rustup target add aarch64-apple-darwin x86_64-apple-darwin

# Windows (PowerShell / cmd)
rustup target add x86_64-pc-windows-msvc

# Linux
rustup target add x86_64-unknown-linux-gnu
```

`smooth-ae/rust/smooth_core/rust-toolchain.toml` で `stable` チャネルが
ピン止めされているので、チャネル指定の手作業は不要です。

## 2. クローン後の初回セットアップ (全プラットフォーム共通)

```bash
git clone https://github.com/<your-fork>/smooth-ofx.git
cd smooth-ofx

# OFX SDK は必須。smooth-ae は Rust crate を含むので USE_RUST_CORE=ON
# (デフォルト) のときも必須。
git submodule update --init --recursive
```

Rust core を使わない場合 (`-DUSE_RUST_CORE=OFF`) は smooth-ae は技術的
には不要ですが、参照用に取得しておくことを推奨します。

## 3. プラットフォーム別ビルド

### 3.1 macOS — アーキ別リリースビルド (推奨)

配布 zip はアーキ別シングルアーキビルドとして用意します。
`CMAKE_OSX_ARCHITECTURES` を切り替えて それぞれビルド:

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

出力: `build-macos-<arch>/smooth.ofx.bundle/Contents/MacOS/smooth.ofx`

Universal 2 (fat binary) もそのまま生成可能:

```bash
cmake -S . -B build-macos-universal \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos-universal --config Release
```

### 3.2 macOS — 配布用署名

社内 / テスト配布はアドホック署名、公開配布は Developer ID + 公証:

```bash
# アドホック (テスト配布)
codesign -fs - --deep --options runtime build-macos-arm64/smooth.ofx.bundle
xattr -cr build-macos-arm64/smooth.ofx.bundle

# 公開配布 (Developer ID Application 証明書が必要)
codesign --deep --options runtime --timestamp \
  --sign "Developer ID Application: Your Name (TEAMID)" \
  build-macos-arm64/smooth.ofx.bundle

# 公証 (Apple ID + app 専用パスワードを notarytool に登録済みの前提)
ditto -c -k --keepParent build-macos-arm64/smooth.ofx.bundle smooth-arm64.zip
xcrun notarytool submit smooth-arm64.zip --apple-id <id> --password <app-pass> \
  --team-id TEAMID --wait
xcrun stapler staple build-macos-arm64/smooth.ofx.bundle
```

配布 zip 構成:

```
smooth-1.6.0-macos-arm64.zip
├── smooth.ofx.bundle/
│   └── Contents/
│       ├── _CodeSignature/CodeResources
│       ├── Info.plist                       (dist/Info.plist.in から自動生成)
│       └── MacOS/smooth.ofx                 (Mach-O thin、各アーキ単独)
├── README.txt                               (dist/README-macOS-<arch>.txt)
└── RELEASE-NOTES.txt                        (dist/RELEASE-NOTES.txt)
```

### 3.3 Windows — MSVC リリースビルド (配布版推奨)

DaVinci Resolve や Fusion など商用 OFX ホストは MSVC 製バイナリで
配布されているため、配布用 OFX プラグインは MSVC ABI に揃える必要が
あります。Visual Studio 2019 以降を使用:

```cmd
:: "x64 Native Tools Command Prompt for VS" を起動して
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64
cmake --build build-msvc --config Release
```

(ジェネレータ名はインストール済みの VS バージョンに合わせてください、
例: `"Visual Studio 17 2022"`)

出力: `build-msvc/Release/smooth.ofx`

これは `.ofx` 拡張子を持つ DLL です。配布時は OFX bundle 構造に
組み込みます:

```
smooth-1.6.0-windows-x64.zip
└── smooth.ofx.bundle/
    └── Contents/
        ├── Info.plist                  (Windows ホストは無視するが対称性のため同梱)
        └── Win64/smooth.ofx            (MSVC DLL)
```

`cmake --install` を使うと `OFX_INSTALL_DIR` 配下に正しい構造で
ステージングされます。手動で zip 化しても OK です。

エンタープライズ配布で Authenticode 署名が必要なら:
`signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /n "Your Cert" smooth.ofx`

### 3.4 Windows — MSYS2 MinGW-w64 開発ビルド (配布不可、開発用のみ)

MSYS2 MinGW-w64 は edit/build サイクルが速く便利ですが、生成される
バイナリは MinGW C++ ABI に依存し、Resolve / Fusion 等の MSVC ホストとは
**ABI 互換性がありません**。MinGW ビルドはスモークテスト専用と
割り切ってください。

また現状 MinGW 経路は Rust core とリンクできません (CMake のグルーは
無条件で MSVC Rust ターゲットを選ぶため)。MinGW では
`-DUSE_RUST_CORE=OFF` を指定:

```bash
# MSYS2 MINGW64 シェルから
cmake -S . -B build-mingw -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_RUST_CORE=OFF
cmake --build build-mingw
```

出力: `build-mingw/smooth.ofx`

MinGW でも `host_smoke.exe` を実行可能:

```bash
build-mingw/host_smoke.exe build-mingw/smooth.ofx
```

### 3.5 Linux — リリースビルド

```bash
cmake -S . -B build-linux \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

出力: `build-linux/smooth.ofx`

配布構造:

```
smooth.ofx.bundle/
└── Contents/
    ├── Info.plist
    └── Linux-x86-64/smooth.ofx
```

Linux OFX ホスト (Natron、Resolve Linux Studio) は通常署名不要です。
tarball もしくは distro パッケージとして配布してください。

## 4. CMake オプション一覧

| オプション | デフォルト | 効果 |
|---|---|---|
| `USE_RUST_CORE` | `ON` | `smooth_core` Rust crate をビルド & リンク (8-bit / 32-bit float 経路)。OFF にすると C++ ベースラインのみ |
| `BUILD_TESTS` | `ON` | `tests/` の cross-platform `host_smoke` ハーネスをビルド |
| `OFX_INSTALL_DIR` | OS 別 | `cmake --install` の bundle 配置先。デフォルト: `/Library/OFX/Plugins` (macOS), `C:/Program Files/Common Files/OFX/Plugins` (Windows), `/usr/OFX/Plugins` (Linux) |

その他よく使う CMake 変数:
- `CMAKE_BUILD_TYPE` — 本番は `Release`、プロファイリング時は `RelWithDebInfo`
- `CMAKE_OSX_ARCHITECTURES` — macOS のみ。`arm64` / `x86_64` / `arm64;x86_64`
- `CMAKE_OSX_DEPLOYMENT_TARGET` — `11.0` 以上推奨

## 5. 検証: `host_smoke`

`host_smoke` は最小 OFX ホストで、`smooth.ofx` を読み込み
`setHost → onLoad → describe → describeInContext → createInstance →
render` を 8-bit / 16-bit / 32-bit float に対して実行し、ピクセル統計を
集計するツールです。合成 64×32 ストライプ画像での期待出力:

```
[host-smoke] [ 8bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [16bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [float] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] buildInfo = "1.6.0+<sha> / rust core 0.1.0+<smooth_core-sha>"
```

実行例:

```bash
# macOS
build-macos-arm64/host_smoke build-macos-arm64/smooth.ofx.bundle/Contents/MacOS/smooth.ofx

# Windows MSVC
build-msvc\Release\host_smoke.exe build-msvc\Release\smooth.ofx

# Linux
build-linux/host_smoke build-linux/smooth.ofx
```

### 診断モード

```bash
# Bench: 任意サイズの render() を計測
SMOOTH_BENCH_SIZE=1920x1080 SMOOTH_BENCH_ITERS=20 host_smoke <smooth.ofx>

# transparent オプションの白判定許容差確認 (各深度 / ドリフト白)
SMOOTH_DIAG=transparent host_smoke <smooth.ofx>

# range スイープ (閾値が効くしきい値の確認)
SMOOTH_DIAG=range host_smoke <smooth.ofx>
```

## 6. UI 上のビルド ID 表示

すべてのビルドにビルド ID が埋め込まれ、Effect Controls / Inspector の
read-only "build" 欄に表示されます。例:

```
1.6.0+c812c03 / rust core 0.1.0+a566908
```

構成要素:
- `1.6.0` — プラグインバージョン (`project(smooth-ofx VERSION ...)` 由来)
- `c812c03` — configure 時の smooth-ofx リポジトリの git SHA
- `+dirty` — 作業ツリーに未コミット変更があれば付与
- `cpp core` または `rust core <semver>+<smooth_core-sha>[+dirty]` — 実行
  経路の表示

UAT 中、ビルドラベルで「正しいビルドが入っているか」を必ず確認して
ください。

## 7. プラットフォーム別インストール先

| プラットフォーム | システム全体 | ユーザー単位 |
|---|---|---|
| macOS | `/Library/OFX/Plugins/smooth.ofx.bundle/` | `~/Library/OFX/Plugins/smooth.ofx.bundle/` (ホスト依存) |
| Windows | `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\` | `%APPDATA%\OFX\Plugins\smooth.ofx.bundle\` (ホスト依存) |
| Linux | `/usr/OFX/Plugins/smooth.ofx.bundle/` または `/opt/OFX/Plugins/smooth.ofx.bundle/` | `~/.OFX/Plugins/smooth.ofx.bundle/` |

`cmake --install` は `OFX_INSTALL_DIR` キャッシュ変数を使い、デフォルト
ではシステム全体側の行と同じパスを使用します。

## 8. トラブルシューティング

**`smooth_core Cargo.toml not found at …`**
`git submodule update --init smooth-ae` を実行 (Rust crate は smooth-ae
配下にあります)。

**`cargo not found in PATH`**
[rustup.rs](https://rustup.rs) で Rust をインストールするか、
`-DUSE_RUST_CORE=OFF` で Rust 統合をスキップ。

**Windows でリンク時に ABI mismatch エラー**
MinGW C++ と MSVC Rust ターゲットを混ぜているケース。MSVC で全部
ビルドするか、MinGW の場合は `-DUSE_RUST_CORE=OFF` を指定。

**macOS: `lipo: can't open input file`**
cargo がいずれかの arch の static lib を生成できていない。
プラットフォーム別 CMake 引数で再 configure し、
`rustup target list --installed` に `aarch64-apple-darwin` と
`x86_64-apple-darwin` の両方が入っているか確認。

**Resolve でプラグインが表示されない**
1. bundle id `jp.loilo.smooth` が他プラグインと重複していないか確認
2. `sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle`
   で検疫属性除去
3. Resolve を完全に再起動 (プラグインメタデータがキャッシュされている)

**macOS で `host_smoke` が `dlopen error=…` を返す**
host_smoke とプラグインのアーキテクチャ不一致。両方を同じ
`CMAKE_OSX_ARCHITECTURES` でビルドする。

## 9. クロスプラットフォーム CI スケッチ (将来作業)

3 OS をカバーする最小 GitHub Actions マトリクス:

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

各 entry の手順: Rust インストール (`actions-rs/toolchain`) →
`git submodule update --init --recursive` → 適切なフラグで CMake
configure → ビルド → `host_smoke` 実行 → bundle / zip を build artefact
として upload。

未実装。出発点として記載のみ。
