# MHGU iOS 使用指南：解包、装 Mod、Actions 打包

适用对象：使用 MHGURecomp-iOS（MHGU 原生 iOS 移植版）的人。项目不含任何版权内容，游戏文件全部自备。

---

## 一、解包游戏（XCI → exefs + romfs）

### 1.1 需要准备

| 东西 | 要求 |
|---|---|
| 游戏 XCI | `MONSTER HUNTER GENERATIONS ULTIMATE [0100770008DD8000]`，**必须是 (1G+1U)**——本体+更新都在包里。v262144 = 游戏内 **1.4.0**，移植版只认这个版本，只有本体的 XCI 解不出更新 |
| prod.keys | 自己主机导出的密钥文件（ProdKeys.NET / Lockpick 导出即可） |
| hactool | 1.4.0（SciresM 开源工具），解包唯一硬依赖 |
| 磁盘 | 预留 ≥30GB |

### 1.2 原理（一分钟版）

XCI 里真正要的是两个 Program NCA：**本体**（约 12GB，普通 romfs）和**更新**（约 200MB，BKTR 差分格式）。更新的 romfs 是差分包，必须叠加在本体上才能解出完整内容。识别不靠文件名：用 hactool 逐个查 NCA 头，`Content Type: Program` + `Title ID: 0100770008dd8000`，其中带 `BKTR` 字样的是更新。

### 1.3 已知坑：密钥文件的畸形行

ProdKeys.NET 导出的 prod.keys 里 `mariko_master_kek_source_*` 等 30 来条是 **34 位十六进制的畸形行**（导出工具 bug），hactool 会直接拒收：`Key (…) must be 32 hex digits!`。

解法：过滤出清洗版——只保留值为 32 或 64 位十六进制的行，其余丢弃（畸形行对卡带解密无用）。原文件不要动。

### 1.4 解包三步

```bash
# 1) 拆出 XCI 的 secure 分区（全部 NCA）
hactool -k <清洗版keys> -t xci --securedir=<临时目录>/nca "<XCI>"

# 2) 逐个查 NCA 头，找出本体（无 BKTR）和更新（带 BKTR）
hactool -t nca <每个.nca>

# 3) 更新叠加本体，解出最终产物（约 2~5 分钟）
hactool -t nca --basenca=<本体.nca> \
        --exefsdir=<资源目录>/exefs \
        --romfsdir=<资源目录>/romfs  <更新.nca>
```

### 1.5 验证（必做）

- `exefs/` 里应有 5 个文件：`main、main.npdm、rtld、sdk、subsdk0`；`romfs/` 里应有 `nativeNX/` 等目录
- **版本铁证**：`exefs/main` 的 NSO0 头偏移 0x40 处 32 字节 build id，1.4.0 应以 `fb08f1d20fd1204fe7070672777f869b` 开头
- `exefs/main` 前 4 字节应为 `NSO0`

### 1.6 放进 iPhone / iPad

App 容器的 `data/` 目录放 `exefs/` 和 `romfs/` 两个文件夹：

- iOS：文件 App → 我的 iPhone/iPad → MHGURecomp → `data/`
- 12GB / 3.5 万个文件，强烈建议走 USB（iTunes/Finder 文件共享拖文件夹），比 Wi-Fi 快得多

---

## 二、安装 Mod

App 内置了 mod 加载系统（`mods/` 文件夹，与 `data/` 同级），支持以下几种形态，**都放 `mods/` 里即可**：

### 2.1 压缩包模组（最常用）

把 `.zip` 或 `.7z` **原样**丢进 `mods/`，**不要解压**——App 内置解压器（含 7z/LZMA），首次启动自动解压安装（一次性），完成后压缩包改名 `.installed` 留档，立即生效。

### 2.2 高清纹理包（MHGU HD Project）

发布页：Monkbreh/MHGU-HD-Project 的 Releases（GameBanana mods/414813 同源）。

