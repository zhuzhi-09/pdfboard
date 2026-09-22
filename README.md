# 大屏 PDF 批注 (PDFBoard)

面向**教室大屏一体机**的 PDF 查看 + 手写批注桌面工具。原生 C++/Qt 实现，为**触控、4K 屏、内存受限**的教学一体机做了专门优化。

> 目标场景：Windows 一体机（红外触控、4K 屏、8 GB 内存、低功耗 CPU），老师用它打开电子教材 / 试卷 / 讲义，边讲边写、随堂批注，并保存下来下次课继续用。

---

## 特性

### 查看
- 纵向**连续滚动**翻阅（像 Word），适宽自动缩放
- **双指捏合缩放**（以两指中心为锚点，横纵都不漂移）
- **双指拖动平移**（含横向），单指/鼠标滚动
- 适配宽度一键复位，且**保持当前焦点位置不跳**

### 批注
- 手写笔：6 色 + 3 档粗细
- **局部橡皮**：指哪擦哪（把笔迹在该处切开，两侧保留），不是整条删除
- 撤销 / 重做 / 清空本页
- 笔迹按**页面归一化坐标**存储：滚动、缩放、换分辨率都不漂移
- 笔迹**粗细随缩放同步**（放大写的字，缩回适配宽度后一起变小）
- 书写采样自动**补密**，快速书写不断笔、擦除更精确
- **双指手势期间不会误画**（触控锁 + 鼠标路径拦截 + 误触过滤）

### 文档与保存
- **多文档同时打开**，底部标签栏切换（文件名标签 + 「打开」标签 + ⚙设置）
- **保存批注 → `.dpz` 批注包**（标准 ZIP 容器）：
  - `source.pdf`：原始 PDF，逐字节不动
  - `annotations.json`：批注数据
  - 双击 `.dpz` 即可回到上次的批注状态
- **另存为**：
  - `打包保存 (*.dpz)`（默认）
  - `注入 PDF (*.pdf)`：把批注烧录进页面，**适合分享、任何阅读器都能打开**
- **设置为 PDF 默认打开方式**（一键注册 + 引导到系统「默认应用」确认）
- **开机自启动**开关
- **默认保存路径**：可指定批注默认存到哪个文件夹，或跟随源文件所在目录
- **诊断日志**（默认关闭）：开启后记录运行状态与耗时，便于排查问题

### 界面
- 底部悬浮**工具岛**（打开/保存入口集中在此），可收起为小箭头
- **工具岛可拖动**：任意位置按住就能拖（按钮上也可以，不用瞄准边缘）；位置只在本
  次运行内有效，重启回到默认的底部居中
- **全屏**：工具岛的「全屏」按钮或 `F11` 进入，`Esc`（或再点一次）退出；应用内的
  工具岛与标签栏保留，只收起窗口边框与任务栏
- **自由移动**模式：拖动画布即可自由平移画面（双指仍缩放/平移），不产生笔迹
- **深色模式**：设置里可选 跟随系统 / 浅色 / 深色，切换即时生效；**PDF 纸面始终保持白色**
  （投影可读性优先），只有应用界面跟随主题
- 页面缩略图网格跳页（点击页码）
- 笔调色板浮层
- **WinUI 风格全屏设置页**（作为标签页，含「关于」与 MIT 许可证）
- 自绘矢量图标，随 DPI 缩放，无图片资源依赖

### 工程
- **内存有界**：整页位图 LRU 缓存，上限 128 MB；单页位图超 100 MB 自动降采样；多文档切换时释放非活动文档的位图
- **确定性自测**：`--selftest-ink` 覆盖墨迹模型 / 局部擦除 / 撤销重做 / 缩放锚点 / 笔迹缩放等，共 30+ 项断言
- **渲染基准**：`--bench <pdf>` 输出逐页渲染耗时与进程内存
- 构建零警告（MSVC `/W4`）

---

## 打包安装器

