# Rocky Linux 9.5 向け smooth-ofx 1.6.0 ビルド申し送り

[English](BUILD_HANDOFF_LINUX.md) ・ [BUILDING_ja.md](BUILDING_ja.md)
(リファレンス)

このドキュメントは macOS で 1.6.0 出荷確定後に、**Rocky Linux 9.5
マシンでそのまま smooth.ofx をビルド・テストするための手順** を
まとめたものです。RHEL-9 系 (AlmaLinux 9 / Oracle Linux 9 等) でも
ほぼ同手順で動作。

**前提**: Rocky Linux 9.5 x86_64、sudo 権限、ネット接続あり、約 5 GB
の空き容量 (Rust / build artefacts 込み)。

---

## 1. 必要ツールのインストール

すべて root or `sudo` で。

### 1.1 ビルド系基本パッケージ

```bash
sudo dnf install -y gcc gcc-c++ make git cmake
```

確認:

```bash
gcc --version       # 11.5.0 以上 (Rocky 9.5 default = 11.5)
cmake --version     # 3.26 以上 (Rocky 9.5 default = 3.26.5)
git --version
```

`cmake --version` が **3.20 未満** の場合は CRB (旧 PowerTools)
リポジトリを有効化してから再インストール:

```bash
sudo dnf config-manager --set-enabled crb
sudo dnf install -y cmake
```

### 1.2 Rust toolchain

rustup でユーザーローカルにインストールするのが推奨 (distro Rust
は古い場合があるため):

```bash
# 通常ユーザーで実行 (sudo 不要)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
source "$HOME/.cargo/env"

cargo --version            # cargo 1.94.0 以上目安
rustc --version
rustup target list --installed | grep linux
# → x86_64-unknown-linux-gnu が表示されること
```

(将来的に GPU プロトタイプ `USE_GPU_CORE=ON` で wgpu Vulkan を使う
場合は `mesa-vulkan-drivers vulkan-loader vulkan-tools` の追加が
必要だが、本番出荷経路では不要。)

---

## 2. リポジトリクローンとサブモジュール初期化

```bash
mkdir -p ~/src
cd ~/src
git clone https://github.com/<your-fork>/smooth-ofx.git
cd smooth-ofx
git submodule update --init --recursive
```

確認: `include/openfx/include/ofxImageEffect.h` と
`smooth-ae/rust/smooth_core/Cargo.toml` が存在すること。

---

## 3. ビルド (本番経路 = `USE_RUST_CORE=ON`、出荷既定)

### 3.1 CMake configure

```bash
cmake -S . -B build-linux \
  -DCMAKE_BUILD_TYPE=Release
```

成功時、最後に以下のような行が出る:

```
-- smooth-ofx build id: 1.6.0+<sha>
-- smooth_core: rust targets = x86_64-unknown-linux-gnu
-- smooth_core: linking .../build-linux/cargo-target/x86_64-unknown-linux-gnu/release/libsmooth_core.a
-- Configuring done
-- Generating done
```

build id の sha が `git rev-parse --short HEAD` と一致しているか
**必ず確認**。`+dirty` が付いていれば work tree に変更があるので
コミットしてから再 configure。

### 3.2 ビルド

```bash
cmake --build build-linux --config Release
```

出力: `build-linux/smooth.ofx` (約 600 KB の ELF shared object、
`.ofx` 拡張子)

```bash
file build-linux/smooth.ofx
# → ELF 64-bit LSB shared object, x86-64, dynamically linked
ldd build-linux/smooth.ofx
# → libstdc++ / libm / libgcc_s / libpthread / libc など標準のみ
```

### 3.3 ビルド検証 (host_smoke)

```bash
cmake --build build-linux --config Release --target host_smoke
build-linux/host_smoke build-linux/smooth.ofx
```

期待出力:

```
[host-smoke] kOfxActionLoad -> 0
[host-smoke] kOfxActionDescribe -> 0
[host-smoke] kOfxImageEffectActionDescribeInContext -> 0
    param: name=buildInfo      type=OfxParamTypeString label=build
[host-smoke] buildInfo = "1.6.0+<sha> / rust core 0.1.0+<smooth-ae-sha>"
[host-smoke] [ 8bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [16bpc] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] [float] pure0=562 pureMax=562 intermediate=924 / 2048
[host-smoke] DONE
```

`pure0=562 pureMax=562 intermediate=924 / 2048` の 3 行が
**3 つの pixel depth で全部出ること** を確認 (macOS / Windows 出荷版と
同じ出力)。

---

## 4. 配布用パッケージング

### 4.1 OFX バンドル構造の作成

```bash
DIST=dist/linux-x86-64
rm -rf "$DIST"
mkdir -p "$DIST/smooth.ofx.bundle/Contents/Linux-x86-64"
cp build-linux/smooth.ofx "$DIST/smooth.ofx.bundle/Contents/Linux-x86-64/"
```

OFX 仕様の Linux バンドル構造:

```
smooth.ofx.bundle/
└── Contents/
    └── Linux-x86-64/
        └── smooth.ofx     (ELF .so)
```

(`Info.plist` は macOS 専用、Linux バンドルには不要)

### 4.2 tarball / zip 作成

tar.gz 形式 (Linux 標準):

```bash
( cd "$DIST" && tar czf ../smooth-1.6.0-linux-x86-64.tar.gz smooth.ofx.bundle )
sha256sum dist/smooth-1.6.0-linux-x86-64.tar.gz | tee dist/smooth-1.6.0-linux-x86-64.tar.gz.sha256
ls -la dist/smooth-1.6.0-linux-x86-64.tar.gz*
```

