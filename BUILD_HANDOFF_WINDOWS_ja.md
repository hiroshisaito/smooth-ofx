# Windows 11 向け smooth-ofx 1.6.0 ビルド申し送り

[English](BUILD_HANDOFF_WINDOWS.md) ・ [BUILDING_ja.md](BUILDING_ja.md)
(リファレンス)

このドキュメントは macOS で 1.6.0 出荷確定後に、**Windows 11 マシンで
そのまま smooth.ofx をビルド・テストするための手順** をまとめた
ものです。経験者向けの簡潔ガイド (`BUILDING_ja.md`) ではなく、
コピペで進められる step-by-step を目指します。

**前提**: Windows 11 64-bit、管理者権限あり、ネット接続あり、約 30 GB
の空き容量 (Visual Studio 込み)。

---

## 1. 必要ツールのインストール

### 1.1 Visual Studio 2022 Community (必須)

1. <https://visualstudio.microsoft.com/downloads/> から **Community
   2022** のインストーラを取得
2. Visual Studio Installer 起動 → **Workloads** タブで以下を選択:
   - ☑ **Desktop development with C++** (これだけで OK)
3. 右側の「Installation details」を確認し、最低限以下が含まれている
   こと:
   - MSVC v14x — お使いの VS の C++ x64/x86 build tools
     (VS 2022 = v143、VS 2026 = v144)
   - Windows 11 SDK (**10.0.26100 以上推奨**。22621 系でもビルドは
     通るが、Rust 1.94 std stdio が要求する `__imp_NtWriteFile` /
     `__imp_RtlNtStatusToDosError` のリンクは CMake 側で自動的に
     `ntdll`/`userenv`/`ws2_32`/`dbghelp` を引き込む構成済 —
     詳細は CHANGELOG の「Windows MSVC 修正」参照)
   - C++ CMake tools for Windows (CMake 3.27 以上を含む)
4. インストール (~10 GB ダウンロード、~30 分)

### 1.2 Git for Windows (必須)

1. <https://git-scm.com/download/win> からインストーラ取得
2. デフォルト設定でインストール
3. 確認: スタートメニュー → "Git Bash" 起動して `git --version`

### 1.3 Rust toolchain (必須)

PowerShell を **管理者権限** で起動:

```powershell
# rustup-init をダウンロードして実行 (デフォルト設定で OK = 1 → Enter)
Invoke-WebRequest -Uri https://win.rustup.rs/x86_64 -OutFile rustup-init.exe
.\rustup-init.exe -y --default-host x86_64-pc-windows-msvc --default-toolchain stable
```

新しい PowerShell ウィンドウを開き直して確認:

```powershell
cargo --version
rustc --version
rustup target list --installed
```

`x86_64-pc-windows-msvc` が出ていれば OK。

---

## 2. リポジトリクローンとサブモジュール初期化

PowerShell (通常権限で OK):

```powershell
# 任意の作業ディレクトリで実行 (例: C:\src)
mkdir C:\src
cd C:\src
git clone https://github.com/<your-fork>/smooth-ofx.git
cd smooth-ofx
git submodule update --init --recursive
```

確認: `include\openfx\include\ofxImageEffect.h` と
`smooth-ae\rust\smooth_core\Cargo.toml` が存在すること。

---

## 3. ビルド (本番経路 = `USE_RUST_CORE=ON`、出荷既定)

### 3.1 CMake configure

PowerShell から (お使いの VS のジェネレータ名に合わせる):

```powershell
# VS 2026 (推奨、commit c23db4c で実機確認済の組合せ)
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64

# VS 2022 利用時はジェネレータ名を読み替え
# cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
```

成功時、最後に以下のような行が出る:

```
-- smooth-ofx build id: 1.6.0+<sha>
-- smooth_core: rust targets = x86_64-pc-windows-msvc
-- smooth_core: linking ...build-msvc/cargo-target/x86_64-pc-windows-msvc/release/libsmooth_core.lib
-- Configuring done
-- Generating done
```

build id の sha が `git rev-parse --short HEAD` と一致していることを
**必ず確認**。`+dirty` が付いていれば work tree に変更があるので
コミットしてから再 configure。

### 3.2 ビルド

```powershell
cmake --build build-msvc --config Release
```

出力: `build-msvc\Release\smooth.ofx` (約 600 KB の DLL、`.ofx`
拡張子)

### 3.3 ビルド検証 (host_smoke)

```powershell
cmake --build build-msvc --config Release --target host_smoke
build-msvc\Release\host_smoke.exe build-msvc\Release\smooth.ofx
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
**3 つの pixel depth で全部出ること** を確認 (macOS 出荷版と同じ
出力)。

---

## 4. 配布用パッケージング

### 4.1 OFX バンドル構造の作成

```powershell
$dist = "dist\windows-x64"
Remove-Item -Recurse -Force $dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$dist\smooth.ofx.bundle\Contents\Win64" | Out-Null
Copy-Item build-msvc\Release\smooth.ofx "$dist\smooth.ofx.bundle\Contents\Win64\"
```

OFX 仕様の Windows バンドル構造:

```
smooth.ofx.bundle\
└── Contents\
    └── Win64\
        └── smooth.ofx     (MSVC DLL)
