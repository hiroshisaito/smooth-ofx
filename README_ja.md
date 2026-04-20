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
- パラメータ: `range`、`line weight`、`transparent`
- CPU 処理のみ (GPU レンダリングパスは未実装)
- 第三者ライブラリ依存なし (OFX ヘッダのみ)

## 対応プラットフォーム

- **macOS 11 以降** — `arm64` (Apple Silicon) と `x86_64` (Intel) の
  アーキテクチャ別シングルアーキビルドを個別に配布
- **Windows 10 以降 (x64)** — MSVC 2019+ もしくは MSYS2 MinGW-w64

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
| `smooth-ae` | [loilo-inc/smooth](https://github.com/loilo-inc/smooth) | 不要 (参照用) |

クローン後:

```bash
git submodule update --init --recursive
# 参照用の smooth-ae を省略したい場合:
git submodule update --init include/openfx
```

## ビルド

### macOS — アーキテクチャ別ビルド

リリース配布物はアーキテクチャ別のシングルアーキ zip として用意して
います。`CMAKE_OSX_ARCHITECTURES` を切り替えて、それぞれビルド
してください。

```bash
# Apple Silicon
cmake -S . -B build-macos-arm64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DBUILD_TESTS=OFF
cmake --build build-macos-arm64 --config Release

# Intel
cmake -S . -B build-macos-x86_64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DBUILD_TESTS=OFF
cmake --build build-macos-x86_64 --config Release
```

各ビルドの成果物は
`build-macos-<arch>/smooth.ofx.bundle/Contents/MacOS/smooth.ofx` に
出力されます。

単一の fat binary (Universal 2) が必要な場合は
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` を指定すれば従来通り
ビルドできます。

### Windows

```bash
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

DLL は `build-msvc/Release/smooth.ofx` に出力されます。

## インストール

ビルドした bundle / DLL を OFX ホストのプラグインディレクトリにコピーします。

- **macOS**: `/Library/OFX/Plugins/smooth.ofx.bundle/`
- **Windows**: `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\Contents\Win64\smooth.ofx`

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
