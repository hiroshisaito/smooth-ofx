# 変更履歴 (CHANGELOG)

[English CHANGELOG](CHANGELOG.md)

本プロジェクトの変更点をこのファイルに記録します。

バージョンは [セマンティックバージョニング](https://semver.org/lang/ja/) に
従います。初回の OFX 移植版は、上流の After Effects プラグイン
([loilo-inc/smooth](https://github.com/loilo-inc/smooth)) のバージョン番号
(`1.4.0`) を引き継いでいます。OFX 移植側の修正はパッチ番号を進めます。

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
