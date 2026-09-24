#include "PdfCanvas.h"
#include "AppLog.h"
#include "CrashLog.h"
#include "IconPainter.h"
#include "InkDirty.h"
#include "InkToolbar.h"
#include "InputProbe.h"
#include "Theme.h"

#include <QPdfDocument>
#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QNativeGestureEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLineF>
#include <QTimer>
#include <QTouchEvent>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QtMath>

namespace {
// Input/gesture diagnostics - a no-op unless the opt-in log is enabled.
//
// Throttled: a classroom panel delivers these samples far faster than anyone can
// read them (and some panels repeat each sample several times), so logging every
// event made the diagnostic itself a stutter source - and made the log unreadable.
// At most four lines per second, with the merged count appended.
void eraseLog(const QString &line)
{
    if (!AppLog::isEnabled())
        return;                        // no formatting cost when the log is off

    static qint64 lastMs = 0;
    static int merged = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastMs != 0 && now - lastMs < 250) {
        ++merged;
        return;
    }
    QString text = line;
    if (merged > 0) {
        text += QStringLiteral("（另有 %1 条同类事件已合并）").arg(merged);
        merged = 0;
    }
    lastMs = now;
    AppLog::write(QStringLiteral("input"), text);
}
}   // namespace

namespace {
// Snapshot cap for ink undo/redo. The model is tiny (normalized points), so
// holding 100 full snapshots costs far less than one rendered page bitmap.
constexpr int kMaxUndoSnapshots = 100;
// Densification: a stroke must not contain segments longer than this (in
// normalized page units), otherwise sparse input (fast drags, touch
// coalescing) makes erasing coarse and hit-testing unreliable.
constexpr qreal kInkMaxStepNorm = 0.0015;   // ~0.15% of the page width
constexpr int   kInkMaxInterpSteps = 400;   // hard cap per input sample

// Fallback normalized stroke width (~0.4 % of the page width) used when the
// page rect cannot be measured yet.
constexpr qreal kInkDefaultWidthNorm = 0.004;

// Appends `p` to `pts`, inserting interpolated points so that no segment is
// longer than `maxStep`.
void appendDensified(QVector<QPointF> &pts, const QPointF &p, qreal maxStep)
{
    if (pts.isEmpty()) {
        pts.append(p);
        return;
    }
    const QPointF a = pts.last();
    const QPointF d = p - a;
    const qreal dist = qSqrt(QPointF::dotProduct(d, d));
    // Panels routinely repeat one physical sample as several identical frames
    // (a classroom panel was measured at ~420 events/s with 4 duplicates per
    // sample). Those add nothing but work - the live path would be rebuilt with
    // 4x the points on every repaint - so drop exact repeats.
    if (dist < 1e-6)
        return;
    if (dist <= maxStep) {
        pts.append(p);
        return;
    }
    const int n = qBound(1, int(qCeil(dist / maxStep)), kInkMaxInterpSteps);
    for (int i = 1; i <= n; ++i)
        pts.append(a + d * (qreal(i) / qreal(n)));
}

// Eraser radius in device px (screen space), added to half the stroke width.
// The eraser removes only what it touches: a stroke is cut where it crosses
// this circle and the parts outside survive as separate strokes.
constexpr qreal kEraserRadiusPx = 14.0;

// Largest travel between two pointer samples still treated as one continuous
// eraser stroke. Anything bigger is an input discontinuity (dropped/coalesced
// events, pointer warp) and must NOT be swept - otherwise a single flick wipes
// out a whole span the eraser never actually touched.
constexpr qreal kEraserMaxJumpPx = 40.0;

// Intersects segment a->b with circle (centre c, radius r); returns the
// parameter interval [t0,t1] clamped to [0,1] that lies inside the circle.
bool circleSegmentInterval(const QPointF &a, const QPointF &b, const QPointF &c,
                           qreal r, qreal *t0, qreal *t1)
{
    const QPointF d = b - a;
    const qreal aa = QPointF::dotProduct(d, d);
    if (aa <= 1e-12)
        return false;
    const QPointF f = a - c;
    const qreal bb = 2.0 * QPointF::dotProduct(f, d);
    const qreal cc = QPointF::dotProduct(f, f) - r * r;
    const qreal disc = bb * bb - 4.0 * aa * cc;
    if (disc < 0.0)
        return false;
    const qreal s = qSqrt(disc);
    qreal u0 = (-bb - s) / (2.0 * aa);
    qreal u1 = (-bb + s) / (2.0 * aa);
    u0 = qMax<qreal>(0.0, u0);
    u1 = qMin<qreal>(1.0, u1);
    if (u0 > u1)
        return false;
    *t0 = u0;
    *t1 = u1;
    return true;
}

// Slim, unobtrusive scroll bars. The track stays transparent so the painted
// desk shows through; the canvas itself gets the same desk colour so the
// gutter next to the bars never flashes the default widget background.
QString scrollSheet()
{
    const Theme::Palette &pal = Theme::light();

    QColor handle = pal.text;
    handle.setAlpha(0x3A);
    QColor handleHover = pal.text;
    handleHover.setAlpha(0x5C);
    QColor handlePressed = pal.accent;
    handlePressed.setAlpha(0xB0);

    const QString bar   = Theme::px(Theme::Space2 + 4);       // 12 px
    const QString inset = Theme::px(Theme::Space1 + 2);       // 6 px
    const QString knob  = Theme::px(Theme::Space2 + 2);       // 10 px
    const QString radius = Theme::px(Theme::Space1);          // 4 px
    const QString minLen = Theme::px(48);

    QString sheet = QStringLiteral(
        "QAbstractScrollArea#pdfCanvas {"
        " border: none;"
        " background: %1; }"
        "QScrollBar:vertical {"
        " background: transparent;"
        " width: %2;"
        " margin: %3 2px %3 2px; }"
        "QScrollBar::handle:vertical {"
        " background: %4;"
        " min-height: %5;"
        " border-radius: %6; }"
        "QScrollBar::handle:vertical:hover { background: %7; }"
        "QScrollBar::handle:vertical:pressed { background: %8; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        " height: 0; background: transparent; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        " background: transparent; }")
        .arg(Theme::rgba(pal.deskShade))
        .arg(bar)
        .arg(inset)
        .arg(Theme::rgba(handle))
        .arg(minLen)
        .arg(radius)
        .arg(Theme::rgba(handleHover))
        .arg(Theme::rgba(handlePressed));

    sheet += QStringLiteral(
        "QScrollBar:horizontal {"
        " background: transparent;"
        " height: %1;"
        " margin: 2px %2 2px %2; }"
        "QScrollBar::handle:horizontal {"
        " background: %3;"
        " min-width: %4;"
        " border-radius: %5; }"
        "QScrollBar::handle:horizontal:hover { background: %6; }"
        "QScrollBar::handle:horizontal:pressed { background: %7; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        " width: 0; background: transparent; }"
        "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {"
        " background: transparent; }")
        .arg(knob)
        .arg(inset)
        .arg(Theme::rgba(handle))
        .arg(minLen)
        .arg(radius)
        .arg(Theme::rgba(handleHover))
        .arg(Theme::rgba(handlePressed));
    return sheet;
}
}   // namespace

PdfCanvas::PdfCanvas(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->setMouseTracking(true);
    viewport()->setCursor(Qt::CrossCursor);
    // We handle touch ourselves (1 finger = ink, 2 fingers = pinch zoom + pan)
    // instead of relying on Windows gesture messages, which IR panels and
    // streamed/forwarded touch input do not always produce.
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents, true);
    setMinimumSize(320, 240);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // The floating island (and the page picker / pen palette) are child widgets, so the
    // canvas sees no mouse events while the pointer is over them: the eraser ring froze at
    // the island's edge and could not pass it. A cheap 16 ms poll of the global cursor
    // keeps the ring and mouse strokes alive across those areas; it idles out immediately
    // whenever there is nothing to follow.
    m_pointerTrack = new QTimer(this);
    m_pointerTrack->setInterval(16);
    connect(m_pointerTrack, &QTimer::timeout, this, &PdfCanvas::trackPointer);
    m_pointerTrack->start();

    // Chrome look: slim scroll bars, and the gutter beside them in the desk
    // colour so no default widget background shows through.
    setObjectName(QStringLiteral("pdfCanvas"));
    setStyleSheet(scrollSheet());

    // Page layout and paper lift come from the same tokens as the toolbar.
    m_ui = Theme::metrics(font());
    m_gap = m_ui.pageGap;
    m_marginX = m_ui.pageMargin;
    m_pageRadius = qMax<qreal>(qreal(Theme::RadiusPage), m_ui.icon / 6.0);

    // Keep the "current page" label in sync while the user scrolls.
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        emit pageChanged(currentPage(), pageCount());
    });

    // While a pinch/zoom gesture runs we only rescale the already-rendered
    // bitmaps; this timer re-renders them crisply once the gesture settles.
    m_zoomSettle = new QTimer(this);
    m_zoomSettle->setSingleShot(true);
    m_zoomSettle->setInterval(160);
    connect(m_zoomSettle, &QTimer::timeout, this, [this]() {
        invalidateRenders();
        relayout();
        viewport()->update();
        AppLog::write(QStringLiteral("view"),
                      QStringLiteral("缩放稳定：%1×（视口 %2×%3，缓存 %4 MB）")
                          .arg(m_zoom, 0, 'f', 2)
                          .arg(viewport()->width()).arg(viewport()->height())
                          .arg(double(m_cacheBytes) / 1048576.0, 0, 'f', 1));
    });

    // Touch input can also be forwarded as synthesized mouse events. Keep the
    // ink lock a little longer than the gesture so those trailing events cannot
    // commit a stray stroke.
    m_touchRelease = new QTimer(this);
    m_touchRelease->setSingleShot(true);
    m_touchRelease->setInterval(250);
    connect(m_touchRelease, &QTimer::timeout, this, [this]() {
        m_touchInkBlocked = false;
    });

    // The classroom-whiteboard style floating toolbar lives on top of the page
    // area.
    m_toolbar = new InkToolbar(this);
}

PdfCanvas::~PdfCanvas() = default;

void PdfCanvas::applyTheme()
{
    setStyleSheet(scrollSheet());
    if (m_toolbar)
        m_toolbar->applyTheme();   // island chip, pen palette and page grid
    viewport()->update();
    update();
}

bool PdfCanvas::openPdf(const QString &path, QString *errorOut)
{
    if (m_doc) {
        m_doc->close();
        delete m_doc;
        m_doc = nullptr;
    }
    m_doc = new QPdfDocument(this);
    const QPdfDocument::Error err = m_doc->load(path);
    if (err != QPdfDocument::Error::None) {
        if (errorOut)
            *errorOut = QStringLiteral("QPdfDocument::load failed (error %1)").arg(int(err));
        delete m_doc;
        m_doc = nullptr;
        return false;
    }

    m_path = path;
    m_pageSizes.clear();
    m_maxPageWpt = 0;
    const int pc = m_doc->pageCount();
    m_pageSizes.reserve(pc);
    for (int i = 0; i < pc; ++i) {
        const QSizeF s = m_doc->pagePointSize(i);
        m_pageSizes.append(s);
        m_maxPageWpt = qMax(m_maxPageWpt, s.width());
    }
    if (m_maxPageWpt <= 0)
        m_maxPageWpt = 595.0;

    cancelGesture();
    m_ink.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    invalidateRenders();

    relayout();
    verticalScrollBar()->setValue(0);
    viewport()->update();

    emit pageChanged(currentPage(), pc);
    emit inkChanged(0);
    emit undoStateChanged();
    return true;
}

void PdfCanvas::closePdf()
{
    if (m_doc) {
        m_doc->close();
        delete m_doc;
        m_doc = nullptr;
    }
    m_path.clear();
    m_pageSizes.clear();
    m_geom.clear();
    m_contentH = 0;
    m_maxPageWpt = 595.0;
    cancelGesture();
    m_ink.clear();
    m_undoStack.clear();
    m_redoStack.clear();
    invalidateRenders();
    verticalScrollBar()->setRange(0, 0);
    viewport()->update();
    emit pageChanged(0, 0);
    emit inkChanged(0);
    emit undoStateChanged();
}

