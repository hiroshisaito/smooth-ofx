# smooth-ofx 1.6.0 — リリースノート

[English](RELEASE_NOTES_1.6.0.md)

**リリース日**: 2026-05-05 (初版)、2026-05-07 リフレッシュ
**Upstream**: AE 側 fork
[hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) v1.6.0
**出力互換性**: 全 3 ピクセル深度で 1.4.0 と byte-identical
(`host_smoke` + PPM cmp で macOS / Windows 検証済)
**本番性能**: マルチコアホストの 1080p / 4K で 1.4.0 比 **2.5×–3.1× 高速化**
(8-bit / 32-bit float 経路)

---

## 主要な変更点

- **Rust core によるアルゴリズム高速化**。8-bit と 32-bit float の
  処理経路を共有 crate `smooth_core` (`smooth-ae/rust/smooth_core` v0.1.0)
  経由に切り替え、rayon の strip 並列で動作。出力は 1.4.0 と
  byte-identical を維持しつつ、8 コア macOS ホストで wall-clock が
  約 1/3 に短縮。
- **Effect Controls に build identity を表示**。Inspector の
  read-only `build` ラベルにプラグインバージョン + git short SHA
  (`+dirty` フラグ付き) + Rust core の build id が表示され、UAT 中に
  どのビルドが入っているかを一目で判別可能に。
- **クロスプラットフォーム ビルドドキュメント**。トップレベルに
  [BUILDING.md](BUILDING.md) / [BUILDING_ja.md](BUILDING_ja.md) を
  新設、macOS arm64 + x86_64 / Windows MSVC / Linux を 1 ファイルで
  網羅。署名、検証、トラブルシューティングまでカバー。README の
  ビルド節はクイックスタートに整理し詳細は BUILDING へ誘導。
- **クロスプラットフォーム `host_smoke`**。OFX ホスト最小ハーネスが
  Windows 専用ではなくなり、同じバイナリが `dlopen` 経由で
  macOS / Linux でも動作。ベンチモード (`SMOOTH_BENCH_SIZE=WxH
  SMOOTH_BENCH_ITERS=N`) や診断モード (`SMOOTH_DIAG=transparent` /
  `range`) も追加。

## 性能

8 物理コア x86_64 ホストでの wall-clock 比較 (1.6.0 Rust core ビルド
vs 1.4.0 C++-only ベースライン):

| 画像          | 8-bit                              | 32-bit float                       |
|---------------|------------------------------------|------------------------------------|
| 1920 × 1080   | 91.7 ms → **36.2 ms** (2.53×)      | 130.2 ms → **48.5 ms** (2.68×)     |
| 3840 × 2160   | 365.8 ms → **118.8 ms** (3.08×)    | 507.5 ms → **185.9 ms** (2.73×)    |

16-bit 整数は変更なし — 引き続き C++ 実装。Rust core は AE 流の
`0x8000` max-value 規約、OFX は `0xFFFF` のため、直接ポートだとロス
有り。crate に OFX flavor の 16bpc max が追加された段階で Rust 経路へ
移行予定。

## 1.4.0 の hotfix 系列から 1.6.0 に引き継いだ修正

- **線上に黒ピクセル発生** (16-bit / 32-bit float カラースペース)。
  原因: `Link8SquareExecute` の累積変数 `int sum_color[4]` が float
  ピクセル (0..1) を加算するたびに int に切り捨てられていた。修正:
  累積変数の型を `PixelRangeType<PixelType>::type` (8/16 int は unsigned
  int、32-bit float は float) に変更。
- **半透明境界に白フリンジ** (DaVinci Resolve、`transparent` ON 時)。
  修正: smoothing 前後で alpha の unpremultiply / premultiply を実施。
- **transparent オプションが 16-bit / float で動作しない**。白判定を
  16-bit では `0xFFFF` と `0x8000` の両方を許容 (OFX / AE 流の規約差を
  カバー)、float では `|v − 1.0| < 0.005` の許容差で判定。GPU プロト
  タイプ経路では kernel 投入前に 1.0 へスナップ。
- **range スライダで効果が見えない** (典型的なスライダ位置で)。
  display max を 10 → 100 に拡張して、有効な閾値範囲全体に届くように。