(zip が必要なら `( cd "$DIST" && zip -r ../smooth-1.6.0-linux-x86-64.zip smooth.ofx.bundle )`)

### 4.3 署名 (Linux では原則不要)

Linux OFX ホスト (DaVinci Resolve Studio Linux 等) は通常コード
署名を要求しない。配布バイナリも未署名で問題なし。

---

## 5. インストールと UAT

### 5.1 OFX プラグインディレクトリへのコピー

OFX 標準のシステムワイドパス:

```bash
sudo rm -rf /usr/OFX/Plugins/smooth.ofx.bundle
sudo mkdir -p /usr/OFX/Plugins
sudo cp -R "$DIST/smooth.ofx.bundle" /usr/OFX/Plugins/
```

DaVinci Resolve Studio Linux の場合、Resolve 専用パスを併用:

```bash
sudo cp -R "$DIST/smooth.ofx.bundle" "/var/BlackmagicDesign/DaVinci Resolve/Support/OFX/Plugins/"
```

(Resolve のバージョンとインストール方法によりパスが異なる場合あり。
標準は `/usr/OFX/Plugins/` だが Resolve が独自パスを優先することがある。)

### 5.2 UAT (実機テスト 5 点 / Resolve Linux Studio 前提)

DaVinci Resolve を **完全終了 → 再起動**。
macOS / Windows 1.6.0 と同じ canonical テスト:

| # | 確認 | 期待 |
|---|------|------|
| 1 | Inspector の "build" 欄 | `1.6.0+<sha> / rust core 0.1.0+<smooth-ae-sha>` (`+dirty` 無し / `cpp core` でない) |
| 2 | 8 / 16bpc 整数で Smooth 適用 | 階段が滑らかに、線上に黒なし |
| 3 | **32bpc float + transparent ON** (重点) | **線上に黒なし** + 白(1,1,1) 透過 + クラッシュなし |
| 4 | 32bpc float + transparent OFF | 8/16bpc 同等出力、白色 pixel は不透明維持 |
| 5 | 4K (3840×2160 以上) で render 速度 | macOS 1.6.0 と同等の速度感 |

すべて PASS なら **Linux 1.6.0 ビルド完了**。

(Resolve Linux が手元にない場合は、フリーで動く OFX ホストの
[Natron](https://natrongithub.github.io/) でも代替検証可能。)

---

## 6. ビルド後の申し送り (macOS チーム宛)

以下の情報を共有:

```text
Linux 1.6.0 ビルド報告
=========================
日時:        YYYY-MM-DD HH:MM JST
ホスト:      Rocky Linux 9.5 (kernel 5.14.x)、x86_64
コンパイラ:  gcc x.y.z
Rust:        rustc 1.x.y (yyy)
CMake:       3.x.y
Build ID:    1.6.0+<sha>
host_smoke:  3 paths PASS (562/562/924)
tarball:     dist/smooth-1.6.0-linux-x86-64.tar.gz
sha256:      <hash>
UAT:         5/5 PASS  (or NG details)
ホスト:      DaVinci Resolve Studio x.y / Natron / etc.
備考:        <あれば>
```

---

## 7. トラブルシューティング

### `cmake --version` が 3.20 未満
→ CRB リポジトリ有効化後に再インストール (§ 1.1 参照)。

### `cargo: command not found` (新規シェル)
→ `~/.cargo/env` を `~/.bashrc` に source していない:
```bash
echo 'source "$HOME/.cargo/env"' >> ~/.bashrc
source ~/.bashrc
```

### `cmake -S . -B build-linux` でエラー: `OFX SDK headers not found`
→ submodule 未初期化:
```bash
git submodule update --init --recursive
```

### `cargo build` で長時間かかる
→ 出荷経路 (`USE_RUST_CORE=ON`) では rayon と関連依存クレートを
   初回にクレート単位でコンパイルするため 1-2 分かかるのが正常。
   `USE_GPU_CORE=ON` を付けた場合は wgpu のフルツリーが追加される
   ためさらに長時間 (4-5 分) かかる。再ビルドは数秒。

### Resolve / Natron に Smooth が現れない
1. プラグインバンドルが `/usr/OFX/Plugins/smooth.ofx.bundle/Contents/Linux-x86-64/smooth.ofx` に存在するか
2. パーミッション: `ls -l /usr/OFX/Plugins/smooth.ofx.bundle/Contents/Linux-x86-64/smooth.ofx` で実行可 (`-rwxr-xr-x`) であること
3. Resolve を完全終了して再起動
4. SELinux が enforcing の場合、`getenforce` 確認、必要なら `restorecon -Rv /usr/OFX/Plugins/smooth.ofx.bundle`

### `ldd` で `not found` のライブラリがある
→ glibc / libstdc++ のバージョン不整合。Rocky 9.5 標準 (glibc 2.34)
   でビルドすれば、glibc 2.34 以上の他 RHEL 9 系ホストで動くはず。
   古いディストロ向けに配るなら別途検討。

### host_smoke が `pure0=562` を返さない
→ 出力が macOS / Windows 1.6.0 と異なる場合、ABI / アルゴリズムの
   本質的な不整合。`uname -a` と CMake 出力を共有して macOS チームに
   エスカレーション。

---

## 8. 参考リンク

- [BUILDING_ja.md](BUILDING_ja.md) — 全プラットフォーム共通リファレンス
- [CHANGELOG.md](CHANGELOG.md) — 1.6.0 で変わった内容
- [RELEASE_NOTES_1.6.0_ja.md](RELEASE_NOTES_1.6.0_ja.md) — リリース詳細
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) — 第三者ライセンス
