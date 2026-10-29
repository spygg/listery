<div align="center">

# Listery

**双击 Ctrl，全盘程序一秒到位。**

一个用 Qt / C++ 写的 Listary 风格快速启动器：全盘索引可执行文件 → 双击 Ctrl 唤起 → 输入即时补全 → 按使用习惯动态排序。

[![Build & Release](https://github.com/spygg/listery/actions/workflows/release.yml/badge.svg)](https://github.com/spygg/listery/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/spygg/listery?label=release)](https://github.com/spygg/listery/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Qt](https://img.shields.io/badge/Qt-5.15%20%2F%206.x-41CD52?logo=qt&logoColor=white)](https://www.qt.io)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](https://github.com/spygg/listery/releases)
[![Language](https://img.shields.io/badge/language-C%2B%2B-orange.svg)](CMakeLists.txt)

[简体中文](README.md) · [English](README_EN.md)

</div>

---

![输入即搜：命中字符高亮，常用程序自动上浮](docs/preview.png)

![没有输入时只显示一个输入框，不占桌面](docs/preview-empty.png)

## 为什么用它

| 能力 | 说明 |
| --- | --- |
| **双击 Ctrl 唤起** | 300ms 内两次 Ctrl 才触发，中间夹了别的键（如 `Ctrl+C`）不算，不会误伤日常快捷键 |
| **输入即搜** | 35ms 防抖 + 后台检索线程，界面从不执行 SQL；连续输入只保留最新一次请求 |
| **拼音检索** | `tengxunhuiyi`、`txhy`、`腾讯会议` 三种输入命中同一个程序；多音字保留全部读音 |
| **越用越准** | 排序分 = `0.72 × 匹配度 + 0.28 × 使用习惯`，常用程序自动上浮 |
| **智能扫描** | 按分区自适应调度：没装过软件的分区自动降低扫描频率；Linux 上直接读 locate 快照 |
| **免安装** | 索引、配置、日志都在可执行文件旁边，拷走 U 盘也能用 |

## 快速开始

1. 到 [Releases](https://github.com/spygg/listery/releases) 下载对应平台的压缩包，解压；
2. 运行 `listery`（Windows 为 `listery.exe`），程序静默进托盘并开始建立索引；
3. **双击 Ctrl**，输入程序名，回车运行。

> Windows XP 用户请下载 `listery-windows-xp.zip`（MinGW 32 位 + Qt 5.7 构建）。

### 快捷键

| 按键 | 作用 |
| --- | --- |
| `双击 Ctrl` | 唤起 / 隐藏搜索面板 |
| `↑` `↓` `PgUp` `PgDn` | 在候选里移动 |
| `Enter` | 运行选中的程序 |
| `Ctrl + Enter` | 在文件管理器中定位 |
| `Shift + Enter` | 以管理员身份运行 |
| `Esc` | 隐藏面板 |
| 右键候选 | 管理员运行 / 打开所在位置 / 复制路径 |

## 智能索引

全盘遍历一次要几分钟、几万次系统调用，而绝大多数分区根本没装过软件。Listery 因此不再"每隔 N 小时无脑全扫一遍"：

### 按分区自适应调度

每个分区单独记录"上次扫到多少条可执行文件、什么时候扫的、连续空扫了几轮"，刷新间隔据此伸缩（以默认 12 小时为例）：

| 分区情况 | 刷新间隔 | 说明 |
| --- | --- | --- |
| 程序很多（> 5 条） | 12 小时 | 用户配置的间隔，正常刷新 |
| 只有零星几个（1–5 条） | 24 小时 | 放宽 2 倍 |
| 一个都没有（第 1 轮空扫） | 2 天 | 放宽 4 倍 |
| 连续空扫 2 / 3 / 4 轮 | 4 天 / 8 天 / 14 天 | 指数退避，封顶 14 天 |
| 从没扫过 | 立即 | 首次启动必须建索引 |
| 上一轮快速通道结果可疑 | 立即（且强制全量遍历） | 例如 locate 库太旧导致"突然一条都没有" |

关键点：**一次只扫到期的分区**，没到期的分区不会被顺带遍历；只扫部分分区时也只清理这些分区的失效记录，其它分区的索引不受影响。

### Linux：用 locate 秒级建索引

Linux 上优先读取系统已有的 `plocate` / `mlocate` 快照（通常在 `/var/lib/{plocate,mlocate}/`）而不是遍历文件系统：

- 两条 `locate -r` 查询搞定：白名单后缀 + `bin`/`sbin`/`games` 目录下带可执行位的文件（Unix 程序大多没有后缀）；
- 建索引从"几分钟"变成"一两秒"，几乎不产生磁盘 IO；
- 快照由系统的 `updatedb` 每天更新，所以**当天新装的程序最多晚一天进入索引**；为保证这点，清理失效记录时会保留 mtime 晚于快照时间的条目；
- locate 不可用（没装或没有数据库）时自动退回全量遍历；托盘里的"重新扫描全盘"始终是强制全量遍历。

### 其它省资源的做法

- 扫描线程以最低优先级运行（Windows 降线程优先级，Linux 抬高 nice 值），不与前台抢 CPU；
- 遍历用 `QDirIterator` 逐个产出，并**先用文件名过滤再做 stat**——系统盘里 95% 以上的文件都不是可执行文件，这一步省掉了它们的系统调用；
- 面板正在被使用时推迟扫描（最多让 3 次），不抢搜索时的磁盘 IO；
- 每 2000 条提交一次事务；数据库 WAL 模式，扫描写入与界面检索互不阻塞。

## 构建

要求：Qt 5.15+ 或 Qt 6.x（Widgets + Sql + Network），CMake ≥ 3.16，Ninja 或 MSVC。

<details open>
<summary><b>Windows</b>（Qt 5.15.2 + MinGW 8.1）</summary>

```bat
build.bat
```

等价的手工命令：

```bat
set "QTDIR=E:\programes\Qt\Qt\5.15.2\mingw81_64"
set "MINGW=E:\programes\Qt\Qt\Tools\mingw810_64"
set "PATH=%MINGW%\bin;E:\programes\Qt\Qt\Tools\Ninja;%QTDIR%\bin;%PATH%"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%QTDIR%
cmake --build build --parallel
```

发布目录（带 Qt 运行库，可直接双击）：

```bat
deploy.bat                  :: 默认取 build\ 产物，Qt5 运行库
deploy.bat build-qt5        :: 也可指定其它构建目录
```

Qt 6 只需把 `QTDIR` 换成对应套件，代码里已做 Qt5/Qt6 兼容处理。
</details>

<details>
<summary><b>Linux</b></summary>

```bash
sudo apt install -y build-essential cmake ninja-build qtbase5-dev libqt5sql5-sqlite libxtst-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=xcb ./build/listery        # 需要 X11 会话；Wayland 下热键不可用，托盘仍可用
```

XRecord 头文件来自 `libxtst-dev`；缺少时 CMake 会给出 WARNING，程序仍可编译，只是无法监听全局双击 Ctrl。Qt6 把 `qtbase5-dev` 换成 `qt6-base-dev`。
</details>

<details>
<summary><b>macOS</b></summary>

```bash
brew install qt@5 cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5)"
cmake --build build --parallel
./build/listery
```

首次运行需要在"系统设置 → 隐私与安全性 → 辅助功能 / 输入监控"里勾选 Listery，否则 `CGEventTap` 创建失败（托盘会提示原因）。
</details>

## 运行

```bat
build\listery.exe              :: 常驻托盘，等待双击 Ctrl
build\listery.exe --show       :: 启动后立即打开搜索框
build\listery.exe --rescan     :: 启动后立刻重建索引
build\listery.exe --autostart  :: 开机自启用：静默进托盘，不主动弹窗
build\listery.exe --selftest   :: 无界面自检（结果写日志，退出码 0 表示全部通过）
build\listery.exe --render-preview out.png "c"      :: 离屏渲染"有结果"面板
build\listery.exe --render-preview out.png --empty  :: 离屏渲染"空输入只有输入框"面板
```

Linux / macOS 把 `build\listery.exe` 换成 `./build/listery`，其余参数相同。

数据目录：**可执行文件所在目录**（目录不可写时回退到各平台的 `AppDataLocation`）

| 文件 | 用途 |
| --- | --- |
| `index.db` | SQLite 索引（`executables` / `usage_stats` / `app_meta` / `volume_stats` 表 + FTS5 倒排） |
| `settings.ini` | 后缀白名单、跳过目录、可移动磁盘、智能调度、热键间隔、结果数量等 |
| `listery.log` | 运行日志（含扫描错误与热键告警） |

删除 `index.db` 或点"重新扫描全盘"即可重建索引；使用统计在 `usage_stats` 中，重建索引不会清空。

## 跨平台支持

三个平台共用同一套索引、检索、排序与界面代码，平台差异集中在 `src/Platform/` 与 `src/Core/LaunchService.cpp`：

| 能力 | Windows | Linux | macOS |
| --- | --- | --- | --- |
| 全局双击 Ctrl | `WH_KEYBOARD_LL` 低级键盘钩子 | X11 XRecord（需 `libXtst`；Wayland 会话不可用） | `CGEventTap`（需授予"辅助功能 / 输入监控"权限） |
| 建索引 | 全量遍历所有本地卷 | **优先读 locate 快照**，不可用才遍历 | 遍历 `/` 与 `/Volumes/*` |
| 启动程序 | `ShellExecuteW`（`runas` 提权） | `QProcess::startDetached`；提权走 `pkexec` | LaunchServices `open`（`.app` 包自动识别）；提权走 `osascript` |
| 打开所在位置 | `explorer /select` | nautilus / dolphin / nemo / thunar，失败回退 `xdg-open` | `open -R` |
| 开机自启动 | HKCU `...\CurrentVersion\Run` | `~/.config/autostart/listery.desktop` | `~/Library/LaunchAgents/com.listery.launcher.plist` |

**验证范围（如实说明）**：

- **Windows**：本机 Qt 5.15.2 + MinGW 8.1 实际编译、运行，56 项自检全部通过，打包后的 `dist\` 在无 Qt 环境也可运行；
- **Linux**：在 WSL2 Ubuntu 24.04（Qt 5.15.13、`libXtst`、XRecord 头文件齐全）实际编译通过，并以 `QT_QPA_PLATFORM=offscreen` 跑完自检；全局双击 Ctrl 需要真实 X11 桌面会话，未在无头环境实测；
- **macOS**：代码路径已按 `CGEventTap`、`open -R`、`LaunchAgents` 实现，但当前没有 Mac 机器，**未编译、未运行验证**。

## 工程结构

```
listery/
├─ CMakeLists.txt
├─ build.bat / deploy.bat
├─ .github/workflows/release.yml   # 三平台（含 XP）矩阵构建与发布
├─ src/
│  ├─ main.cpp                 程序入口、单实例、预览与自检开关
│  ├─ AppController.*          组装索引库 / 扫描调度 / 热键 / 面板 / 托盘
│  ├─ Core/
│  │  ├─ AppSettings.*         基于 settings.ini 的配置
│  │  ├─ Database.*            SQLite 表结构、批量写入、检索 SQL、卷统计
│  │  ├─ Scanner.*             扫描线程：全量遍历 / locate 快照 / 按卷统计
│  │  ├─ Pinyin.* / PinyinData.* 汉字转全拼与首字母（静态字表，多音字多变体）
│  │  ├─ SearchEngine.*        粗筛 + 内存精排、匹配高亮
│  │  ├─ SearchService.*       后台检索线程（合并排队请求、按序号丢弃过期结果）
│  │  ├─ UsageRecorder.*       使用统计写入（独立线程 + 独立连接）
│  │  └─ LaunchService.*       启动 / 提权 / 定位（三平台分支）
│  ├─ Platform/
│  │  ├─ HotkeyHook.*          全局双击 Ctrl（Win 钩子 / X11 XRecord / CGEventTap）
│  │  ├─ DoubleCtrlDetector.h  双击 Ctrl 状态机（可单测）
│  │  ├─ Autostart.*           开机自启动（Run 项 / .desktop / LaunchAgent）
│  │  ├─ WindowUtils.*         强制前台、屏幕居中靠上定位
│  │  └─ Branding.*            运行时绘制图标
│  ├─ Ui/
│  │  ├─ LauncherWindow.*      搜索面板（无边框 + 阴影 + 键盘导航 + 右键菜单 + 空输入收起）
│  │  ├─ ResultModel.* / ResultDelegate.*  结果模型与行绘制（图标 / 高亮 / 路径 / 次数）
│  │  └─ SettingsDialog.*      设置界面
│  └─ SelfTest.*               56 项自检
└─ docs/preview.png
```

## 检索与排序细节

<details>
<summary><b>检索流程</b></summary>

1. 输入框 35ms 防抖后把查询交给 `SearchService` 的检索线程，GUI 线程从不执行 SQL；
2. 请求带自增序号：连续输入时排队中的旧请求会被合并，结果回来时序号不是最新的一律丢弃；
3. 先用使用统计种入候选：取启动次数最高的几百个程序，按本次输入做名字前缀过滤，保证常用程序不会因为粗筛截断被挤掉；
4. 单词查询先走 `name` 索引做前缀范围扫描（20 万条约 0.6~1.5ms），逐字打字时这一趟通常就能填满一屏；
5. 前缀扫描不够一屏时补一次 FTS5 词首匹配（`"chr"*`，`name` / `dir` / `pinyin` / `pinyin_initials`），覆盖 `studio → visual studio code`、`tengxunhuiyi → 腾讯会议` 这类词首命中；带扩展名的输入（`kylin.bat`）会拆成子词分别匹配——FTS5 在 `detail=column` 下不接受含标点的整段短语；
6. 只有前缀和 FTS 都落空时才跑 `LIKE '%关键字%'` 子串扫描兜底（20 万条约 50ms，仅在检索线程）；
7. 命中候选只取 `path/name/dir/ext` 等列，使用统计通过一次快照补齐，再在内存里精排后返回前 N 条。

升级后的首次启动会把已有索引灌进 FTS（20 万条约 0.4 秒，一次性）；系统 SQLite 没编译 FTS5 时静默退化为 LIKE 扫描，功能不受影响。
</details>

<details>
<summary><b>排序公式</b></summary>

```
最终分 = 0.72 × 匹配度 + 0.28 × 使用习惯

匹配度：每个关键字取最高分后求平均
  文件名完全相等 1.55 | 前缀 1.32 | 子串 1.05（越靠后越低，词边界加分）
  子序列 0.45~0.63（按命中密度）
  拼音完全相等 1.5 | 全拼前缀 1.2 | 全拼子串 0.9 | 首字母相等 1.25 | 首字母前缀 1.02
  目录命中 0.30~0.48

使用习惯：
  0.65 × log(1+启动次数)/log(1+候选最高次数)
+ 0.35 × exp(-距今天数 / 14)      ← 14 天半衰期的"最近使用"权重
```

输入越精确时匹配度主导，输入模糊时常用程序上浮。
</details>

<details>
<summary><b>表结构</b></summary>

```sql
CREATE TABLE executables (
  path TEXT PRIMARY KEY,      -- 完整路径（原生分隔符）
  name TEXT NOT NULL,         -- 小写文件名，供 FTS 与 LIKE 前缀使用
  display_name TEXT NOT NULL, -- 原始大小写，用于显示
  dir TEXT NOT NULL,          -- 小写目录，用于路径片段匹配
  ext TEXT NOT NULL,
  pinyin TEXT NOT NULL DEFAULT '',           -- 全拼变体，多音字空格分隔
  pinyin_initials TEXT NOT NULL DEFAULT '',  -- 首字母变体
  size INTEGER, mtime INTEGER, drive TEXT,
  gen INTEGER                 -- 本轮扫描代次
);

CREATE TABLE volume_stats (   -- 智能调度：每个分区的索引概况
  root TEXT PRIMARY KEY, exe_count INTEGER, last_scan INTEGER,
  empty_rounds INTEGER, deep_pending INTEGER
);

CREATE TABLE usage_stats (path TEXT PRIMARY KEY, launch_count INTEGER, last_used INTEGER);

CREATE VIRTUAL TABLE exe_fts USING fts5(
  name, dir, pinyin, pinyin_initials,
  content='executables', content_rowid='rowid', detail=column
);
```
</details>

## 已知限制

- **双击 Ctrl**：Windows 上少数带反作弊的游戏 / 安全软件会拦截 `WH_KEYBOARD_LL`；Linux 只在 X11 会话有效（Wayland 下取不到全局按键）；macOS 必须授予辅助功能 / 输入监控权限。三者都失败时仍可用托盘菜单唤起。
- **索引新鲜度**：Linux 走 locate 快照时，新装的程序最多晚一天进入索引；需要立刻生效请用托盘的"重新扫描全盘"。
- **增量精度**：扫描按代次清理失效记录，已收录文件的体积 / 修改时间只在真正遍历时更新；mtime 变化不影响启动。
- **开机自启动**：Unix 下写入的条目在下次登录时生效。
- **拼音表**只覆盖常用汉字（U+4E00–U+9FA5，约 2 万字），生僻字没有拼音，但仍可用汉字或文件名子串命中。
- **`.sh` 在 Windows 上**需要 Git Bash / WSL 提供 `bash`；`.ps1` 用 PowerShell 执行。
- **macOS** 的可执行文件大多没有后缀（`.app` 包内二进制），默认后缀白名单可能扫不到应用主体；可在设置里补充后缀。
- **macOS 分支与 Linux 全局热键分支**尚未在真实桌面会话中实机验证。

## 许可证

本项目以 **MIT 许可证** 开源，详见 [LICENSE](LICENSE)。

## 自动构建与发布

仓库通过 GitHub Actions 的 **Matrix Strategy** 并行构建，推送 `v*` 标签（或手动触发）时自动生成 Release 并上传资产：

| Asset | 平台 | 说明 |
| --- | --- | --- |
| `listery-windows.zip` | Windows x64 | Qt 5.15.2 / MSVC2019 构建，Windows 7 及以上 |
| `listery-windows-xp.zip` | Windows 32 位 | Qt 5.7.1 / MinGW 5.3 构建，兼容 Windows XP |
| `listery-linux.AppImage` | Linux x64 | 独立运行镜像（已打包 Qt 运行库） |
| `listery-macos.zip` | macOS | 可执行程序（已打包 Qt 框架） |

```bash
git tag v1.0.0
git push origin v1.0.0      # 触发 Actions 构建并发布 Release
```