int PdfCanvas::pageCount() const { return m_pageSizes.size(); }

int PdfCanvas::currentPage() const
{
    if (m_geom.isEmpty())
        return 0;
    const qreal cy = verticalScrollBar()->value() + 1.0;
    const int p = pageAtContentY(cy);
    return p < 0 ? 0 : p;
}

void PdfCanvas::invalidateRenders()
{
    m_cache.clear();
    m_lru.clear();
    m_cacheBytes = 0;
}

// Public wrapper used by the host: only the visible document keeps rendered
// page bitmaps; inactive documents keep their handle and ink, but no rasters.
// Touching only the render cache is deliberate - the whole-page LRU strategy
// itself is untouched.
void PdfCanvas::releaseCachedPages()
{
    invalidateRenders();
}

// --- relayout re-entrancy guard ------------------------------------------------
// relayout() used to end with a scroll-bar POLICY change. A policy change makes
// QAbstractScrollArea resize the viewport, which re-enters resizeEvent -> relayout();
// the two bars then kept flipping each other's visibility (the horizontal one depends
// on the viewport width, the vertical one on its height), and the loop only stopped
// when the stack ran out - that is the 0xC00000FD we chased through three dumps.
// Two things prevent it now: the policy is never toggled here (it is set once in the
// constructor), and this cap turns any residue into a logged no-op.
static int s_relayoutDepth = 0;
static int s_relayoutMaxDepth = 0;
static int s_relayoutRefusals = 0;      // how often the loop asked past the cap
static constexpr int kRelayoutMaxDepth = 4;

namespace {
struct RelayoutDepthGuard {
    RelayoutDepthGuard()
    {
        if (++s_relayoutDepth > s_relayoutMaxDepth)
            s_relayoutMaxDepth = s_relayoutDepth;
    }
    ~RelayoutDepthGuard() { --s_relayoutDepth; }
};
}   // namespace

int PdfCanvas::testMaxRelayoutDepth() const { return s_relayoutMaxDepth; }
void PdfCanvas::testResetMaxRelayoutDepth() { s_relayoutMaxDepth = s_relayoutDepth; }
int PdfCanvas::testRelayoutRefusals() const { return s_relayoutRefusals; }

void PdfCanvas::relayout()
{
    if (s_relayoutDepth >= kRelayoutMaxDepth) {
        // The feedback loop asking yet again: refuse and leave a trace.
        ++s_relayoutRefusals;
        CrashLog::breadcrumb("relayout-nested",
                             QStringLiteral("排版嵌套第 %1 层，已截断")
                                 .arg(s_relayoutDepth + 1));
        return;
    }
    RelayoutDepthGuard layoutGuard;

    if (!m_doc || m_pageSizes.isEmpty()) {
        m_geom.clear();
        m_contentH = 0;
        m_contentW = 0;
        m_scale = 1.0;
        verticalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setRange(0, 0);
        return;
    }

    // Memo: everything below is a pure function of these values, so re-asking with the
    // same ones - which is exactly what the resize/scroll-bar feedback loop does -
    // changes nothing and is skipped.
    const int vwNow = viewport()->width();
    const int vhNow = viewport()->height();
    if (m_doc == m_layoutDoc && m_zoom == m_layoutZoom && vwNow == m_layoutW
        && vhNow == m_layoutH && m_pageSizes.size() == m_layoutPageCount
        && m_maxPageWpt == m_layoutMaxPageWpt)
        return;
    m_layoutDoc        = m_doc;
    m_layoutZoom       = m_zoom;
    m_layoutW          = vwNow;
    m_layoutH          = vhNow;
    m_layoutPageCount  = m_pageSizes.size();
    m_layoutMaxPageWpt = m_maxPageWpt;

    const qreal vw = qMax(100, vwNow);
    const qreal availW = qMax<qreal>(100.0, vw - 2.0 * m_marginX);
    m_scale = (m_maxPageWpt > 0 ? availW / m_maxPageWpt : 1.0) * m_zoom;
    m_scale = qBound(0.05, m_scale, 8.0);

    m_geom.resize(m_pageSizes.size());
    qreal y = m_ui.pagePadTop;              // breathing room above the first page
    for (int i = 0; i < m_pageSizes.size(); ++i) {
        const QSizeF s = m_pageSizes.at(i);
        PageGeom g;
        g.w = qMax<qreal>(1.0, s.width()  * m_scale);
        g.h = qMax<qreal>(1.0, s.height() * m_scale);
        g.y = y;
        m_geom[i] = g;
        y += g.h + m_gap;
    }
    m_contentH = (y > 0 ? y - m_gap + m_ui.pagePadBottom : 0);

    const int maxScroll = qMax(0, int(m_contentH) - vhNow);
    verticalScrollBar()->setRange(0, maxScroll);
    verticalScrollBar()->setPageStep(qMax(1, vhNow));
    verticalScrollBar()->setSingleStep(48);

    // Horizontal: with fit-width the content is exactly the viewport width; when
    // zoomed in it is wider, so the user can pan sideways (2-finger drag).
    qreal widest = 0.0;
    for (const PageGeom &g : m_geom)
        widest = qMax(widest, g.w);
    m_contentW = qMax<qreal>(vwNow, widest);
    const int maxHScroll = qMax(0, int(m_contentW) - vwNow);
    horizontalScrollBar()->setRange(0, maxHScroll);
    horizontalScrollBar()->setPageStep(qMax(1, vwNow));
    horizontalScrollBar()->setSingleStep(48);
    // The horizontal POLICY is deliberately never touched here (it is set once in the
    // constructor): toggling it resized the viewport and made this whole function
    // re-enter itself through resizeEvent. A hidden bar still holds the range and the
    // value that two-finger panning uses, so nothing is lost.
}

int PdfCanvas::pageAtContentY(qreal contentY) const
{
    for (int i = 0; i < m_geom.size(); ++i) {
        const PageGeom &g = m_geom.at(i);
        if (contentY < g.y)
            return -1;
        if (contentY < g.y + g.h)
            return i;
    }
    return -1;
}

QRectF PdfCanvas::pageRectInViewport(int page) const
{
    if (page < 0 || page >= m_geom.size())
        return {};
    const PageGeom &g = m_geom.at(page);
    const qreal x = (m_contentW - g.w) / 2.0 - horizontalScrollBar()->value();
    const qreal y = g.y - verticalScrollBar()->value();
    return QRectF(x, y, g.w, g.h);
}

void PdfCanvas::touchLru(int page)
{
    m_lru.removeAll(page);
    m_lru.append(page);
}

QImage PdfCanvas::imageFor(int page)
{
    const auto it = m_cache.constFind(page);
    if (it != m_cache.constEnd()) {
        touchLru(page);
        return it.value();
    }
    if (!m_doc || page < 0 || page >= m_geom.size())
        return {};

    const PageGeom &g = m_geom.at(page);
    // The layout is in logical units, but the bitmap must be rasterized at
    // DEVICE resolution, otherwise a 150%/200%/250% display shows an upscaled
    // (soft) page. Tagging the image with the ratio lets QPainter map it 1:1
    // when it draws into the logical rect.
    const qreal dpr = qMax<qreal>(1.0, viewport() ? viewport()->devicePixelRatioF() : 1.0);
    // While the pen is down (or a pinch is live) a full-resolution raster of a 4K
    // page costs tens of milliseconds ON THE UI THREAD. That is exactly the stutter
    // felt at the START of a stroke right after a zoom: the cache was just cleared,
    // the first repaint re-rasterizes every visible page, and the pen waits for it.
    // Under load render a coarse raster (4x fewer pixels) instead; the settle timer
    // sharpens everything once the pen is up.
    const bool busy = m_drawing || m_erasing || m_pinchActive;
    const int div = busy ? 2 : 1;
    QSize target(qMax(1, int(qRound(g.w * dpr)) / div),
                 qMax(1, int(qRound(g.h * dpr)) / div));

    // Safety cap so a single enormous page cannot blow the budget in one shot.
    // ~48 MB = 12 Mpx; A4 stays un-capped up to about 2.9x zoom on a 1366 px
    // wide page, which keeps memory bounded while pinch-zooming.
    constexpr qint64 kMaxPageBytes = 48LL * 1024 * 1024;
    const qint64 wantBytes = qint64(target.width()) * qint64(target.height()) * 4;
    if (wantBytes > kMaxPageBytes) {
        const qreal f = qSqrt(qreal(kMaxPageBytes) / qreal(wantBytes));
        target = QSize(qMax(1, int(target.width() * f)), qMax(1, int(target.height() * f)));
    }

    QElapsedTimer t;
    t.start();
    // The crash we are chasing is a stack overflow while zooming, so measure the depth
    // of the render path: if a page ever renders *inside* another page's render, that
    // nesting is what eats the stack and the breadcrumb will say so in the crash log.
    static int renderDepth = 0;
    ++renderDepth;
    if (renderDepth > 1)
        CrashLog::breadcrumb("render-nested",
                             QStringLiteral("渲染嵌套第 %1 层（第 %2 页）")
                                 .arg(renderDepth).arg(page + 1));
    QImage img = m_doc->render(page, target);
    --renderDepth;
    InputProbe::instance().addRender();
    // A coarse (busy-time) raster is drawn scaled up: tagging it with dpr/div keeps
    // its LOGICAL size identical to the page rect, so nothing shifts on screen.
    img.setDevicePixelRatio(dpr / div);
    m_lastRenderMs = t.elapsed();
    m_lastRenderSize = img.size();
    // Queue the readout: it feeds the status bar, which lives outside this paint.
    // Emitting it straight from here let Qt flush a dirty region immediately and
    // re-enter painting - that recursion is the 0xC00000FD. Coalesced, so a burst of
    // renders still costs exactly one queued event.
    if (!m_renderNotifyPending) {
        m_renderNotifyPending = true;
        QMetaObject::invokeMethod(this, [this] {
            m_renderNotifyPending = false;
            emit renderMeasured(m_lastRenderMs, m_lastRenderSize);
        }, Qt::QueuedConnection);
    }
    // Slow pages are the first thing to look at when scrolling feels bad.
    if (m_lastRenderMs > 40)
        AppLog::write(QStringLiteral("render"),
                      QStringLiteral("第 %1 页渲染 %2 ms（%3×%4）")
                          .arg(page + 1).arg(m_lastRenderMs)
                          .arg(m_lastRenderSize.width()).arg(m_lastRenderSize.height()));

    m_cache.insert(page, img);
    m_cacheBytes += img.sizeInBytes();
    touchLru(page);
    trimCache();
    return img;
}

// Thumbnail raster for the page picker. Deliberately separate from imageFor():
// it renders straight through QPdfDocument without touching the whole-page LRU
// (a 300 page grid must not evict the pages the reader is actually showing).
QImage PdfCanvas::pageThumbnail(int page, const QSize &size) const
{
    if (!m_doc || page < 0 || page >= m_doc->pageCount())
        return {};

    const QSizeF pt = m_doc->pagePointSize(page);
    int w = size.width() > 0 ? size.width() : int(qRound(pt.width()));
    w = qBound(1, w, 2048);

    // Preserve the page aspect ratio: a landscape page must give a landscape
    // tile, a portrait page a portrait one.
    int h = size.height();
    if (pt.width() > 0.0)
        h = int(qRound(qreal(w) * pt.height() / pt.width()));
    h = qBound(1, h, 4096);

    return m_doc->render(page, QSize(w, h));
}

