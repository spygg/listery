# Listery

用 Qt / C++ 实现的 Listary 风格快速启动器：**全盘索引可执行文件 → 双击 Ctrl 唤起 → 输入即时补全 → 按使用习惯动态排序**。

![界面预览](docs/preview.png)

![空输入时只有输入框](docs/preview-empty.png)

## 已实现的能力

| 需求 | 实现 |
| --- | --- |
| 自动扫描全盘可执行文件并入库 | 独立扫描线程递归遍历所有本地卷（含可移动磁盘），命中 `exe / com / bat / cmd / sh / ps1` 后写入 SQLite；默认跳过 `WinSxS`、`SoftwareDistribution`、`WindowsApps`、`node_modules` 等目录；不再支持“扫描根目录”，始终全盘 |
| 索引开始菜单 / 桌面快捷方式 | `.lnk` / `.url` 不受后缀设置影响、始终索引；开始菜单在 `ProgramData`、`AppData` 这类隐藏目录下，全盘遍历进不去，扫描时会单独补扫所有用户与当前用户的开始菜单；显示名去掉扩展名（`腾讯会议.lnk` → `腾讯会议`） |
| 拼音检索 | 汉字名在扫描时生成全拼与首字母（多音字保留全部读音变体），`tengxunhuiyi`、`txhy`、`腾讯会议` 都能命中同一个快捷方式/程序 |
| 双击 Ctrl 弹出输入框 | Windows `WH_KEYBOARD_LL`、Linux X11 XRecord、macOS CGEventTap 三套实现 + 同一套可单测状态机，300ms 内两次 Ctrl 且中间无其它按键才触发，不会误伤 `Ctrl+C` |
| 输入时自动补全候选 | 单词先走 `name` 索引前缀扫描（约 1ms），不足一屏再补 FTS5 词首匹配（名称 / 目录 / 全拼 / 首字母）；两者都落空才回退子串扫描兜底；命中字符高亮；多关键字空格分隔、全部命中才算匹配 |
| 按使用习惯调整顺序 | 每次启动累计频次与最近使用时间，排序分 = `0.72×匹配度 + 0.28×使用习惯`，同名候选时常用程序自动上浮 |
| 输入不卡界面 | 检索在独立线程执行，输入变化只保留最新一次请求，过期结果直接丢弃 |

其他细节：

- 无边框置顶面板，**屏幕水平居中、垂直靠上**（约屏幕高度 12% 处）弹出，失焦自动隐藏；
- **没有输入时只显示输入框**（面板高度 110px），有输入才展开过滤结果；**有输入但没有候选**时显示“输入框 + 状态栏”，不会弹出一大片空白；
- 结果列表支持**右键菜单**：以管理员身份运行 / 打开所在位置 / 复制路径到剪贴板；
- `Enter` 启动、`Ctrl+Enter` 在文件管理器中定位、`Shift+Enter` 以管理员身份运行、`Esc` 隐藏；
- 托盘菜单：打开搜索框 / 重新扫描全盘 / 设置 / 退出；
- **开机自启动默认开启**（Windows 写 HKCU 的 Run 项，macOS 写 `LaunchAgents`，Linux 写 `~/.config/autostart`，可在设置里关闭）；开机启动时静默进托盘，不主动弹搜索框；
- 单实例：重复启动会把“唤起”消息发给已有进程；
- 扫描按代次（generation）清理失效记录，重建索引不会丢失使用统计；
- 数据库 WAL 模式，扫描写入与界面检索互不阻塞；
- 首次启动自动建库；索引已完整时启动不再重扫，只有上次扫描中断或距上次扫描超过 24 小时才在启动后补一次，另有后台定时刷新（默认 12 小时）；
- **索引、配置、日志都保存在可执行文件所在目录**（目录不可写时回退到 `QStandardPaths::AppDataLocation`：Windows 为 `%APPDATA%`，Linux 为 `~/.local/share`，macOS 为 `~/Library/Application Support`）。

## 跨平台支持

三个平台共用同一套索引、检索、排序与界面代码，平台差异集中在 `src/Platform/` 与 `src/Core/LaunchService.cpp`：

| 能力 | Windows | Linux | macOS |
| --- | --- | --- | --- |
| 全局双击 Ctrl | `WH_KEYBOARD_LL` 低级键盘钩子 | X11 XRecord（需 `libXtst`；Wayland 会话不可用） | `CGEventTap`（需授予“辅助功能 / 输入监控”权限） |
| 全盘扫描 | 枚举所有本地卷（固定盘 + 可移动盘） | 枚举挂载在 `/dev/*` 的本地卷，跳过 `/proc`、`/sys`、`/dev`、`/run`、`/tmp` | 同左，扫描 `/` 与 `/Volumes/*` 本地卷 |
| 启动程序 | `ShellExecuteW`（`runas` 提权） | `QProcess::startDetached`；提权走 `pkexec` | LaunchServices `open`（`.app` 包自动识别）；提权走 `osascript ... with administrator privileges` |
| 打开所在位置 | `explorer /select` | 依次尝试 nautilus / dolphin / nemo / thunar 定位，失败回退 `xdg-open` | `open -R` |
| 开机自启动 | HKCU `...\CurrentVersion\Run` 的 `Listery` 值 | `~/.config/autostart/listery.desktop` | `~/Library/LaunchAgents/com.listery.launcher.plist` |

