#pragma once

// Lay a PDF rendering onto paper-white.
//
// Qt's PDF renderer hands back an image WITH ALPHA: a PDF page has no background
// of its own, so whatever is painted under the raster shows through. The canvas
// has always filled `Theme::light().paper` under its pages (that is why the PDF
// keeps looking like paper in dark mode) - but the page-picker thumbnails did
// not, so in dark mode each tile showed the dark "well" tint behind it instead of
// white paper. Compositing once, where the thumbnail is cached, is cheaper than
// filling a rect on every repaint and cannot be forgotten by a future painter.
//
// `paper` is deliberately the LIGHT palette's paper even in dark mode: the page
// itself stays white so projections stay readable.

#include <QColor>
#include <QImage>
#include <QPainter>

inline QImage onPaper(const QImage &src, const QColor &paper)
{
    if (src.isNull())
        return src;

    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(src.devicePixelRatio());
    out.fill(paper);

    QPainter p(&out);
    p.drawImage(0, 0, src);
    return out;
}
