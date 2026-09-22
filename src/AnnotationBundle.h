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
bool write(const QString &bundlePath, const QByteArray &pdfBytes,
           const QJsonObject &annotations, QString *errorOut = nullptr);

// Reads a `.dpz`: fills `pdfBytes` with `source.pdf` and `annotations` with the
// parsed `annotations.json`. Returns false on malformed input or when an entry
// uses anything other than stored (method 0) compression.
bool read(const QString &bundlePath, QByteArray *pdfBytes,
          QJsonObject *annotations, QString *errorOut = nullptr);

// True when `path` has a `.dpz` extension OR begins with the local ZIP magic
// `PK\x03\x04` (so a renamed bundle is still recognised).
bool isBundle(const QString &path);

}   // namespace AnnotationBundle