**验证范围（如实说明）**：

- Windows：本机 Qt 5.15.2 + MinGW 8.1 实际编译、运行，自检 43/43 通过，打包后的 `dist\` 在无 Qt 环境也可运行；
- Linux：在 WSL2 Ubuntu 24.04（Qt 5.15.13、`libXtst`、XRecord 头文件齐全）实际编译通过，并以 `QT_QPA_PLATFORM=offscreen` 跑完 43 项自检；全局双击 Ctrl 需要真实 X11 桌面会话，未在无头环境里实测；
- macOS：代码路径已按 `CGEventTap`、`open -R`、`LaunchAgents` 实现，但当前没有 Mac 机器，**未编译、未运行验证**，首次在 Mac 上构建时可能需要按实际 SDK 微调。

## 构建

要求：Qt 5.15+ 或 Qt 6.x（Widgets + Sql + Network），CMake ≥ 3.16，Ninja 或 MSVC。

本机（Qt 5.15.2 + MinGW 8.1）可直接运行：

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

Qt 6 只需把 `QTDIR` 换成对应套件（如 `E:\programes\Qt\Qt\6.11.1\mingw_64`，配套 `Tools\mingw1310_64`），代码里已做 Qt5/Qt6 兼容处理。

发布目录（带 Qt 运行库，可直接双击）：

```bat
deploy.bat                  :: 默认取 build\ 产物，Qt5 运行库
deploy.bat build-qt5        :: 也可指定其它构建目录
```

`deploy.bat` 会收集 Qt Core/Gui/Widgets/Sql/Network、`platforms\qwindows.dll`、`sqldrivers\qsqlite.dll`、样式插件以及 MinGW 运行库，产物在 `dist\`。Qt6 部署时设置 `QTDIR` 与 `QT_MAJOR=6` 即可。

### Linux

```bash
sudo apt install -y build-essential cmake ninja-build qtbase5-dev libqt5sql5-sqlite libxtst-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
QT_QPA_PLATFORM=xcb ./build/listery        # 需要 X11 会话；Wayland 下热键不可用，托盘仍可用
```

XRecord 头文件来自 `libxtst-dev`；缺少时 CMake 会给出 WARNING，程序仍可编译，只是无法监听全局双击 Ctrl。Qt6 把 `qtbase5-dev` 换成 `qt6-base-dev` 即可。

### macOS

```bash
brew install qt@5 cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5)"
cmake --build build --parallel
./build/listery
```

首次运行需要在“系统设置 → 隐私与安全性 → 辅助功能 / 输入监控”里勾选 Listery，否则 `CGEventTap` 创建失败（托盘会提示原因）。

## 运行

```bat
build\listery.exe            :: 常驻托盘，等待双击 Ctrl
build\listery.exe --show     :: 启动后立即打开搜索框
build\listery.exe --rescan   :: 启动后立刻重建索引
build\listery.exe --autostart :: 开机自启用：静默进托盘，不主动弹窗
build\listery.exe --selftest :: 无界面自检（结果写日志，退出码 0 表示全部通过）
build\listery.exe --render-preview out.png "c"      :: 离屏渲染“有结果”面板
build\listery.exe --render-preview out.png --empty  :: 离屏渲染“空输入只有输入框”面板
```

Linux / macOS 把 `build\listery.exe` 换成 `./build/listery`，其余参数相同。

数据目录：**可执行文件所在目录**（如 `G:\listery\dist\`；目录不可写时回退到各平台的 `AppDataLocation`，例如 Windows 的 `%APPDATA%\Listery\Listery\`）

| 文件 | 用途 |
| --- | --- |
| `index.db` | SQLite 索引（`executables` 表 + `usage_stats` 表 + `app_meta` 表） |
| `settings.ini` | 后缀白名单、跳过目录、可移动磁盘、开机自启动、热键间隔、结果数量等 |
| `listery.log` | 运行日志（含扫描错误与热键告警） |

删除 `index.db` 或点“重新扫描全盘”即可重建索引；使用统计在 `usage_stats` 中，重建索引不会清空。

## 目录结构

```
listery/
├─ CMakeLists.txt
├─ build.bat / deploy.bat
├─ src/
│  ├─ main.cpp                 程序入口、单实例、预览与自检开关
│  ├─ AppController.*          组装索引库 / 扫描 / 热键 / 面板 / 托盘
│  ├─ Core/
│  │  ├─ AppSettings.*         基于 settings.ini 的配置
│  │  ├─ Database.*            SQLite 表结构、批量写入、检索 SQL、使用统计
│  │  ├─ Scanner.*             全盘扫描线程（可剪枝、可取消、按代次清理）
│  │  ├─ Pinyin.* / PinyinData.* 汉字转全拼与首字母（静态字表，多音字多变体）
│  │  ├─ SearchEngine.*        粗筛 + 内存精排、匹配高亮
│  │  ├─ SearchService.*       后台检索线程（合并排队请求、按序号丢弃过期结果）
│  │  └─ LaunchService.*       启动 / 提权 / 定位（三平台分支）
│  ├─ Platform/
│  │  ├─ HotkeyHook.*          全局双击 Ctrl（Win 钩子 / X11 XRecord / CGEventTap）
│  │  ├─ DoubleCtrlDetector.h  双击 Ctrl 状态机（可单测）
│  │  ├─ Autostart.*           开机自启动（Run 项 / .desktop / LaunchAgent）
│  │  ├─ WindowUtils.*         强制前台、屏幕居中靠上定位
│  │  └─ Branding.*            运行时绘制图标
│  ├─ Ui/
│  │  ├─ LauncherWindow.*      搜索面板（无边框 + 阴影 + 键盘导航 + 右键菜单 + 空输入收起）
│  │  ├─ ResultModel.*         结果模型
│  │  ├─ ResultDelegate.*      行绘制（图标 / 高亮 / 路径 / 次数）
│  │  └─ SettingsDialog.*      设置界面
│  └─ SelfTest.*               43 项自检
└─ docs/preview.png
```

## 索引与排序细节

### 表结构

```sql
CREATE TABLE executables (
  path TEXT PRIMARY KEY,      -- 完整路径（原生分隔符）
  name TEXT NOT NULL,         -- 小写文件名，供 FTS 与 LIKE 前缀使用
  display_name TEXT NOT NULL, -- 原始大小写，用于显示
  dir TEXT NOT NULL,          -- 小写目录，用于路径片段匹配
  ext TEXT NOT NULL,
  pinyin TEXT NOT NULL DEFAULT '',           -- 全拼读音变体，多音字空格分隔（tengxunhuiyi tengxunkuaiyi）
  pinyin_initials TEXT NOT NULL DEFAULT '',  -- 首字母变体（txhy txky）
  size INTEGER, mtime INTEGER, drive TEXT,
  gen INTEGER                 -- 本轮扫描代次
);
CREATE INDEX idx_exe_name ON executables(name);
CREATE INDEX idx_exe_dir  ON executables(dir);

