# smooth-ofx 1.6.0

OpenFX port of [LOILO smooth](https://github.com/loilo-inc/smooth) — 4 プラットフォーム同時リリース。AE 側 fork [hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) v1.6.0 とバージョンライン整合。

## ハイライト

- **Rust core で 2.5×〜3.1× 高速化** — 8-bit / 32-bit float 経路を `smooth_core` (rayon 並列) に置換。出力は 1.4.0 と全 3 ピクセル深度で **byte-identical**
- **Build identity 表示** — Effect Controls の `build` ラベルにバージョン + git SHA + Rust core build id を表示。UAT で読み込み済ビルドを一目で確認可能
- **4 プラットフォーム検証済** — macOS arm64 / x86_64、Windows x64、Linux x86-64 すべてで `host_smoke` の canonical 出力 (`pure0=562 / pureMax=562 / intermediate=924`) を確認

## 配布物

| Platform | Archive | 署名 |
|----------|---------|------|
| macOS arm64 (Apple Silicon, 11+) | `smooth-1.6.0-macos-arm64.zip` | ad-hoc (公証なし) |
| macOS x86_64 (Intel, 11+) | `smooth-1.6.0-macos-x86_64.zip` | ad-hoc (公証なし) |
| Windows x64 (10/11) | `smooth-1.6.0-windows-x64.zip` | 無署名 |
| Linux x86-64 (RHEL-9 系, glibc 2.34+) | `smooth-1.6.0-linux-x86-64.tar.gz` | 無署名 |

各アーカイブに `*.sha256` 併記。各アーカイブには `smooth.ofx.bundle` + プラットフォーム別 `README.txt` + 共通 `RELEASE-NOTES.txt` を同梱。

## インストール (要約)

**macOS** (Gatekeeper 対策で quarantine 属性除去が必要):

```bash
unzip smooth-1.6.0-macos-arm64.zip   # arm64 / x86_64 を環境に合わせて選択
sudo cp -R smooth.ofx.bundle /Library/OFX/Plugins/
sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/smooth.ofx.bundle
```

**Windows** (PowerShell 管理者権限):

```powershell
Expand-Archive smooth-1.6.0-windows-x64.zip -DestinationPath .
Copy-Item -Recurse smooth.ofx.bundle "C:\Program Files\Common Files\OFX\Plugins\"
```

**Linux**:

```bash
tar xzf smooth-1.6.0-linux-x86-64.tar.gz
sudo cp -R smooth.ofx.bundle /usr/OFX/Plugins/
```

DaVinci Resolve / Natron 等を再起動 → **Effects → Filters → Smooth**。

## 詳細ドキュメント

- [CHANGELOG.md](https://github.com/hiroshisaito/smooth-ofx/blob/v1.6.0/CHANGELOG.md) — 完全な変更点
- [RELEASE_NOTES_1.6.0.md](https://github.com/hiroshisaito/smooth-ofx/blob/v1.6.0/RELEASE_NOTES_1.6.0.md) — 技術詳細・性能ベンチマーク
- [BUILDING.md](https://github.com/hiroshisaito/smooth-ofx/blob/v1.6.0/BUILDING.md) — クロスプラットフォームビルド手順
- [THIRD_PARTY_LICENSES.md](https://github.com/hiroshisaito/smooth-ofx/blob/v1.6.0/THIRD_PARTY_LICENSES.md) — 第三者ライセンス

日本語版もそれぞれ `*_ja.md` で用意。

## 既知の制限

- Tile rendering 非対応 (`kOfxImageEffectPropSupportsTiles = 0`)
- Multi-resolution / proxy rendering 未検証
- 16-bit 整数経路は引き続き C++ ベースライン (Rust の `Pixel16` max-value 規約差異対応待ち)

## 謝辞

- オリジナル smooth プラグイン: 杉山浩二氏 (Kouji Sugiyama, 2004)、[LOILO Inc.](https://loilo.tv/) により Apache 2.0 でオープンソース化
- AE 側近代化 fork: [hiroshisaito/smooth](https://github.com/hiroshisaito/smooth) — Rust core, MFR, 32bpc float, build-id UI, クロスプラットフォームテストハーネス
- OpenFX SDK 1.5.1 ([Academy Software Foundation](https://www.openeffects.org/))

---

Apache License 2.0. ライセンス全文は [LICENSE](https://github.com/hiroshisaito/smooth-ofx/blob/v1.6.0/LICENSE) を参照。