```

(macOS / Linux と異なり Windows バンドルには `Info.plist` 不要)

### 4.2 zip 作成

```powershell
Compress-Archive -Path "$dist\smooth.ofx.bundle" -DestinationPath "dist\smooth-1.6.0-windows-x64.zip" -Force
```

SHA-256 計算:

```powershell
Get-FileHash -Algorithm SHA256 dist\smooth-1.6.0-windows-x64.zip | Format-List | Out-File -FilePath dist\smooth-1.6.0-windows-x64.zip.sha256 -Encoding ASCII
Get-Content dist\smooth-1.6.0-windows-x64.zip.sha256
```

### 4.3 Authenticode 署名 (社内テスト配布なら省略可)

社内テスト配布 / 個人検証であれば未署名で問題なし。Windows
SmartScreen が初回ロード時に警告を出す場合があるが「詳細情報 →
実行」で回避可能。

公開配布する場合は Developer ID 相当の Authenticode 証明書が必要:

```powershell
# 署名 (証明書を入れた PFX ファイルがある場合の例)
signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /f your-cert.pfx /p <password> build-msvc\Release\smooth.ofx

# 署名検証
signtool verify /pa /v build-msvc\Release\smooth.ofx
```

---

## 5. インストールと UAT

### 5.1 DaVinci Resolve / Fusion へのインストール

PowerShell **管理者権限** で:

```powershell
$plug = "C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle"
if (Test-Path $plug) { Remove-Item -Recurse -Force $plug }
Copy-Item -Recurse "$dist\smooth.ofx.bundle" "C:\Program Files\Common Files\OFX\Plugins\"
```

DaVinci Resolve を **完全終了 → 再起動**。

### 5.2 UAT (実機テスト 5 点)

macOS 1.6.0 UAT と同じ canonical テストを実施:

| # | 確認 | 期待 |
|---|------|------|
| 1 | Inspector の "build" 欄 | `1.6.0+<sha> / rust core 0.1.0+<smooth-ae-sha>` (`+dirty` 無し / `cpp core` でない) |
| 2 | 8 / 16bpc 整数で Smooth 適用 | 階段が滑らかに、線上に黒なし |
| 3 | **32bpc float + transparent ON** (重点) | **線上に黒なし** + 白(1,1,1) 透過 + クラッシュなし |
| 4 | 32bpc float + transparent OFF | 8/16bpc 同等出力、白色 pixel は不透明維持 |
| 5 | 4K (3840×2160 以上) で render 速度 | macOS 1.6.0 と同等の速度感 (4K 8bpc で ~120ms/frame 目安) |

すべて PASS なら **Windows 1.6.0 ビルド完了**。

---

## 6. ビルド後の申し送り (macOS チーム宛)

以下の情報を共有:

```text
Windows 1.6.0 ビルド報告
=========================
日時:        YYYY-MM-DD HH:MM JST
ホスト:      Windows 11 (バージョン xxx)、x86_64
SDK:         Windows 11 SDK 10.0.xxxxx
コンパイラ:  MSVC v14x (VS 2022 17.x.x or VS 2026 18.x.x)
Rust:        rustc 1.x.y (yyy)
Build ID:    1.6.0+<sha>
host_smoke:  3 paths PASS (562/562/924)
zip:         dist\smooth-1.6.0-windows-x64.zip
sha256:      <hash>
UAT:         5/5 PASS  (or NG details)
備考:        <あれば>
```

---

## 7. トラブルシューティング

### `cmake -G "Visual Studio 17 2022"` で「ジェネレータが見つからない」
→ Visual Studio Installer で C++ CMake tools が未選択。Workload
再インストール。

### `cargo: command not found` / `rustc not found`
→ rustup インストール後に PowerShell を再起動していない。
新規 PowerShell ウィンドウで再確認。

### `cargo build` で「linker `link.exe` not found」
→ Rust が MSVC リンカを見つけられていない。
PowerShell の代わりに **"x64 Native Tools Command Prompt for VS 2022"**
からビルドコマンドを実行 (スタートメニューから検索)。

### `git submodule update` でネットワークエラー
→ プロキシ環境では `git config --global http.proxy ...` が必要な
場合あり。社内 IT に確認。

### Resolve に Smooth が現れない
1. プラグインバンドルが
   `C:\Program Files\Common Files\OFX\Plugins\smooth.ofx.bundle\Contents\Win64\smooth.ofx`
   に存在するか確認
2. Resolve を完全終了して再起動
3. ファイルが他プロセス (前の Resolve / explorer の preview) に
   ロックされていれば、再ログオンして再コピー

### host_smoke が `pure0=562` を返さない
→ 出力が macOS 1.6.0 と異なる場合、ABI / アルゴリズムの本質的な
   不整合。stack trace と CMake 出力を共有して macOS チームに
   エスカレーション。

---

## 8. 参考リンク

- [BUILDING_ja.md](BUILDING_ja.md) — 全プラットフォーム共通リファレンス
- [CHANGELOG.md](CHANGELOG.md) — 1.6.0 で変わった内容
- [RELEASE_NOTES_1.6.0_ja.md](RELEASE_NOTES_1.6.0_ja.md) — リリース詳細
- [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) — 第三者ライセンス