void PdfCanvas::trimCache()
{
    if (m_cacheBytes <= m_cacheBudget)
        return;

    const qreal top = verticalScrollBar()->value();
    const qreal bot = top + viewport()->height();
    QSet<int> visible;
    for (int i = 0; i < m_geom.size(); ++i) {
        const PageGeom &g = m_geom.at(i);
        if (g.y + g.h < top)
            continue;
        if (g.y > bot)
            break;
        visible.insert(i);
    }

    const qint64 before = m_cacheBytes;
    for (int i = 0; i < m_lru.size() && m_cacheBytes > m_cacheBudget;) {
        const int page = m_lru.at(i);
        if (visible.contains(page)) {   // never evict a visible page
            ++i;
            continue;
        }
        m_cacheBytes -= m_cache.value(page).sizeInBytes();
        m_cache.remove(page);
        m_lru.removeAt(i);
    }
    if (m_cacheBytes < before)
        AppLog::write(QStringLiteral("cache"),
                      QStringLiteral("淘汰 %1 MB，缓存 %2 MB / 上限 %3 MB")
                          .arg(double(before - m_cacheBytes) / 1048576.0, 0, 'f', 1)
                          .arg(double(m_cacheBytes) / 1048576.0, 0, 'f', 1)
                          .arg(double(m_cacheBudget) / 1048576.0, 0, 'f', 0));
}

