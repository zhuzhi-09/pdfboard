#include "ImageImport.h"

#include "AppLog.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSaveFile>
#include <QSet>
#include <QVector>
#include <QtMath>

namespace {

void setError(QString *out, const QString &msg)
{
    if (out)
        *out = msg;
}

// A plain decimal with trailing zeros trimmed. PDF accepts integers and reals,
// and the 1:1 page wants the exact fraction of a point (800x600 px -> 600x450).
QByteArray pdfNumber(qreal v)
{
    QString s = QString::number(v, 'f', 2);
    while (s.endsWith(QLatin1Char('0')))
        s.chop(1);
    if (s.endsWith(QLatin1Char('.')))
        s.chop(1);
    if (s == QStringLiteral("-0"))
        s = QStringLiteral("0");
    return s.toLatin1();
}

}   // namespace

namespace ImageImport {

// The formats we accept. This is the single source of truth: the open dialog and the
// self test both read it, and the test additionally proves that Qt can DECODE every
// one of them in this build - otherwise we would advertise formats we cannot open.
// (webp and tiff are deliberately absent: Qt's binary package does not ship those
// image plugins, so a .webp would have failed to open on a classroom machine even
// though the file dialog offered it.)
static const QStringList kExtensions{
    QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
    QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("ico"),
};

bool isImage(const QString &path)
{
    return kExtensions.contains(QFileInfo(path).suffix().toLower());
}

QStringList extensions()
{
    return kExtensions;
}

QString dialogFilter()
{
    QStringList patterns;
    for (const QString &ext : kExtensions)
        patterns << QStringLiteral("*.") + ext;
    return QStringLiteral("图片 (%1)").arg(patterns.join(QLatin1Char(' ')));
}

QSizeF pageSizePtFor(const QSize &pixels)
{
    return QSizeF(pixels.width() * 72.0 / 96.0, pixels.height() * 72.0 / 96.0);
}

bool toPdf(const QString &imagePath, const QString &pdfOutPath, QString *errorOut)
{
    if (pdfOutPath.isEmpty()) {
        setError(errorOut, QStringLiteral("缺少输出路径"));
        return false;
    }

    QImageReader reader(imagePath);
    reader.setAutoTransform(true);       // honour EXIF orientation
    QImage img = reader.read();
    if (img.isNull()) {
        setError(errorOut, QStringLiteral("无法读取图片 %1：%2")
                               .arg(imagePath, reader.errorString()));
        return false;
    }

    // Oversized sources are scaled down (aspect kept); the factor is logged.
    const qreal sourcePixels = qreal(img.width()) * qreal(img.height());
    if (sourcePixels > qreal(kMaxPixels)) {
        const qreal f = qSqrt(qreal(kMaxPixels) / sourcePixels);
        const QSize scaled(qMax(1, int(img.width() * f)),
                           qMax(1, int(img.height() * f)));
        img = img.scaled(scaled, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        AppLog::write(QStringLiteral("image"),
                      QStringLiteral("导入图片 %1 超过 %2 像素，按 %3 缩放为 %4×%5")
                          .arg(QFileInfo(imagePath).fileName())
                          .arg(kMaxPixels)
                          .arg(f, 0, 'f', 4)
                          .arg(img.width()).arg(img.height()));
    }

    // Flatten onto opaque white: a PDF page is paper, so it has no alpha.
    QImage rgb(img.width(), img.height(), QImage::Format_RGB32);
    rgb.fill(Qt::white);
    {
        QPainter p(&rgb);
        p.drawImage(0, 0, img);
    }

    const int w = rgb.width();
    const int h = rgb.height();

    // Raw DeviceRGB samples in R,G,B order, losslessly FlateDecode-compressed.
    QByteArray raw;
    raw.reserve(qsizetype(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = line[x];
            raw.append(char(uchar(qRed(c))));
            raw.append(char(uchar(qGreen(c))));
            raw.append(char(uchar(qBlue(c))));
        }
    }
    // qCompress prepends a 4-byte big-endian length; /FlateDecode wants the bare
    // zlib stream that follows it, so the prefix is stripped.
    const QByteArray deflated = qCompress(raw).mid(4);

    const QSizeF page = pageSizePtFor(QSize(w, h));
    const QByteArray pw = pdfNumber(page.width());
    const QByteArray ph = pdfNumber(page.height());

    // Object tree: 1 = Catalog, 2 = Pages, 3 = Image, 4 = Content, 5 = Page.
    QVector<int> offsets(6, 0);
    QByteArray out;
    out.reserve(deflated.size() + 1024);
    out += "%PDF-1.4\n";
    out += QByteArray("%\xE2\xE3\xCF\xD3\n", 6);   // binary marker comment

    auto beginObj = [&out, &offsets](int number) {
        offsets[number] = int(out.size());
        out += QByteArray::number(number);
        out += " 0 obj\n";
    };
    auto endObj = [&out] { out += "\nendobj\n"; };

    beginObj(1);
    out += "<< /Type /Catalog /Pages 2 0 R >>";
    endObj();

    beginObj(2);
    out += "<< /Type /Pages /Kids [5 0 R] /Count 1 >>";
    endObj();

    beginObj(3);
    out += "<< /Type /XObject /Subtype /Image /Width ";
    out += QByteArray::number(w);
    out += " /Height ";
    out += QByteArray::number(h);
    out += " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length ";
    out += QByteArray::number(int(deflated.size()));
    out += " >>\nstream\n";
    out += deflated;
    out += "\nendstream";
    endObj();

    const QByteArray content =
        "q " + pw + " 0 0 " + ph + " 0 0 cm /Im0 Do Q";
    beginObj(4);
    out += "<< /Length ";
    out += QByteArray::number(int(content.size()));
    out += " >>\nstream\n";
    out += content;
    out += "\nendstream";
    endObj();

    beginObj(5);
    out += "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ";
    out += pw;
    out += " ";
    out += ph;
    out += "] /Resources << /XObject << /Im0 3 0 R >> >> /Contents 4 0 R >>";
    endObj();

    const int xrefOffset = int(out.size());
    out += "xref\n0 6\n";
    out += "0000000000 65535 f \n";
    for (int i = 1; i <= 5; ++i) {
        out += QByteArray::number(offsets.at(i)).rightJustified(10, '0');
        out += " 00000 n \n";
    }
    out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n";
    out += QByteArray::number(xrefOffset);
    out += "\n%%EOF\n";

    // Write only after the bytes are ready, so a failure leaves nothing behind.
    QSaveFile file(pdfOutPath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorOut, QStringLiteral("无法写入 %1：%2")
                               .arg(pdfOutPath, file.errorString()));
        return false;
    }
    if (file.write(out) != out.size()) {
        setError(errorOut, QStringLiteral("写入 %1 失败：%2")
                               .arg(pdfOutPath, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(errorOut, QStringLiteral("提交 %1 失败：%2")
                               .arg(pdfOutPath, file.errorString()));
        return false;
    }
    return true;
}

bool toPdf(const QString &imagePath, QString *pdfOutPath, QString *errorOut)
{
    if (!pdfOutPath) {
        setError(errorOut, QStringLiteral("缺少输出路径"));
        return false;
    }
    const QString dir = QDir(QDir::temp()).filePath(QStringLiteral("pdfboard"));
    if (!QDir().mkpath(dir)) {
        setError(errorOut, QStringLiteral("无法创建临时目录 %1").arg(dir));
        return false;
    }
    const QByteArray digest =
        QCryptographicHash::hash(imagePath.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString out = QDir(dir).filePath(
        QStringLiteral("image-%1.pdf").arg(QString::fromLatin1(digest)));
    if (!toPdf(imagePath, out, errorOut))
        return false;
    *pdfOutPath = out;
    return true;
}

}   // namespace ImageImport
