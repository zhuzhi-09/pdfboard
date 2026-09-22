#pragma once

#include <QString>

// Opt-in diagnostic logging.
//
// OFF by default: the shipping app writes nothing to disk. It is switched on
// from the settings page, or by pointing the PDFBOARD_LOG environment variable
// at a file path - which is what you ask a user to do when troubleshooting a
// machine you cannot touch.
//
// What gets recorded is deliberately limited to things that matter when a bug
// is reported: startup environment, document open/close timings, slow page
// renders, cache evictions, zoom/gesture decisions, ink edits, save/export
// results, and every Qt warning. No document content and no annotation
// coordinates are ever written.
namespace AppLog {

// Reads the stored preference (and the environment override) and opens the log.
// Safe to call more than once.
void applySettings();

// Turns logging on/off at runtime and persists the preference.
bool setEnabled(bool on, QString *errorOut = nullptr);
bool isEnabled();

// File currently being written, or an empty string when logging is off.
QString logFilePath();

// Directory that holds the log (created on demand) - used by "open log folder".
QString logDirectory();

// Appends one timestamped, category-tagged line. A cheap no-op when disabled.
void write(const QString &category, const QString &message);

}   // namespace AppLog
