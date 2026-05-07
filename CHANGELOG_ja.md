# 変更履歴 (CHANGELOG)

[English CHANGELOG](CHANGELOG.md)

本プロジェクトの変更点をこのファイルに記録します。

バージョンは [セマンティックバージョニング](https://semver.org/lang/ja/) に
従い、AE 側 fork
[hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) の major /
minor 番号を追従します — OFX 側で独自の version line は維持しません。
1.5.x シリーズは AE 側のみで OFX 移植では飛ばし、1.6.0 で両サイドを
合わせ直します。

## 1.6.0 — 2026-05-05

OFX 移植のバージョンを AE 側 fork
[hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) の v1.6.0 に
揃えるリリース。出力は 1.4.0 と全 3 深度でバイト一致を確認済みで、
変更点は内部 (Rust コアによるアルゴリズム置換) + UI (build-id 表示) です。

### 追加 (Added)

- `smooth_core` Rust crate (`smooth-ae/rust/smooth_core` v0.1.0 を共有
  利用) を 8-bit / 32-bit float 経路で採用。rayon による
  strip-parallel 並列化 internally。出力は 1.4.0 の C++ 実装と
  host_smoke + PPM cmp でバイト一致を確認済み。
- CMake オプション `USE_RUST_CORE` (1.6.0 以降は **default ON**) を新設、
  Rust core ビルドを有効化。Rust ツールチェーンが無い環境では
  `-DUSE_RUST_CORE=OFF` を指定すれば C++ ベースラインでビルド可能。
  配布版 macOS zip はデフォルト (ON) でビルド。
- Effect Controls に read-only `build` パラメータを追加。プラグイン
  バージョン + OFX 側 git SHA (`+dirty` フラグ付き) + Rust core の
  build identity を表示し、UAT 中にどのビルドが入っているかを一目で
  確認可能。
- `host_smoke` を cross-platform 化 (Windows 専用だったものを
  `dlopen` 経由で macOS / Linux でも動作)。`SMOOTH_BENCH_SIZE=WxH
  SMOOTH_BENCH_ITERS=N` で起動するベンチモードを追加。

### パフォーマンス (Performance)

8 コアホストでの wall-clock 比較 (Rust-core ビルド vs 1.4.0 の
C++-only baseline):

| 画像サイズ    | 8-bit                | 32-bit float         |
|---------------|----------------------|----------------------|
| 1920 x 1080   | 91.7 ms → 36.2 ms (2.53×) | 130.2 ms → 48.5 ms (2.68×) |
| 3840 x 2160   | 365.8 ms → 118.8 ms (3.08×) | 507.5 ms → 185.9 ms (2.73×) |

16-bit 整数は未変更 (引き続き C++ 経路)。OFX の 16-bit max が `0xFFFF`、
`smooth_core::Pixel16` の max が AE 流の `0x8000` で異なるため、Rust 側に
OFX flavor の 16bpc max を追加してから対応予定。

### ドキュメント (Documentation)

- クロスプラットフォーム ビルドガイドを新設: [BUILDING.md](BUILDING.md) /
  [BUILDING_ja.md](BUILDING_ja.md)。macOS / Windows / Linux 各 OS の
  ツールチェーン要件、Rust toolchain 前提、配布署名、検証手順、
  トラブルシューティングまで網羅。README のビルド節はクイック
  スタートに整理し、詳細は BUILDING に誘導する構成に変更。

### 実験的機能: GPU プロトタイプ (`USE_GPU_CORE`、デフォルト OFF)

wgpu (Metal / Vulkan / DX12) を使ったクロスプラットフォーム GPU 加速の
プロトタイプとして `smooth_gpu` Rust crate をソースツリーに追加。
**デフォルト OFF**、配布バイナリには含まれません。

- 新 CMake オプション `USE_GPU_CORE=ON` で `rust/smooth_gpu/` をビルド
  & リンク。`USE_RUST_CORE` とリンク時に相互排他 (両 staticlib が Rust
  ランタイムを内蔵し重複シンボルになるため。smooth_core は staticlib
  専用で Rust 依存として埋め込めない)。
- WGSL kernel: `passthrough` / `preprocess` (AE + OFX 両 layout) /
  `mode_flg` 検出 / `link8_square` 中心 handler の 4 つ、それぞれ
  CPU 参照実装と byte-identical を `cargo test` で検証済。
- ハイブリッドレンダー経路: `USE_GPU_CORE=ON` のとき、`smoothing<>()` は
  8bpc preprocess を GPU で実行し、残りのアルゴリズムは C++ ベース
  ライン経路に流す。状態は "build" ラベルに smooth_gpu の build identity
  と並べて表示。
- Effect Controls に **GPU トグル (boolean パラメータ、default ON)** を
  追加。再ビルド不要で GPU/CPU preprocess を切替可能。
- `host_smoke` に `SMOOTH_BENCH_TOGGLE_GPU=1` モードを追加、両経路を
  side-by-side でベンチ可能。
- **性能**: このプロトタイプは出荷経路 (`USE_RUST_CORE=ON`) より **遅い**。
  GPU の転送オーバーヘッド (upload + dispatch + readback ≈ 5–10 ms) が
  preprocess の節約 (1–2 ms) を上回るため。次の 2 方向への足場として
  存在する: (i) `USE_RUST_CORE` / `USE_GPU_CORE` のリンク相互排他を
  `smooth_core` の `rlib` 化で解消し、rayon CPU + GPU kernel を共存させる、
  (ii) OFX 1.5 `kOfxImageEffectActionRenderGPU` 経由で zero-copy 経路
  を実装。いずれかが解禁されるまで本番構成は
  `USE_RUST_CORE=ON, USE_GPU_CORE=OFF` のまま。

### ビルド (macOS リリース後の Windows 向け修正、2026-05-08)

macOS arm64 / x86_64 リリースは 2026-05-05 に
VS 17 2022 + Windows 10 SDK 想定でカット済。1.6.0 を Windows 11
クリーン環境で **VS 18 2026 + Windows 11 SDK 10.0.26100 + Rust 1.94**
の組み合わせでビルドすると、3 件のリグレッションが顕在化したため
修正を加えた。いずれも MSVC スコープに閉じた変更で、生成される
smooth.ofx は macOS 出荷ビルドとバイト一致 (`host_smoke` の 8/16/float
3 パスとも `pure0=562 pureMax=562 intermediate=924 / 2048`)。

- `isWhitePixel<OfxRGBAColourS>` 内の `near` ラムダを `is_near` に
  リネームし、`windef.h` の `near` マクロ (16-bit セグメント修飾子の
  遺物。現代 SDK では空マクロ) との衝突を回避。MSVC プリプロセッサが
  識別子を消し去って `C2513` ビルドエラーになっていた問題への対応。
- CMake グルーが cargo の static lib 命名規則を、MSVC ターゲットでは
  `<crate>.lib`、GNU 系では `lib<crate>.a` と分岐するように修正。
  以前はリンク時に
  `LNK1181: cannot open input file libsmooth_core.a` で失敗していた。
- Rust 1.94 の std が要求する Windows ネイティブ依存ライブラリ
  (`ntdll` / `userenv` / `ws2_32` / `dbghelp`) を `MSVC` 時に明示
  リンク。これらが無いと `LNK1120: 2 unresolved externals` で
  `__imp_NtWriteFile` / `__imp_RtlNtStatusToDosError`
  (`std::sys::stdio::windows::write` 経由) が解決できなかった。

### 注意 (Notes)

- バンドルサイズが ~134 KB → ~607 KB (arm64) に増加。rayon + その
  依存 crate を staticlib として同梱したため。実行時の追加依存はなく、
  `otool -L` は引き続き `libc++` / `libSystem` のみ。GPU プロトタイプ
  ビルドはさらに ~1.3 MB 増加し、macOS では Metal / MetalKit /
  QuartzCore / Foundation framework もリンクされる。
- 配布 zip の名前は `smooth-1.4.0-*` → `smooth-1.6.0-*` に変更。

## 1.4.0 — 2026-04-20

**smooth** プラグインの OpenFX 初回移植版。Windows / macOS 両方で
DaVinci Resolve 上での動作を確認しています。

### 追加 (Added)

- OpenFX 1.5.1 のプラグイン実装 (`jp.loilo.smooth`, version 1.4):
  `describe` / `describeInContext` / `createInstance` / `destroyInstance`
  / `render` アクションに対応。
- ピクセル深度: 8-bit 整数 / 16-bit 整数 / 16-bit float / 32-bit float の
  RGBA ワーキングカラースペースをサポート。
- プリマルチプライドアルファ対応: 入力バッファを straight に展開してから
  スムージング処理、出力時に再び premultiply。これにより premult で
  ピクセルを渡してくるホスト (DaVinci Resolve / Fusion 等) でも正しい
  エッジが得られます。
- プラグインが公開するパラメータ:
  - `range` (double, 0–100, default 1.0)
  - `lineWeight` (double, 0–1, default 0.0)
  - `whiteOption` / transparent (boolean)
- macOS の `arm64` (Apple Silicon) / `x86_64` (Intel) ビルド、
  デプロイメントターゲット 11.0。リリース配布物はアーキテクチャ
  別のシングルアーキ zip (`smooth-1.4.0-macos-arm64.zip` /
  `smooth-1.4.0-macos-x86_64.zip`) として提供し、それぞれ
  `Info.plist` を自動生成した正式な `smooth.ofx.bundle` を出力、
  アドホック署名 (ad hoc codesign) 済み。
- Windows x64 ビルド: MSVC 2019+ または MSYS2 MinGW-w64 に対応。
- `tests/host_smoke.cpp` に最小 OFX ホストハーネスを用意
  (Windows のみ) — フルホストなしでプラグインのスモーク
  テストが可能。
- ドキュメント類: `README.md` / `README_ja.md`、上流から継承した
  Apache-2.0 ライセンス (`LICENSE`)。

### 修正 (Fixed)

- 16-bit / 32-bit float カラースペースで線上に黒ピクセルが発生
  する不具合を修正。`Link8SquareExecute` の累積変数が
  `int sum_color[4]` だったため、float ピクセル (0..1 範囲) を
  加算するたびに int に切り捨てられ、合計が常に 0 近傍 →
  `/4` で 0 → 黒ピクセル出力、という経路でした。累積変数の型を
  `PixelRangeType<PixelType>::type` (8/16-bit 整数では
  `unsigned int`、32-bit float では `float`) に変更。
- DaVinci Resolve で `transparent` オプション ON 時、半透明境界に
  白フリンジが発生する不具合を修正。元アルゴリズムが
  straight RGBA 前提だったため、処理前後でアルファの
  unpremultiply / premultiply を行うよう変更。

### 既知の制限 (Known limitations)

- タイル描画には非対応 (`kOfxImageEffectPropSupportsTiles = 0`)。
- マルチレゾリューション / プロキシ描画は未検証。
- GPU レンダリングパス (`ofxGPURender.h`) は未実装 — CPU のみ。
- macOS リリース zip (`smooth-1.4.0-macos-arm64.zip` /
  `smooth-1.4.0-macos-x86_64.zip`) は **アドホック署名のみ**、
  Developer ID による**公証 (notarization) は未実施**です。
  Gatekeeper が初回ロード時にブロックする可能性があるため、
  受け取り側で `xattr -dr com.apple.quarantine` による検疫属性の
  除去が必要です。

### 謝辞 (Acknowledgments)

各スムージングアルゴリズム (upMode / downMode / 8link / Lack) は、
2004 年に杉山浩二氏が作成され、その後
[LOILO Inc.](https://loilo.tv/) により
<https://github.com/loilo-inc/smooth> にてオープンソース化された
オリジナル After Effects 実装の移植です。詳細は
[README_ja.md](README_ja.md#謝辞) を参照してください。
