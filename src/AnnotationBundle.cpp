#include "AnnotationBundle.h"

#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QVector>

#include <array>

namespace {

// --- ZIP constants ---------------------------------------------------------
constexpr quint32 kLocalSig   = 0x04034B50u;
constexpr quint32 kCentralSig = 0x02014B50u;
constexpr quint32 kEocdSig    = 0x06054B50u;

// Fixed, sane DOS timestamp: 2024-01-01 00:00:00.
// DOS date: bits 15..9 = year-1980, 8..5 = month, 4..0 = day.
constexpr quint16 kDosTime = 0x0000u;
constexpr quint16 kDosDate = quint16(((2024 - 1980) << 9) | (1 << 5) | 1);

// --- CRC-32 (IEEE, reflected poly 0xEDB88320) ------------------------------
const std::array<quint32, 256> &crcTable()
{
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> t{};
        for (quint32 n = 0; n < 256; ++n) {
            quint32 c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[n] = c;
        }
        return t;
    }();
    return table;
}

quint32 crc32(const QByteArray &data)
{
    const std::array<quint32, 256> &t = crcTable();
    quint32 c = 0xFFFFFFFFu;
    const uchar *p = reinterpret_cast<const uchar *>(data.constData());
    for (qsizetype i = 0; i < data.size(); ++i)
        c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// --- little-endian byte helpers --------------------------------------------
void appendU16(QByteArray &out, quint16 v)
{
    out.append(char(v & 0xFFu));
    out.append(char((v >> 8) & 0xFFu));
}

void appendU32(QByteArray &out, quint32 v)
{
    out.append(char(v & 0xFFu));
    out.append(char((v >> 8) & 0xFFu));
    out.append(char((v >> 16) & 0xFFu));
    out.append(char((v >> 24) & 0xFFu));
}

quint16 readU16(const QByteArray &b, qsizetype off)
{
    if (off < 0 || off + 2 > b.size())
        return 0;
    const uchar *p = reinterpret_cast<const uchar *>(b.constData()) + off;
    return quint16(p[0] | (p[1] << 8));
}

quint32 readU32(const QByteArray &b, qsizetype off)
{
    if (off < 0 || off + 4 > b.size())
        return 0;
    const uchar *p = reinterpret_cast<const uchar *>(b.constData()) + off;
    return quint32(p[0]) | (quint32(p[1]) << 8)
           | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

void setError(QString *out, const QString &msg)
{
    if (out)
        *out = msg;
}

struct Entry {
    QByteArray name;
    QByteArray data;
    quint32    crc    = 0;
    quint32    offset = 0;
};

}   // namespace

namespace AnnotationBundle {

bool write(const QString &bundlePath, const QByteArray &pdfBytes,
           const QJsonObject &annotations, QString *errorOut)
{
    // A bundle is only ever a .dpz. Refusing every other target is what keeps
    // 保存 from ever overwriting the document it came from: even if a caller
    // hands us the wrong path (a .docx, the original .pdf), the source file
    // stays untouchable and the failure is visible instead of destructive.
    if (!bundlePath.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive)) {
        setError(errorOut, QStringLiteral("批注包只能保存为 .dpz（拒绝写入 %1）").arg(bundlePath));
        return false;
    }

    const QByteArray json = QJsonDocument(annotations).toJson(QJsonDocument::Indented);

    QVector<Entry> entries;
    entries.reserve(2);
    {
        Entry e;
        e.name = QByteArrayLiteral("source.pdf");
        e.data = pdfBytes;
        entries.append(e);
    }
    {
        Entry e;
        e.name = QByteArrayLiteral("annotations.json");
        e.data = json;
        entries.append(e);
    }

    QByteArray out;
    out.reserve(int(pdfBytes.size()) + int(json.size()) + 512);

    // Local file headers + stored data, recording each entry's header offset.
    for (Entry &e : entries) {
        e.crc = crc32(e.data);
        e.offset = quint32(out.size());
        appendU32(out, kLocalSig);
        appendU16(out, 20);                         // version needed to extract
        appendU16(out, 0);                          // general purpose flags
        appendU16(out, 0);                          // method 0 = stored
        appendU16(out, kDosTime);
        appendU16(out, kDosDate);
        appendU32(out, e.crc);
        appendU32(out, quint32(e.data.size()));     // compressed size == raw
        appendU32(out, quint32(e.data.size()));     // uncompressed size
        appendU16(out, quint16(e.name.size()));
        appendU16(out, 0);                          // extra field length
        out.append(e.name);
        out.append(e.data);
    }

    // Central directory.
    const quint32 centralOffset = quint32(out.size());
    for (const Entry &e : entries) {
        appendU32(out, kCentralSig);
        appendU16(out, 20);                         // version made by
        appendU16(out, 20);                         // version needed
        appendU16(out, 0);                          // flags
        appendU16(out, 0);                          // method
        appendU16(out, kDosTime);
        appendU16(out, kDosDate);
        appendU32(out, e.crc);
        appendU32(out, quint32(e.data.size()));
        appendU32(out, quint32(e.data.size()));
        appendU16(out, quint16(e.name.size()));
        appendU16(out, 0);                          // extra field length
        appendU16(out, 0);                          // comment length
        appendU16(out, 0);                          // disk number start
        appendU16(out, 0);                          // internal attributes
        appendU32(out, 0);                          // external attributes
        appendU32(out, e.offset);                   // local header offset
        out.append(e.name);
    }
    const quint32 centralSize = quint32(out.size()) - centralOffset;

    // End of central directory.
    appendU32(out, kEocdSig);
    appendU16(out, 0);                              // this disk
    appendU16(out, 0);                              // disk with central dir
    appendU16(out, quint16(entries.size()));        // entries on this disk
    appendU16(out, quint16(entries.size()));        // total entries
    appendU32(out, centralSize);
    appendU32(out, centralOffset);
    appendU16(out, 0);                              // comment length

    QSaveFile file(bundlePath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorOut, QStringLiteral("无法写入 %1：%2")
                               .arg(bundlePath, file.errorString()));
        return false;
    }
    if (file.write(out) != out.size()) {
        setError(errorOut, QStringLiteral("写入 %1 失败：%2")
                               .arg(bundlePath, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(errorOut, QStringLiteral("提交 %1 失败：%2")
                               .arg(bundlePath, file.errorString()));
        return false;
    }
    return true;
}

bool read(const QString &bundlePath, QByteArray *pdfBytes,
          QJsonObject *annotations, QString *errorOut)
{
    QFile file(bundlePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(errorOut, QStringLiteral("无法打开 %1：%2")
                               .arg(bundlePath, file.errorString()));
        return false;
    }
    const QByteArray zip = file.readAll();
    file.close();

    if (zip.size() < 22) {
        setError(errorOut, QStringLiteral("%1 不是有效的批注包（文件太小）")
                               .arg(bundlePath));
        return false;
    }

    // Locate the EOCD by scanning backwards over the last ~64 KB.
    const qsizetype scanStart = qMax<qsizetype>(0, zip.size() - (65536 + 22));
    qsizetype eocd = -1;
    for (qsizetype i = zip.size() - 22; i >= scanStart; --i) {
        if (readU32(zip, i) == kEocdSig) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        setError(errorOut, QStringLiteral("%1 不是有效的批注包（缺少 ZIP 结束记录）")
                               .arg(bundlePath));
        return false;
    }

    const quint16 entryCount = readU16(zip, eocd + 10);
    const quint32 centralOff = readU32(zip, eocd + 16);

    QHash<QByteArray, QByteArray> entries;
    qsizetype p = qsizetype(centralOff);
    for (int i = 0; i < int(entryCount); ++i) {
        if (readU32(zip, p) != kCentralSig) {
            setError(errorOut, QStringLiteral("%1 的中央目录已损坏").arg(bundlePath));
            return false;
        }
        const quint16 method    = readU16(zip, p + 10);
        const quint32 compSize  = readU32(zip, p + 20);
        const quint16 nameLen   = readU16(zip, p + 28);
        const quint16 extraLen  = readU16(zip, p + 30);
        const quint16 commentLen = readU16(zip, p + 32);
        const quint32 localOff  = readU32(zip, p + 42);
        const qsizetype nameOff = p + 46;
        if (nameOff + nameLen > zip.size()) {
            setError(errorOut, QStringLiteral("%1 的中央目录越界").arg(bundlePath));
            return false;
        }
        const QByteArray name = zip.mid(nameOff, nameLen);

        if (method != 0) {
            setError(errorOut,
                     QStringLiteral("%1 使用了不支持的压缩方式（method %2），仅支持未压缩存储")
                         .arg(bundlePath).arg(method));
            return false;
        }

        // The central entry records the offset of the LOCAL header; the data
        // begins after that header's own name and extra fields.
        if (readU32(zip, localOff) != kLocalSig) {
            setError(errorOut, QStringLiteral("%1 的本地文件头已损坏").arg(bundlePath));
            return false;
        }
        const quint16 localNameLen  = readU16(zip, localOff + 26);
        const quint16 localExtraLen = readU16(zip, localOff + 28);
        const qsizetype dataOff = qsizetype(localOff) + 30 + localNameLen + localExtraLen;
        if (dataOff + compSize > zip.size()) {
            setError(errorOut, QStringLiteral("%1 的条目数据越界").arg(bundlePath));
            return false;
        }
        entries.insert(name, zip.mid(dataOff, compSize));

        p = nameOff + nameLen + extraLen + commentLen;
    }

    if (!entries.contains(QByteArrayLiteral("source.pdf"))
        || !entries.contains(QByteArrayLiteral("annotations.json"))) {
        setError(errorOut, QStringLiteral("%1 缺少 source.pdf 或 annotations.json")
                               .arg(bundlePath));
        return false;
    }

    QJsonParseError perr{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(entries.value(QByteArrayLiteral("annotations.json")), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        setError(errorOut, QStringLiteral("%1 的 annotations.json 无法解析：%2")
                               .arg(bundlePath, perr.errorString()));
        return false;
    }

    if (pdfBytes)
        *pdfBytes = entries.value(QByteArrayLiteral("source.pdf"));
    if (annotations)
        *annotations = doc.object();
    return true;
}

bool isBundle(const QString &path)
{
    if (path.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive))
        return true;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return file.read(4) == QByteArrayLiteral("PK\x03\x04");
}

}   // namespace AnnotationBundle
