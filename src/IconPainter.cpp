#include "IconPainter.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QtMath>

// All glyphs are authored on a 24 x 24 grid and scaled to the requested box, so
// one drawing serves every DPI and every icon size.
namespace {

struct Grid {
    QRectF box;
    qreal  s = 1.0;     // one design unit in logical px

    qreal   x(qreal v) const { return box.left() + v * s; }
    qreal   y(qreal v) const { return box.top() + v * s; }
    QPointF p(qreal vx, qreal vy) const { return QPointF(x(vx), y(vy)); }
    QRectF  r(qreal vx, qreal vy, qreal vw, qreal vh) const
    { return QRectF(x(vx), y(vy), vw * s, vh * s); }
};

void setStroke(QPainter &p, const QColor &c, qreal w,
               Qt::PenCapStyle cap = Qt::RoundCap,
               Qt::PenJoinStyle join = Qt::RoundJoin)
{
    p.setPen(QPen(c, w, Qt::SolidLine, cap, join));
    p.setBrush(Qt::NoBrush);
}

// Folder with an up arrow: "bring a document in".
void glyphOpen(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath body;
    body.addRoundedRect(g.r(3.0, 7.4, 18.0, 11.8), 2.2 * g.s, 2.2 * g.s);
    QPainterPath tab;
    tab.addRoundedRect(g.r(3.0, 5.2, 8.4, 4.6), 1.5 * g.s, 1.5 * g.s);
    p.drawPath(body.united(tab));

    p.drawLine(g.p(12.0, 16.4), g.p(12.0, 10.4));
    QPainterPath head;
    head.moveTo(g.p(9.6, 12.8));
    head.lineTo(g.p(12.0, 10.4));
    head.lineTo(g.p(14.4, 12.8));
    p.drawPath(head);
}

// Marker: outlined body running bottom left to top right, a solid nib, and a
// cap band. Drawn as an outline (not as a thick line) so it cannot be mistaken
// for an arrow.
void glyphPen(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    p.save();
    p.translate(g.p(12.0, 12.0));
    p.rotate(-45.0);

    // l() takes design grid coordinates (12.0 is the glyph centre) and returns
    // the position in the rotated frame.
    auto l = [&g](qreal v) { return (v - 12.0) * g.s; };

    setStroke(p, c, w);
    QPainterPath body;
    body.addRoundedRect(QRectF(l(8.6), l(8.4), 13.0 * g.s, 7.2 * g.s),
                        1.7 * g.s, 1.7 * g.s);
    p.drawPath(body);
    p.drawLine(QPointF(l(17.0), l(8.6)), QPointF(l(17.0), l(15.4)));

    QPainterPath nib;
    nib.moveTo(QPointF(l(3.4), l(12.0)));
    nib.lineTo(QPointF(l(8.4), l(8.6)));
    nib.lineTo(QPointF(l(8.4), l(15.4)));
    nib.closeSubpath();
    p.setBrush(c);
    p.drawPath(nib);
    p.setBrush(Qt::NoBrush);
    p.restore();
}

// Tilted eraser block resting on the page edge.
void glyphEraser(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    p.save();
    p.translate(g.p(12.2, 11.9));
    p.rotate(-38.0);
    setStroke(p, c, w);
    const QRectF body(-5.6 * g.s, -3.5 * g.s, 11.2 * g.s, 7.0 * g.s);
    QPainterPath path;
    path.addRoundedRect(body, 1.7 * g.s, 1.7 * g.s);
    p.drawPath(path);
    // Band splitting the block: one half is the rubber, the other the holder.
    p.drawLine(QPointF(body.left() + 4.0 * g.s, body.top() + 0.9 * g.s),
               QPointF(body.left() + 4.0 * g.s, body.bottom() - 0.9 * g.s));
    p.restore();

    setStroke(p, c, w);
    p.drawLine(g.p(3.6, 20.4), g.p(20.4, 20.4));
}

// Undo arrow: a "<" head with the path curving right, down and back left.
void glyphUndo(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath head;
    head.moveTo(g.p(9.4, 14.6));
    head.lineTo(g.p(4.0, 9.2));
    head.lineTo(g.p(9.4, 3.8));
    p.drawPath(head);

    QPainterPath curve;
    curve.moveTo(g.p(4.0, 9.2));
    curve.lineTo(g.p(14.4, 9.2));
    curve.arcTo(g.r(9.2, 9.2, 10.4, 10.4), 90.0, -180.0);
    curve.lineTo(g.p(10.4, 19.6));
    p.drawPath(curve);
}

// Waste bin: lid, handle, tapered body and two ribs.
void glyphTrash(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.drawLine(g.p(3.8, 6.6), g.p(20.2, 6.6));

    QPainterPath handle;
    handle.moveTo(g.p(9.4, 6.6));
    handle.lineTo(g.p(9.4, 4.2));
    handle.lineTo(g.p(14.6, 4.2));
    handle.lineTo(g.p(14.6, 6.6));
    p.drawPath(handle);

    QPainterPath body;
    body.moveTo(g.p(6.2, 6.6));
    body.lineTo(g.p(7.2, 18.6));
    body.quadTo(g.p(7.3, 19.6), g.p(8.3, 19.6));
    body.lineTo(g.p(15.7, 19.6));
    body.quadTo(g.p(16.7, 19.6), g.p(16.8, 18.6));
    body.lineTo(g.p(17.8, 6.6));
    p.drawPath(body);

    p.drawLine(g.p(10.3, 10.2), g.p(10.7, 16.6));
    p.drawLine(g.p(13.7, 10.2), g.p(13.3, 16.6));
}

// Page with a double headed horizontal arrow: "fit to width".
void glyphFitWidth(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath page;
    page.addRoundedRect(g.r(3.4, 5.2, 17.2, 13.6), 1.8 * g.s, 1.8 * g.s);
    p.drawPath(page);

    p.drawLine(g.p(7.4, 12.0), g.p(16.6, 12.0));
    QPainterPath left;
    left.moveTo(g.p(9.7, 9.7));
    left.lineTo(g.p(7.4, 12.0));
    left.lineTo(g.p(9.7, 14.3));
    p.drawPath(left);
    QPainterPath right;
    right.moveTo(g.p(14.3, 9.7));
    right.lineTo(g.p(16.6, 12.0));
    right.lineTo(g.p(14.3, 14.3));
    p.drawPath(right);
}

void glyphChevronUp(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath chevron;
    chevron.moveTo(g.p(6.4, 14.6));
    chevron.lineTo(g.p(12.0, 9.0));
    chevron.lineTo(g.p(17.6, 14.6));
    p.drawPath(chevron);
}

void glyphChevronDown(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath chevron;
    chevron.moveTo(g.p(6.4, 9.4));
    chevron.lineTo(g.p(12.0, 15.0));
    chevron.lineTo(g.p(17.6, 9.4));
    p.drawPath(chevron);
}

// Gear: a ring with eight tapered teeth and a hollow centre - 「设置」.
void glyphGear(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    const QPointF centre = g.p(12.0, 12.0);
    const qreal rRoot = 6.4 * g.s;
    const qreal rTip  = 9.5 * g.s;
    constexpr int teeth = 8;
    constexpr qreal twoPi = 6.283185307179586;
    const qreal sector = twoPi / teeth;

    auto at = [&centre](qreal r, qreal a) {
        return QPointF(centre.x() + r * qCos(a), centre.y() + r * qSin(a));
    };

    QPainterPath path;
    for (int i = 0; i < teeth; ++i) {
        const qreal a = i * sector;
        if (i == 0)
            path.moveTo(at(rRoot, a - sector * 0.42));
        else
            path.lineTo(at(rRoot, a - sector * 0.42));
        path.lineTo(at(rTip, a - sector * 0.20));   // tooth: tapered outwards
        path.lineTo(at(rTip, a + sector * 0.20));
        path.lineTo(at(rRoot, a + sector * 0.42));
        path.lineTo(at(rRoot, a + sector * 0.50));  // ease along the root circle
    }
    path.closeSubpath();
    p.drawPath(path);
    p.drawEllipse(centre, 2.8 * g.s, 2.8 * g.s);
}

// Four rounded tiles in a 2 x 2 block: the page-thumbnail picker.
void glyphPages(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    const qreal tiles[4][2] = {
        { 4.0, 4.0 }, { 13.0, 4.0 },
        { 4.0, 13.0 }, { 13.0, 13.0 },
    };
    QPainterPath paths;
    for (const auto &tile : tiles)
        paths.addRoundedRect(g.r(tile[0], tile[1], 7.0, 7.0), 1.6 * g.s, 1.6 * g.s);
    p.drawPath(paths);
}

// Page with a folded corner and two text lines: the empty state illustration.
void glyphDocument(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath page;
    page.moveTo(g.p(4.6, 3.2));
    page.lineTo(g.p(14.2, 3.2));
    page.lineTo(g.p(19.4, 8.4));
    page.lineTo(g.p(19.4, 20.8));
    page.lineTo(g.p(4.6, 20.8));
    page.closeSubpath();
    p.drawPath(page);

    QPainterPath fold;
    fold.moveTo(g.p(14.2, 3.2));
    fold.lineTo(g.p(14.2, 8.4));
    fold.lineTo(g.p(19.4, 8.4));
    p.drawPath(fold);

    p.drawLine(g.p(7.6, 12.6), g.p(16.4, 12.6));
    p.drawLine(g.p(7.6, 15.8), g.p(13.4, 15.8));
}

void glyphSwash(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w * 1.5);
    QPainterPath swash;
    swash.moveTo(g.p(3.2, 17.6));
    swash.cubicTo(g.p(7.6, 5.6), g.p(13.4, 21.4), g.p(20.8, 6.6));
    p.drawPath(swash);
}

