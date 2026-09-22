#pragma once

#include <QString>

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
// still has to confirm it in 默认应用.
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

}   // namespace AppSettings