需要 [Inno Setup 6](https://jrsoftware.org/isdl.php)（脚本按默认路径 `D:\dev\tools\InnoSetup` 查找 `ISCC.exe`，可自行修改）：

```powershell
package.cmd
# 产物：dist\PDFBoard-1.4.1-setup.exe   （自带 Qt 运行库，约 17 MB）
```

安装器做这些事（全部**当前用户**范围，不需要管理员）：

| 项目 | 说明 |
|---|---|
| 安装位置 | `%LOCALAPPDATA%\Programs\PDFBoard` |
| 系统管理 | 注册到「应用和功能」，带图标与版本，可正常卸载 |
| 快捷方式 | 开始菜单（桌面快捷方式为可选项） |
| 文件关联 | `.pdf` / `.dpz` 的 OpenWith + Capabilities（会出现在「默认应用」里） |
| 卸载 | 同时清理文件、快捷方式、关联注册项与临时缓存 |

**静默部署**（供教室集中管理客户端调用）：

```bat
PDFBoard-1.4.1-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
rem 不要桌面快捷方式：
PDFBoard-1.4.1-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /TASKS=""
```

> 卸载时若程序正在运行，静默模式下无法提示关闭，会残留被占用的文件（Windows 常见行为）。先退出程序再卸载即可完全清除。

## 自动构建（Nightly）

[`.github/workflows/nightly.yml`](.github/workflows/nightly.yml) 会在以下时机自动构建**安装器 + 便携版**：

- 推送到 `main`（只改文档除外）
- 每天一次（UTC 18:00）
- 手动触发（Actions → nightly → Run workflow）

产物发布到固定的 **`nightly` 预发布**（每次覆盖同名文件，不会越堆越多）：
`PDFBoard-nightly-setup.exe` / `PDFBoard-nightly-portable.zip`，版本号形如 `1.4.1-nightly.<提交号>`。

### 稳定版发布

[`.github/workflows/release.yml`](.github/workflows/release.yml) 在推送 `v*` 标签时构建并发布**正式 Release**（非预发布）：

- 产物：`PDFBoard-<版本>-setup.exe` / `PDFBoard-<版本>-portable.zip`
- 版本号取自 [`installer/pdfboard.iss`](installer/pdfboard.iss) 的 `AppVersion`；workflow 会**校验它与标签一致**，不一致直接失败（防止发出版本号错位的包）
- 发布前会先跑 `--selftest-log` 自测作为门禁

发版流程：

```powershell
# 1. 改版本号：installer/pdfboard.iss 的 AppVersion + assets/app.rc 的 FILEVERSION/FileVersion
# 2. 提交推送
git commit -am "chore: 版本号 1.4.1"
git push
# 3. 打标签并推送，CI 自动出正式 Release
git tag v1.4.1
git push origin v1.4.1
```

### 代码签名（可选）

Windows 上签名能显著减少「未知发布者」警告与杀软误报。workflow 内置了签名步骤，**配好密钥即自动生效**，没配就跳过（fork 不会因此失败）：

| 仓库 Secret | 内容 |
|---|---|
| `SIGN_PFX_BASE64` | 代码签名证书 `.pfx` 的 base64（`certutil -encode` 或 `[Convert]::ToBase64String()`） |
| `SIGN_PFX_PASS` | 该 `.pfx` 的密码 |

配好后，**应用 exe 与安装器**都会被 `signtool` 签名并加时间戳。

> 证书选择提醒：**OV** 证书约 $150–300/年（2023 起私钥必须放在 HSM/云签名服务或 USB token 内）；**EV** 更贵，且自 2024 年起两者在 SmartScreen 上都需要逐步积累信誉，不再有"EV 立刻免警告"的特权。自签名证书对 SmartScreen **无效**，只适合内部测试。

## 诊断日志

默认**关闭**，程序不会写任何日志。在「设置 → 调试」里打开，或临时用环境变量指定路径：

```powershell
$env:PDFBOARD_LOG = "D:\tmp\pdfboard.log"
build\pdfboard.exe --bench some.pdf
```

记录内容：启动环境（Qt 版本、程序路径、屏幕与 DPI）、文档打开/关闭耗时与页数、**慢页渲染**、缓存淘汰、缩放稳定值、墨迹编辑与保存/导出结果、以及 Qt 的所有警告。
**不记录**文档内容，也不记录批注坐标。日志默认位置：`%LOCALAPPDATA%\PDFBoard\logs\pdfboard.log`（超过 2 MB 自动轮转为 `.1`）。

`PDFBOARD_LOG` 只决定**本次启动默认是否开启**以及日志写到哪个文件，**不会锁住设置里的开关**：
用它启动后依然可以在「设置 → 调试」里随手关掉（设置页会注明该变量正在生效）；下次再用同一个终端启动时它又会是开启的。
关闭开关同时会释放日志文件的句柄，不会出现「明明关了却一直占着文件」。

## 环境要求

| 依赖 | 版本 |
|---|---|
| Windows | 10 / 11 64-bit |
| 编译器 | Visual Studio 2022（MSVC v143） |
| Qt | **6.8.x**，需 `Widgets` + `Pdf` 模块 |
| CMake | ≥ 3.21 |
| Ninja | 任意较新版本（也可换其它 CMake 生成器） |

> Qt 也可用官方在线安装器装；命令行安装示例：
> `aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --modules qtpdf`

## 构建

```powershell
# 1. 配置（把 <Qt 路径> 换成你的 Qt msvc2022_64 目录）
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="<Qt 路径>"

# 2. 编译
cmake --build build
```

产物：`build/pdfboard.exe`

> 仓库内 `build.cmd` 是一个 Windows 便捷脚本（会调用 `vcvars64.bat`），可自行修改路径后使用。

## 运行

```powershell
# 直接打开 PDF 或 .dpz 批注包
build\pdfboard.exe "D:\课件\选必2综合练习三.pdf"
build\pdfboard.exe "D:\课件\选必2综合练习三.dpz"
```

不带参数启动后，从底部标签栏的 **「打开」** 选择文件（也支持把文件拖进窗口）。

## 自测

```powershell
# 墨迹 / 擦除 / 撤销 / 缩放 的确定性断言（不依赖鼠标输入）
build\pdfboard.exe --selftest-ink path\to\test.pdf

# 诊断日志开关与 PDFBOARD_LOG 优先级的回归断言（不需要文档，不改动你的设置）
build\pdfboard.exe --selftest-log

# 主题模式取值/回退、深色调色板确实更暗的断言（不改动你的系统设置）
build\pdfboard.exe --selftest-theme

# 工具岛 80% 缩放比例、拖动位置钳制、拖动阈值（点击不误判为拖动）
build\pdfboard.exe --selftest-ui

# 渲染耗时与内存基准
build\pdfboard.exe --bench path\to\test.pdf
```

## 快捷键

| 快捷键 | 功能 |
|---|---|
| `Ctrl+O` / `Ctrl+T` | 打开文档（新标签） |
| `Ctrl+W` | 关闭当前标签 |
| `Ctrl+S` | 保存（打包为 `.dpz`） |
| `Ctrl+Shift+S` | 另存为（打包 / 注入 PDF） |

## `.dpz` 批注包格式

`.dpz` 就是一个**标准 ZIP**（未压缩存储），可以改名成 `.zip` 用任意解压工具查看：

```
xxx.dpz
├── source.pdf         原始 PDF（逐字节保持不变）
└── annotations.json   批注数据
```

`annotations.json` 结构：

```json
{
  "version": 1,
  "pages": {
    "0": [ { "c": "#D32F2F", "w": 0.004, "p": [x0,y0, x1,y1, ...] } ]
  }
}
```

- 键 = 页索引（从 0 开始）
- `c` 颜色、`w` 笔画粗细（**相对页宽的比例**，因此随缩放同步）、`p` 归一化坐标序列

## 项目结构

```
src/
  main.cpp            入口；--bench / --selftest-ink
  MainWindow.*        窗口、多文档标签、保存/另存为、拖放、状态栏
  PdfCanvas.*         连续滚动视图：渲染、缓存、墨迹模型、触控/缩放
  InkToolbar.*        底部工具岛 + 笔调色板浮层
  PageGrid.*          页面缩略图浮层
  DocumentTabs.*      底部标签栏
  SettingsPage.*      WinUI 风格设置页
  AppSettings.*       注册表：开机自启、文件关联
  AnnotationBundle.*  .dpz 容器读写（自实现 ZIP + CRC32）
  PdfExport.*         注入 PDF（把批注烧录进页面）
  IconPainter.*       自绘矢量图标
  Theme.h             设计令牌（颜色/间距/圆角/字号/阴影）
  OverlayDismiss.h    浮层"点外部关闭"的共享判定
  MemProbe.h          进程内存探针
docs/                 开发文档（见下）
```

## 已知限制

- **注入 PDF** 采用页面栅格化（150 DPI）后烧录：任何阅读器可开、适合分享，但**文字不可选中**、体积偏大。若需保留 PDF 原始结构并把批注写成标准 `/Ink` 矢量注释，需要直接集成 PDFium 写入接口（Qt 自带的 PDFium 封装是只读的）。
- 尚未实现：批注的文本/高亮/形状等更多标注类型；导出图片；打印。
- 触控优化针对红外/电容触摸屏；压感笔（少数高配机型）会按固定线宽绘制。

## 文档

- [`docs/01-技术选型与调研.md`](docs/01-技术选型与调研.md) — 为什么是原生而不是 Web / 引擎与内存的实测依据
- [`docs/02-技术方案与里程碑.md`](docs/02-技术方案与里程碑.md) — 架构、内存预算、里程碑与验收
- [`docs/03-AI协作开发过程.md`](docs/03-AI协作开发过程.md) — 完整开发过程、踩过的坑与修复记录

## 许可证

本项目采用 **MIT License**（见 [LICENSE](LICENSE)）。

第三方组件：

| 组件 | 许可 |
|---|---|
| Qt 6（Widgets / PDF 模块） | LGPL-3.0（动态链接使用） |
| PDFium（由 Qt PDF 模块内置） | BSD-3-Clause |

> 本项目与任何硬件厂商、商业白板软件无关，不包含其商标或代码。