// Classic floppy disk outline (notched top-right corner, top shutter, bottom
// label), drawn inside the design rect (x, y, bw, bh). Shared by Save / SaveAs.
void drawFloppy(QPainter &p, const Grid &g, const QColor &c, qreal w,
                qreal x, qreal y, qreal bw, qreal bh)
{
    setStroke(p, c, w);
    const qreal cut = bw * 0.24;

    QPainterPath body;
    body.moveTo(g.x(x), g.y(y));
    body.lineTo(g.x(x + bw - cut), g.y(y));
    body.lineTo(g.x(x + bw), g.y(y + cut));
    body.lineTo(g.x(x + bw), g.y(y + bh));
    body.lineTo(g.x(x), g.y(y + bh));
    body.closeSubpath();
    p.drawPath(body);

    // Shutter along the top edge.
    const qreal sw = bw * 0.42;
    const qreal sh = bh * 0.30;
    p.drawRect(QRectF(g.x(x + (bw - sw) / 2.0), g.y(y), sw * g.s, sh * g.s));

    // Label near the bottom edge.
    const qreal lw = bw * 0.60;
    const qreal lh = bh * 0.42;
    p.drawRect(QRectF(g.x(x + (bw - lw) / 2.0), g.y(y + bh - lh - 0.6),
                      lw * g.s, lh * g.s));
}

