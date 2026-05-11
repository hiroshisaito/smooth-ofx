# smooth-ofx 1.6.0 — リリースノート

[English](RELEASE_NOTES_1.6.0.md)

**リリース日**: 2026-05-05 (初版)、2026-05-07 リフレッシュ
**Upstream**: AE 側 fork
[hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) v1.6.0
**出力互換性**: 全 3 ピクセル深度で 1.4.0 と byte-identical
(`host_smoke` + PPM cmp で macOS / Windows / Linux 検証済 —
3 経路すべてで `pure0=562 pureMax=562 intermediate=924 / 2048`)
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
  [BUILDING.md](docs/BUILDING.md) / [BUILDING_ja.md](docs/BUILDING_ja.md) を
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
  カバー)、float では `|v − 1.0| < 0.005` の許容差で判定。
- **range スライダで効果が見えない** (典型的なスライダ位置で)。
  display max を 10 → 100 に拡張して、有効な閾値範囲全体に届くように。
- **build identity がラベル文字列を 2 度表示** (Resolve Inspector)。
  read-only 表示を `kOfxParamStringIsLabel` から disabled な
  `kOfxParamStringIsSingleLine` に変更 (他 OFX プラグインの慣習に揃え)。

## リリース後の Windows MSVC 修正 (2026-05-08)

クリーンな Windows 11 ホスト上で VS 18 2026 + Windows 11 SDK
10.0.26100 + Rust 1.94 の組合せで 1.6.0 を再ビルドした際に 3 件の
regression が露見。すべて MSVC スコープの修正 (`if(MSVC)` /
`MATCHES "windows-msvc$"` で gated) で、生成される `smooth.ofx` は
macOS リリースと byte-identical。詳細は [CHANGELOG.md](CHANGELOG.md)
§「ビルド (macOS リリース後の Windows MSVC 修正)」参照:

- `near` ラムダを `is_near` に改名 (`windef.h` の legacy `near`
  マクロと衝突して `C2513` でビルド失敗していた)。
- CMake の cargo 連携が `*-pc-windows-msvc` の `<crate>.lib` を
  選択するよう分岐 (GNU 系の `lib<crate>.a` を一律使っていて
  `LNK1181` で失敗していた)。
- Rust 1.94 の std が必要とする Windows ネイティブ依存
  (`ntdll`, `userenv`, `ws2_32`, `dbghelp`) を `MSVC` ビルド時のみ
  CMake にリンクさせる (`LNK1120: 2 unresolved externals` を解消)。

出荷済みの macOS zip (`smooth-1.6.0-macos-{arm64,x86_64}.zip`) は
本修正以前にカット済みで影響なし。Windows / Linux zip 出荷時には
本修正が自動的に取り込まれる。

## 配布

各プラットフォーム別シングルアーキ配布物。macOS のみアドホック署名済み
(notarization 未実施 — ロード前に quarantine 属性を除去してください)、
Windows / Linux は無署名 (Linux ホストは通常コード署名を要求しない):

- `dist/smooth-1.6.0-macos-arm64.zip` — Apple Silicon
- `dist/smooth-1.6.0-macos-x86_64.zip` — Intel
- `dist/smooth-1.6.0-windows-x64.zip` — Windows 10/11 x64
  (VS 18 2026 + Rust 1.94 でビルド、§「リリース後の Windows MSVC 修正」参照)
- `dist/smooth-1.6.0-linux-x86-64.tar.gz` — Rocky Linux 9.5 / RHEL-9 系 x86_64
  (gcc 11.5 + Rust 1.95、glibc 2.34+)

```bash
# インストール (macOS Intel 例、Apple Silicon は -arm64 に置換)
sudo bash -c 'rm -rf /Library/OFX/Plugins/smooth.ofx.bundle && cp -R /path/to/smooth.ofx.bundle /Library/OFX/Plugins/ && xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle'
```

macOS zip / Windows zip / Linux tarball には `smooth.ofx.bundle` +
アーキ別 `README.txt` + 共通 `RELEASE-NOTES.txt` を同梱。
SHA-256 digest は `*.sha256` で併記。プラットフォーム別ビルド手順の
詳細は [BUILDING.md](docs/BUILDING.md) (§ 3.3 Windows MSVC、§ 3.5 Linux、
§ 9 に CI スケッチ)。

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
