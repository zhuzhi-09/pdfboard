#pragma once

// Variable-width stroke outline.
//
// Why: a pure-touch panel has no pressure, but it does report the contact size
// (QEventPoint::ellipseDiameters). That is the closest thing to pressure a finger
// has, and mapping it to width is what makes finger ink look like marker ink
// instead of a constant-width pipe (the CVTE patent CN119917011A does the same
// with 压感 + 速度).
//
// The outline is a single closed contour: the left side forward, a semicircular
// end cap, the right side back, and a semicircular start cap. Drawing it as one
// path (rather than a pen per segment) keeps the joins free of seams and lets
// Qt fill it with one antialiased pass.
//
// Callers pass device-space points and a device-space base width, so the same
// helper serves the canvas (viewport pixels) and the PNG/PDF export (page pixels).

#include <QPainterPath>
#include <QPointF>
#include <QVector>
#include <QtMath>

// `widthFactors` is parallel to `pts` (1.0 = base width). When it is empty or a
// different length, every point uses 1.0. Returns an empty path for fewer than
// two points (a tap is a dot, which the caller draws itself).
inline QPainterPath inkOutlinePath(const QVector<QPointF> &pts,
                                   const QVector<qreal> &widthFactors,
                                   qreal baseWidthPx)
{
    const int n = int(pts.size());
    if (n < 2 || baseWidthPx <= 0.0)
        return {};

    const auto factorAt = [&widthFactors, n](int i) {
        return widthFactors.size() == n ? widthFactors.at(i) : 1.0;
    };
    const auto halfAt = [&](int i) {
        return qMax<qreal>(0.25, baseWidthPx * factorAt(i)) * 0.5;
    };

    // Per-point direction: the average of the two adjacent segments, so the normal
    // turns smoothly instead of stepping at every densified sample.
    QVector<QPointF> dirs;
    dirs.reserve(n);
    QPointF lastDir(1.0, 0.0);
    for (int i = 0; i < n; ++i) {
        QPointF d;
        if (i == 0)
            d = pts.at(1) - pts.at(0);
        else if (i == n - 1)
            d = pts.at(n - 1) - pts.at(n - 2);
        else
            d = pts.at(i + 1) - pts.at(i - 1);
        const qreal len = qSqrt(QPointF::dotProduct(d, d));
        if (len > 1e-9) {
            d /= len;
            lastDir = d;
        } else {
            d = lastDir;                    // repeated/duplicate sample
        }
        dirs.append(d);
    }

    QVector<QPointF> left;
    QVector<QPointF> right;
    left.reserve(n);
    right.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QPointF dir = dirs.at(i);
        const QPointF normal(-dir.y(), dir.x());
        const qreal half = halfAt(i);
        left.append(pts.at(i) + normal * half);
        right.append(pts.at(i) - normal * half);
    }

    constexpr int kCapSteps = 10;
    constexpr qreal kPi = 3.14159265358979323846;

    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.moveTo(left.at(0));
    for (int i = 1; i < n; ++i)
        path.lineTo(left.at(i));

    {   // End cap: left[n-1] -> right[n-1], bulging along the last direction.
        const QPointF c = pts.at(n - 1);
        const QPointF normal(-dirs.at(n - 1).y(), dirs.at(n - 1).x());
        const qreal r = halfAt(n - 1);
        const QPointF f = dirs.at(n - 1);
        for (int k = 1; k <= kCapSteps; ++k) {
            const qreal a = kPi * qreal(k) / kCapSteps;
            path.lineTo(c + normal * (r * qCos(a)) + f * (r * qSin(a)));
        }
    }

    for (int i = n - 1; i >= 0; --i)
        path.lineTo(right.at(i));

    {   // Start cap: right[0] -> left[0], bulging backwards.
        const QPointF c = pts.at(0);
        const QPointF normal(-dirs.at(0).y(), dirs.at(0).x());
        const qreal r = halfAt(0);
        const QPointF f = dirs.at(0);
        for (int k = kCapSteps - 1; k >= 1; --k) {
            const qreal a = kPi * qreal(k) / kCapSteps;
            path.lineTo(c + normal * (r * qCos(a)) - f * (r * qSin(a)));
        }
    }

    path.closeSubpath();
    return path;
}