void glyphSave(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    drawFloppy(p, g, c, w, 4.0, 4.0, 16.0, 16.0);
}

// Save As: a smaller floppy with an export arrow escaping to the bottom right.
void glyphSaveAs(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    drawFloppy(p, g, c, w, 3.0, 3.0, 13.6, 13.6);

    setStroke(p, c, w);
    p.drawLine(g.p(14.6, 14.6), g.p(21.0, 21.0));
    QPainterPath head;
    head.moveTo(g.p(15.8, 21.0));
    head.lineTo(g.p(21.0, 21.0));
    head.lineTo(g.p(21.0, 15.8));
    p.drawPath(head);
}

void glyphBadge(QPainter &p, const Grid &g, const QColor &badge)
{
    if (!badge.isValid())
        return;
    const QPointF centre = g.p(17.8, 18.0);
    const qreal radius = 3.7 * g.s;
    p.setBrush(badge);
    p.setPen(QPen(QColor(255, 255, 255, 240), 1.5 * g.s));
    p.drawEllipse(centre, radius, radius);
    p.setBrush(Qt::NoBrush);
}

}   // namespace

namespace IconPainter {

void paintGlyph(QPainter &p, Glyph glyph, const QRectF &box,
                const QColor &color, qreal stroke, const QColor &badge)
{
    if (box.width() <= 0.0 || box.height() <= 0.0)
        return;

    Grid g;
    g.box = box;
    g.s   = qMin(box.width(), box.height()) / 24.0;
    const qreal w = qMax<qreal>(0.5, stroke) * g.s;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);

