#pragma once

// Bounding rect (viewport coordinates) that must be repainted while a live
// stroke grows.
//
// Why this exists: repainting the whole viewport for every input sample is what
// made handwriting lag behind the finger. A classroom panel sends a sample
// roughly every 8 ms, while one full repaint of a 4K viewport (backdrop + page
// shadows + rounded-rect clips + bilinear-scaled page bitmaps) costs tens of
// milliseconds - the samples were arriving, but their pixels were queued behind
// a repaint that had nothing to do with them. Only the geometry the new samples
// actually add needs to be repainted.
//
// The stroke painter joins samples with quadratic curves through the midpoints,
// using the samples as control points, so everything the newest samples can add
// lies inside the convex hull of the last three samples (Bezier hull property).
// That hull, padded by half the pen width plus a little anti-aliasing bleed, is
// exactly the box returned here.

#include <QPointF>
#include <QRect>
#include <QRectF>

// `normPts` are normalized (0..1) page coordinates, `pageRect` is the page in
// viewport coordinates, `fromIndex` is the first sample to consider (pass the
// index of the sample before the first new one so its control point counts) and
// `padPx` is the extra margin in viewport pixels.
inline QRect inkDirtyRect(const QRectF &pageRect, const QPointF *normPts, int count,
                          int fromIndex, qreal padPx)
{
    if (!normPts || count <= 0 || pageRect.isEmpty())
        return QRect();

    const int first = qBound(0, fromIndex, count - 1);
    qreal minX = normPts[first].x();
    qreal maxX = minX;
    qreal minY = normPts[first].y();
    qreal maxY = minY;
    for (int i = first + 1; i < count; ++i) {
        minX = qMin(minX, normPts[i].x());
        maxX = qMax(maxX, normPts[i].x());
        minY = qMin(minY, normPts[i].y());
        maxY = qMax(maxY, normPts[i].y());
    }

    const QRectF box(pageRect.left() + minX * pageRect.width(),
                     pageRect.top() + minY * pageRect.height(),
                     (maxX - minX) * pageRect.width(),
                     (maxY - minY) * pageRect.height());
    return box.adjusted(-padPx, -padPx, padPx, padPx).toAlignedRect();
}
