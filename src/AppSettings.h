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

}   // namespace AppSettings
