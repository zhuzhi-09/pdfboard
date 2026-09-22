#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// The `.dpz` annotation container.
//
// A `.dpz` IS a ZIP archive (so Explorer / 7-Zip / WinRAR can inspect it) with
// exactly two STORED (uncompressed, method 0) entries:
//   - `source.pdf`       the untouched source PDF bytes
//   - `annotations.json` the ink model + metadata, UTF-8
//
// Nothing here depends on a ZIP library: the container is written and read
// byte-exactly by this file (local headers, central directory, EOCD, CRC-32).
namespace AnnotationBundle {

// Writes a new `.dpz` at `bundlePath`. Returns false (never throws / crashes)
// when the target cannot be written; `errorOut` then carries a readable message.
//
// `sourceFingerprint`, when non-empty, is stored as an extra top-level "source"
// object inside `annotations.json`. Only the working copy written by 保存 uses
// it; a real `.dpz` the user saves with 另存为 carries no fingerprint, so the
// document container stays byte-for-byte what it always was. Readers ignore an
// unknown top-level key, so old and new bundles read identically.
bool write(const QString &bundlePath, const QByteArray &pdfBytes,
           const QJsonObject &annotations, QString *errorOut = nullptr,
           const QJsonObject &sourceFingerprint = {});

// Reads a `.dpz`: fills `pdfBytes` with `source.pdf` and `annotations` with the
// parsed `annotations.json`. Returns false on malformed input or when an entry
// uses anything other than stored (method 0) compression.
bool read(const QString &bundlePath, QByteArray *pdfBytes,
          QJsonObject *annotations, QString *errorOut = nullptr);

// True when `path` has a `.dpz` extension OR begins with the local ZIP magic
// `PK\x03\x04` (so a renamed bundle is still recognised).
bool isBundle(const QString &path);

// `<temp>/pdfboard/work-<sha1(native source path)>.dpz`
//
// The working copy a document without a bundle of its own is 保存d into: the
// source file is never written next to, so a document on a USB stick stays a
// single file. The key is the source path ONLY - no size or mtime - so the
// annotations are found again however long the document sat untouched. Pure:
// it only computes a name, it never touches the disk (the caller creates the
// directory before writing).
QString workingBundlePathFor(const QString &sourcePath);

// Content identity of the file, as
// `{ "path": <absolute native path>, "size": <bytes>,
//    "mtimeMs": <ms since epoch>, "head": <sha1 hex of the first 65536 bytes> }`.
//
// The key is the SOURCE PATH, so a working copy must be able to answer "is the
// file behind this name still the same file?" - a swapped USB stick or a
// replaced file at the same path must not inherit the old annotations. Empty
// object when the file cannot be read at all. Pure: read-only, deterministic.
QJsonObject sourceFingerprintFor(const QString &path);

// True only when both objects carry path, size, mtimeMs and head and all four
// are equal. A missing key (an old bundle without a "source" object, a file
// that could not be read) is never a match. Pure.
bool fingerprintMatches(const QJsonObject &recorded, const QJsonObject &actual);

}   // namespace AnnotationBundle
