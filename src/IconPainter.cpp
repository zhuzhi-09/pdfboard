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

// House: a roof over a wall with a door - 「主页」. The outline spans the same
// 3.5 .. 20.5 band as the gear, so the two read as one icon family on the tab
// strip; the walls drop straight from the eaves, which keeps every corner clean
// at small sizes.
void glyphHouse(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);

    QPainterPath outline;
    outline.moveTo(g.p(3.6, 10.6));    // 左屋檐
    outline.lineTo(g.p(12.0, 3.8));    // 屋脊
    outline.lineTo(g.p(20.4, 10.6));   // 右屋檐
    outline.lineTo(g.p(20.4, 20.6));   // 右墙
    outline.lineTo(g.p(3.6, 20.6));    // 地面
    outline.closeSubpath();            // 左墙回到屋檐
    p.drawPath(outline);

    // Door: rounded at the top, flush with the floor.
    QPainterPath door;
    door.moveTo(g.p(9.8, 20.6));
    door.lineTo(g.p(9.8, 15.4));
    door.quadTo(g.p(9.8, 14.5), g.p(10.7, 14.5));
    door.lineTo(g.p(13.3, 14.5));
    door.quadTo(g.p(14.2, 14.5), g.p(14.2, 15.4));
    door.lineTo(g.p(14.2, 20.6));
    p.drawPath(door);
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

// Four corner brackets with their elbows at the box corners: "go fullscreen".
void glyphFullscreen(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    constexpr qreal a = 4.0;    // corner inset
    constexpr qreal b = 9.0;    // arm end
    constexpr qreal e = 24.0 - a;
    constexpr qreal f = 24.0 - b;

    QPainterPath brackets;
    brackets.moveTo(g.p(b, a)); brackets.lineTo(g.p(a, a)); brackets.lineTo(g.p(a, b));
    brackets.moveTo(g.p(f, a)); brackets.lineTo(g.p(e, a)); brackets.lineTo(g.p(e, b));
    brackets.moveTo(g.p(e, f)); brackets.lineTo(g.p(e, e)); brackets.lineTo(g.p(f, e));
    brackets.moveTo(g.p(a, f)); brackets.lineTo(g.p(a, e)); brackets.lineTo(g.p(b, e));
    p.drawPath(brackets);
}

// The same brackets flipped: the elbows sit inwards and the arms point at the
// corners, the usual "leave fullscreen" affordance.
void glyphFullscreenExit(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    constexpr qreal a = 4.0;    // arm end, at the box edge
    constexpr qreal b = 9.0;    // elbow, inwards
    constexpr qreal e = 24.0 - a;
    constexpr qreal f = 24.0 - b;

    QPainterPath brackets;
    brackets.moveTo(g.p(a, b)); brackets.lineTo(g.p(b, b)); brackets.lineTo(g.p(b, a));
    brackets.moveTo(g.p(e, b)); brackets.lineTo(g.p(f, b)); brackets.lineTo(g.p(f, a));
    brackets.moveTo(g.p(e, f)); brackets.lineTo(g.p(f, f)); brackets.lineTo(g.p(f, e));
    brackets.moveTo(g.p(a, f)); brackets.lineTo(g.p(b, f)); brackets.lineTo(g.p(b, e));
    p.drawPath(brackets);
}