- **build identity がラベル文字列を 2 度表示** (Resolve Inspector)。
  read-only 表示を `kOfxParamStringIsLabel` から disabled な
  `kOfxParamStringIsSingleLine` に変更 (他 OFX プラグインの慣習に揃え)。

## 配布

アーキ別シングルアーキ zip、アドホック署名済み (notarization 未実施 —
ロード前に quarantine 属性を除去してください):

- `dist/smooth-1.6.0-macos-arm64.zip` — Apple Silicon
- `dist/smooth-1.6.0-macos-x86_64.zip` — Intel
- Windows / Linux ビルドは macOS リリース後の別タスク。
  [BUILDING.md](BUILDING.md) のプラットフォーム別レシピを参照
  (§ 3.3 Windows MSVC、§ 3.5 Linux、§ 10 に CI スケッチ)。

```bash
# インストール (macOS Intel 例、Apple Silicon は -arm64 に置換)
sudo bash -c 'rm -rf /Library/OFX/Plugins/smooth.ofx.bundle && cp -R /path/to/smooth.ofx.bundle /Library/OFX/Plugins/ && xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle'
```

各 zip には `smooth.ofx.bundle` + アーキ別 `README.txt` + 共通
`RELEASE-NOTES.txt` を同梱。SHA-256 digest は `*.zip.sha256` で併記。

## 実験的機能: GPU プロトタイプ (`USE_GPU_CORE`、デフォルト OFF)

[wgpu](https://wgpu.rs) (Metal / Vulkan / DX12) によるクロスプラット
フォーム GPU 加速のプロトタイプとして `smooth_gpu` crate を
`rust/smooth_gpu/` に追加。**デフォルト OFF、配布バイナリには含まれません**。

- ビルド: `-DUSE_GPU_CORE=ON -DUSE_RUST_CORE=OFF` (現状リンク時に相互
  排他、BUILDING § 9 の Rust ランタイム重複シンボルの説明参照)
- WGSL kernel: `preprocess` (AE / OFX 両 byte layout) / `mode_flg` 検出 /
  `link8_square` 中心 handler — それぞれ CPU 参照と byte-identical を検証済
- ハイブリッドレンダー経路: 8bpc preprocess を GPU、残りは C++ ベース
  ライン。Effect Controls の **GPU チェックボックス** で実行時に on/off 切替
- `host_smoke` に `SMOOTH_BENCH_TOGGLE_GPU=1` モード追加、両経路 side-by-side
  ベンチ可能
- **本プロトタイプは出荷経路 (`USE_RUST_CORE=ON`) より遅い**。
  1080p 8-bit で約 22 ms/frame の悪化。GPU の upload/dispatch/readback 往復
  オーバーヘッドが軽量 preprocess kernel の compute density を上回る、
  毎フレーム CPU↔GPU バッファ往復の構造的問題。次の 2 方向への足場として
  ソースに残してある:
  - **(i)** `smooth_core` を `rlib` 化して `smooth_gpu` に Rust 依存
    として埋め込み、リンク相互排他を解消 → rayon-CPU + GPU kernel 共存
  - **(ii)** OFX 1.5 `kOfxImageEffectActionRenderGPU` で host から
    GPU buffer 直渡し → 往復オーバーヘッド消失

(i) (ii) のいずれかが解禁されるまで、本番構成は
`USE_RUST_CORE=ON, USE_GPU_CORE=OFF` のまま。

## バージョニング方針

OFX port は AE 側 fork の major / minor を追従し、独自の version line
は持ちません。1.5.x は AE 側のみで OFX 移植では飛ばし、1.6.0 で再
アライン。1.6.0 上の patch / hotfix も同じ upstream 追従方針で進めます。

## 謝辞

- オリジナル **smooth** プラグイン: 杉山浩二氏 (Kouji Sugiyama、2004)、
  [LOILO Inc.](https://loilo.tv/) により
  <https://github.com/loilo-inc/smooth> にて Apache 2.0 で
  オープンソース化
- AE 側 近代化 fork
  ([hiroshisaito/smooth](https://github.com/hiroshisaito/smooth)) —
  Rust core、MFR、32bpc float、build-id UI、クロスプラットフォーム
  テストハーネス
- OpenFX SDK 1.5.1 ([Academy Software Foundation](https://www.openeffects.org/))

ライセンス全文は [LICENSE](LICENSE)、プロジェクト概要は
[README.md](README.md) / [README_ja.md](README_ja.md) を参照。
