#pragma once

#include <QString>

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

// --- exposed for the self test (pure, no side effects) ---------------------

// <temp>/pdfboard/word-<sha1(path|size|mtime)>.pdf
QString tempPdfPathFor(const QString &src, qint64 size, qint64 mtimeMs);

}   // namespace WordConvert