void PdfCanvas::drawInk(QPainter &p, int page, const QRectF &rect)
{
    const auto it = m_ink.constFind(page);
    const bool live = (m_drawing && m_drawPage == page);

    auto drawStroke = [&p, &rect](const Stroke &s) {
        if (s.pts.isEmpty())
            return;
        const auto device = [&rect](const QPointF &n) {
            return QPointF(rect.left() + n.x() * rect.width(),
                           rect.top() + n.y() * rect.height());
        };
        // A lone sample is a tap, and a tap is a dot: a round-capped pen over zero length
        // paints exactly one. Taps used to disappear because this function demanded two
        // points and endInput() discarded the stroke - see the commit rule there.
        if (s.pts.size() == 1) {
            p.setPen(QPen(s.color, qMax<qreal>(0.5, s.width * rect.width()),
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPoint(device(s.pts.first()));
            return;
        }
        // Smooth through the samples instead of joining them with straight
        // segments: sparse input (a fast pen stroke, touch coalescing) otherwise
        // turns into visible corners, and zooming in makes every corner larger.
        // This is the standard quadratic scheme - the curve runs through the
        // midpoints and each sample acts as its control point.
        QPainterPath path;
        path.moveTo(device(s.pts.first()));
        if (s.pts.size() == 2) {
            path.lineTo(device(s.pts.last()));
        } else {
            for (int i = 1; i < s.pts.size() - 1; ++i) {
                const QPointF mid = (s.pts.at(i) + s.pts.at(i + 1)) * 0.5;
                path.quadTo(device(s.pts.at(i)), device(mid));
            }
            path.lineTo(device(s.pts.last()));
        }
        // Stroke width is stored relative to the page width -> scale to device px
        p.setPen(QPen(s.color, qMax<qreal>(0.5, s.width * rect.width()),
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };

    if (it != m_ink.constEnd()) {
        for (const Stroke &s : it.value())
            drawStroke(s);
    }
    if (live)
        drawStroke(m_current);
}

// The canvas is a calm light neutral ("desk"): the pages are the only white in
// the picture, so they read as sheets of paper the moment a document is open.
void PdfCanvas::paintBackdrop(QPainter &p) const
{
    const Theme::Palette &pal = Theme::light();
    {   // TEMP DIAG: what the painter actually sees, once per process.
        static bool once = false;
        if (!once) {
            once = true;
            AppLog::write(QStringLiteral("theme"),
                          QStringLiteral("paint: 解析 %1 pal=%2 desk=%3,%4,%5")
                              .arg(int(Theme::resolvedMode()))
                              .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(&pal)))
                              .arg(pal.desk.red()).arg(pal.desk.green()).arg(pal.desk.blue()));
        }
    }
    const QRect area = viewport()->rect();
    p.fillRect(area, pal.desk);

    // A barely-there shade along the top edge: the plane reads as a surface
    // (light coming from above) rather than as flat paint, at the cost of one
    // thin gradient strip.
    const qreal strip = qMax<qreal>(12.0, m_ui.icon * 1.4);
    QLinearGradient shade(0.0, 0.0, 0.0, strip);
    shade.setColorAt(0.0, pal.deskShade);
    shade.setColorAt(1.0, pal.desk);
    p.fillRect(QRectF(area.left(), area.top(), area.width(), strip), shade);
}

// Soft page lift: a few stacked translucent rounded rects instead of a real
// blur, which keeps the scroll repaint cheap even on a 4K panel.
void PdfCanvas::paintPageShadow(QPainter &p, const QRectF &page, qreal radius) const
{
    const Theme::Palette &pal = Theme::light();
    const Theme::PageShadow spec;
    p.setPen(Qt::NoPen);
    for (int i = 0; i < Theme::PageShadow::layers; ++i) {
        QColor c = pal.pageShadow;
        c.setAlpha(spec.alpha[i]);
        p.setBrush(c);
        const qreal grow = spec.grow[i];
        p.drawRoundedRect(page.adjusted(-grow, -grow + spec.dy[i], grow, grow + spec.dy[i]),
                          radius + grow, radius + grow);
    }
    p.setBrush(Qt::NoBrush);
}

// The invitation shown while no document is open.
void PdfCanvas::paintEmptyState(QPainter &p) const
{
    const Theme::Palette &pal = Theme::light();
    const QRect area = viewport()->rect();

    const QFont titleFont = Theme::titleFont(font());
    const QFont bodyFont = Theme::bodyFont(font());
    const QFontMetricsF titleFm(titleFont);
    const QFontMetricsF bodyFm(bodyFont);

    const qreal gap = Theme::Space4;
    const qreal card = m_ui.emptyCard;
    const qreal blockH = card + gap + titleFm.height() + Theme::Space2 + bodyFm.height();
    qreal y = qMax<qreal>(Theme::Space6, (area.height() - blockH) / 2.0);

    const QRectF cardRect((area.width() - card) / 2.0, y, card, card);
    QPainterPath cardPath;
    cardPath.addRoundedRect(cardRect, card * 0.28, card * 0.28);
    QColor wash = pal.accent;
    wash.setAlpha(0x14);
    p.fillPath(cardPath, wash);
    QColor edge = pal.accent;
    edge.setAlpha(0x33);
    p.setPen(QPen(edge, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(cardPath);

    // A page with a hand drawn stroke across it: PDF + 批注.
    const QRectF glyph(cardRect.center().x() - m_ui.emptyGlyph / 2.0,
                       cardRect.center().y() - m_ui.emptyGlyph / 2.0,
                       m_ui.emptyGlyph, m_ui.emptyGlyph);
    QColor docInk = pal.text;
    docInk.setAlpha(0xD0);
    IconPainter::paintGlyph(p, IconPainter::Glyph::Document, glyph, docInk, 1.9);
    const QRectF swash(glyph.left() - glyph.width() * 0.10,
                       glyph.top() + glyph.height() * 0.60,
                       glyph.width() * 1.20, glyph.height() * 0.36);
    IconPainter::paintGlyph(p, IconPainter::Glyph::Swash, swash, pal.accent, 1.7);

    y = cardRect.bottom() + gap;
    p.setFont(titleFont);
    p.setPen(pal.text);
    p.drawText(QRectF(area.left(), y, area.width(), titleFm.height()),
               Qt::AlignHCenter | Qt::AlignVCenter, QStringLiteral("落墨·大屏批注"));

    y += titleFm.height() + Theme::Space2;
    p.setFont(bodyFont);
    p.setPen(pal.textMuted);
    p.drawText(QRectF(area.left(), y, area.width(), bodyFm.height()),
               Qt::AlignHCenter | Qt::AlignVCenter,
               QStringLiteral("点击“打开”，或把 PDF / .dpz 批注包拖进来"));
}

void PdfCanvas::trackPointer()
{
    // Gestures and touch/stylus input own the pointer themselves. This is not cosmetic:
    // Windows also synthesises mouse events for touch WITH the button pressed, so a
    // mouseButtons() check alone let the global cursor drag an eraser/pen stroke over to
    // wherever the physical mouse happened to sit (the "dragging sometimes damages the
    // wrong strokes" report). And during a pinch the poll only steals frames from the
    // gesture. The touch lock is set the moment a touch or stylus sequence starts, which
    // makes it the right gate for both.
    if (m_touchInkBlocked || m_pinchActive || m_moveDragActive)
        return;
    const bool dragging = m_erasing || m_drawing;
    if (dragging && QGuiApplication::mouseButtons() == Qt::NoButton)
        return;
    if (!dragging && m_tool != InkTool::Eraser) {
        if (m_eraserHover)
            clearEraserHover();
        return;
    }

    const QPointF pos = viewport()->mapFromGlobal(QCursor::pos());
    const bool changed = !m_hasTrackedPos || pos != m_lastTrackedPos;
    m_lastTrackedPos = pos;
    m_hasTrackedPos = true;

    if (dragging) {
        // Keep sampling even where an overlay swallowed the mouse events, so the page
        // behind the island is still erased instead of the stroke dying at its edge.
        if (changed && viewport()->rect().contains(pos.toPoint()))
            moveInputTo(pos);
        return;
    }
    if (viewport()->rect().contains(pos.toPoint()))
        setEraserHover(pos);
    else if (m_eraserHover)
        clearEraserHover();
}

// --- eraser indicator (screen only) ---------------------------------------------
// The ring's diameter IS the wipe diameter, so the drawing doubles as the size readout;
// the glyph in the middle says which tool it is. Both are painted from paintEvent and
// nowhere else, and the PDF/PNG export paths rasterise through pageThumbnail, so the
// indicator cannot leak into a saved file.

bool PdfCanvas::eraserIndicatorVisible() const
{
    // Any erase shows the ring - including a stylus used with its eraser end, where the toolbar
    // still says "pen". The ring is the size readout, and it is needed most in that case.
    return (m_tool == InkTool::Eraser || m_erasing) && !m_pinchActive && !m_touchInkBlocked
           && !m_moveDragActive && (m_erasing || m_eraserHover);
}

QRect PdfCanvas::eraserIndicatorBounds(const QPointF &viewportPos, qreal radiusPx) const
{
    const qreal r = radiusPx + 4.0;          // ring + halo + anti-aliasing bleed
    return QRectF(viewportPos.x() - r, viewportPos.y() - r, 2.0 * r, 2.0 * r)
        .toAlignedRect();
}

void PdfCanvas::updateEraserIndicator(const QPointF &from, bool hadFrom,
                                      const QPointF &to, bool hasTo)
{
    QRect dirty;
    if (hadFrom)
        dirty = eraserIndicatorBounds(from, kEraserRadiusPx);
    if (hasTo) {
        const QRect now = eraserIndicatorBounds(to, kEraserRadiusPx);
        dirty = dirty.isNull() ? now : dirty.united(now);
    }
    if (!dirty.isNull())
        viewport()->update(dirty);
}

void PdfCanvas::drawEraserIndicator(QPainter &p, const QPointF &viewportPos, qreal radiusPx)
{
    // A single stroke colour cannot stay visible on the white paper AND on a dark desk,
    // so pick by what the indicator actually sits on.
    const bool onPaper = pageAtViewportPos(viewportPos) >= 0;
    const QColor desk = Theme::light().desk;
    const bool darkDesk = (0.299 * desk.redF() + 0.587 * desk.greenF()
                           + 0.114 * desk.blueF()) < 0.5;
    const bool darkBg = !onPaper && darkDesk;
    const QColor ringLine = darkBg ? QColor(0xFF, 0xFF, 0xFF, 0xE6)
                                   : QColor(0x1F, 0x24, 0x2B, 0xE0);
    const QColor glyphColor = darkBg ? QColor(0xFF, 0xFF, 0xFF, 0xF0)
                                     : Theme::light().accent;
    const QRectF ring(viewportPos.x() - radiusPx, viewportPos.y() - radiusPx,
                      radiusPx * 2.0, radiusPx * 2.0);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    // The halo is always drawn. On blank paper it is invisible, but the moment the ring
    // crosses ink, a photo or the dark desk it is what keeps the outline readable - a
    // first render with a bare 1.6 px line disappeared inside dark page content.
    p.setPen(QPen(QColor(255, 255, 255, darkBg ? 170 : 120), 3.4));
    p.drawEllipse(ring);
    p.setPen(QPen(ringLine, 2.0));
    p.drawEllipse(ring);

    if (m_erasing) {
        // Accent wash over the disc being removed: the page stays readable while cutting.
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::light().accentSoft);
        p.drawEllipse(ring);
        p.setBrush(Qt::NoBrush);
    }

    // The glyph uses the accent colour - the app's "active" colour - rather than the ring's
    // neutral, so the two shapes never merge at small radii. It is capped so it stays a
    // small recognisable icon instead of growing into a huge eraser; while cutting it
    // presses down by one pixel.
    const qreal side = qBound<qreal>(10.0, radiusPx * 1.05, 20.0);
    const qreal press = m_erasing ? 1.0 : 0.0;
    const QRectF box(viewportPos.x() - side / 2.0,
                     viewportPos.y() - side / 2.0 + press, side, side);
    IconPainter::paintGlyph(p, IconPainter::Glyph::Eraser, box, glyphColor, 2.0);
    p.restore();
}

void PdfCanvas::applyToolCursor()
{
    // Over an overlay child (island, page picker, pen palette) the indicator is hidden
    // behind it anyway, so the normal pointer must come back - otherwise the blank cursor
    // would make those buttons impossible to aim at.
    const QPointF pos = viewport()->mapFromGlobal(QCursor::pos());
    const bool overChild = viewport()->childAt(pos.toPoint()) != nullptr;
    if (!overChild && m_tool == InkTool::Eraser && eraserIndicatorVisible()) {
        // The indicator IS the pointer; two pointers on screen is one too many.
        viewport()->setCursor(Qt::BlankCursor);
        return;
    }
    viewport()->setCursor(m_tool == InkTool::Eraser ? Qt::PointingHandCursor
                         : m_tool == InkTool::Move   ? Qt::SizeAllCursor
                                                     : Qt::CrossCursor);
}

void PdfCanvas::setEraserHover(const QPointF &viewportPos)
{
    const bool had = m_eraserHover;
    const QPointF from = m_eraserHoverPos;
    m_eraserHover = true;
    m_eraserHoverPos = viewportPos;
    applyToolCursor();
    updateEraserIndicator(from, had, viewportPos, true);
}

void PdfCanvas::clearEraserHover()
{
    const bool had = m_eraserHover;
    const QPointF from = m_eraserHoverPos;
    m_eraserHover = false;
    applyToolCursor();
    updateEraserIndicator(from, had, from, false);
}

void PdfCanvas::testSetEraserHover(const QPointF &viewportPos) { setEraserHover(viewportPos); }
void PdfCanvas::testClearEraserHover() { clearEraserHover(); }
void PdfCanvas::testTrackPointer() { trackPointer(); }
void PdfCanvas::testPinchBegin(const QPointF &a, const QPointF &b) { startPinch(a, b); }
void PdfCanvas::testPinchFrame(const QPointF &a1, const QPointF &b1) { applyPinchFrame(a1, b1); }
void PdfCanvas::testPinchEnd() { endPinch(); }
void PdfCanvas::testEraserTailStroke(const QPointF &from, const QPointF &to)
{
    beginInputAt(from, /*asEraser=*/true);
    moveInputTo(to);
    endInput();
}
QPointF PdfCanvas::testEraserHoverPos() const { return m_eraserHoverPos; }

QImage PdfCanvas::testRenderEraserIndicator(const QPointF &viewportPos, qreal radiusPx,
                                            const QSize &imageSize)
{
    // Same composition as paintEvent, on an offscreen image: page raster first, then the
    // indicator. A test can measure the ring in it; a human can eyeball the art.
    QImage img(imageSize, QImage::Format_RGB32);
    img.fill(Theme::light().desk);
    {
        QPainter p(&img);
        const QImage page = pageThumbnail(0, imageSize);
        if (!page.isNull())
            p.drawImage(QPoint(0, 0), page);
        drawEraserIndicator(p, viewportPos, radiusPx);
    }
    return img;
}

// --- paint re-entrancy guard -----------------------------------------------------
// The crash we chased was a stack overflow (0xC00000FD) while zooming: ~2200 nested
// frames, every one of them re-entering this function through Qt6Widgets/Qt6Gui. The
// trigger was a synchronous repaint started from INSIDE a paint event (a status-bar
// update made Qt flush the dirty region straight away), and Qt keeps painting while
// dirty regions exist - so the recursion ends only when the stack does.
//
// Painting is idempotent, so refusing to nest costs nothing: whoever asked for the
// inner paint gets it from the event loop as soon as this one returns. That makes
// the whole class of crash impossible no matter what the trigger is.
static int s_paintDepth = 0;
static int s_paintMaxDepth = 0;

namespace {
struct PaintDepthGuard {
    PaintDepthGuard()
    {
        if (++s_paintDepth > s_paintMaxDepth)
            s_paintMaxDepth = s_paintDepth;
    }
    ~PaintDepthGuard() { --s_paintDepth; }
};
}   // namespace

int PdfCanvas::testMaxPaintDepth() const { return s_paintMaxDepth; }
void PdfCanvas::testResetMaxPaintDepth() { s_paintMaxDepth = s_paintDepth; }

void PdfCanvas::paintEvent(QPaintEvent *)
{
    if (s_paintDepth > 0) {
        // Already painting: this is exactly the recursion the guard is for.
        CrashLog::breadcrumb(
            "paint-nested",
            QStringLiteral("绘制嵌套第 %1 层，已截断").arg(s_paintDepth + 1));
        return;
    }
    PaintDepthGuard paintGuard;

    InputProbe &probe = InputProbe::instance();
    probe.addPaint();
    probe.setZoom(m_zoom);
    if (AppLog::isEnabled()) {
        // Costs nothing while the opt-in log is off, which is the default.
        const QString line = probe.take(QDateTime::currentMSecsSinceEpoch());
        if (!line.isEmpty())
            AppLog::write(QStringLiteral("probe"), line);
    }

    QPainter p(viewport());
    paintBackdrop(p);

    if (m_geom.isEmpty()) {
        paintEmptyState(p);
        return;
    }

    const qreal top = verticalScrollBar()->value();
    const qreal bot = top + viewport()->height();

    // Visible pages first: every shadow is laid down before any page is drawn,
    // so a page can never cover the lift of the one above it.
    QVector<int> visible;
    for (int i = 0; i < m_geom.size(); ++i) {
        const PageGeom &g = m_geom.at(i);
        if (g.y + g.h < top)
            continue;
        if (g.y > bot)
            break;
        visible.append(i);
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    for (int page : visible)
        paintPageShadow(p, pageRectInViewport(page), m_pageRadius);

    for (int page : visible) {
        const QRectF r = pageRectInViewport(page);

        QPainterPath paper;
        paper.addRoundedRect(r, m_pageRadius, m_pageRadius);

        // Paper-white under the page: QPdfDocument::render() returns an image
        // with alpha and PDF pages have no background of their own.
        p.fillPath(paper, Theme::light().paper);

        p.save();
        p.setClipPath(paper);
        // Bilinear scaling of a page-sized bitmap that does not match the current
        // zoom yet is the most expensive thing in a pinch frame. During the
        // gesture those bitmaps are knowingly stale previews anyway, so drop the
        // smoothing (nearest neighbour) and re-render crisply once the gesture
        // settles.
        p.setRenderHint(QPainter::SmoothPixmapTransform, !m_pinchActive);
        const QImage img = imageFor(page);
        if (!img.isNull())
            p.drawImage(r, img);
        p.restore();

        // Ink stays on top of the page, exactly where it was drawn.
        p.setRenderHint(QPainter::Antialiasing, true);
        drawInk(p, page, r);

        // Page edge hairline, so the white paper keeps an edge in the gutter.
        p.setPen(QPen(Theme::light().pageEdge, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(paper);
    }

    // Screen-only eraser indicator, drawn last so it floats above every page and edge -
    // and drawn only here, which is why it can never reach an exported PDF or PNG.
    if (eraserIndicatorVisible())
        drawEraserIndicator(p, m_eraserHoverPos, kEraserRadiusPx);
}

QPointF PdfCanvas::toNormalized(int page, const QPointF &viewportPos) const
{
    const QRectF r = pageRectInViewport(page);
    if (r.isEmpty())
        return {};
    qreal nx = (viewportPos.x() - r.left()) / r.width();
    qreal ny = (viewportPos.y() - r.top())  / r.height();
    return QPointF(qBound(0.0, nx, 1.0), qBound(0.0, ny, 1.0));
}

void PdfCanvas::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    invalidateRenders();   // fit-width scale changed -> cached bitmaps are stale
    relayout();
    if (m_toolbar)
        m_toolbar->reposition();
    viewport()->update();
}

// Pages are always painted from scratch, so there is no bitmap to blit: a
// plain update is both correct and keeps the floating toolbar anchored to the
// viewport instead of scrolling away with the content.
void PdfCanvas::scrollContentsBy(int dx, int dy)
{
    Q_UNUSED(dx)
    Q_UNUSED(dy)
    if (m_toolbar)
        m_toolbar->reposition();
    viewport()->update();
}

// Page under the given viewport position, or -1 when the pointer is not on a
// page (gutter, inter-page gap, blank area below the last page).
int PdfCanvas::pageAtViewportPos(const QPointF &viewportPos) const
{
    if (m_geom.isEmpty())
        return -1;
    const qreal cy = viewportPos.y() + verticalScrollBar()->value();
    const int page = pageAtContentY(cy);
    if (page < 0)
        return -1;
    if (!pageRectInViewport(page).contains(viewportPos))
        return -1;
    return page;
}

void PdfCanvas::cancelGesture()
{
    m_drawing     = false;
    m_erasing     = false;
    m_erasePushed = false;
    m_hasLastErasePos = false;
    m_moveDragActive  = false;
    m_drawPage    = -1;
    m_current     = Stroke{};
}

// --- Test hooks (only used by `--selftest-ink`) ---------------------------

void PdfCanvas::testAddStroke(int page, const QPointF &aNorm, const QPointF &bNorm,
                              const QColor &color, qreal width, int steps, bool densify)
{
    Stroke s;
    s.color = color;
    {
        // test callers pass a device-px width; store it normalized like the
        // real input path does.
        const QRectF pr = pageRectInViewport(page);
        s.width = (pr.width() > 0.0) ? width / pr.width() : kInkDefaultWidthNorm;
    }
    steps = qMax(1, steps);
    for (int i = 0; i <= steps; ++i) {
        const QPointF p = aNorm + (bNorm - aNorm) * (qreal(i) / qreal(steps));
        if (densify)
            appendDensified(s.pts, p, kInkMaxStepNorm);   // like the real input path
        else
            s.pts.append(p);                              // raw/sparse sampling
    }
    pushUndoSnapshot();
    m_ink[page].append(s);
    viewport()->update();
    emit inkChanged(strokeCount());
}

int PdfCanvas::testStrokePoints(int page, int index) const
{
    const auto it = m_ink.constFind(page);
    if (it == m_ink.constEnd() || index < 0 || index >= it.value().size())
        return -1;
    return it.value().at(index).pts.size();
}

qreal PdfCanvas::testStrokeDeviceWidth(int page, int index) const
{
    const auto it = m_ink.constFind(page);
    if (it == m_ink.constEnd() || index < 0 || index >= it.value().size())
        return -1.0;
    return it.value().at(index).width * pageRectInViewport(page).width();
}

bool PdfCanvas::testEraseAtNormalized(int page, const QPointF &norm)
{
    const QRectF r = pageRectInViewport(page);
    if (r.isEmpty())
        return false;
    const QPointF vp(r.left() + norm.x() * r.width(),
                     r.top()  + norm.y() * r.height());
    return eraseAtPointer(page, vp);
}

bool PdfCanvas::testEraseSweepNormalized(int page, const QPointF &aNorm, const QPointF &bNorm)
{
    const QRectF r = pageRectInViewport(page);
    if (r.isEmpty())
        return false;
    auto vp = [&r](const QPointF &n) {
        return QPointF(r.left() + n.x() * r.width(), r.top() + n.y() * r.height());
    };
    return eraseSweep(page, vp(aNorm), vp(bNorm));
}

QString PdfCanvas::testStrokeSummary(int page) const
{
    const auto it = m_ink.constFind(page);
    if (it == m_ink.constEnd())
        return QStringLiteral("(none)");
    QString s = QStringLiteral("n=%1").arg(it.value().size());
    for (const Stroke &st : it.value()) {
        if (st.pts.isEmpty()) {
            s += QStringLiteral(" [empty]");
            continue;
        }
        qreal x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
        for (const QPointF &p : st.pts) {
            x0 = qMin(x0, p.x()); x1 = qMax(x1, p.x());
            y0 = qMin(y0, p.y()); y1 = qMax(y1, p.y());
        }
        s += QStringLiteral(" [x %1..%2 y %3..%4 pts=%5]")
                 .arg(x0, 0, 'f', 2).arg(x1, 0, 'f', 2)
                 .arg(y0, 0, 'f', 2).arg(y1, 0, 'f', 2)
                 .arg(st.pts.size());
    }
    return s;
}

void PdfCanvas::testFreeMoveDrag(const QPointF &from, const QPointF &to)
{
    // Call the real handlers with a synthetic left-button sequence, so the
    // test drives the exact press / move / release path a user drag uses.
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    mousePressEvent(&press);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    mouseMoveEvent(&move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    mouseReleaseEvent(&release);
}

void PdfCanvas::setTool(InkTool tool)
{
    if (m_tool == tool)
        return;
    m_tool = tool;
    cancelGesture();
    // Tool changes reset the indicator: the cursor logic lives in applyToolCursor() so the
    // "blank while the indicator shows" rule cannot drift out of sync.
    m_eraserHover = false;
    applyToolCursor();
    viewport()->update();
    emit toolChanged();
}

void PdfCanvas::setPenColor(const QColor &color)
{
    if (!color.isValid() || m_penColor == color)
        return;
    m_penColor = color;
    emit penChanged();
}

void PdfCanvas::setPenWidth(qreal width)
{
    const qreal w = qBound(1.0, width, 24.0);
    if (m_penWidth == w)
        return;
    m_penWidth = w;
    emit penChanged();
}

void PdfCanvas::pushUndoSnapshot()
{
    m_undoStack.append(m_ink);
    while (m_undoStack.size() > kMaxUndoSnapshots)
        m_undoStack.removeFirst();
    m_redoStack.clear();
    emit undoStateChanged();
}

void PdfCanvas::undo()
{
    if (m_undoStack.isEmpty())
        return;
    m_redoStack.append(m_ink);
    m_ink = m_undoStack.takeLast();
    cancelGesture();
    viewport()->update();
    emit inkChanged(strokeCount());
    emit undoStateChanged();
}

void PdfCanvas::redo()
{
    if (m_redoStack.isEmpty())
        return;
    m_undoStack.append(m_ink);
    m_ink = m_redoStack.takeLast();
    cancelGesture();
    viewport()->update();
    emit inkChanged(strokeCount());
    emit undoStateChanged();
}

// Sweeps the eraser from `from` to `to`. Pointer samples arrive at discrete
// positions, so a fast drag can jump right over a stroke; interpolating the
// path guarantees the whole trajectory is erased (no dashed/ missed gaps).
bool PdfCanvas::eraseSweep(int pageHint, const QPointF &from, const QPointF &to)
{
    const QPointF d = to - from;
    const qreal dist = qSqrt(QPointF::dotProduct(d, d));

    // Jump guard: if the pointer jumped (dropped/coalesced events), sweep only
    // a bounded window ending at the pointer instead of the whole span. The
    // eraser still follows the pointer continuously, but a single flick can no
    // longer wipe out an unbounded piece of the stroke.
    QPointF a = from;
    if (dist > kEraserMaxJumpPx) {
        const QPointF dir = d / dist;
        a = to - dir * kEraserMaxJumpPx;
    }
    eraseLog(QStringLiteral("sweep (%1,%2)->(%3,%4) dist=%5 %6")
                 .arg(from.x(), 0, 'f', 1).arg(from.y(), 0, 'f', 1)
                 .arg(to.x(), 0, 'f', 1).arg(to.y(), 0, 'f', 1)
                 .arg(dist, 0, 'f', 1)
                 .arg(dist > kEraserMaxJumpPx ? QStringLiteral("JUMP")
                                              : QStringLiteral("ok")));

    bool any = false;
    const QPointF dd = to - a;
    const qreal len = qSqrt(QPointF::dotProduct(dd, dd));
    const qreal step = qMax<qreal>(2.0, kEraserRadiusPx * 0.5);
    const int steps = qMax(1, int(qCeil(len / step)));
    for (int i = 0; i <= steps; ++i) {
        const QPointF p = a + dd * (qreal(i) / qreal(steps));
        int page = pageAtViewportPos(p);
        if (page < 0)
            page = pageHint;                 // keep erasing the gesture's page
        if (page >= 0 && eraseAtPointer(page, p))
            any = true;
    }
    return any;
}

// Partial ("erase exactly where you point") eraser, evaluated in page device
// pixels. Each stroke is cut where it crosses the eraser circle: the part
// inside is removed and the outer parts survive as separate strokes, so one
// stroke can end up split in two.
bool PdfCanvas::eraseAtPointer(int page, const QPointF &viewportPos)
{
    const auto it = m_ink.constFind(page);
    if (it == m_ink.constEnd())
        return false;
    const QRectF rect = pageRectInViewport(page);
    if (rect.isEmpty() || rect.width() <= 0.0 || rect.height() <= 0.0)
        return false;

    auto toDevice = [&rect](const QPointF &n) {
        return QPointF(rect.left() + n.x() * rect.width(),
                       rect.top() + n.y() * rect.height());
    };
    auto toNorm = [&rect](const QPointF &d) {
        return QPointF((d.x() - rect.left()) / rect.width(),
                       (d.y() - rect.top()) / rect.height());
    };

    const QPointF center = viewportPos;
    // Copy by value: the hash is implicitly shared, and the snapshot taken
    // below shares this storage until we detach. Holding a reference/iterator
    // across the snapshot would let the write below mutate the snapshot too.
    const QVector<Stroke> strokes = it.value();

    QVector<Stroke> rebuilt;
    rebuilt.reserve(strokes.size() + 2);
    bool changed = false;

    for (const Stroke &s : strokes) {
        const qreal R = kEraserRadiusPx + s.width * rect.width() / 2.0;
        const qreal R2 = R * R;
        auto inside = [&center, R2](const QPointF &p) {
            const QPointF d = p - center;
            return QPointF::dotProduct(d, d) <= R2;
        };

        if (s.pts.isEmpty()) {
            rebuilt.append(s);
            continue;
        }
        if (s.pts.size() == 1) {
            if (inside(toDevice(s.pts.first()))) {
                changed = true;         // single-point dot: nothing left to keep
                continue;
            }
            rebuilt.append(s);
            continue;
        }

        QVector<QPointF> dev;
        dev.reserve(s.pts.size());
        for (const QPointF &n : s.pts)
            dev.append(toDevice(n));

        QVector<QVector<QPointF>> runs;
        QVector<QPointF> cur;
        auto flush = [&runs, &cur]() {
            if (cur.size() >= 2)
                runs.append(cur);
            cur.clear();
        };

        bool prevInside = inside(dev.first());
        if (!prevInside)
            cur.append(dev.first());

        for (int i = 0; i + 1 < dev.size(); ++i) {
            const QPointF a = dev.at(i);
            const QPointF b = dev.at(i + 1);
            const bool bInside = inside(b);
            qreal t0 = 0.0, t1 = 0.0;
            const bool crosses = circleSegmentInterval(a, b, center, R, &t0, &t1);

            if (crosses) {
                if (!prevInside && t0 > 0.0) {
                    if (cur.isEmpty())
                        cur.append(a);
                    cur.append(a + t0 * (b - a));   // last point before the cut
                }
                flush();
                if (!bInside && t1 < 1.0) {
                    cur.append(a + t1 * (b - a));   // first point after the cut
                    cur.append(b);                  // b is outside: keep the tail
                }
            } else if (!prevInside && !bInside) {
                cur.append(b);
            } else {
                flush();
            }
            prevInside = bInside;
        }
        flush();

        if (runs.size() == 1 && runs.first().size() == dev.size()) {
            rebuilt.append(s);          // untouched by this erase
            continue;
        }

        changed = true;
        for (const QVector<QPointF> &run : runs) {
            Stroke ns = s;
            ns.pts.clear();
            ns.pts.reserve(run.size());
            for (const QPointF &d : run)
                ns.pts.append(toNorm(d));
            rebuilt.append(ns);
        }
    }

    if (!changed)
        return false;

    // Snapshot BEFORE mutating; one snapshot per drag gesture so a whole erase
    // drag undoes in one step.
    if (!m_erasePushed) {
        pushUndoSnapshot();
        m_erasePushed = true;
    }

    // Re-index instead of writing through the stale iterator: operator[]
    // detaches the shared hash, so the snapshot keeps the pre-erase state.
    eraseLog(QStringLiteral("cut page=%1 strokes %2 -> %3")
                 .arg(page).arg(strokes.size()).arg(rebuilt.size()));
    m_ink[page] = rebuilt;
    if (m_ink.value(page).isEmpty())
        m_ink.remove(page);

    // Only the eraser circle changed, so only that needs repainting - the rest
    // of the page is untouched. A full update here ran for every sweep step (a
    // 40 px flick is ~7 steps), which is a lot of wasted 4K repainting.
    // The eraser circle changed, and the indicator following the pointer is a little
    // larger than that circle, so repaint the indicator's bounds (which contain it).
    // Keeping the hover position in sync here also makes a touch-driven erase show the
    // ring at the right place.
    m_eraserHoverPos = viewportPos;
    viewport()->update(eraserIndicatorBounds(viewportPos, kEraserRadiusPx));
    emit inkChanged(strokeCount());
    return true;
}

// --- shared input entry points --------------------------------------------
// Mouse and touch both funnel through these, so pen / eraser / pinch behave
// identically whichever input produced them.

void PdfCanvas::beginInputAt(const QPointF &viewportPos, bool asEraser)
{
    // `asEraser` comes from hardware that names its own eraser (a stylus turned around): this
    // stroke erases and the toolbar keeps whatever tool it had.
    if (m_tool == InkTool::Move && !asEraser)    // free move never draws ink
        return;

    // Writing wins over sharpening: if the page bitmaps are still queued for a
    // crisp re-render (the 160 ms settle after a zoom), do not start it now - it
    // would rasterize every visible page while the teacher is writing. endInput()
    // restarts the timer when the pen comes up.
    if (m_zoomSettle)
        m_zoomSettle->stop();

    const int page = pageAtViewportPos(viewportPos);
    if (page < 0)
        return;

    if (m_tool == InkTool::Eraser || asEraser) {
        m_erasing = true;
        m_erasePushed = false;
        m_lastErasePos = viewportPos;
        m_hasLastErasePos = true;
        eraseAtPointer(page, viewportPos);     // never starts an ink stroke
        return;
    }

    m_drawing  = true;
    m_drawPage = page;
    m_current  = Stroke{};
    m_current.color = m_penColor;
    {
        // m_penWidth is the on-screen thickness at the current zoom; store it
        // relative to the page so it scales with zoom from now on.
        const QRectF pr = pageRectInViewport(page);
        m_current.width = (pr.width() > 0.0) ? m_penWidth / pr.width()
                                             : kInkDefaultWidthNorm;
    }
    const int first = m_current.pts.size();
    appendDensified(m_current.pts, toNormalized(page, viewportPos), kInkMaxStepNorm);
    if (m_current.pts.size() == first)
        return;
    // Same partial repaint as the rest of the stroke: a full-viewport update here
    // would repaint the whole 4K page just to show the first dot.
    const QRectF pr = pageRectInViewport(page);
    const qreal inkPx = qMax<qreal>(1.0, m_current.width * pr.width());
    viewport()->update(inkDirtyRect(pr, m_current.pts.constData(), m_current.pts.size(),
                                    0, inkPx * 0.5 + 4.0));
}

void PdfCanvas::moveInputTo(const QPointF &viewportPos)
{
    if (m_erasing) {
        if (!m_hasLastErasePos) {
            m_lastErasePos = viewportPos;
            m_hasLastErasePos = true;
        }
        // Erase along the whole travelled path, not just at this sample.
        setEraserHover(viewportPos);
        eraseSweep(pageAtViewportPos(viewportPos), m_lastErasePos, viewportPos);
        m_lastErasePos = viewportPos;
        return;
    }
    if (!m_drawing)
        return;
    // Repaint only the neighbourhood of the new samples: a full-viewport update
    // per sample is what made handwriting lag behind the finger (see InkDirty.h).
    const int first = m_current.pts.size();
    appendDensified(m_current.pts, toNormalized(m_drawPage, viewportPos),
                    kInkMaxStepNorm);
    if (m_current.pts.size() == first)
        return;
    const QRectF pr = pageRectInViewport(m_drawPage);
    const qreal inkPx = qMax<qreal>(1.0, m_current.width * pr.width());
    viewport()->update(inkDirtyRect(pr, m_current.pts.constData(), m_current.pts.size(),
                                    first - 2, inkPx * 0.5 + 4.0));
}

void PdfCanvas::endInput()
{
    // The pen is up: the page may be sharpened again (see beginInputAt). This is
    // also what replaces a coarse busy-time raster with a crisp one.
    if (m_zoomSettle)
        m_zoomSettle->start();

    // A two-finger gesture owns the input: never commit a stroke while the ink
    // lock is on (this is where synthesized mouse releases used to leak ink).
    if (m_touchInkBlocked) {
        cancelGesture();
        return;
    }
    if (m_erasing) {
        m_erasing = false;
        m_hasLastErasePos = false;
        if (m_erasePushed) {
            m_erasePushed = false;
            emit inkChanged(strokeCount());
            emit undoStateChanged();
        }
        viewport()->update();
        return;
    }
    if (!m_drawing)
        return;
    m_drawing = false;
    if (!m_current.pts.isEmpty() && m_drawPage >= 0) {
        // Discard taps/noise: a stroke shorter than ~3 device px is not what a
        // teacher drew on purpose, and it is exactly what a stray finger leaves
        // behind during a two-finger pinch.
        const QRectF r = pageRectInViewport(m_drawPage);
        qreal lenPx = 0.0;
        for (int i = 1; i < m_current.pts.size(); ++i) {
            const QPointF d = m_current.pts.at(i) - m_current.pts.at(i - 1);
            lenPx += qSqrt(QPointF::dotProduct(d, d)) * r.width();
        }
        if (lenPx < 3.0 && m_strokeFromTouch) {
            eraseLog(QStringLiteral("ink discarded (touch tap/noise, len=%1 px)")
                         .arg(lenPx, 0, 'f', 2));
            m_current = Stroke{};
            m_drawPage = -1;
            viewport()->update();
            return;
        }
        if (lenPx < 3.0) {
            // A mouse or stylus tap (or a jittery click) is a dot the teacher meant to
            // place. Only touch keeps the "too short = noise" rule - that filter exists
            // for a stray finger during a gesture, not for a deliberate tap.
            eraseLog(QStringLiteral("ink committed as a dot (len=%1 px, pts=%2)")
                         .arg(lenPx, 0, 'f', 2).arg(m_current.pts.size()));
        } else {
            eraseLog(QStringLiteral("ink committed (len=%1 px, pts=%2)")
                         .arg(lenPx, 0, 'f', 1).arg(m_current.pts.size()));
        }
        pushUndoSnapshot();                    // state before the new stroke
        m_ink[m_drawPage].append(m_current);
        emit inkChanged(strokeCount());
    }
    m_current = Stroke{};
    m_drawPage = -1;
    viewport()->update();
}

// Free-move mode: pans the view on both axes by the pointer delta (hand tool),
// never the toolbar island. Only the integer part of the delta is applied and
// the sub-pixel remainder stays in m_moveLastPos, so slow drags still
// accumulate movement.
bool PdfCanvas::freePanTo(const QPointF &viewportPos)
{
    if (!m_moveDragActive)
        return false;

    const QPoint delta = (viewportPos - m_moveLastPos).toPoint();
    if (!delta.isNull()) {
        m_moveLastPos += QPointF(delta);
        panBy(QPointF(delta));
    }
    return true;
}

void PdfCanvas::panBy(const QPointF &delta)
{
    verticalScrollBar()->setValue(int(verticalScrollBar()->value() - delta.y()));
    horizontalScrollBar()->setValue(int(horizontalScrollBar()->value() - delta.x()));
}

void PdfCanvas::mousePressEvent(QMouseEvent *e)
{
    m_strokeFromTouch = false;       // a real mouse: taps are dots (see endInput)
    if (m_touchInkBlocked) {          // a two-finger touch gesture owns the input
        eraseLog(QStringLiteral("mouse press ignored (touch lock)"));
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    if (e->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    if (m_tool == InkTool::Move) {
        // Free move: grab anywhere on the viewport (page or gutter, with or
        // without a document); the drag pans the view and never inks.
        m_moveDragActive = true;
        m_moveLastPos = e->position();
        e->accept();
        return;
    }
    if (!m_doc || pageAtViewportPos(e->position()) < 0) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    beginInputAt(e->position());
}

void PdfCanvas::mouseMoveEvent(QMouseEvent *e)
{
    if (m_touchInkBlocked) {
        QAbstractScrollArea::mouseMoveEvent(e);
        return;
    }
    if (m_tool == InkTool::Move) {
        if (!freePanTo(e->position()))
            QAbstractScrollArea::mouseMoveEvent(e);
        else
            e->accept();
        return;
    }
    if (m_tool == InkTool::Eraser && !m_drawing && !m_erasing) {
        // Hovering with the eraser: the indicator follows the pointer so the wipe size is
        // visible before touching anything. Only the two affected circles are repainted.
        setEraserHover(e->position());
        e->accept();
        return;
    }
    if (!m_drawing && !m_erasing) {
        QAbstractScrollArea::mouseMoveEvent(e);
        return;
    }
    moveInputTo(e->position());
}

// Windows turns a "press and hold" touch into a right-click, which Qt delivers as a
// context menu event: during touch writing that popped the system menu and interrupted
// the stroke. The canvas has nothing to offer in a context menu (undo / clear / page
// jump all live on the island), so the request is swallowed rather than propagated to
// the window - see also the QEvent::ContextMenu case in viewportEvent().
void PdfCanvas::contextMenuEvent(QContextMenuEvent *e)
{
    e->accept();
}

void PdfCanvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_touchInkBlocked) {
        eraseLog(QStringLiteral("mouse release ignored (touch lock)"));
        QAbstractScrollArea::mouseReleaseEvent(e);
        return;
    }
    if (m_tool == InkTool::Move && m_moveDragActive) {
        m_moveDragActive = false;
        e->accept();
        return;
    }
    if (!m_drawing && !m_erasing) {
        QAbstractScrollArea::mouseReleaseEvent(e);
        return;
    }
    endInput();
}

int PdfCanvas::strokeCount() const
{
    int n = 0;
    for (auto it = m_ink.constBegin(); it != m_ink.constEnd(); ++it)
        n += it.value().size();
    return n;
}

void PdfCanvas::clearInk()
{
    cancelGesture();
    if (!m_ink.isEmpty()) {
        pushUndoSnapshot();
        m_ink.clear();
    }
    viewport()->update();
    emit inkChanged(strokeCount());
}

void PdfCanvas::clearCurrentPage()
{
    cancelGesture();
    const int page = currentPage();
    if (!m_ink.contains(page))
        return;
    pushUndoSnapshot();
    m_ink.remove(page);
    viewport()->update();
    emit inkChanged(strokeCount());
}

// --- Annotation (de)serialization -----------------------------------------
// The model is tiny (normalized points), so a whole-model dump is cheap and
// keeps the on-disk format independent of any internal ordering.
QJsonObject PdfCanvas::exportInk() const
{
    QJsonObject pages;
    for (auto it = m_ink.constBegin(); it != m_ink.constEnd(); ++it) {
        const QVector<Stroke> &strokes = it.value();
        QJsonArray arr;
        for (const Stroke &s : strokes) {
            QJsonObject so;
            so.insert(QStringLiteral("c"), s.color.name(QColor::HexRgb));
            so.insert(QStringLiteral("w"), s.width);
            QJsonArray pts;
            for (const QPointF &p : s.pts) {
                pts.append(p.x());
                pts.append(p.y());
            }
            so.insert(QStringLiteral("p"), pts);
            arr.append(so);
        }
        pages.insert(QString::number(it.key()), arr);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("pages"), pages);
    return root;
}

void PdfCanvas::importInk(const QJsonObject &obj)
{
    cancelGesture();

    InkModel model;
    const QJsonObject pages = obj.value(QStringLiteral("pages")).toObject();
    for (auto it = pages.constBegin(); it != pages.constEnd(); ++it) {
        bool ok = false;
        const int page = it.key().toInt(&ok);
        if (!ok || page < 0)
            continue;

        const QJsonArray arr = it.value().toArray();
        QVector<Stroke> strokes;
        strokes.reserve(arr.size());
        for (const QJsonValue &value : arr) {
            const QJsonObject so = value.toObject();
            Stroke s;
            s.color = QColor(so.value(QStringLiteral("c")).toString());
            if (!s.color.isValid())
                s.color = QColor(Qt::red);
            s.width = so.value(QStringLiteral("w")).toDouble(0.003);
            const QJsonArray pts = so.value(QStringLiteral("p")).toArray();
            for (qsizetype i = 0; i + 1 < pts.size(); i += 2)
                s.pts.append(QPointF(pts.at(i).toDouble(), pts.at(i + 1).toDouble()));
            if (!s.pts.isEmpty())
                strokes.append(s);
        }
        if (!strokes.isEmpty())
            model.insert(page, strokes);
    }

    m_ink = model;
    m_undoStack.clear();
    m_redoStack.clear();
    viewport()->update();
    emit inkChanged(strokeCount());
    emit undoStateChanged();
}

QSizeF PdfCanvas::pagePointSize(int page) const
{
    return m_doc ? m_doc->pagePointSize(page) : QSizeF();
}

void PdfCanvas::goToPage(int index)
{
    if (m_geom.isEmpty())
        return;
    index = qBound(0, index, int(m_geom.size()) - 1);
    verticalScrollBar()->setValue(int(m_geom.at(index).y));
    emit pageChanged(index, pageCount());
}

void PdfCanvas::nextPage() { goToPage(currentPage() + 1); }
void PdfCanvas::prevPage() { goToPage(currentPage() - 1); }

void PdfCanvas::setFitWidth(bool on)
{
    m_fitWidth = on;
    if (!on)
        return;
    // Fit width == zoom 1.0; anchor on the viewport centre so the page does not
    // jump (which it did when we simply reset the zoom and relaid out).
    applyZoom(1.0, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0),
              /*immediate=*/true);
    m_fitWidth = true;
}

void PdfCanvas::applyZoom(qreal targetZoom, const QPointF &anchor, bool immediate)
{
    // Re-entry counter for the WHOLE call (a guard object, not a counter around the log
    // line: incrementing and decrementing around a single statement always reads 1).
    // A stack overflow means something is nesting, and this breadcrumb names it.
    static int zoomDepth = 0;
    struct DepthGuard {
        int &depth;
        explicit DepthGuard(int &d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } guard(zoomDepth);
    if (zoomDepth > 1)
        CrashLog::breadcrumb("zoom-reentry",
                             QStringLiteral("applyZoom 重入第 %1 层").arg(zoomDepth));

    if (!m_doc || m_geom.isEmpty())
        return;

    // Remember the page under the anchor plus the relative position inside it.
    const int anchorPage = pageAtContentY(verticalScrollBar()->value() + anchor.y());
    qreal fracY = 0.5;
    qreal fracX = 0.5;
    if (anchorPage >= 0 && anchorPage < m_geom.size()) {
        const QRectF r0 = pageRectInViewport(anchorPage);
        if (r0.height() > 0.0)
            fracY = qBound(0.0, (anchor.y() - r0.top()) / r0.height(), 1.0);
        if (r0.width() > 0.0)
            fracX = qBound(0.0, (anchor.x() - r0.left()) / r0.width(), 1.0);
    }

    const qreal target = qBound(0.25, targetZoom, 4.0);
    if (qFuzzyCompare(target, m_zoom) && !immediate)
        return;

    m_zoom = target;
    // Record where the last step landed: the crash report shows the zoom trail.
    CrashLog::breadcrumb("zoom", QString::number(m_zoom, 'f', 3));
    if (immediate) {
        invalidateRenders();
        relayout();
    } else {
        // During a pinch keep the existing bitmaps (drawn scaled) and re-render
        // crisply once the gesture settles - invalidating each touch event
        // caused stutter and memory churn.
        relayout();
        if (m_zoomSettle)
            m_zoomSettle->start();
    }

    if (anchorPage >= 0 && anchorPage < m_geom.size()) {
        const PageGeom &g = m_geom.at(anchorPage);
        verticalScrollBar()->setValue(int(g.y + fracY * g.h - anchor.y()));
        const qreal left0 = (m_contentW - g.w) / 2.0;   // page left at hScroll 0
        horizontalScrollBar()->setValue(int(left0 + fracX * g.w - anchor.x()));
    }
    viewport()->update();
    emit zoomChanged(m_zoom);
    emit pageChanged(currentPage(), pageCount());
}

void PdfCanvas::zoomAt(qreal factor, const QPointF &viewportAnchor)
{
    if (!m_doc || m_geom.isEmpty())
        return;
    applyZoom(m_zoom * factor, viewportAnchor, /*immediate=*/false);
}

// Absolute zoom from the status-bar control: keep whatever is in the middle of the
// viewport in the middle, so the page does not jump sideways while dragging.
void PdfCanvas::setZoomLevel(qreal zoom)
{
    if (!m_doc || m_geom.isEmpty())
        return;
    const QPointF centre(viewport()->width() / 2.0, viewport()->height() / 2.0);
    applyZoom(zoom, centre, /*immediate=*/false);
}

void PdfCanvas::zoomIn()
{
    applyZoom(m_zoom * 1.25,
              QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0), true);
}

void PdfCanvas::zoomOut()
{
    applyZoom(m_zoom / 1.25,
              QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0), true);
}

bool PdfCanvas::event(QEvent *e)
{
    // Moving the window to a display with a different scale factor invalidates
    // every cached page bitmap (they are rasterized at the device resolution).
    if (e->type() == QEvent::DevicePixelRatioChange) {
        invalidateRenders();
        relayout();
        viewport()->update();
        AppLog::write(QStringLiteral("view"),
                      QStringLiteral("缩放因子变化：DPR %1")
                          .arg(viewport()->devicePixelRatioF(), 0, 'f', 2));
    }
    // Pinch to zoom. On Windows a two-finger pinch arrives as a native gesture
    // (both touchscreen and precision touchpad), so it does not disturb the
    // single-point ink path.
    if (e->type() == QEvent::NativeGesture) {
        auto *g = static_cast<QNativeGestureEvent *>(e);
        switch (g->gestureType()) {
        case Qt::ZoomNativeGesture: {
            const qreal delta = g->value();                  // e.g. 0.10 = +10 %
            zoomAt(qBound(0.5, 1.0 + delta, 2.0), g->position());
            return true;
        }
        case Qt::BeginNativeGesture:
        case Qt::EndNativeGesture:
            return true;
        default:
            break;
        }
    }
    return QAbstractScrollArea::event(e);
}

bool PdfCanvas::viewportEvent(QEvent *e)
{
    switch (e->type()) {
    case QEvent::ContextMenu:
        // "Press and hold" on a touch panel arrives as a context menu request; during
        // writing that used to break the stroke. See contextMenuEvent().
        e->accept();
        return true;
    case QEvent::Leave:
        // The pointer left the canvas: drop the indicator (and give the cursor back).
        clearEraserHover();
        break;
    case QEvent::Wheel:
        if (handleWheel(static_cast<QWheelEvent *>(e)))
            return true;
        break;
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        if (handleTablet(static_cast<QTabletEvent *>(e)))
            return true;
        break;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        if (handleTouch(static_cast<QTouchEvent *>(e)))
            return true;
        break;
    default:
        break;
    }
    return QAbstractScrollArea::viewportEvent(e);
}

// Mouse-wheel input.
//
// Ctrl+wheel is the ONLY safe zoom trigger. Windows/Qt turn a two-finger PAN - on
// a classroom touch panel just as on a touchpad - into ordinary scroll messages
// (MS: pan gestures become WM_VSCROLL/WM_HSCROLL; Qt: "Pan gestures are converted
// to mouse wheel messages"), and there is no way to tell those apart from a real
// wheel. Zooming on the plain wheel would therefore fire on every two-finger pan.
// The plain wheel keeps scrolling the document, as it always did.
//
// On top of that, the wheel is ignored while a touch gesture owns the input, and
// for the 250 ms after it ends - the same window in which synthesized mouse and
// wheel events trail in. So even a panel that reports a pan as a wheel can never
// change the zoom. When the diagnostics log is on, an ignored wheel is recorded,
// because that log line is how we would ever find out a panel does this.
bool PdfCanvas::handleWheel(QWheelEvent *we)
{
    if (m_pinchActive || m_drawing || m_erasing || m_touchInkBlocked
        || (m_touchRelease && m_touchRelease->isActive())) {
        if (AppLog::isEnabled())
            eraseLog(QStringLiteral("wheel 忽略（触摸手势进行中/刚结束）delta=%1,%2 ctrl=%3")
                         .arg(we->angleDelta().x())
                         .arg(we->angleDelta().y())
                         .arg(we->modifiers().testFlag(Qt::ControlModifier) ? 1 : 0));
        return true;                   // swallow: never zoom or scroll under a finger
    }

    const int dy = we->angleDelta().y();
    if (dy == 0 || !we->modifiers().testFlag(Qt::ControlModifier))
        return false;                  // plain wheel: the scroll area handles it

    // One notch (120 units = 15° wheel step) is a comfortable step. The point under
    // the cursor stays put, exactly like a pinch; pages re-render crisply through
    // the settle timer, so spinning the wheel does not rasterise per notch.
    zoomAt(qPow(1.25, qreal(dy) / 120.0), we->position());
    return true;
}

// A pen/stylus reports several times the sample rate of the synthesized mouse
// events that follow it, so its own positions are used for the stroke. The touch
// lock is reused to keep those synthesized mouse events out for the duration of
// the stroke (and briefly after it) - the same trick the touch path uses.
bool PdfCanvas::handleTablet(QTabletEvent *te)
{
    m_strokeFromTouch = false;       // a stylus: taps are dots (see endInput)
    InputProbe::instance().addEvent(1);

    // Probe: pointerType() == Eraser means the pen is being used with its ERASER end - we
    // never looked at it, so today that end simply draws ink. The log also carries pressure,
    // which is the other candidate for an "adaptive" eraser size.
    eraseLog(QStringLiteral("tablet type=%1 pointer=%2 pressure=%3 pos=%4,%5")
                 .arg(int(te->type()))
                 .arg(int(te->pointerType()))
                 .arg(te->pressure(), 0, 'f', 2)
                 .arg(int(te->position().x()))
                 .arg(int(te->position().y())));

    if (m_tool == InkTool::Move)
        return false;                      // free move is a finger gesture
    if (!m_doc || m_geom.isEmpty())
        return false;

    const QPointF pos = te->position();    // viewport coordinates
    switch (te->type()) {
    case QEvent::TabletPress:
        if (m_touchInkBlocked)
            return true;                   // a gesture owns the input
        m_touchInkBlocked = true;          // swallow the synthesized mice
        if (m_touchRelease)
            m_touchRelease->stop();
        beginInputAt(pos, te->pointerType() == QPointingDevice::PointerType::Eraser);
        return true;
    case QEvent::TabletMove:
        if (m_drawing || m_erasing)
            moveInputTo(pos);
        return true;
    case QEvent::TabletRelease:
        endInput();
        // Keep the lock for a moment: the synthesized mouse release trails in.
        if (m_touchRelease)
            m_touchRelease->start();
        return true;
    default:
        break;
    }
    return false;
}

// One finger draws / erases; two fingers pinch-zoom and pan at the same time.
// --- two-finger gesture smoothing ---------------------------------------------------
// IR panels report their contact points with a few pixels of noise, and that noise hits BOTH
// the finger distance (scale) and their centroid (pan): the page breathed and shook, which
// is exhausting to read on a classroom panel. Each frame is therefore NOT applied directly.
// The raw gesture is accumulated, an EMA follows it, and only the difference between the
// smoothed value and what has already been applied is spent - so the noise averages out, a
// real gesture still tracks with ~2-3 frames of lag, and nothing drifts (the smoothed value
// converges exactly to the raw one).
constexpr qreal kPinchSmoothAlpha   = 0.35;   // ~3 frames of lag at 60 Hz
constexpr qreal kPinchScaleDeadZone = 0.005;  // 0.5 %: above the smoothed noise wobble
                                              // (~0.23 %/frame) and far below a real pinch
                                              // (~0.7 %/frame). This one matters most: every
                                              // zoom step re-anchors the scrollbars, so scale
                                              // noise shows up as the page moving sideways.
constexpr qreal kPinchPanDeadZonePx = 3.0;    // below this the pan is panel noise, not intent
constexpr qreal kPinchRateMin       = 0.6;    // a single ghost frame cannot teleport
constexpr qreal kPinchRateMax       = 1.7;    // the scale by more than this

constexpr qreal kPinchRestAlpha    = 0.08;    // slow average: ~12 frames
constexpr qreal kPinchRestSpanTol  = 0.02;    // 2 % of span still counts as "not pinching"
constexpr qreal kPinchRestPanPx    = 6.0;     // ...and 6 px of centroid travel as "not panning"

void PdfCanvas::startPinch(const QPointF &a, const QPointF &b)
{
    m_pinchActive = true;
    m_touchInkBlocked = true;              // no inking until every finger lifts
    m_pinchPts = {a, b};
    m_pinchScaleRaw = m_pinchScaleSmooth = m_pinchScaleApplied = 1.0;
    m_pinchPanRaw = m_pinchPanSmooth = m_pinchPanApplied = QPointF();
    m_pinchCentroidSmooth = (a + b) / 2.0;
    m_pinchRestSpan = QLineF(a, b).length();
    m_pinchRestCentroid = (a + b) / 2.0;
}

void PdfCanvas::applyPinchFrame(const QPointF &a1, const QPointF &b1)
{
    const QPointF a0 = m_pinchPts.value(0);
    const QPointF b0 = m_pinchPts.value(1);
    m_pinchPts = {a1, b1};

    // Is the gesture AT REST? Two fingers held on the glass while the teacher reads keep
    // emitting noise, and any residual drift is exactly what "the page shakes" feels like.
    // Compare the live gesture against a slow average of it: when the two agree, the fingers
    // are not actually moving, so NOTHING is applied and the page is perfectly still - which
    // no amount of smoothing achieves on its own, because smoothing only lowers the amplitude
    // of the shake, never its slow creep.
    const qreal dLive = QLineF(a1, b1).length();
    const QPointF cLive = (a1 + b1) / 2.0;
    m_pinchRestSpan += (dLive - m_pinchRestSpan) * kPinchRestAlpha;
    m_pinchRestCentroid += (cLive - m_pinchRestCentroid) * kPinchRestAlpha;
    const bool atRest =
        (m_pinchRestSpan > 20.0 && qAbs(dLive / m_pinchRestSpan - 1.0) < kPinchRestSpanTol)
        && (QLineF(cLive, m_pinchRestCentroid).length() < kPinchRestPanPx);
    if (atRest) {
        // Keep the accumulators in step, so leaving "rest" starts clean instead of replaying
        // the noise that was ignored while reading.
        m_pinchScaleRaw = m_pinchScaleSmooth = m_pinchScaleApplied = 1.0;
        m_pinchPanRaw = m_pinchPanSmooth = m_pinchPanApplied = QPointF();
        m_pinchCentroidSmooth = cLive;
        return;
    }

    // Pan first. Smoothing the accumulated travel is NOT enough on its own: an EMA over a
    // random walk has the same long-term variance as the walk itself (measured: 32 px of
    // drift with smoothing, 32 px without), and the panel noise IS a random walk. What
    // actually stops it is a dead zone - a centroid move below the noise floor is simply not
    // spent. A deliberate pan moves far more than that per frame, so it still tracks.
    m_pinchPanRaw += (a1 + b1) / 2.0 - (a0 + b0) / 2.0;
    m_pinchPanSmooth += (m_pinchPanRaw - m_pinchPanSmooth) * kPinchSmoothAlpha;
    const QPointF pending(m_pinchPanSmooth.x() - m_pinchPanApplied.x(),
                          m_pinchPanSmooth.y() - m_pinchPanApplied.y());
    if (qAbs(pending.x()) >= kPinchPanDeadZonePx || qAbs(pending.y()) >= kPinchPanDeadZonePx) {
        const QPointF step(qRound(pending.x()), qRound(pending.y()));
        if (!step.isNull())
            panBy(step);
        // The remainder is dropped on purpose: keeping it would let the noise accumulate
        // again, which is exactly the drift this dead zone exists to remove.
        m_pinchPanApplied = m_pinchPanSmooth;
    }

    // Scale: clamp the frame, follow it with the EMA, apply only above the dead zone.
    const qreal d0 = QLineF(a0, b0).length();
    const qreal d1 = QLineF(a1, b1).length();
    m_pinchCentroidSmooth += ((a1 + b1) / 2.0 - m_pinchCentroidSmooth) * kPinchSmoothAlpha;
    if (d0 > 20.0 && d1 > 20.0) {
        m_pinchScaleRaw *= qBound(kPinchRateMin, d1 / d0, kPinchRateMax);
        m_pinchScaleSmooth += (m_pinchScaleRaw - m_pinchScaleSmooth) * kPinchSmoothAlpha;
        const qreal stepScale = m_pinchScaleSmooth / m_pinchScaleApplied;
        if (stepScale > 1.0 + kPinchScaleDeadZone
            || stepScale < 1.0 - kPinchScaleDeadZone) {
            const qreal before = m_zoom;
            // Anchor at the SMOOTHED centroid: anchoring at the raw one injects its jitter
            // into every zoom step, which is half of what shakes the page.
            zoomAt(stepScale, m_pinchCentroidSmooth);
            // Record what was actually achieved (applyZoom clamps at the zoom limits) so the
            // next step stays relative to reality instead of accumulating an error.
            if (before > 0.0)
                m_pinchScaleApplied *= (m_zoom / before);
        }
    }
}

void PdfCanvas::endPinch()
{
    m_pinchActive = false;
    m_pinchPts.clear();
}

bool PdfCanvas::touchBelongsToOverlay(const QVector<QPointF> &viewportPts) const
{
    // A gesture that is already running owns the touch. The island, palette and
    // page grid are children of the viewport, so anything written across them is
    // covered by them on screen - the ink goes underneath, which is exactly what
    // "keep writing" means. Refusing these samples mid-stroke used to set the ink
    // lock, and endInput() then cancelled the WHOLE stroke: one line dragged over
    // the island made the part before it disappear too.
    if (m_drawing || m_erasing || m_pinchActive || m_moveDragActive)
        return false;

    for (const QPointF &p : viewportPts) {
        QWidget *child = viewport()->childAt(p.toPoint());
        if (child && child->isVisible())
            return true;               // that widget gets the touch instead
    }
    return false;
}

bool PdfCanvas::handleTouch(QTouchEvent *te)
{
    // Free move pans the view, so it works on a blank canvas too.
    if (m_tool != InkTool::Move && (!m_doc || m_geom.isEmpty()))
        return false;
    // The only path that feeds real finger input: a finger tap may still be noise, so it
    // keeps the "too short = discard" rule in endInput(). A mouse/stylus tap leaves a dot.
    m_strokeFromTouch = true;

    QVector<QPointF> pts;
    for (const QEventPoint &p : te->points()) {
        // Cancelled touches arrive as a TouchCancel event; here a point is
        // either still down or released.
        if (p.state() != QEventPoint::Released)
            pts.append(p.position());
    }
    InputProbe::instance().addEvent(pts.size());

    // A touch that LANDS on a floating overlay (toolbar, palette, page grid)
    // belongs to that widget: never start a stroke underneath it and never treat
    // it as a pinch source. A touch that arrives while something is already
    // running is handled by touchBelongsToOverlay() - it must not interrupt.
    if (touchBelongsToOverlay(pts)) {
        m_touchInkBlocked = true;
        if (m_touchRelease)
            m_touchRelease->start();
        return true;
    }

    const bool ended = (te->type() == QEvent::TouchEnd);

    // Probe: the panel's contact ELLIPSE is what an adaptive-size eraser would follow, and it
    // is the one thing we never looked at (we only ever used position()). Log it, so a run on
    // the real panel can say whether the hardware reports it at all - guessing here would be
    // pointless, exactly like the touch-compression discovery in 1.5.2.
    const QList<QEventPoint> &eps = te->points();
    const QSizeF e0 = eps.isEmpty() ? QSizeF() : eps.at(0).ellipseDiameters();
    const QSizeF e1 = eps.size() < 2 ? QSizeF() : eps.at(1).ellipseDiameters();
    eraseLog(QStringLiteral("touch type=%1 total=%2 active=%3 pinch=%4 blocked=%5 "
                            "contact0=%6x%7 contact1=%8x%9")
                 .arg(int(te->type()))
                 .arg(te->points().size())
                 .arg(pts.size())
                 .arg(m_pinchActive ? 1 : 0)
                 .arg(m_touchInkBlocked ? 1 : 0)
                 .arg(e0.width(), 0, 'f', 1).arg(e0.height(), 0, 'f', 1)
                 .arg(e1.width(), 0, 'f', 1).arg(e1.height(), 0, 'f', 1));

    // A cancelled touch stream must NOT commit the pending stroke - that is a
    // classic source of stray ink when the system takes over a gesture.
    if (te->type() == QEvent::TouchCancel) {
        cancelGesture();
        m_pinchActive = false;
        m_pinchPts.clear();
        if (m_touchRelease)
            m_touchRelease->start();       // keep the lock a bit longer
        return true;
    }

    if (pts.size() >= 2) {
        // The two-finger gesture wins: a running free-move drag must stop here
        // and must not resume until a fresh single-finger sequence begins.
        m_moveDragActive = false;
        if (!m_pinchActive) {
            cancelGesture();               // drop any partial single-finger stroke
            startPinch(pts.at(0), pts.at(1));
            return true;
        }
        m_touchInkBlocked = true;
        applyPinchFrame(pts.at(0), pts.at(1));
        return true;
    }

    if (m_pinchActive) {
        // One finger lifted: stop the pinch, but keep inking blocked until the
        // remaining finger is gone too (otherwise a stray contact draws).
        if (pts.size() < 2) {
            m_pinchActive = false;
            m_pinchPts.clear();
        }
        return true;
    }

    if (ended) {
        m_moveDragActive = false;
        endInput();
        // Keep the lock briefly: synthesized mouse events may still trail in.
        if (m_touchRelease)
            m_touchRelease->start();
        m_pinchActive = false;
        m_pinchPts.clear();
        return true;
    }

    if (pts.size() == 1) {
        if (te->type() == QEvent::TouchBegin) {
            // A fresh single-finger sequence starts here (extra fingers arrive as
            // TouchUpdate, never as TouchBegin), so it is safe to unlock at once
            // instead of waiting out the release delay.
            if (m_touchRelease)
                m_touchRelease->stop();
            m_touchInkBlocked = false;
            if (m_tool == InkTool::Move) {
                // One finger in free-move mode pans the view, not the page.
                m_moveDragActive = true;
                m_moveLastPos = pts.first();
                return true;
            }
            beginInputAt(pts.first());
            return true;
        }
        if (m_touchInkBlocked)
            return true;                   // residual finger of a pinch gesture
        if (m_tool == InkTool::Move) {
            freePanTo(pts.first());
            return true;
        }
        moveInputTo(pts.first());
        return true;
    }
    return true;
}

// --- test hooks (used by `--selftest-ink`) --------------------------------

int PdfCanvas::testDensifiedCount(const QVector<QPointF> &raw) const
{
    QVector<QPointF> out;                  // runs the real input path
    for (const QPointF &p : raw)
        appendDensified(out, p, kInkMaxStepNorm);
    return out.size();
}

// The real touch entry points, headless: lets the self test prove that a stroke
// started on the page survives being dragged across the floating island (and that
// a touch landing on the island itself is still left to the island).
void PdfCanvas::testWheelAt(const QPointF &viewportPos, int angleDeltaY, bool ctrl)
{
    QWheelEvent event(viewportPos, viewport()->mapToGlobal(viewportPos.toPoint()),
                      QPoint(0, 0), QPoint(0, angleDeltaY), Qt::NoButton,
                      ctrl ? Qt::ControlModifier : Qt::NoModifier, Qt::NoScrollPhase,
                      /*inverted=*/false);
    QApplication::sendEvent(viewport(), &event);
}

void PdfCanvas::testTouchBegin(const QPointF &viewportPos)
{
    // Mirrors handleTouch's TouchBegin: a fresh sequence clears the trailing lock.
    if (m_touchRelease)
        m_touchRelease->stop();
    beginInputAt(viewportPos);
}

void PdfCanvas::testTouchMove(const QPointF &viewportPos) { moveInputTo(viewportPos); }

void PdfCanvas::testTouchEnd()
{
    endInput();
    // ...and TouchEnd keeps the lock briefly, exactly like the real touch path.
    // That 250 ms window is what stops a synthesized pan-as-wheel from zooming,
    // so the hook has to reproduce it or the wheel rule would go untested.
    if (m_touchRelease)
        m_touchRelease->start();
}

bool PdfCanvas::testTouchBelongsToOverlay(const QVector<QPointF> &viewportPts) const
{
    return touchBelongsToOverlay(viewportPts);
}

void PdfCanvas::testZoomAt(const QPointF &viewportAnchor, qreal factor)
{
    zoomAt(factor, viewportAnchor);
}

qreal PdfCanvas::testFracX(int page, qreal x) const
{
    if (page < 0 || page >= m_geom.size())
        return -1.0;
    const QRectF r = pageRectInViewport(page);
    return r.width() > 0.0 ? (x - r.left()) / r.width() : -1.0;
}

int PdfCanvas::testPageAtViewportY(qreal y) const
{
    return pageAtContentY(verticalScrollBar()->value() + y);
}

qreal PdfCanvas::testFracAtViewportY(qreal y) const
{
    const qreal contentY = verticalScrollBar()->value() + y;
    const int page = pageAtContentY(contentY);
    if (page < 0 || page >= m_geom.size())
        return -1.0;
    const PageGeom &g = m_geom.at(page);
    return g.h > 0.0 ? (contentY - g.y) / g.h : -1.0;
}
