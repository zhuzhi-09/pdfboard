#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

// Read-only Word (.docx/.doc) support.
//
// The document is exported to PDF through the Word (or WPS) automation that is
// installed on the machine - the only way to get Word's own layout instead of a
// re-flow approximation - and the generated PDF then flows through the normal
// PDF pipeline (tabs, ink, .dpz save, inject-PDF export, thumbnails). The
// original Word file is never modified, copied or renamed.
//
// The automation is driven directly from C++ through the raw COM API
// (IDispatch): no PowerShell sidecar, no child process and no script text
// anywhere. AV software that prompts on the first launch of a child process
// (火绒 on an unattended classroom machine) never sees one, and no file path is
// ever handed to an interpreter.
namespace WordConvert {

// True for the extensions we can hand to Word/WPS.
bool isWordDoc(const QString &path);

// True when this machine has Word or WPS. Probed once per process with a
// registration lookup only - the application is never launched to probe.
bool hasConverter();

// True when .docx is actually associated with Word. Word shows its modal
// "not the default program" dialog - the one that blocks Open/Export and
// refuses Quit - exactly in the opposite case, so this only makes the failure
// message precise (nothing is blocked up front).
bool wordIsDefaultHandler();

// Exports `src` to a cached PDF under the temp dir and returns its path. The
// cache key is the source path, its size and its mtime, so an unchanged
// document is reused and an edited one is re-exported. Returns false and sets
// *errorOut when no converter exists or the export fails.
bool convertToPdf(const QString &src, QString *pdfOut, QString *errorOut);

// --- the "Word is not the default program" nag fix --------------------------
//
// Word pops a modal "Microsoft Word is not the default program for viewing and
// editing documents" dialog when it starts on a machine where .docx belongs to
// another program; while the dialog is up, Open/Export fail and Quit is
// refused. Word reads the two option values behind it when it STARTS, so they
// have to be written into Word's own registry key before the automation
// instance is created. Because that changes Word's settings, the pre-existing
// state is recorded first and restoreWordNag() can put it back.

// Silences the nag. On its first call it records the original state of
// AlertIfNotDefault and DoNotCheckIfWordIsDefaultApp for every existing
// HKCU\Software\Microsoft\Office\<ver>\Word\Options key into this app's own
// HKCU\Software\PDFBoard value WordNagBackup (a JSON object keyed
// "<version>|<valueName>", each {"exists": bool, "value": <int>}), then writes
// the silenced values. An existing backup is never overwritten, so it always
// describes the true pre-existing state; writing the silenced values again is
// harmless (idempotent). Returns false with a Chinese message when no Office
// version key exists.
bool silenceWordNag(QString *errorOut = nullptr);

// What 恢复 Word 设置 would do right now. It decides the settings row (button
// enabled state, body text, confirmation) and the way restoreWordNag() works.
enum class NagRestoreMode {
    None,        // no backup and the values look untouched: nothing to restore
    FromBackup,  // WordNagBackup exists: exact replay of the pre-write state
    ToDefaults,  // no backup, but the values look like a silencing write (an
                 // older build or a manual edit): Word's documented defaults
                 // are the only restore left, and the nag comes back
};

// FromBackup > ToDefaults > None, judged from OUR backup plus a READ-ONLY look
// at the current Word option values.
NagRestoreMode nagRestoreMode();

// Restores according to nagRestoreMode():
//   * FromBackup: a value that existed is written back with its original data,
//     a value that did not exist is deleted, then the backup is removed.
//   * ToDefaults: with no backup but the values still looking silenced,
//     AlertIfNotDefault is set back to 1 and DoNotCheckIfWordIsDefaultApp is
//     deleted - but only where those values actually exist. Success hands back
//     a Chinese NOTICE through `errorOut` ("没有备份记录：已恢复为 Word 默认设置
//     （会重新提醒）"), because the nag will pop again; the settings page shows
//     it in the status bar. That is the only success path that leaves
//     `errorOut` non-empty - every other success clears it.
//   * None: fails with a Chinese message and touches nothing.
bool restoreWordNag(QString *errorOut = nullptr);

// True when a backup exists, whether or not the values it describes are
// currently in place (nagRestoreMode() uses it for FromBackup).
bool wordNagBackupExists();

// True when a backup exists AND every recorded option value currently holds
// the silenced data that silenceWordNag() writes.
bool wordNagSilenced();

// Pure backup encoding, round-trip exact: the map keys are
// "<version>|<valueName>" and each value is a {"exists", "value"} map.
QJsonObject encodeNagBackup(const QVariantMap &recorded);
QVariantMap decodeNagBackup(const QJsonObject &backup);

// --- exposed for the self test (pure, no side effects) ---------------------

// <temp>/pdfboard/word-<sha1(path|size|mtime)>.pdf
QString tempPdfPathFor(const QString &src, qint64 size, qint64 mtimeMs);

}   // namespace WordConvert