-- 检索用倒排索引：只映射 rowid，正文仍在 executables 里
CREATE VIRTUAL TABLE exe_fts USING fts5(
  name, dir, pinyin, pinyin_initials,
  content='executables', content_rowid='rowid',
  detail=column               -- 不存词位置（只做词首前缀匹配），索引体积约为默认模式的一半
);
-- INSERT / UPDATE / DELETE 触发器保持同步；扫描时名字和目录没变的行不会重写索引

CREATE TABLE usage_stats (
  path TEXT PRIMARY KEY,
  launch_count INTEGER,       -- 累计启动次数
  last_used INTEGER           -- 最近启动时间（毫秒）
);
```

### 扫描策略

1. `QStorageInfo::mountedVolumes()` 枚举本地固定磁盘（可选 U 盘），跳过网络盘与只读卷；
2. 自行递归（而不是 `QDirIterator`）以便在进入目录前剪枝，跳过系统缓存目录；
3. 目录联接 / 符号链接目录直接跳过，避免环；但**不能用 `QDir::NoSymLinks`**——开始菜单里的 `.lnk` 带重解析点属性，会被整批滤掉；
4. 额外单独扫一遍所有用户与当前用户的开始菜单（它们在 `ProgramData`、`AppData` 隐藏目录下，全盘遍历进不去）；
5. 每 2000 条提交一次事务，扫描结束后 `DELETE FROM executables WHERE gen <> 本轮代次`；
6. 扫描中途取消则保留旧索引，不做清理——不会出现“取消后全空”的情况。

### 检索流程

1. 输入框 35ms 防抖后把查询交给 `SearchService` 的检索线程，GUI 线程从不执行 SQL；
2. 请求带自增序号：连续输入时排队中的旧请求会被合并，结果回来时序号不是最新的一律丢弃；
3. 先用使用统计种入候选：取启动次数最高的几百个程序，按本次输入做名字前缀过滤，保证常用程序不会因为粗筛截断被挤掉；
4. 单词查询先走 `name` 索引做前缀范围扫描（20 万条约 0.6~1.5ms），逐字打字时这一趟通常就能填满一屏；
5. 前缀扫描不够一屏时补一次 FTS5 词首匹配（`"chr"*`，`name` / `dir` / `pinyin` / `pinyin_initials`），覆盖 `studio → visual studio code`、`tengxunhuiyi → 腾讯会议` 这类词首命中；带扩展名的输入（`kylin.bat`）会拆成子词分别匹配——FTS5 在 `detail=column` 下不接受含标点的整段短语；
6. 只有前缀和 FTS 都落空时才跑 `LIKE '%关键字%'` 子串扫描兜底（20 万条约 50ms，仅在检索线程，且只用来捞真正的错字/中缀输入）；
7. 命中候选只取 `path/name/dir/ext` 四列，使用统计通过一次快照补齐，再在内存里按“匹配度 + 使用习惯”精排后返回前 N 条。

升级后的首次启动会把已有索引灌进 FTS（20 万条约 0.4 秒，一次性）；如果系统 SQLite 没编译 FTS5，则静默退化为原来的 LIKE 全表扫描，功能不受影响。

### 排序公式

```
最终分 = 0.72 × 匹配度 + 0.28 × 使用习惯

