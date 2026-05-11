# smooth-ofx

**smooth** の OpenFX 移植版 — ピクセル境界の階段状パターンを滑らかにする
スムージングフィルタです。元は LOILO Inc. が After Effects プラグイン
として公開していたものを、OpenFX 1.5.1 API に移植し、DaVinci Resolve /
Fusion など OFX 対応ホストで利用できるようにしました。

元のスムージングアルゴリズムをそのまま保ちつつ、OFX プラグイン境界を
被せる形で実装しています。

## 特徴

- 8-bit / 16-bit 整数、16-bit / 32-bit float の RGBA に対応
- プリマルチプライドアルファ対応 (入力を自動で straight に展開し、処理後に戻す)
- パラメータ: `range`、`line weight`、`transparent`、加えて Inspector に
  read-only の `build` ラベルを表示
- CPU 実装のみ (Rust core、rayon 並列)
- 第三者ライブラリ依存なし (OFX ヘッダのみ)。macOS の出荷バンドルは
  `libc++` と `libSystem` のみリンク

## 対応プラットフォーム

- **macOS 11 以降** — `arm64` (Apple Silicon) と `x86_64` (Intel) の
  アーキテクチャ別シングルアーキビルドを個別に配布
- **Windows 10 / 11 (x64)** — Visual Studio 2022 / 2026 (MSVC) でビルド。
  MinGW-w64 もサポート
- **Linux x86-64** — RHEL-9 系 (Rocky Linux 9.5 / AlmaLinux 9 /
  Oracle Linux 9)、glibc 2.34 以上、gcc 11.5 以上

## リポジトリ構成

| パス | 用途 |
|---|---|
| `smooth/` | OFX プラグインソース (移植ターゲット) |
| `include/openfx` | OpenFX SDK (git submodule) |
| `smooth-ae` | オリジナルの AE プラグイン (git submodule、参照用) |
| `tests/` | スモークテスト用の最小 OFX ホストハーネス |
| `dist/` | パッケージングテンプレート (Info.plist、インストール README) |

### サブモジュール

| パス | 取得元 | ビルド必須 |
|---|---|---|
| `include/openfx` | [AcademySoftwareFoundation/openfx](https://github.com/AcademySoftwareFoundation/openfx) @ `OFX_Release_1.5.1` | 必須 |
| `smooth-ae` | [hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) ([loilo-inc/smooth](https://github.com/loilo-inc/smooth) のメンテナンス fork) | 不要 (参照用) |

クローン後:

```bash
git submodule update --init --recursive
# 参照用の smooth-ae を省略したい場合:
git submodule update --init include/openfx
```

## ビルド

クロスプラットフォーム (macOS / Windows / Linux) の詳細手順は
**[BUILDING_ja.md](docs/BUILDING_ja.md)** を参照してください — Rust core
(`smooth_core`) のセットアップ、CMake オプション、署名、検証を網羅
しています。クイックスタート:

```bash
# macOS arm64 (Apple Silicon)
cmake -S . -B build-macos-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-macos-arm64 --config Release

# Windows MSVC x64 ("x64 Native Tools Command Prompt for VS" から)
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release

# Linux x86_64
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
```

CMake オプション `USE_RUST_CORE` (1.6.0 以降は default ON) は、共有
`smooth_core` Rust crate を 8-bit / 32-bit float 経路に組み込みます。
Rust toolchain が無い環境や MSYS2 MinGW 開発経路では
`-DUSE_RUST_CORE=OFF` でスキップ可能です。

## インストール

ビルドした bundle / DLL を OFX ホストのプラグインディレクトリにコピーします。

- **macOS**: `/Library/OFX/Plugins/smooth.ofx.bundle/`
- **Windows**: `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\Contents\Win64\smooth.ofx`
- **Linux**: `/usr/OFX/Plugins/smooth.ofx.bundle/Contents/Linux-x86-64/smooth.ofx` (または Resolve 専用パス `/var/BlackmagicDesign/DaVinci Resolve/Support/OFX/Plugins/` など、ホストごとの専用ディレクトリ)

OFX ホスト (DaVinci Resolve / Fusion など) を再起動すると、
**Effects → Filters → Smooth** にプラグインが現れます。

署名していない macOS ビルドは Gatekeeper に弾かれる可能性があるため、
必要に応じて以下で検疫属性を除去してください:

```bash
sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle
```

## ライセンス

本プロジェクトは **Apache License, Version 2.0** の下で公開されています。
これは上流の [loilo-inc/smooth](https://github.com/loilo-inc/smooth)
プロジェクトから継承したライセンスです。全文は [LICENSE](LICENSE) を
参照してください。

## 謝辞

- **オリジナルプラグイン** — smooth After Effects プラグインは
  2004 年に杉山浩二氏 (Kouji Sugiyama) により作成され、その後
  [LOILO Inc.](https://loilo.tv/) により
  <https://github.com/loilo-inc/smooth> にて Apache 2.0 ライセンスで
  オープンソース化されました。本リポジトリの各スムージング
  アルゴリズム (upMode / downMode / 8link / Lack) は、その実装の
  直接的な移植です。
- **OpenFX SDK** — [Academy Software Foundation](https://www.openeffects.org/) に
  よる OpenFX 1.5.1 仕様およびリファレンスヘッダを利用しています。

本移植で追加した部分: OFX プラグイン境界 (describe / render / clips /
parameters) の実装、高ビット深度対応 (16-bit 整数、16/32-bit float)、
OFX ホストが要求するプリマルチプライドアルファ変換、スモーク
テスト用の最小ホストハーネス。

---

English README: [README.md](README.md)