// Four-way arrow: a short vertical and a short horizontal segment crossing at
// the centre, every end capped with an arrowhead - 「自由移动」.
void glyphMove(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.drawLine(g.p(12.0, 5.2), g.p(12.0, 18.8));
    p.drawLine(g.p(5.2, 12.0), g.p(18.8, 12.0));

    QPainterPath heads;
    heads.moveTo(g.p(9.4, 7.8));  heads.lineTo(g.p(12.0, 5.2));  heads.lineTo(g.p(14.6, 7.8));
    heads.moveTo(g.p(14.6, 16.2)); heads.lineTo(g.p(12.0, 18.8)); heads.lineTo(g.p(9.4, 16.2));
    heads.moveTo(g.p(7.8, 9.4));  heads.lineTo(g.p(5.2, 12.0));  heads.lineTo(g.p(7.8, 14.6));
    heads.moveTo(g.p(16.2, 14.6)); heads.lineTo(g.p(18.8, 12.0)); heads.lineTo(g.p(16.2, 9.4));
    p.drawPath(heads);
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

// --- settings page glyphs ----------------------------------------------------
// Same 24-unit grid and the same 3.5 .. 20.5 outer band as the gear/house, so
// the navigation rail and the setting cards read as one icon family.

void fillDot(QPainter &p, const Grid &g, const QColor &c, qreal x, qreal y, qreal r)
{
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(g.p(x, y), r * g.s, r * g.s);
    p.restore();
}

// Breadcrumb separator.
void glyphChevronRight(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath chevron;
    chevron.moveTo(g.p(9.0, 5.6));
    chevron.lineTo(g.p(15.4, 12.0));
    chevron.lineTo(g.p(9.0, 18.4));
    p.drawPath(chevron);
}

// Painter's palette: a disc with a thumb hole and three paint dots - 外观 / 主题色.
void glyphPalette(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath disc;
    disc.addEllipse(g.p(12.0, 12.0), 8.6 * g.s, 8.6 * g.s);
    QPainterPath hole;
    hole.addEllipse(g.p(15.6, 15.4), 2.1 * g.s, 2.1 * g.s);
    p.drawPath(disc);
    p.drawPath(hole);
    fillDot(p, g, c, 8.0, 10.6, 1.4);
    fillDot(p, g, c, 11.4, 7.4, 1.4);
    fillDot(p, g, c, 15.6, 8.6, 1.4);
}

// Circled "i" - 关于.
void glyphInfo(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.drawEllipse(g.p(12.0, 12.0), 8.6 * g.s, 8.6 * g.s);
    p.drawLine(g.p(12.0, 11.0), g.p(12.0, 16.6));
    fillDot(p, g, c, 12.0, 7.7, 1.25);
}

// Power symbol - 开机自动启动.
void glyphPower(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    const QRectF ring = g.r(4.2, 5.0, 15.6, 15.6);
    // Open at the top: 300 degrees starting just right of 12 o'clock.
    p.drawArc(ring, (90 - 35) * 16, -(360 - 70) * 16);
    p.drawLine(g.p(12.0, 3.4), g.p(12.0, 11.6));
}

// Open hand - 手掌当橡皮.
void glyphHand(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.drawLine(g.p(8.2, 12.2), g.p(8.2, 6.6));
    p.drawLine(g.p(11.2, 11.0), g.p(11.2, 4.0));
    p.drawLine(g.p(14.2, 11.0), g.p(14.2, 4.8));
    p.drawLine(g.p(17.2, 12.4), g.p(17.2, 7.4));
    QPainterPath palm;
    palm.moveTo(g.p(8.2, 12.2));
    palm.lineTo(g.p(8.2, 14.6));
    palm.lineTo(g.p(5.6, 12.4));     // thumb
    palm.cubicTo(g.p(4.4, 11.4), g.p(3.2, 12.8), g.p(4.2, 14.2));
    palm.cubicTo(g.p(6.4, 17.6), g.p(8.4, 20.6), g.p(12.6, 20.6));
    palm.cubicTo(g.p(16.2, 20.6), g.p(17.2, 18.0), g.p(17.2, 15.4));
    palm.lineTo(g.p(17.2, 12.4));
    p.drawPath(palm);
}

// Four-point sparkle - 减少动效.
void glyphSparkle(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath star;
    star.moveTo(g.p(12.0, 3.4));
    star.quadTo(g.p(12.6, 11.4), g.p(20.6, 12.0));
    star.quadTo(g.p(12.6, 12.6), g.p(12.0, 20.6));
    star.quadTo(g.p(11.4, 12.6), g.p(3.4, 12.0));
    star.quadTo(g.p(11.4, 11.4), g.p(12.0, 3.4));
    p.drawPath(star);
}

// Two chain links - 默认打开方式 / 项目主页.
void glyphLink(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.save();
    p.translate(g.p(12.0, 12.0));
    p.rotate(-45.0);
    const qreal s = g.s;
    p.drawRoundedRect(QRectF(-9.6 * s, -3.4 * s, 10.4 * s, 6.8 * s), 3.4 * s, 3.4 * s);
    p.drawRoundedRect(QRectF(-0.8 * s, -3.4 * s, 10.4 * s, 6.8 * s), 3.4 * s, 3.4 * s);
    p.restore();
}

// Arrow into a tray - 检查更新.
void glyphDownload(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.drawLine(g.p(12.0, 3.6), g.p(12.0, 14.8));
    QPainterPath head;
    head.moveTo(g.p(7.4, 10.4));
    head.lineTo(g.p(12.0, 15.0));
    head.lineTo(g.p(16.6, 10.4));
    p.drawPath(head);
    QPainterPath tray;
    tray.moveTo(g.p(4.2, 15.6));
    tray.lineTo(g.p(4.2, 20.2));
    tray.lineTo(g.p(19.8, 20.2));
    tray.lineTo(g.p(19.8, 15.6));
    p.drawPath(tray);
}

// Half-filled disc - 外观（浅色 / 深色）.
void glyphContrast(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    const QPointF centre = g.p(12.0, 12.0);
    const qreal r = 8.6 * g.s;
    p.drawEllipse(centre, r, r);
    QPainterPath half;
    half.moveTo(centre.x(), centre.y() - r);
    half.arcTo(QRectF(centre.x() - r, centre.y() - r, 2 * r, 2 * r), 90.0, -180.0);
    half.closeSubpath();
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPath(half);
    p.restore();
}

// Bug - 调试 / 诊断日志.
void glyphBug(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.drawRoundedRect(g.r(7.4, 8.0, 9.2, 12.6), 4.6 * g.s, 4.6 * g.s);
    p.drawLine(g.p(12.0, 11.6), g.p(12.0, 20.4));
    QPainterPath head;
    head.moveTo(g.p(9.0, 8.4));
    head.cubicTo(g.p(9.0, 4.6), g.p(15.0, 4.6), g.p(15.0, 8.4));
    p.drawPath(head);
    p.drawLine(g.p(7.4, 11.4), g.p(4.0, 9.6));
    p.drawLine(g.p(7.4, 14.6), g.p(3.6, 14.6));
    p.drawLine(g.p(7.6, 17.8), g.p(4.2, 19.6));
    p.drawLine(g.p(16.6, 11.4), g.p(20.0, 9.6));
    p.drawLine(g.p(16.6, 14.6), g.p(20.4, 14.6));
    p.drawLine(g.p(16.4, 17.8), g.p(19.8, 19.6));
}

// Isometric box - LibreOffice 附加包.
void glyphPackage(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath box;
    box.moveTo(g.p(12.0, 3.4));
    box.lineTo(g.p(19.8, 7.6));
    box.lineTo(g.p(19.8, 16.4));
    box.lineTo(g.p(12.0, 20.6));
    box.lineTo(g.p(4.2, 16.4));
    box.lineTo(g.p(4.2, 7.6));
    box.closeSubpath();
    p.drawPath(box);
    QPainterPath inner;
    inner.moveTo(g.p(4.2, 7.6));
    inner.lineTo(g.p(12.0, 11.8));
    inner.lineTo(g.p(19.8, 7.6));
    inner.moveTo(g.p(12.0, 11.8));
    inner.lineTo(g.p(12.0, 20.6));
    p.drawPath(inner);
}

// Bell with a slash - 关闭 Word 的提醒.
void glyphBellOff(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath bell;
    bell.moveTo(g.p(4.8, 17.0));
    bell.lineTo(g.p(19.2, 17.0));
    bell.moveTo(g.p(6.6, 17.0));
    bell.lineTo(g.p(6.6, 11.0));
    bell.cubicTo(g.p(6.6, 7.4), g.p(9.0, 5.0), g.p(12.0, 5.0));
    bell.cubicTo(g.p(15.0, 5.0), g.p(17.4, 7.4), g.p(17.4, 11.0));
    bell.lineTo(g.p(17.4, 17.0));
    p.drawPath(bell);
    p.drawLine(g.p(10.4, 20.0), g.p(13.6, 20.0));
    p.drawLine(g.p(4.0, 4.0), g.p(20.0, 20.0));
}

// A pill with three buttons - 工具栏风格.
void glyphToolbar(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    p.drawRoundedRect(g.r(3.0, 8.0, 18.0, 8.0), 4.0 * g.s, 4.0 * g.s);
    fillDot(p, g, c, 8.0, 12.0, 1.3);
    fillDot(p, g, c, 12.0, 12.0, 1.3);
    fillDot(p, g, c, 16.0, 12.0, 1.3);
}

// Two arrows chasing each other - 转换引擎.
void glyphConvert(QPainter &p, const Grid &g, const QColor &c, qreal w)
{
    setStroke(p, c, w);
    QPainterPath top;
    top.moveTo(g.p(4.4, 9.0));
    top.lineTo(g.p(19.2, 9.0));
    top.moveTo(g.p(15.4, 5.2));
    top.lineTo(g.p(19.2, 9.0));
    top.lineTo(g.p(15.4, 12.8));
    QPainterPath bottom;
    bottom.moveTo(g.p(19.6, 15.0));
    bottom.lineTo(g.p(4.8, 15.0));
    bottom.moveTo(g.p(8.6, 11.2));
    bottom.lineTo(g.p(4.8, 15.0));
    bottom.lineTo(g.p(8.6, 18.8));
    p.drawPath(top);
    p.drawPath(bottom);
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
    case Glyph::House:
        glyphHouse(p, g, color, w);
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
    case Glyph::Fullscreen:
        glyphFullscreen(p, g, color, w);
        break;
    case Glyph::FullscreenExit:
        glyphFullscreenExit(p, g, color, w);
        break;
    case Glyph::Move:
        glyphMove(p, g, color, w);
        break;
    case Glyph::Document:
        glyphDocument(p, g, color, w);
        break;
    case Glyph::Swash:
        glyphSwash(p, g, color, w);
        break;
    case Glyph::ChevronRight:
        glyphChevronRight(p, g, color, w);
        break;
    case Glyph::Palette:
        glyphPalette(p, g, color, w);
        break;
    case Glyph::Info:
        glyphInfo(p, g, color, w);
        break;
    case Glyph::Power:
        glyphPower(p, g, color, w);
        break;
    case Glyph::Hand:
        glyphHand(p, g, color, w);
        break;
    case Glyph::Sparkle:
        glyphSparkle(p, g, color, w);
        break;
    case Glyph::Link:
        glyphLink(p, g, color, w);
        break;
    case Glyph::Download:
        glyphDownload(p, g, color, w);
        break;
    case Glyph::Contrast:
        glyphContrast(p, g, color, w);
        break;
    case Glyph::Bug:
        glyphBug(p, g, color, w);
        break;
    case Glyph::Package:
        glyphPackage(p, g, color, w);
        break;
    case Glyph::BellOff:
        glyphBellOff(p, g, color, w);
        break;
    case Glyph::Toolbar:
        glyphToolbar(p, g, color, w);
        break;
    case Glyph::Convert:
        glyphConvert(p, g, color, w);
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