匹配度：每个关键字取最高分后求平均
  文件名完全相等 1.55 | 前缀 1.32 | 子串 1.05（越靠后越低，词边界加分）
  子序列 0.45~0.63（按命中密度）
  拼音完全相等 1.5 | 全拼前缀 1.2 | 全拼子串 0.9 | 首字母相等 1.25 | 首字母前缀 1.02
  目录命中 0.30~0.48

使用习惯：
  0.65 × log(1+启动次数)/log(1+候选最高次数)
+ 0.35 × exp(-距今天数 / 14)      ← 14 天半衰期的“最近使用”权重
```

这样“输入越精确时匹配度主导，输入模糊时常用程序上浮”，符合 Listary 的手感。

## 已知限制

- 双击 Ctrl：Windows 上少数带反作弊的游戏/安全软件会拦截 `WH_KEYBOARD_LL`；Linux 只在 X11 会话有效（Wayland 下取不到全局按键，可在托盘菜单打开搜索框）；macOS 必须在系统设置里授予辅助功能/输入监控权限。三者都失败时仍可用托盘菜单唤起。
- 开机自启动：Windows 写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` 的 `Listery` 项；Linux 写 `~/.config/autostart/listery.desktop`；macOS 写 `~/Library/LaunchAgents/com.listery.launcher.plist`。Unix 下写入的条目在**下次登录**时生效。
- 数据目录默认就在 exe 旁边；把程序放进只读目录或 Program Files 时无法建库，会自动回退到各平台的用户配置目录。
- `.lnk` / `.url` 始终索引（不受后缀设置影响），Windows 的 ShellExecute 能直接启动它们；非 Windows 平台按普通文件处理，不会额外解析快捷方式目标。
- 拼音表只覆盖常用汉字（U+4E00–U+9FA5，约 2 万字），生僻字或后加入的新字没有拼音，但仍可用汉字或文件名子串命中。
- `.sh` 在 Windows 上需要 Git Bash / WSL 提供 `bash`；`.ps1` 用 PowerShell 执行。
- 全盘首次扫描耗时取决于文件数（百万级文件通常数分钟），扫描在后台线程进行，期间可正常搜索已有索引。
- macOS 的可执行文件大多没有后缀（`.app` 包内二进制），默认后缀白名单可能扫不到应用主体；需要时可在设置里补充后缀，后续版本再针对 `.app` 包做专门索引。
- macOS 分支与 Linux 全局热键分支尚未在真实桌面会话中实机验证（详见上文“验证范围”）。

## 许可证

本项目以 **MIT 许可证** 开源，详见 [LICENSE](LICENSE)。

## 自动构建与发布（GitHub Actions 矩阵策略）

仓库通过 GitHub Actions 的 **Matrix Strategy** 在 Windows / Linux / macOS 三个平台并行构建；推送 `v*` 标签（或手动在 Actions 页面触发 `Build & Release`）时，自动生成 GitHub Release，并把三个平台的可执行程序作为 **Assets** 上传：

| Asset | 平台 | 说明 |
| --- | --- | --- |
| `listery-windows.zip` | Windows | 可执行文件 + Qt 运行库 + VC 运行时 |
| `listery-linux.AppImage` | Linux | 独立运行镜像（已打包 Qt 运行库） |
| `listery-macos.zip` | macOS | `.app` 包（已打包 Qt 框架） |

本地发版示例：

```bash
git tag v1.0.0
git push origin v1.0.0      # 触发 Actions 构建并发布 Release
```

