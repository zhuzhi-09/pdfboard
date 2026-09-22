#include "PdfExport.h"

#include "PdfCanvas.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QSizeF>
#include <QVector>
#include <QtMath>

namespace {

// A single sheet may not exceed this many pixels (keeps a giant source page
// from allocating a multi-gigabyte raster).
constexpr qint64 kMaxPagePixels = 40LL * 1000 * 1000;

struct Sheet {
    QByteArray jpeg;
    int w  = 0;   // raster width  (px)
    int h  = 0;   // raster height (px)
    int pw = 0;   // page width  (points)
    int ph = 0;   // page height (points)
};

void setError(QString *out, const QString &msg)
{
    if (out)
        *out = msg;
}

QByteArray encodeJpeg(const QImage &img)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    if (!img.save(&buffer, "JPG", 85))
        return {};
    buffer.close();
    return bytes;
}

}   // namespace

namespace PdfExport {

bool exportFlattened(const PdfCanvas *canvas, const QString &outPath,
                     QString *errorOut, int dpi)
{
    if (!canvas) {
        setError(errorOut, QStringLiteral("无效的文档"));
        return false;
    }
    const int pageCount = canvas->pageCount();
    if (pageCount <= 0) {
        setError(errorOut, QStringLiteral("文档没有可导出的页面"));
        return false;
    }
    if (dpi <= 0)
        dpi = 150;

    // The public ink accessor: never touch PdfCanvas private state.
    const QJsonObject ink = canvas->exportInk();
    const QJsonObject pages = ink.value(QStringLiteral("pages")).toObject();

    QVector<Sheet> sheets;
    sheets.reserve(pageCount);

    for (int page = 0; page < pageCount; ++page) {
        const QSizeF pt = canvas->pagePointSize(page);
        if (pt.width() <= 0.0 || pt.height() <= 0.0) {
            setError(errorOut, QStringLiteral("第 %1 页尺寸无效").arg(page + 1));
            return false;
        }

        const qreal scale = qreal(dpi) / 72.0;
        qreal pw = pt.width() * scale;
        qreal ph = pt.height() * scale;
        const qreal pixels = pw * ph;
        if (pixels > qreal(kMaxPagePixels)) {
            const qreal f = qSqrt(qreal(kMaxPagePixels) / pixels);
            pw *= f;
            ph *= f;
        }
        const int iw = qMax(1, int(qRound(pw)));
        const int ih = qMax(1, int(qRound(ph)));

        const QImage raster = canvas->pageThumbnail(page, QSize(iw, ih));
        if (raster.isNull()) {
            setError(errorOut, QStringLiteral("第 %1 页渲染失败").arg(page + 1));
            return false;
        }

        // Composite page + ink onto white RGB (JPEG has no alpha channel).
        QImage composite(iw, ih, QImage::Format_RGB32);
        composite.fill(Qt::white);
        {
            QPainter p(&composite);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.drawImage(0, 0, raster);
            p.setRenderHint(QPainter::Antialiasing, true);

            const QJsonArray strokes = pages.value(QString::number(page)).toArray();
            for (const QJsonValue &sv : strokes) {
                const QJsonObject so = sv.toObject();
                QColor color(so.value(QStringLiteral("c")).toString());
                if (!color.isValid())
                    color = QColor(Qt::red);
                const qreal wNorm = so.value(QStringLiteral("w")).toDouble(0.004);
                const QJsonArray pts = so.value(QStringLiteral("p")).toArray();
                if (pts.size() < 4)
                    continue;

                // Same normalized -> pixel mapping the on-screen ink uses.
                QPainterPath path;
                path.moveTo(pts.at(0).toDouble() * (iw - 1),
                            pts.at(1).toDouble() * (ih - 1));
                for (qsizetype i = 2; i + 1 < pts.size(); i += 2)
                    path.lineTo(pts.at(i).toDouble() * (iw - 1),
                                pts.at(i + 1).toDouble() * (ih - 1));

                p.setPen(QPen(color, qMax<qreal>(1.0, wNorm * iw),
                              Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
        }

        Sheet sheet;
        sheet.jpeg = encodeJpeg(composite);
        if (sheet.jpeg.isEmpty()) {
            setError(errorOut, QStringLiteral("第 %1 页 JPEG 编码失败").arg(page + 1));
            return false;
        }
        sheet.w  = iw;
        sheet.h  = ih;
        sheet.pw = qMax(1, int(qRound(pt.width())));
        sheet.ph = qMax(1, int(qRound(pt.height())));
        sheets.append(sheet);
    }

    // --- assemble a minimal PDF 1.4 ---------------------------------------
    // Object layout:  1 = Catalog, 2 = Pages, then per page (image, content,
    // page). Every offset is an exact byte offset into the buffer below.
    const int total = 2 + 3 * pageCount;
    QVector<int> offsets(total + 1, 0);

    QByteArray out;
    out.reserve(4096 + pageCount * 2048);
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
    out += "<< /Type /Pages /Kids [";
    for (int i = 0; i < pageCount; ++i) {
        out += QByteArray::number(3 + i * 3 + 2);
        out += " 0 R ";
    }
    out += "] /Count ";
    out += QByteArray::number(pageCount);
    out += " >>";
    endObj();

    for (int i = 0; i < pageCount; ++i) {
        const Sheet &s = sheets.at(i);
        const int imageObj   = 3 + i * 3;
        const int contentObj = imageObj + 1;
        const int pageObj    = imageObj + 2;

        beginObj(imageObj);
        out += "<< /Type /XObject /Subtype /Image /Width ";
        out += QByteArray::number(s.w);
        out += " /Height ";
        out += QByteArray::number(s.h);
        out += " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length ";
        out += QByteArray::number(int(s.jpeg.size()));
        out += " >>\nstream\n";
        out += s.jpeg;
        out += "\nendstream";
        endObj();

        const QByteArray content =
            "q " + QByteArray::number(s.pw) + " 0 0 " + QByteArray::number(s.ph)
            + " 0 0 cm /Im0 Do Q";
        beginObj(contentObj);
        out += "<< /Length ";
        out += QByteArray::number(int(content.size()));
        out += " >>\nstream\n";
        out += content;
        out += "\nendstream";
        endObj();

        beginObj(pageObj);
        out += "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ";
        out += QByteArray::number(s.pw);
        out += " ";
        out += QByteArray::number(s.ph);
        out += "] /Resources << /XObject << /Im0 ";
        out += QByteArray::number(imageObj);
        out += " 0 R >> >> /Contents ";
        out += QByteArray::number(contentObj);
        out += " 0 R >>";
        endObj();
    }

    const int xrefOffset = int(out.size());
    out += "xref\n0 ";
    out += QByteArray::number(total + 1);
    out += "\n";
    out += "0000000000 65535 f \n";
    for (int i = 1; i <= total; ++i) {
        out += QByteArray::number(offsets.at(i)).rightJustified(10, '0');
        out += " 00000 n \n";
    }

    out += "trailer\n<< /Size ";
    out += QByteArray::number(total + 1);
    out += " /Root 1 0 R >>\nstartxref\n";
    out += QByteArray::number(xrefOffset);
    out += "\n%%EOF\n";

    QSaveFile file(outPath);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(errorOut, QStringLiteral("无法写入 %1：%2")
                               .arg(outPath, file.errorString()));
        return false;
    }
    if (file.write(out) != out.size()) {
        setError(errorOut, QStringLiteral("写入 %1 失败：%2")
                               .arg(outPath, file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(errorOut, QStringLiteral("提交 %1 失败：%2")
                               .arg(outPath, file.errorString()));
        return false;
    }
    return true;
}

}   // namespace PdfExport
