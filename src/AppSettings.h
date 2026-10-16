#pragma once

#include <QString>
#include <QStringList>

// Non-UI application settings.
//
// Everything here is a thin, testable wrapper around the registry: the auto
// start entry under the current user's Run key and the .pdf file-association
// registration. The page that exposes them (SettingsPage) contains no registry
// code of its own.
namespace AppSettings {

// True when the Run key holds this executable's path (quoted) as the PDFBoard
// value. A stale entry pointing at another/moved executable reads as false.
bool isAutoStartEnabled();

// Adds (on = true) or removes (on = false) the PDFBoard value in
// HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run.
// The value is the full executable path in quotes, ready for Windows to run.
bool setAutoStart(bool on, QString *errorOut = nullptr);

// Registers this executable as a .pdf handler for the current user: ProgID,
// OpenWithProgids and the Capabilities keys, then notifies Explorer. Windows
// never lets an application silently become the default handler, so the user
// still has to confirm it in 默认应用. Also adds OpenWithProgids and
// Capabilities entries for .docx/.doc (as an 打开方式 candidate only - the
// existing Word default is never touched).
bool registerPdfHandler(QString *errorOut);

// Directory that 保存 / 另存为 should suggest first. An EMPTY string means
// "next to the source document", which is the default.
QString defaultSavePath();

// Stores the preferred save directory (empty = follow the source document).
bool setDefaultSavePath(const QString &dir, QString *errorOut = nullptr);

// Diagnostic logging preference. OFF by default: the shipping app must not
// write anything to disk unless the user asks for it.
bool debugLogEnabled();
bool setDebugLogEnabled(bool on, QString *errorOut = nullptr);

// Recent documents (最近项目), most recent first. Only the PATH is remembered:
// a file is never copied, moved or renamed. The list is capped at 8 entries and
// duplicates are matched case-insensitively (Windows paths).
QStringList recentFiles();
// Adds `path` to the front, dropping any existing equal entry first.
// An empty path is a no-op, not an error. A path under the system temp
// directory (which includes the `<temp>/pdfboard` working copies) is refused:
// it returns false with a Chinese error and never touches the stored list.
bool addRecentFile(const QString &path, QString *errorOut = nullptr);
bool removeRecentFile(const QString &path, QString *errorOut = nullptr);
bool clearRecentFiles(QString *errorOut = nullptr);

// Appearance preference: 0 = follow the system (default), 1 = light, 2 = dark.
// Anything else stored in the registry reads back as 0.
int  themeMode();
bool setThemeMode(int mode, QString *errorOut = nullptr);

// How a double-clicked / chosen Word document is handled: 0 = ask every time
// (default), 1 = always annotate (convert to PDF), 2 = always hand it to the
// installed Word/WPS. Anything else stored in the registry reads back as 0.
int  wordOpenMode();
bool setWordOpenMode(int mode, QString *errorOut = nullptr);

// The Word nag fix: ON by default, because a conversion on a machine where
// this app owns .docx would otherwise fail behind Word's "not the default
// program" dialog. When ON, a conversion first writes Word's own two option
// values (after recording the previous state; see
// WordConvert::restoreWordNag). When OFF the conversion just runs as before.
bool wordNagFixEnabled();
bool setWordNagFixEnabled(bool on, QString *errorOut = nullptr);

// 手掌擦除: ON by default (the classroom panel's palm-as-eraser is the feature
// the app ships with). When OFF, a palm-sized contact never triggers the
// dynamic eraser - it does nothing at all - while pinch and single-finger
// writing keep working exactly as before.
bool palmEraserEnabled();
bool setPalmEraserEnabled(bool on, QString *errorOut = nullptr);

// Toolbar look: 0 = 简约 (glyph only, the default), 1 = 文字 (glyph over its
// label). Only the presentation changes - both styles expose every button.
// Anything else stored in the registry reads back as 0.
int  toolbarStyle();
bool setToolbarStyle(int style, QString *errorOut = nullptr);

// 「主题色」seed preset (0 = 品牌蓝, the default, up to ThemePresets::kSeedCount - 1)
// and 「调色板风格」 (0 = fidelity, the default ... 3 = monochrome). The index
// spaces live in ThemePresets.generated.h. An out-of-range stored value reads
// back as 0.
int  themePreset();
bool setThemePreset(int preset, QString *errorOut = nullptr);
int  themeStyle();
bool setThemeStyle(int style, QString *errorOut = nullptr);

// 减少动效: OFF by default. When ON, the MD3 chrome animations (switch handle,
// tool indicator, ...) jump straight to their end state - for slow classroom
// machines and for anyone who finds motion distracting.
bool reduceMotion();
bool setReduceMotion(bool on, QString *errorOut = nullptr);

// 硬件加速（实验性）: OFF by default. When ON, a newly opened document paints its
// pages through a GPU-backed OpenGL viewport instead of the CPU raster engine.
// The switch is read when a document's canvas is created, so it applies to
// documents opened afterwards. It only takes effect where a real OpenGL driver
// exists - a software rasteriser (llvmpipe / GDI Generic) is refused, because it
// is slower than the CPU path it would replace.
bool gpuAcceleration();
bool setGpuAcceleration(bool on, QString *errorOut = nullptr);

}   // namespace AppSettings