| 下载什么 | 说明 |
|---|---|
| `Main.7z`（约 618MB） | 主体高清纹理，7600+ 张，必装 |
| `Monsters.7z`（约 240MB） | 怪物高清纹理，推荐 |
| UI 布局包 **三选一** × 语言 | `Nintendo.Layout_chs.7z`（Switch 键位简中）等 3 种键位风格 × 7 语言；**只能装一个**，同文件互相覆盖 |

同样直接拖 7z 进 `mods/`，自动安装。预留 3-4GB 空间。

### 2.3 代码插件（Forge .nro）

`.nro` 代码模组（伤害数字、关键任务标记等）以 `.zip` 或解压后的文件夹形式放进 `mods/`，App 内用 Forge 加载，游戏里有 ImGui 悬浮面板可开关。

### 2.4 手动 romfs 覆盖 / 代码补丁

- 想手工替换某个游戏文件：建 `mods/<模组名>/romfs/…` 按原路径放文件，同路径直接顶替原版
- IPS / IPSwitch（.pchtxt）代码补丁：放 `mods/<模组名>/exefs/`，按 NSO 的 build id 命名匹配

### 2.5 回退

删掉 `mods/` 下对应文件夹（或 `.installed` 留档和解压目录一起删）重启即可。

---

## 三、用 GitHub Actions 打包 IPA

### 3.1 准备

fork `WiseManChris/MHGURecomp-iOS`（或任何已修好链接错误的分支）到自己的账号。仓库自带 `.github/workflows/build-ios.yml`，在 macos-14 runner 上编译（依赖：MoltenVK v1.3.0 官方静态库 + SDL3 + shaderc，均由 CI 自动下载/编译）。

> 注意：上游 master 存在链接错误（overlay.cpp 引用无 `extern "C"` 守卫的 config.h，`config_set_join` 符号未定义）——如果 fork 上游后 CI 红，先给 `runtime/config.h` 套上 `extern "C" {}` 守卫再构建。

### 3.2 触发构建

```bash
# 坑：新 fork 的 push 可能不触发 Actions（GitHub 的首用未确认状态），手动点火最稳
gh workflow run build-ios.yml --repo <你的用户名>/MHGURecomp-iOS --ref master

# 盯进度；注意网络瞬断时 watch 会提前退出且 exit code 可能误导，务必实查结论
gh run watch <run-id> --repo <你的用户名>/MHGURecomp-iOS --exit-status
gh run view <run-id> --repo <你的用户名>/MHGURecomp-iOS --json status,conclusion
```

耗时：首次构建依赖链要 20-40 分钟；之后命中缓存约 5-10 分钟。也可在仓库网页 Actions 页面手动 Run workflow。

### 3.3 下载与发布

```bash
# 下载产物（Artifacts 名 = MHGURecomp-iOS-ipa，保留 14 天）
gh run download <run-id> --repo <你的用户名>/MHGURecomp-iOS \
  --name MHGURecomp-iOS-ipa -D <本地目录>

# 发 Release（资产重命名带版本号）
gh release create v<版本> MHGURecomp-v<版本>.ipa \
  --repo <你的用户名>/MHGURecomp-iOS --title "…" --notes "…"
```

### 3.4 签名说明

CI 产出的 IPA 是 **ad-hoc 签名**（entitlement 已内嵌），不能直接安装——用 Sideloadly / AltStore 重签（自动用你的 Apple ID）：免费账号 7 天重签、最多 3 个 App；TrollStore 安装则永久免签。**CI 全程不需要任何开发者证书。**

构建环境：iphoneos SDK 17.5（macos-14 runner 自带），部署目标 iOS 15.0。

### 3.5 排障速查

| 症状 | 解决 |
|---|---|
| push 后 Actions 没反应 | 用 `gh workflow run` 手动触发（或到 Actions 页面确认启用） |
| CI 红：`Undefined symbols: config_set_join` | config.h 套 extern "C" 守卫（见 3.1 注） |
| 下载的 IPA 装不上 | 正常，需要 Sideloadly/AltStore 重签 |
| 首次构建超久 | 正常，依赖链在编译，第二次起有缓存 |