    switch (glyph) {
    case Glyph::Open:
        glyphOpen(p, g, color, w);
        break;
    case Glyph::Pen:
        glyphPen(p, g, color, w);
        glyphBadge(p, g, badge);
        break;
    case Glyph::Eraser:
        glyphEraser(p, g, color, w);
        break;
    case Glyph::Undo:
        glyphUndo(p, g, color, w);
        break;
    case Glyph::Redo:
        // Same drawing, mirrored around the box centre.
        p.translate(box.left() + box.right(), 0.0);
        p.scale(-1.0, 1.0);
        glyphUndo(p, g, color, w);
        break;
    case Glyph::Trash:
        glyphTrash(p, g, color, w);
        break;
    case Glyph::FitWidth:
        glyphFitWidth(p, g, color, w);
        break;
    case Glyph::ChevronUp:
        glyphChevronUp(p, g, color, w);
        break;
    case Glyph::ChevronDown:
        glyphChevronDown(p, g, color, w);
        break;
    case Glyph::Gear:
        glyphGear(p, g, color, w);
        break;
    case Glyph::Pages:
        glyphPages(p, g, color, w);
        break;
    case Glyph::Save:
        glyphSave(p, g, color, w);
        break;
    case Glyph::SaveAs:
        glyphSaveAs(p, g, color, w);
        break;
    case Glyph::Document:
        glyphDocument(p, g, color, w);
        break;
    case Glyph::Swash:
        glyphSwash(p, g, color, w);
        break;
    }

    p.restore();
}

QIcon makeIcon(Glyph glyph, int logicalPx, const States &colors, qreal dpr,
               qreal strokeScale, const QColor &badge)
{
    const int px = qMax(8, logicalPx);
    const qreal ratio = (dpr > 0.0) ? dpr : 1.0;

    auto render = [&](const QColor &color) {
        const int device = qMax(1, int(qRound(px * ratio)));
        QPixmap pm(device, device);
        pm.setDevicePixelRatio(ratio);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        const QRectF box(0.0, 0.0, qreal(device) / ratio, qreal(device) / ratio);
        paintGlyph(p, glyph, box, color, 2.0 * strokeScale, badge);
        p.end();
        return pm;
    };

    const QPixmap normal   = render(colors.normal);
    const QPixmap selected = render(colors.selected);
    const QPixmap disabled = render(colors.disabled);

    QIcon icon;
    icon.addPixmap(normal,   QIcon::Normal,   QIcon::Off);
    icon.addPixmap(selected, QIcon::Normal,   QIcon::On);
    icon.addPixmap(normal,   QIcon::Active,   QIcon::Off);
    icon.addPixmap(selected, QIcon::Active,   QIcon::On);
    icon.addPixmap(disabled, QIcon::Disabled, QIcon::Off);
    icon.addPixmap(disabled, QIcon::Disabled, QIcon::On);
    return icon;
}

}   // namespace IconPainter
