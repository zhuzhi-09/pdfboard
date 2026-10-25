#include "PdfCanvas.h"
#include "AppLog.h"
#include "AppSettings.h"
#include "CrashLog.h"
#include "IconPainter.h"
#include "InkDirty.h"
#include "InkErase.h"
#include "InkStroke.h"
#include "InkToolbar.h"
#include "InputProbe.h"
#include "Theme.h"

#include <QPdfDocument>
#include <QPdfPageRenderer>
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
#include <QAbstractNativeEventFilter>

#include <algorithm>

#ifdef Q_OS_WIN
// After the Qt includes: windows.h defines the min/max macros.
#include <windows.h>
#endif

#ifdef Q_OS_WIN
namespace {

// Windows turns a finger (or pen) held still on the glass into a right click - the
// "press and hold" system gesture. On this canvas that is never wanted: it pops a
// context menu AND makes the system take the touch stream away in the middle of a
// stroke (TouchCancel), which is how a chunk of handwriting disappeared. The canvas
// has no context menu, so this filter
//   * answers WM_TABLET_QUERYSYSTEMGESTURESTATUS with "press and hold disabled"
//     (also pen tap / barrel-button feedback and flicks), and
//   * swallows every right-button and context-menu message that lands on the canvas.
// Both the panel's own right-click emulation and Windows' gesture are covered.
class CanvasMessageFilter : public QAbstractNativeEventFilter
{
public:
    explicit CanvasMessageFilter(PdfCanvas *canvas) : m_canvas(canvas) {}

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *result) override
    {
        if (!m_canvas || !message)
            return false;
        if (type != "windows_generic_MSG" && type != "windows_dispatcher_MSG")
            return false;
        const MSG *msg = static_cast<const MSG *>(message);
        // internalWinId() does NOT create the native window (winId() does), so this is
        // safe to call from inside message dispatch - calling winId() here created and
        // re-entered the window while a message was being handled.
        const HWND hwnd = reinterpret_cast<HWND>(m_canvas->window()->internalWinId());
        if (!msg || !hwnd || msg->hwnd != hwnd)
            return false;

        // WM_TABLET_QUERYSYSTEMGESTURESTATUS. Not defined by every SDK header, so the
        // value is spelled out: 0x1 press-and-hold, 0x8 pen tap feedback, 0x10 pen
        // barrel feedback, 0x100 flicks.
        constexpr UINT  kQuerySystemGestureStatus = 0x02C0u;
        constexpr qintptr kDisabledSystemGestures = 0x1 | 0x8 | 0x10 | 0x100;
        if (msg->message == kQuerySystemGestureStatus) {
            if (result)
                *result = kDisabledSystemGestures;
            // One line in the opt-in log: it says whether this machine actually asks
            // (and therefore whether the long-press gesture was really switched off).
            static bool logged = false;
            if (!logged) {
                logged = true;
                AppLog::write(QStringLiteral("input"),
                              QStringLiteral("已向系统关闭长按 / 右键手势"));
            }
            return true;
        }

        if (!canvasUnderCursor())
            return false;
        switch (msg->message) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
            if (result)
                *result = 0;
            return true;
        default:
            return false;
        }
    }

private:
    bool canvasUnderCursor() const
    {
        if (!m_canvas->isVisible())
            return false;
        const QPoint local = m_canvas->viewport()->mapFromGlobal(QCursor::pos());
        return m_canvas->viewport()->rect().contains(local);
    }

    PdfCanvas *m_canvas = nullptr;
};

}   // namespace
#endif   // Q_OS_WIN

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

// Contact-area brush width (pure touch): the per-sample multiplier is kept inside
// these bounds. The upper bound is also what the dirty-rect padding and the ink
// culling use, so a wide sample can never be clipped by a too-small repaint.
constexpr qreal kInkMinWidthFactor = 0.80;
constexpr qreal kInkMaxWidthFactor = 1.80;

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

// Bounding box of a finished stroke, in the same normalized space as its samples.
// drawInk() uses it to skip the (expensive) path building for strokes outside the
// repainted rect; an empty box (a dot, or nothing) means "no extent worth culling on"
// and keeps the stroke drawn, so a missing box can never hide ink.
QRectF strokeBounds(const QVector<QPointF> &pts)
{
    if (pts.size() < 2)
        return QRectF();
    qreal l = pts.first().x(), r = l;
    qreal t = pts.first().y(), b = t;
    for (const QPointF &p : pts) {
        l = qMin(l, p.x());
        t = qMin(t, p.y());
        r = qMax(r, p.x());
        b = qMax(b, p.y());
    }
    return QRectF(l, t, r - l, b - t);
}

// 橡皮的可选档位（逻辑像素半径），三档：细 / 中 / 粗。列表只此一处，
// 工具栏面板、擦除命中、指示环、命中测试都从这里取数。第一档 14 就是旧版本
// 唯一的固定值（kEraserRadiusPx），所以没动过设置的安装擦除行为完全不变。
constexpr qreal kEraserRadiusSteps[3] = { 14.0, 26.0, 44.0 };

// --- 掌擦（手掌接触面橡皮）分类器的全部参数 --------------------------------
// 一台没有笔的教室大屏：手掌按上去时面板把它报成好几个接触点，而且点数逐帧
// 抖动（3、4、5、2……）；真正的双指缩放同样是多点。分类器靠这些阈值和迟滞把
// "手掌 / 手指 / 双指"分开，半径的防抖也在这一块：数字都只在这里，调参只动这。
constexpr qreal kPalmClusterDistPx = 72.0;  // 聚簇距离：手掌接触面宽度量级
constexpr int   kPalmEnterPoints  = 4;      // 一簇里至少这么多点才可能是手掌
constexpr int   kPalmEnterFrames  = 2;      // ...且连续这么多帧才真的进入（幽灵帧不开擦）
constexpr int   kPalmStayPoints   = 2;      // 已在掌擦：剩余点数 >= 2 就继续
constexpr qreal kPalmRadiusPadPx  = 18.0;   // 手指接触面本身的余量（加在圆周半径上）
constexpr qreal kPalmRadiusMinPx  = kEraserRadiusSteps[0];        // 不低于最小静态档
constexpr qreal kPalmRadiusMaxPx  = kEraserRadiusSteps[2] * 3.0;  // 不高于最大档的 3 倍
constexpr qreal kPalmRadiusAlpha  = 0.28;   // 半径 EMA（把点数抖动平均掉）
constexpr qreal kPalmRadiusDeadPx = 2.5;    // 半径死区：小于它不动（按住时圆环不呼吸）
constexpr qreal kPalmRadiusRateCap = 14.0;  // 单帧最大半径变化（幽灵点不能一次撑大）
constexpr qreal kPalmRestMovePx   = 5.0;    // 簇心与慢平均的距离小于它 = 静止（冻结半径）
constexpr qreal kPalmRestAlpha    = 0.10;   // 静止检测的慢平均系数（约 10 帧）

// --- 接触面积规则（纯触摸面板）---------------------------------------------
// 有的面板会随触点上报接触椭圆（QEventPoint::ellipseDiameters，逻辑 px）。视源
// 专利 CN119917011A 就是用物理接触面积区分手指与手掌/手背：同一帧里最小的那个
// 接触是手指，明显更大的那个是手掌。这里不写死绝对尺寸（面板单位、DPI、缩放都
// 不同），而是以本次使用中见过的最小接触为“手指基准”，超过它的若干倍才算手掌。
constexpr qreal kPalmSizeRatio     = 2.2;   // 手指基准的 2.2 倍 = 手掌（进入）
constexpr qreal kPalmSizeStayRatio = 1.6;   // 已在掌擦时降到 1.6 倍（迟滞，不来回翻）
constexpr qreal kPalmSizePadPx     = 6.0;   // 擦除半径在接触半径之外的余量

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
        if (!m_settleNeeded)
            return;                 // nothing was left coarse or at the old zoom
        m_settleNeeded = false;
        invalidateRenders();
        relayout();
        viewport()->update();
        AppLog::write(QStringLiteral("view"),
                      QStringLiteral("缩放稳定：%1×（视口 %2×%3，缓存 %4 MB）")
                          .arg(m_zoom, 0, 'f', 2)
                          .arg(viewport()->width()).arg(viewport()->height())
                          .arg(double(m_rasterBytes) / 1048576.0, 0, 'f', 1));
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

    // Page tiles are rendered on a worker thread. The UI paints the low-res
    // preview first, so opening a page or settling a zoom no longer freezes for
    // the whole tile set (the 50-100 ms cold paint measured in docs 2.87).
    m_tileRenderer = new QPdfPageRenderer(this);
    m_tileRenderer->setRenderMode(QPdfPageRenderer::RenderMode::MultiThreaded);
    connect(m_tileRenderer, &QPdfPageRenderer::pageRendered, this,
            [this](int page, QSize size, const QImage &image,
                   QPdfDocumentRenderOptions, quint64 requestId) {
                onTileRendered(page, size, image, requestId);
            });

    // The classroom-whiteboard style floating toolbar lives on top of the page
    // area.
    m_toolbar = new InkToolbar(this);

#ifdef Q_OS_WIN
    // Right click / "press and hold" are meaningless on the page: see CanvasMessageFilter.
    m_nativeFilter = new CanvasMessageFilter(this);
    qApp->installNativeEventFilter(m_nativeFilter);
#endif
}

PdfCanvas::~PdfCanvas()
{
    // Stop (and join) the tile worker before the document child is destroyed:
    // a render still holding the page must not outlive its QPdfDocument.
    if (m_tileRenderer) {
        m_tileRenderer->setDocument(nullptr);
        delete m_tileRenderer;
        m_tileRenderer = nullptr;
    }
#ifdef Q_OS_WIN
    if (m_nativeFilter) {
        qApp->removeNativeEventFilter(m_nativeFilter);
        delete m_nativeFilter;
        m_nativeFilter = nullptr;
    }
#endif
    setDrawing(false);   // a tab closed mid-stroke must release the motion gate
}

// Every write of m_drawing goes through here, so the chrome's motion gate
// (Theme::Motion::inking) sees exactly one begin and one end per stroke.
void PdfCanvas::setDrawing(bool on)
{
    if (m_drawing == on)
        return;
    m_drawing = on;
    if (on)
        Theme::Motion::inkBegan();
    else
        Theme::Motion::inkEnded();
}

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
    if (m_tileRenderer)
        m_tileRenderer->setDocument(nullptr);
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
    if (m_tileRenderer)
        m_tileRenderer->setDocument(m_doc);

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
    // 橡皮大小跟着"这份文档"走：新文档 = 默认档（用户没动过就是 14 px）。
    m_eraserRadius = eraserRadiusSteps()[0];
    m_touchClass = TouchClassState{};      // 触摸记忆不跨文档
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
    if (m_tileRenderer)
        m_tileRenderer->setDocument(nullptr);
    if (m_doc) {
        m_doc->close();
        delete m_doc;
        m_doc = nullptr;
    }
    invalidateRenders();
    m_path.clear();
    m_pageSizes.clear();
    m_geom.clear();
    m_contentH = 0;
    m_maxPageWpt = 595.0;
    cancelGesture();
    m_touchClass = TouchClassState{};
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
    // Results from requests made before this point are stale (old zoom or old
    // document) and must be dropped even if the worker is still emitting them.
    ++m_tileGeneration;
    m_pendingTiles.clear();
    m_pendingTileKeys.clear();
    m_rasters.clear();
    m_rasterLru.clear();
    m_rasterBytes = 0;
    m_settleNeeded = false;         // a fresh cache starts crisp
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

void PdfCanvas::touchRaster(int page)
{
    m_rasterLru.removeAll(page);
    m_rasterLru.append(page);
}

void PdfCanvas::noteRender(int page, qint64 ms, const QSize &size)
{
    m_lastRenderMs = ms;
    m_lastRenderSize = size;
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
    // A slow tile is the first thing to look at when a page feels sticky.
    if (ms > 40)
        AppLog::write(QStringLiteral("render"),
                      QStringLiteral("第 %1 页栅格化 %2 ms（%3×%4）")
                          .arg(page + 1).arg(ms)
                          .arg(size.width()).arg(size.height()));
}

PdfCanvas::PageRaster &PdfCanvas::rasterFor(int page, const QSize &deviceSize)
{
    const auto it = m_rasters.find(page);
    if (it != m_rasters.end()) {
        PageRaster &pr = it.value();
        // A pinch (or the 160 ms settle after it) draws the previous scale's tiles
        // scaled; invalidateRenders() replaces them at the new scale. Any other size
        // change (a layout that did not invalidate) starts a fresh set.
        if (!m_pinchActive && !m_settleNeeded && pr.deviceSize != deviceSize) {
            m_rasterBytes -= pr.bytes;
            pr = PageRaster();
            pr.deviceSize = deviceSize;
            pr.cols = qMax(1, (deviceSize.width() + kTilePx - 1) / kTilePx);
            pr.rows = qMax(1, (deviceSize.height() + kTilePx - 1) / kTilePx);
            pr.tiles.resize(pr.cols * pr.rows);
        }
        touchRaster(page);
        return pr;
    }

    PageRaster pr;
    pr.deviceSize = deviceSize;
    pr.cols = qMax(1, (deviceSize.width() + kTilePx - 1) / kTilePx);
    pr.rows = qMax(1, (deviceSize.height() + kTilePx - 1) / kTilePx);
    pr.tiles.resize(pr.cols * pr.rows);
    PageRaster &ref = m_rasters.insert(page, pr).value();
    touchRaster(page);
    return ref;
}

// One 512x512 DEVICE-pixel tile at the page's current 1:1 resolution. The clip is
// expressed in the SCALED page space (QPdfDocumentRenderOptions::setScaledSize),
// and the returned image is exactly the clip, so the tile can be blitted 1:1.
QImage PdfCanvas::tileAt(int page, PageRaster &pr, int tx, int ty)
{
    if (!m_doc || page < 0 || page >= m_doc->pageCount())
        return {};
    if (tx < 0 || tx >= pr.cols || ty < 0 || ty >= pr.rows)
        return {};
    const int idx = ty * pr.cols + tx;
    if (!pr.tiles.at(idx).isNull())
        return pr.tiles.at(idx);

    const int x0 = tx * kTilePx;
    const int y0 = ty * kTilePx;
    const int tw = qMin(kTilePx, pr.deviceSize.width() - x0);
    const int th = qMin(kTilePx, pr.deviceSize.height() - y0);
    if (tw <= 0 || th <= 0)
        return {};

    QPdfDocumentRenderOptions opt;
    opt.setScaledSize(pr.deviceSize);
    // QPdfDocument builds its matrix from clip.right()/bottom() (inclusive), so a
    // clip of exactly tw pixels would render at (tw-1)/tw of the requested scale -
    // each tile would shrink by ~0.2 % and leave a seam. One extra pixel on the far
    // edge makes the matrix exactly 1:1; the output image stays tw x th.
    opt.setScaledClipRect(QRect(x0, y0, tw + 1, th + 1));

    QElapsedTimer t;
    t.start();
    // Measure the depth of the render path: a render *inside* another render is
    // what once ate the stack while zooming, and the breadcrumb names it.
    static int renderDepth = 0;
    ++renderDepth;
    if (renderDepth > 1)
        CrashLog::breadcrumb("render-nested",
                             QStringLiteral("渲染嵌套第 %1 层（第 %2 页）")
                                 .arg(renderDepth).arg(page + 1));
    QImage img = m_doc->render(page, QSize(tw, th), opt);
    --renderDepth;
    InputProbe::instance().addRender();
    if (img.isNull())
        return {};
    // Tag the tile with the screen ratio: its logical size is then tw/dpr, which
    // is exactly the logical rect it is drawn into (no resampling at 1:1).
    img.setDevicePixelRatio(qMax<qreal>(1.0, viewport() ? viewport()->devicePixelRatioF() : 1.0));
    pr.tiles[idx] = img;
    const qint64 bytes = img.sizeInBytes();
    pr.bytes += bytes;
    m_rasterBytes += bytes;
    noteRender(page, t.elapsed(), img.size());
    return pr.tiles.at(idx);
}

// A low-resolution whole-page underlay, used only while the tiles belong to the
// previous scale (pinch, or a zoom whose settle timer was postponed by writing).
// Without it a pinch-out would show blank paper where no tile exists yet.
QImage PdfCanvas::previewAt(int page, PageRaster &pr)
{
    if (!pr.preview.isNull())
        return pr.preview;
    if (!m_doc || page < 0 || page >= m_doc->pageCount())
        return {};

    // 1/4 linear resolution, capped: at 8x zoom the page bitmap would otherwise be
    // hundreds of megabytes even though this is only a transient gesture preview.
    int w = qMax(1, pr.deviceSize.width() / 4);
    int h = qMax(1, pr.deviceSize.height() / 4);
    if (w > 1600) {
        h = qMax(1, int(qint64(h) * 1600 / w));
        w = 1600;
    }

    QElapsedTimer t;
    t.start();
    QImage img = m_doc->render(page, QSize(w, h));
    InputProbe::instance().addRender();
    if (img.isNull())
        return {};
    pr.preview = img;
    const qint64 bytes = img.sizeInBytes();
    pr.bytes += bytes;
    m_rasterBytes += bytes;
    noteRender(page, t.elapsed(), img.size());
    return pr.preview;
}

// Hands one tile to the worker. The key dedups repeated paints while the result
// is in flight, and the generation stamp lets onTileRendered() drop anything
// that belongs to a previous zoom or document.
void PdfCanvas::requestTile(int page, PageRaster &pr, int tx, int ty)
{
    if (!m_tileRenderer || !m_doc)
        return;
    if (tx < 0 || tx >= pr.cols || ty < 0 || ty >= pr.rows)
        return;
    if (!pr.tiles.at(ty * pr.cols + tx).isNull())
        return;

    const int x0 = tx * kTilePx;
    const int y0 = ty * kTilePx;
    const int tw = qMin(kTilePx, pr.deviceSize.width() - x0);
    const int th = qMin(kTilePx, pr.deviceSize.height() - y0);
    if (tw <= 0 || th <= 0)
        return;
    // A burst of scroll steps must not queue an unbounded backlog; the next
    // paint simply asks again for whatever is still missing.
    if (m_pendingTiles.size() >= 64)
        return;

    const quint64 key = (quint64(m_tileGeneration & 0xffff) << 48)
                        | (quint64(page & 0xffff) << 32)
                        | (quint64(tx & 0xff) << 8) | quint64(ty & 0xff);
    if (m_pendingTileKeys.contains(key))
        return;

    QPdfDocumentRenderOptions opt;
    opt.setScaledSize(pr.deviceSize);
    // Same inclusive-edge correction as tileAt(): +1 pixel on the far side.
    opt.setScaledClipRect(QRect(x0, y0, tw + 1, th + 1));
    const quint64 id = m_tileRenderer->requestPage(page, QSize(tw, th), opt);
    if (!id || m_pendingTiles.contains(id))
        return;                     // the renderer deduped to a tracked request
    m_pendingTiles.insert(id, PendingTile{page, tx, ty, int(m_tileGeneration),
                                          pr.deviceSize, key});
    m_pendingTileKeys.insert(key);
}

void PdfCanvas::onTileRendered(int page, const QSize &size, const QImage &image,
                               quint64 requestId)
{
    const auto it = m_pendingTiles.find(requestId);
    if (it == m_pendingTiles.end())
        return;                     // invalidated (old zoom/document) while in flight
    const PendingTile req = it.value();
    m_pendingTiles.erase(it);
    m_pendingTileKeys.remove(req.key);

    if (req.generation != int(m_tileGeneration))
        return;
    const auto rit = m_rasters.find(page);
    if (rit == m_rasters.end())
        return;
    PageRaster &pr = rit.value();
    if (pr.deviceSize != req.deviceSize
        || req.tx < 0 || req.tx >= pr.cols || req.ty < 0 || req.ty >= pr.rows)
        return;
    const int idx = req.ty * pr.cols + req.tx;
    if (!pr.tiles.at(idx).isNull())
        return;                     // a synchronous render won the race
    if (image.isNull() || size.isEmpty() || image.size() != size)
        return;

    QImage tile = image;
    tile.setDevicePixelRatio(qMax<qreal>(1.0, viewport() ? viewport()->devicePixelRatioF() : 1.0));
    pr.tiles[idx] = tile;
    const qint64 bytes = tile.sizeInBytes();
    pr.bytes += bytes;
    m_rasterBytes += bytes;
    InputProbe::instance().addRender();

    // Repaint just this tile where it currently is. If the page scrolled away in
    // the meantime the rect is off-screen and Qt discards the update; the tile
    // stays cached for when it comes back.
    const QRectF r = pageRectInViewport(page);
    if (!r.isEmpty()) {
        const int x0 = req.tx * kTilePx;
        const int y0 = req.ty * kTilePx;
        const int tw = qMin(kTilePx, pr.deviceSize.width() - x0);
        const int th = qMin(kTilePx, pr.deviceSize.height() - y0);
        const QRectF target(
            r.left() + qreal(x0) / pr.deviceSize.width() * r.width(),
            r.top()  + qreal(y0) / pr.deviceSize.height() * r.height(),
            qreal(tw) / pr.deviceSize.width() * r.width(),
            qreal(th) / pr.deviceSize.height() * r.height());
        viewport()->update(target.toAlignedRect().adjusted(-1, -1, 1, 1));
    }
}

int PdfCanvas::testCachedTileCount(int page) const
{
    const auto it = m_rasters.constFind(page);
    if (it == m_rasters.constEnd())
        return 0;
    int n = 0;
    for (const QImage &tile : it.value().tiles) {
        if (!tile.isNull())
            ++n;
    }
    return n;
}

QSize PdfCanvas::testRasterDeviceSize(int page) const
{
    const auto it = m_rasters.constFind(page);
    return it == m_rasters.constEnd() ? QSize() : it.value().deviceSize;
}

void PdfCanvas::drawPageRaster(QPainter &p, int page, const QRectF &r)
{
    if (!m_doc || r.isEmpty())
        return;

    const qreal dpr = qMax<qreal>(1.0, viewport() ? viewport()->devicePixelRatioF() : 1.0);
    const QSize dev(qMax(1, int(qRound(r.width() * dpr))),
                    qMax(1, int(qRound(r.height() * dpr))));
    PageRaster &pr = rasterFor(page, dev);
    const bool stale = m_pinchActive || m_settleNeeded || pr.deviceSize != dev;
    // On screen the tiles arrive from the worker; offscreen canvases (paint
    // benchmark, UI/ink self tests) keep the deterministic synchronous path. The
    // smoke test marks the MainWindow offscreen, so check the window too.
    const bool offscreen = testAttribute(Qt::WA_DontShowOnScreen)
                           || (window() && window()->testAttribute(Qt::WA_DontShowOnScreen));
    const bool async = m_asyncTiles && (!offscreen || m_forceAsyncTiles);

    // Only the part of the page that is on screen and inside the paint region
    // needs tiles: a live ink sample dirties ~140x90 px and must touch one tile,
    // not the whole page.
    QRectF vis = r.intersected(QRectF(viewport()->rect()));
    if (p.hasClipping())
        vis = vis.intersected(p.clipBoundingRect());
    if (vis.isEmpty())
        return;

    const qreal fx0 = (vis.left()   - r.left()) / r.width();
    const qreal fy0 = (vis.top()    - r.top())  / r.height();
    const qreal fx1 = (vis.right()  - r.left()) / r.width();
    const qreal fy1 = (vis.bottom() - r.top())  / r.height();
    const int devX0 = int(fx0 * pr.deviceSize.width());
    const int devY0 = int(fy0 * pr.deviceSize.height());
    const int devX1 = int(qCeil(fx1 * pr.deviceSize.width()));
    const int devY1 = int(qCeil(fy1 * pr.deviceSize.height()));
    const int tx0 = qBound(0, devX0 / kTilePx, pr.cols - 1);
    const int ty0 = qBound(0, devY0 / kTilePx, pr.rows - 1);
    const int tx1 = qBound(0, qMax(devX0, devX1 - 1) / kTilePx, pr.cols - 1);
    const int ty1 = qBound(0, qMax(devY0, devY1 - 1) / kTilePx, pr.rows - 1);

    bool missing = false;
    for (int ty = ty0; ty <= ty1 && !missing; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (pr.tiles.at(ty * pr.cols + tx).isNull()) {
                missing = true;
                break;
            }
        }
    }

    // Previous-scale tiles (pinch / pending settle) and the async cold path: fill
    // the gaps with the preview instead of blocking the UI thread on 30+ tiles.
    if ((stale || async) && missing) {
        const QImage pv = previewAt(page, pr);
        if (!pv.isNull())
            p.drawImage(r, pv);
    }

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            const int x0 = tx * kTilePx;
            const int y0 = ty * kTilePx;
            const int tw = qMin(kTilePx, pr.deviceSize.width() - x0);
            const int th = qMin(kTilePx, pr.deviceSize.height() - y0);
            if (tw <= 0 || th <= 0)
                continue;
            QImage img = pr.tiles.at(ty * pr.cols + tx);
            if (img.isNull()) {
                if (stale)
                    continue;               // the preview underneath covers it
                if (async) {
                    requestTile(page, pr, tx, ty);
                    continue;               // preview underneath, tile repaints later
                }
                img = tileAt(page, pr, tx, ty);
                if (img.isNull())
                    continue;
            }

            QRectF target;
            if (!stale && pr.deviceSize == dev) {
                // 1:1: align every tile to whole DEVICE pixels so QPainter blits
                // instead of resampling, and adjacent tiles share exact edges.
                const qreal ox = qRound(r.left() * dpr);
                const qreal oy = qRound(r.top() * dpr);
                target = QRectF((ox + x0) / dpr, (oy + y0) / dpr,
                                qreal(tw) / dpr, qreal(th) / dpr);
            } else {
                target = QRectF(r.left() + qreal(x0) / pr.deviceSize.width() * r.width(),
                                r.top()  + qreal(y0) / pr.deviceSize.height() * r.height(),
                                qreal(tw) / pr.deviceSize.width() * r.width(),
                                qreal(th) / pr.deviceSize.height() * r.height());
            }
            p.drawImage(target, img);
        }
    }

    // Bound ONE page too. trimRasters() never evicts a visible page, so at a high
    // zoom a long pan would otherwise accumulate thousands of tiles. Keep the
    // range the VIEWPORT needs (not just this paint's dirty rect, which during a
    // live stroke is one tile) and drop the rest; panning re-renders its strip.
    constexpr qint64 kPageTileBytes = 96LL * 1024 * 1024;
    if (pr.bytes > kPageTileBytes) {
        const QRectF vpVis = r.intersected(QRectF(viewport()->rect()));
        int keepX0 = tx0;
        int keepX1 = tx1;
        int keepY0 = ty0;
        int keepY1 = ty1;
        if (!vpVis.isEmpty()) {
            const int vx0 = int((vpVis.left()   - r.left()) / r.width()  * pr.deviceSize.width());
            const int vy0 = int((vpVis.top()    - r.top())  / r.height() * pr.deviceSize.height());
            const int vx1 = int(qCeil((vpVis.right()  - r.left()) / r.width()
                                      * pr.deviceSize.width()));
            const int vy1 = int(qCeil((vpVis.bottom() - r.top())  / r.height()
                                      * pr.deviceSize.height()));
            keepX0 = qBound(0, vx0 / kTilePx, pr.cols - 1);
            keepY0 = qBound(0, vy0 / kTilePx, pr.rows - 1);
            keepX1 = qBound(0, qMax(vx0, vx1 - 1) / kTilePx, pr.cols - 1);
            keepY1 = qBound(0, qMax(vy0, vy1 - 1) / kTilePx, pr.rows - 1);
        }
        qint64 freed = 0;
        for (int ty = 0; ty < pr.rows; ++ty) {
            for (int tx = 0; tx < pr.cols; ++tx) {
                if (tx >= keepX0 && tx <= keepX1 && ty >= keepY0 && ty <= keepY1)
                    continue;
                QImage &tile = pr.tiles[ty * pr.cols + tx];
                if (!tile.isNull()) {
                    freed += tile.sizeInBytes();
                    tile = QImage();
                }
            }
        }
        if (freed > 0) {
            pr.bytes -= freed;
            m_rasterBytes -= freed;
        }
    }

    trimRasters();
}

// Thumbnail raster for the page picker. Deliberately separate from the tile
// cache: it renders straight through QPdfDocument without touching the rasters
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

void PdfCanvas::trimRasters()
{
    if (m_rasterBytes <= m_rasterBudget)
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

    const qint64 before = m_rasterBytes;
    for (int i = 0; i < m_rasterLru.size() && m_rasterBytes > m_rasterBudget;) {
        const int page = m_rasterLru.at(i);
        if (visible.contains(page)) {   // never evict a visible page
            ++i;
            continue;
        }
        m_rasterBytes -= m_rasters.value(page).bytes;
        m_rasters.remove(page);
        m_rasterLru.removeAt(i);
    }
    if (m_rasterBytes < before)
        AppLog::write(QStringLiteral("cache"),
                      QStringLiteral("淘汰 %1 MB，瓦片缓存 %2 MB / 上限 %3 MB")
                          .arg(double(before - m_rasterBytes) / 1048576.0, 0, 'f', 1)
                          .arg(double(m_rasterBytes) / 1048576.0, 0, 'f', 1)
                          .arg(double(m_rasterBudget) / 1048576.0, 0, 'f', 0));
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
        // Per-point width factors come from the finger's contact area. The vector is
        // parallel to `pts` for strokes that have them; empty means a uniform stroke.
        bool weighted = s.wf.size() == s.pts.size();
        if (weighted) {
            bool varied = false;
            for (qreal f : s.wf) {
                if (qAbs(f - 1.0) > 0.01) {
                    varied = true;
                    break;
                }
            }
            weighted = varied;      // all-1.0 keeps the cheaper uniform-pen rendering
        }
        const auto factorAt = [&s, weighted](int i) {
            return weighted ? s.wf.at(i) : 1.0;
        };
        // A lone sample is a tap, and a tap is a dot: a round-capped pen over zero length
        // paints exactly one. Taps used to disappear because this function demanded two
        // points and endInput() discarded the stroke - see the commit rule there.
        if (s.pts.size() == 1) {
            p.setPen(QPen(s.color,
                          qMax<qreal>(0.5, s.width * factorAt(0) * rect.width()),
                          Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPoint(device(s.pts.first()));
            return;
        }
        if (weighted) {
            // A filled outline whose width follows the contact area: the touch-screen
            // version of pen pressure. Drawn as one path, so the joins have no seams.
            QVector<QPointF> dev;
            dev.reserve(s.pts.size());
            for (const QPointF &n : s.pts)
                dev.append(device(n));
            const QPainterPath outline =
                inkOutlinePath(dev, s.wf, qMax<qreal>(0.5, s.width * rect.width()));
            if (!outline.isEmpty()) {
                p.setPen(Qt::NoPen);
                p.setBrush(s.color);
                p.drawPath(outline);
                return;
            }
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

    // Live samples repaint a small rect, but the ink model keeps every stroke on the
    // page. Building a QPainterPath for strokes that cannot touch that rect is pure
    // cost, and it grows as the lesson goes on - the page gets slower to write on.
    // Skip those by bounding box. A null box means the extent is unknown, so the
    // stroke is drawn (correctness never depends on the cache being complete).
    const QRectF clip = p.hasClipping() ? p.clipBoundingRect() : QRectF();
    const auto mayTouchClip = [&rect, &clip](const Stroke &s) {
        if (clip.isEmpty() || s.box.isNull())
            return true;
        const QRectF box(rect.left() + s.box.left()   * rect.width(),
                         rect.top()  + s.box.top()    * rect.height(),
                         s.box.width()  * rect.width(),
                         s.box.height() * rect.height());
        const qreal pad = qMax<qreal>(2.0, s.width * rect.width() * 0.5
                                              * kInkMaxWidthFactor) + 1.0;
        return box.adjusted(-pad, -pad, pad, pad).intersects(clip);
    };

    if (it != m_ink.constEnd()) {
        for (const Stroke &s : it.value()) {
            if (mayTouchClip(s))
                drawStroke(s);
        }
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
    if (m_touchInkBlocked || m_touchActive || m_pinchActive || m_moveDragActive)
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
    // 掌擦时圆环就是动态大小的读数，触摸锁（挡住合成鼠标事件）不该把它藏起来。
    if (m_palmEraseActive)
        return true;
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
        dirty = eraserIndicatorBounds(from, activeEraseRadius());
    if (hasTo) {
        const QRect now = eraserIndicatorBounds(to, activeEraseRadius());
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

    // 按下反馈：静态橡皮拖动、或手掌按在屏上时，圆环内淡淡着色、图标下压。
    const bool pressed = m_erasing || m_palmEraseActive;
    if (pressed) {
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
    const qreal press = pressed ? 1.0 : 0.0;
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
    QPainter p(viewport());
    paintForViewport(p);
}

void PdfCanvas::paintForViewport(QPainter &p)
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

        // A clip *path* is not free: the raster engine scan-converts the whole
        // page-sized rounded rect into a region on every paint. Measured at 4K this is
        // ~5 ms even when the dirty rect is only 140x90 - a fixed cost on EVERY live ink
        // sample, which is exactly what makes handwriting trail the finger. The rounded
        // corners only matter when the repainted rect actually reaches one, so the clip
        // is only applied then. A full repaint (no clip) still gets it.
        const bool clipped = p.hasClipping();
        const QRectF clipRect = clipped ? p.clipBoundingRect() : QRectF();
        bool needCornerClip = false;
        if (m_pageRadius > 0.0) {
            if (!clipped) {
                needCornerClip = true;          // full repaint: every corner is painted
            } else if (!clipRect.isEmpty()) {
                const qreal rad = m_pageRadius;
                const QRectF corners[4] = {
                    QRectF(r.left(),        r.top(),         rad, rad),
                    QRectF(r.right() - rad, r.top(),         rad, rad),
                    QRectF(r.left(),        r.bottom() - rad, rad, rad),
                    QRectF(r.right() - rad, r.bottom() - rad, rad, rad),
                };
                for (const QRectF &c : corners)
                    needCornerClip = needCornerClip || c.intersects(clipRect);
            }
        }

        p.save();
        if (needCornerClip)
            p.setClipPath(paper);
        // Tiles are 1:1 at the current zoom, so the normal path blits them without
        // resampling. During a pinch the previous scale's tiles (and the low-res
        // preview under them) are knowingly stale, so drop the smoothing and
        // re-render crisply once the gesture settles.
        p.setRenderHint(QPainter::SmoothPixmapTransform, !m_pinchActive);
        drawPageRaster(p, page, r);
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
        drawEraserIndicator(p, m_eraserHoverPos, activeEraseRadius());
}

QPixmap PdfCanvas::freezeFramePixmap()
{
    return grab();
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

// Scrolling used to repaint the whole viewport. At 4K that is a full page blit
// (~25 ms measured), so dragging the document felt like 35 fps while a whiteboard
// app feels like 60. The page content is static at a given zoom, so the already
// painted pixels can be moved by the backing store and only the newly exposed
// strip has to be painted (QWidget::scroll does exactly that).
void PdfCanvas::scrollContentsBy(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        if (m_toolbar)
            m_toolbar->reposition();
        viewport()->update();
        return;
    }

    // The scrollbar delta is opposite to the direction the content travels.
    // QWidget::scroll also moves the viewport's child widgets (the island lives on
    // the viewport), so reposition it afterwards to put it back where it belongs.
    viewport()->scroll(-dx, -dy);
    if (m_toolbar)
        m_toolbar->reposition();

    // The desk gradient at the viewport's top edge is anchored to the viewport
    // (not to the document), so it must not travel with the blit.
    const int strip = int(qCeil(qMax<qreal>(12.0, m_ui.icon * 1.4)));
    QRegion repaint(QRect(0, 0, viewport()->width(), strip));
    // The eraser ring is screen-space (it follows the pointer, not the page), so
    // put it back where it was instead of letting the blit carry it away.
    if (m_eraserHover)
        repaint += eraserIndicatorBounds(m_eraserHoverPos, activeEraseRadius());
    viewport()->update(repaint);
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
    setDrawing(false);
    m_erasing     = false;
    m_erasePushed = false;
    m_liveEraseRadius = 0.0;
    m_hasLastErasePos = false;
    m_moveDragActive  = false;
    // 手势被别处取消（切工具 / 撤销 / 关文档）时，触摸占用也一并作废：否则一个已经
    // 不会再收到 TouchEnd 的序列会把鼠标左键永久挡在门外。
    m_touchActive = false;
    m_palmSequence = false;
    m_sequenceWrote = false;
    m_drawPage    = -1;
    m_current     = Stroke{};
    // 掌擦手势与它的动态半径一并不留。分类器状态 m_touchClass 故意不动：
    // 那是"这只手还在屏上"的记忆，清掉它会让掌擦每隔一帧重新进入一次
    // （点数迟滞被反复重置），手势就闪了。真正需要重置的地方只有
    // openPdf/closePdf 和 TouchCancel，那里显式清。
    m_palmEraseActive = false;
    m_hasPalmLastPos  = false;
    m_palmRadius      = 0.0;
}

// The OS cancelled the touch stream. A finger stroke that was already drawn is KEPT:
// the teacher meant to write, and silently deleting it is what reads as "the stroke
// flashes white" (the previous rule - "a cancel must never commit ink" - was written
// against stray ink from a system gesture, but it also deletes deliberate writing that
// Windows cancelled for its own reasons). A pinch / palm / eraser gesture still ends
// without committing anything.
void PdfCanvas::endCancelledTouch()
{
    // A stroke belongs to the touch stream when this sequence set the flag (or when the
    // stroke itself came from a finger); a stylus stroke that happens to be running is
    // left to its own release event.
    const bool keepStroke =
        m_drawing && !m_touchInkBlocked && (m_touchActive || m_strokeFromTouch);
    m_touchActive = false;
    if (keepStroke) {
        if (m_touchSmooth.hasPending())
            moveInputTo(m_touchSmooth.flush());
        endInput();                 // keeps the < 3 px touch-noise rule
    } else
        cancelGesture();
    // 被系统打断：触摸记忆作废，下一次落指从干净状态重新分类。
    m_touchClass = TouchClassState{};
    m_pinchActive = false;
    m_pinchPts.clear();
    if (m_touchRelease)
        m_touchRelease->start();    // keep the lock a bit longer
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
    s.box = strokeBounds(s.pts);
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

QPointF PdfCanvas::testStrokeFirstPoint(int page, int index) const
{
    const auto it = m_ink.constFind(page);
    if (it == m_ink.constEnd() || index < 0 || index >= it.value().size())
        return QPointF(-1.0, -1.0);
    const Stroke &s = it.value().at(index);
    return s.pts.isEmpty() ? QPointF(-1.0, -1.0) : s.pts.first();
}

int PdfCanvas::testStrokeWidthCount(int page, int index) const
{
    const auto it = m_ink.constFind(page);
    if (it == m_ink.constEnd() || index < 0 || index >= it.value().size())
        return -1;
    return int(it.value().at(index).wf.size());
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
    return eraseAtPointer(page, vp, m_eraserRadius);
}

bool PdfCanvas::testEraseSweepNormalized(int page, const QPointF &aNorm, const QPointF &bNorm)
{
    const QRectF r = pageRectInViewport(page);
    if (r.isEmpty())
        return false;
    auto vp = [&r](const QPointF &n) {
        return QPointF(r.left() + n.x() * r.width(), r.top() + n.y() * r.height());
    };
    return eraseSweep(page, vp(aNorm), vp(bNorm), m_eraserRadius);
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

// --- 橡皮大小（静态三档）---------------------------------------------------

const qreal *PdfCanvas::eraserRadiusSteps()
{
    return kEraserRadiusSteps;
}

int PdfCanvas::eraserRadiusStepCount()
{
    return int(sizeof(kEraserRadiusSteps) / sizeof(kEraserRadiusSteps[0]));
}

void PdfCanvas::setEraserRadius(qreal radiusPx)
{
    // 面板只提供三档，所以任何值都吸附到最近的一档：越界的落到首/末档，
    // 不会出现"当前值不对应任何按钮"的状态。
    const qreal *steps = eraserRadiusSteps();
    qreal best = steps[0];
    qreal bestDist = qAbs(radiusPx - best);
    for (int i = 1; i < eraserRadiusStepCount(); ++i) {
        const qreal d = qAbs(radiusPx - steps[i]);
        if (d < bestDist) {
            best = steps[i];
            bestDist = d;
        }
    }
    if (qFuzzyCompare(m_eraserRadius, best))
        return;

    // 指针处的圆环正在显示：新旧两个尺寸的脏区都要重画，否则换档后旧环留一圈残影。
    const QRect oldRing = m_eraserHover
                              ? eraserIndicatorBounds(m_eraserHoverPos, m_eraserRadius)
                              : QRect();
    m_eraserRadius = best;
    if (!oldRing.isNull())
        viewport()->update(oldRing.united(eraserIndicatorBounds(m_eraserHoverPos, m_eraserRadius)));
    emit eraserChanged();
}

qreal PdfCanvas::activeEraseRadius() const
{
    if (m_palmEraseActive && m_palmRadius > 0.0)
        return m_palmRadius;
    if (m_erasing && m_liveEraseRadius > 0.0)
        return m_liveEraseRadius;       // contact-area adaptive fingertip eraser
    return m_eraserRadius;
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
bool PdfCanvas::eraseSweep(int pageHint, const QPointF &from, const QPointF &to, qreal radiusPx)
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
    eraseLog(QStringLiteral("sweep (%1,%2)->(%3,%4) dist=%5 r=%6 %7")
                 .arg(from.x(), 0, 'f', 1).arg(from.y(), 0, 'f', 1)
                 .arg(to.x(), 0, 'f', 1).arg(to.y(), 0, 'f', 1)
                 .arg(dist, 0, 'f', 1)
                 .arg(radiusPx, 0, 'f', 1)
                 .arg(dist > kEraserMaxJumpPx ? QStringLiteral("JUMP")
                                              : QStringLiteral("ok")));

    bool any = false;
    const QPointF dd = to - a;
    const qreal len = qSqrt(QPointF::dotProduct(dd, dd));
    // 步长跟着擦除半径走：半径越大，相邻擦除圆之间越不能留缝。
    const qreal step = qMax<qreal>(2.0, radiusPx * 0.5);
    const int steps = qMax(1, int(qCeil(len / step)));
    for (int i = 0; i <= steps; ++i) {
        const QPointF p = a + dd * (qreal(i) / qreal(steps));
        int page = pageAtViewportPos(p);
        if (page < 0)
            page = pageHint;                 // keep erasing the gesture's page
        if (page >= 0 && eraseAtPointer(page, p, radiusPx))
            any = true;
    }
    return any;
}

// Partial ("erase exactly where you point") eraser, evaluated in page device
// pixels. Each stroke is cut where it crosses the eraser circle (radius
// `radiusPx`, the caller's selected / live size, plus half the stroke width so
// a fat stroke is only cut when the circle really touches its body): the part
// inside is removed and the outer parts survive as separate strokes, so one
// stroke can end up split in two.
bool PdfCanvas::eraseAtPointer(int page, const QPointF &viewportPos, qreal radiusPx)
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

    // Fast rejection (CVTE patent CN122816524A's efficiency point in spirit): a sweep
    // calls this several times per input sample, and a lesson-sized page carries
    // thousands of samples. If no stroke's box can reach the eraser circle, there is
    // nothing to rebuild - skip the whole copy/intersect pass.
    {
        bool anyNear = false;
        for (const Stroke &s : strokes) {
            if (s.box.isNull()) {
                anyNear = true;
                break;
            }
            const qreal pad = radiusPx + s.width * rect.width() * 0.5 * kInkMaxWidthFactor;
            const QRectF box(rect.left() + s.box.left() * rect.width(),
                             rect.top() + s.box.top() * rect.height(),
                             s.box.width() * rect.width(),
                             s.box.height() * rect.height());
            if (box.adjusted(-pad, -pad, pad, pad).contains(center)) {
                anyNear = true;
                break;
            }
        }
        if (!anyNear)
            return false;
    }

    // A cut piece carries its own points AND its own width factors: the factors are
    // parallel to the points, so they have to be cut with them. Both are interpolated
    // at the crossing, otherwise a thickened stroke would snap back to base width at
    // the new ends.
    struct CutRun {
        QVector<QPointF> pts;
        QVector<qreal> wf;
    };

    QVector<Stroke> rebuilt;
    rebuilt.reserve(strokes.size() + 2);
    bool changed = false;

    for (const Stroke &s : strokes) {
        const bool hasWidths = s.wf.size() == s.pts.size();
        const qreal R = radiusPx + s.width * rect.width() * 0.5 * kInkMaxWidthFactor;
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
        QVector<qreal> devW;
        dev.reserve(s.pts.size());
        if (hasWidths)
            devW.reserve(s.pts.size());
        for (int i = 0; i < s.pts.size(); ++i) {
            dev.append(toDevice(s.pts.at(i)));
            if (hasWidths)
                devW.append(s.wf.at(i));
        }
        const auto widthAt = [&devW, hasWidths](int i) {
            return hasWidths ? devW.at(i) : 1.0;
        };
        const auto widthAtT = [&devW, hasWidths](int i, qreal t) {
            return hasWidths ? devW.at(i) + (devW.at(i + 1) - devW.at(i)) * t : 1.0;
        };

        QVector<CutRun> runs;
        CutRun cur;
        const auto push = [&cur, hasWidths](const QPointF &p, qreal w) {
            cur.pts.append(p);
            if (hasWidths)
                cur.wf.append(w);
        };
        const auto flush = [&runs, &cur]() {
            if (cur.pts.size() >= 2)
                runs.append(cur);
            cur = CutRun{};
        };

        bool prevInside = inside(dev.first());
        if (!prevInside)
            push(dev.first(), widthAt(0));

        for (int i = 0; i + 1 < dev.size(); ++i) {
            const QPointF a = dev.at(i);
            const QPointF b = dev.at(i + 1);
            const bool bInside = inside(b);
            qreal t0 = 0.0, t1 = 0.0;
            const bool crosses = circleSegmentInterval(a, b, center, R, &t0, &t1);

            if (crosses) {
                if (!prevInside && t0 > 0.0) {
                    if (cur.pts.isEmpty())
                        push(a, widthAt(i));
                    push(a + t0 * (b - a), widthAtT(i, t0));   // last point before the cut
                }
                flush();
                if (!bInside && t1 < 1.0) {
                    push(a + t1 * (b - a), widthAtT(i, t1));   // first point after the cut
                    push(b, widthAt(i + 1));                    // b is outside: keep the tail
                }
            } else if (!prevInside && !bInside) {
                push(b, widthAt(i + 1));
            } else {
                flush();
            }
            prevInside = bInside;
        }
        flush();

        if (runs.size() == 1 && runs.first().pts.size() == dev.size()) {
            rebuilt.append(s);          // untouched by this erase
            continue;
        }

        changed = true;
        for (const CutRun &run : runs) {
            Stroke ns = s;
            ns.pts.clear();
            ns.wf.clear();
            ns.pts.reserve(run.pts.size());
            for (const QPointF &d : run.pts)
                ns.pts.append(toNorm(d));
            if (hasWidths && run.wf.size() == run.pts.size())
                ns.wf = run.wf;
            ns.box = strokeBounds(ns.pts);      // the cut piece has its own extent
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
    viewport()->update(eraserIndicatorBounds(viewportPos, radiusPx));
    emit inkChanged(strokeCount());
    return true;
}

// --- 掌擦（手掌接触面橡皮）的手势执行 --------------------------------------
// 分类器（classifyTouch）已经把"这一帧是不是手掌"和"该用多大半径"算好了，
// 这里只执行：取消在跑的笔迹/缩放/平移，把指示环跟到簇心，并沿路径擦过来。
// 定义在文件后部（匿名命名空间里，掌擦分类器旁边）；这里提前声明同一个函数。
namespace {
void resetPalmClass(PdfCanvas::TouchClassState &state);
}   // namespace

void PdfCanvas::setPalmEraserEnabled(bool on)
{
    if (m_palmEraserEnabled == on)
        return;
    m_palmEraserEnabled = on;
    // 关掉时必须立刻收手：否则这一簇会继续当橡皮擦，而开关已经关了。
    if (!on && m_palmEraseActive)
        endPalmErase();
    resetPalmClass(m_touchClass);   // 不留旧滞回，重新打开时从干净状态开始
}

void PdfCanvas::palmEraseTo(const QPointF &centroid, qreal radiusPx)
{
    if (!m_palmEraseActive) {
        // 手掌的命令只有"擦"：笔迹、双指缩放、自由移动全部让位。
        cancelGesture();
        m_pinchActive = false;
        m_pinchPts.clear();
        m_palmEraseActive = true;
        m_hasPalmLastPos = false;
        m_erasePushed = false;
        // 触摸锁：Windows 会为触摸合成鼠标事件，没有它一次合成按下就会画出
        // 笔迹；滚轮也靠这个锁在掌擦期间不缩放（见 handleWheel）。
        m_touchInkBlocked = true;
        if (m_touchRelease)
            m_touchRelease->stop();
    }

    // 半径可能变小：先按旧半径补一块脏区，否则上一帧的圆环会留一圈残影。
    const qreal prevRadius = m_palmRadius > 0.0 ? m_palmRadius : m_eraserRadius;
    m_palmRadius = radiusPx;
    if (m_eraserHover)
        viewport()->update(eraserIndicatorBounds(m_eraserHoverPos, qMax(prevRadius, radiusPx)));

    setEraserHover(centroid);      // 环跟手，activeEraseRadius() 已经是新半径

    const QPointF from = m_hasPalmLastPos ? m_palmLastPos : centroid;
    eraseSweep(pageAtViewportPos(centroid), from, centroid, radiusPx);
    m_palmLastPos = centroid;
    m_hasPalmLastPos = true;
}

void PdfCanvas::endPalmErase()
{
    if (!m_palmEraseActive)
        return;
    m_palmEraseActive = false;
    m_hasPalmLastPos = false;
    m_palmRadius = 0.0;
    if (m_erasePushed) {
        // 一次掌擦手势 = 一步撤销（和普通擦除拖动一致）。
        m_erasePushed = false;
        emit inkChanged(strokeCount());
        emit undoStateChanged();
    }
    viewport()->update();
}

// --- shared input entry points --------------------------------------------
// Mouse and touch both funnel through these, so pen / eraser / pinch behave
// identically whichever input produced them.

void PdfCanvas::beginInputAt(const QPointF &viewportPos, bool asEraser, qreal widthFactor,
                             qreal eraseRadiusPx)
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
        m_liveEraseRadius = eraseRadiusPx > 0.0 ? eraseRadiusPx : 0.0;
        const qreal wipe = m_liveEraseRadius > 0.0 ? m_liveEraseRadius : m_eraserRadius;
        m_lastErasePos = viewportPos;
        m_hasLastErasePos = true;
        eraseAtPointer(page, viewportPos, wipe);     // never starts an ink stroke
        return;
    }

    setDrawing(true);
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
    for (int i = first; i < m_current.pts.size(); ++i)
        m_current.wf.append(widthFactor);
    if (m_current.pts.size() == first)
        return;
    // Same partial repaint as the rest of the stroke: a full-viewport update here
    // would repaint the whole 4K page just to show the first dot. The pad uses the
    // widest factor the brush is allowed to produce, so a thick sample is never cut.
    const QRectF pr = pageRectInViewport(page);
    const qreal inkPx = qMax<qreal>(1.0, m_current.width * pr.width());
    viewport()->update(inkDirtyRect(pr, m_current.pts.constData(), m_current.pts.size(),
                                    0, inkPx * 0.5 * kInkMaxWidthFactor + 4.0));
}

void PdfCanvas::moveInputTo(const QPointF &viewportPos, qreal widthFactor)
{
    if (m_erasing) {
        if (!m_hasLastErasePos) {
            m_lastErasePos = viewportPos;
            m_hasLastErasePos = true;
        }
        // Erase along the whole travelled path, not just at this sample.
        const qreal wipe = m_liveEraseRadius > 0.0 ? m_liveEraseRadius : m_eraserRadius;
        setEraserHover(viewportPos);
        eraseSweep(pageAtViewportPos(viewportPos), m_lastErasePos, viewportPos, wipe);
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
    for (int i = first; i < m_current.pts.size(); ++i)
        m_current.wf.append(widthFactor);
    if (m_current.pts.size() == first)
        return;
    const QRectF pr = pageRectInViewport(m_drawPage);
    const qreal inkPx = qMax<qreal>(1.0, m_current.width * pr.width());
    viewport()->update(inkDirtyRect(pr, m_current.pts.constData(), m_current.pts.size(),
                                    first - 2, inkPx * 0.5 * kInkMaxWidthFactor + 4.0));
}

void PdfCanvas::endInput()
{
    // The pen is up: sharpen the page again IF this stroke produced a coarse raster
    // (see beginInputAt). Starting the timer unconditionally re-rendered a whole 4K
    // page after every single stroke - 48-60 ms of UI-thread freeze per stroke.
    if (m_zoomSettle && m_settleNeeded)
        m_zoomSettle->start();

    // A two-finger gesture owns the input: never commit a stroke while the ink
    // lock is on (this is where synthesized mouse releases used to leak ink).
    if (m_touchInkBlocked) {
        cancelGesture();
        return;
    }
    if (m_erasing) {
        m_erasing = false;
        m_liveEraseRadius = 0.0;
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
    setDrawing(false);
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
        m_current.box = strokeBounds(m_current.pts);   // extent for drawInk's culling
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
        // Right / middle click: nothing on the page uses them, and a held finger is
        // delivered as a right click (see CanvasMessageFilter).
        e->accept();
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
    if (e->button() != Qt::LeftButton) {
        e->accept();
        return;
    }
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
            // Contact-area widths are optional: only write them when they actually
            // vary, so uniform strokes keep the old, smaller file format.
            if (s.wf.size() == s.pts.size()) {
                bool varied = false;
                for (qreal f : s.wf) {
                    if (qAbs(f - 1.0) > 0.01) {
                        varied = true;
                        break;
                    }
                }
                if (varied) {
                    QJsonArray wf;
                    for (qreal f : s.wf)
                        wf.append(f);
                    so.insert(QStringLiteral("wf"), wf);
                }
            }
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
            const QJsonArray wf = so.value(QStringLiteral("wf")).toArray();
            if (wf.size() == s.pts.size()) {
                s.wf.reserve(wf.size());
                for (const QJsonValue &f : wf)
                    s.wf.append(f.toDouble(1.0));
            }
            s.box = strokeBounds(s.pts);
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
        m_settleNeeded = true;      // the bitmaps are still at the previous scale
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
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove: {
        const auto *me = static_cast<QMouseEvent *>(e);
        // The page has no right-click / middle-click action, so those never reach the
        // mouse handlers (a held finger is delivered as a right click).
        if (e->type() != QEvent::MouseMove && me->button() != Qt::LeftButton) {
            e->accept();
            return true;
        }
        // Windows synthesizes mouse events for a touch contact. Feeding them to the ink
        // path is not just redundant: a press calls beginInputAt(), which throws the live
        // stroke away and restarts it at the mouse position, and a release commits it
        // early. That is what punched white gaps into handwriting mid-stroke.
        if (m_touchActive) {
            e->accept();
            return true;
        }
        break;
    }
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

// --- 掌擦分类器（纯函数，逐帧驱动）-----------------------------------------
// 所有参数都在文件顶部的"掌擦分类器参数"块里；这里只有逻辑。
namespace {

// 按距离把接触点聚簇（单链并查集）：两点距离不超过 kPalmClusterDistPx 就算
// 同一簇。面板最多十来个点，O(n²) 足够，不需要 k-d 树。
QVector<QVector<QPointF>> clusterTouchPoints(const QVector<QPointF> &pts)
{
    const int n = int(pts.size());
    QVector<int> parent(n);
    for (int i = 0; i < n; ++i)
        parent[i] = i;
    auto root = [&parent](int i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];      // 路径压缩
            i = parent[i];
        }
        return i;
    };
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (QLineF(pts.at(i), pts.at(j)).length() <= kPalmClusterDistPx) {
                const int a = root(i);
                const int b = root(j);
                if (a != b)
                    parent[a] = b;
            }
        }
    }

    QVector<QVector<QPointF>> clusters;
    QHash<int, int> index;                      // 根 -> clusters 下标
    for (int i = 0; i < n; ++i) {
        const int r = root(i);
        const auto it = index.constFind(r);
        if (it == index.constEnd()) {
            index.insert(r, int(clusters.size()));
            clusters.append(QVector<QPointF>{ pts.at(i) });
        } else {
            clusters[it.value()].append(pts.at(i));
        }
    }
    return clusters;
}

QPointF pointsCentroid(const QVector<QPointF> &pts)
{
    if (pts.isEmpty())
        return {};
    QPointF c;
    for (const QPointF &p : pts)
        c += p;
    return c / qreal(pts.size());
}

void resetPalmClass(PdfCanvas::TouchClassState &state)
{
    state.palmFrames = 0;
    state.palmActive = false;
    state.hasRadius = false;
    state.hasRest = false;
}

// A touch sequence may only become a palm eraser if it STARTED as a palm/hand press.
// A single contact never qualifies: a felt pen tip and a finger are the same signal,
// and a pen pressing harder must not turn the stroke into an eraser mid-writing.
bool palmLikeBegin(const QVector<QPointF> &pts, const QVector<qreal> &diameters,
                   const PdfCanvas::TouchClassState &state)
{
    if (pts.size() >= kPalmEnterPoints)
        return true;                        // 4+ contacts in the first frame: not a pen
    if (pts.size() >= 2 && diameters.size() == pts.size() && state.hasFingerDiam) {
        bool anyPalm = false;
        bool anyFinger = false;
        for (qreal d : diameters) {
            if (d >= state.fingerDiam * kPalmSizeRatio)
                anyPalm = true;
            else
                anyFinger = true;
        }
        return anyPalm && !anyFinger;       // several contacts, all palm-sized
    }
    return false;
}

// 半径防抖三件套（单帧限幅、EMA、死区）与“按住不动就冻结”，两种来源共用：
// 簇的圆周半径（面板不上报接触面积时）与接触椭圆半径（纯触摸面板上报时）。
qreal smoothPalmRadius(PdfCanvas::TouchClassState &state, qreal rawRadius,
                       const QPointF &centroid)
{
    const qreal raw = qBound(kPalmRadiusMinPx, rawRadius, kPalmRadiusMaxPx);

    if (!state.hasRest) {
        state.restCentroid = centroid;
        state.hasRest = true;
    }
    state.restCentroid += (centroid - state.restCentroid) * kPalmRestAlpha;
    const bool atRest = QLineF(centroid, state.restCentroid).length() < kPalmRestMovePx;

    if (!state.hasRadius) {
        state.radius = raw;          // 进入的那一刻就用真实大小，不做 EMA 爬升
        state.hasRadius = true;
    } else if (!atRest) {
        const qreal step = qBound(-kPalmRadiusRateCap, raw - state.radius, kPalmRadiusRateCap);
        const qreal next = state.radius + step * kPalmRadiusAlpha;
        if (qAbs(next - state.radius) >= kPalmRadiusDeadPx)
            state.radius = next;
    }
    return qBound(kPalmRadiusMinPx, state.radius, kPalmRadiusMaxPx);
}

// 面板不上报接触面积时：手掌半径 = 簇心到最远点的距离 + 手指接触面的余量。
qreal updatePalmRadius(PdfCanvas::TouchClassState &state, const QVector<QPointF> &cluster,
                       const QPointF &centroid)
{
    qreal spread = 0.0;
    for (const QPointF &p : cluster)
        spread = qMax(spread, QLineF(centroid, p).length());
    return smoothPalmRadius(state, spread + kPalmRadiusPadPx, centroid);
}

}   // namespace

// 一帧接触点的四选一分类。规则（参数见文件顶部）：
//   * 空点集                                   -> Idle
//   * 多簇：恰好一个"手掌簇"（>= kPalmEnterPoints 点）且其余都是单点
//                                              -> Writing（手掌 + 书写手指）
//     其它多簇                                  -> Pinch（原有双指路径）
//   * 单簇：1 点                                -> Writing
//     已进入掌擦且 >= kPalmStayPoints 点          -> PalmEraser（点数抖动不打断）
//     未进入、一簇 >= kPalmEnterPoints 点且连续
//       kPalmEnterFrames 帧                      -> PalmEraser（进入）
//     其余（2-3 点、未进入）                     -> Writing，但无可落笔点
//        （这一帧什么都不做，等下一帧看清）
// `palmEraserEnabled` = false（设置里关掉了手掌擦除）时，上面所有会变成
// PalmEraser 的单簇分支都改成"什么都不做"的 Writing；多簇（双指）与单点
// 书写的路径完全不受开关影响。
PdfCanvas::TouchClassResult PdfCanvas::classifyTouch(const QVector<QPointF> &pts,
                                                     TouchClassState &state,
                                                     bool palmEraserEnabled)
{
    return classifyTouch(pts, QVector<qreal>(), state, palmEraserEnabled);
}

PdfCanvas::TouchClassResult PdfCanvas::classifyTouch(const QVector<QPointF> &pts,
                                                     const QVector<qreal> &diameters,
                                                     TouchClassState &state,
                                                     bool palmEraserEnabled)
{
    TouchClassResult r;
    if (pts.isEmpty()) {
        resetPalmClass(state);
        return r;                              // Idle
    }

    // --- 接触面积规则（纯触摸面板，专利 CN119917011A）------------------------
    // 面板上报接触椭圆时，这一条比“数点/聚簇”可靠得多：同一帧里最小的接触是手指，
    // 明显更大的接触是手掌/手背。基准是最近接触尺寸的中位数，所以不依赖面板单位、
    // DPI 或缩放。**单触点永远不按面积判手掌**：毛毡笔尖和手指是同一种信号，笔尖
    // 按重一点接触面积就会变大，那正是“写着写着变橡皮”的来路。没有上报（全 0）时
    // 整段跳过，走下面的点数/聚簇规则。
    if (diameters.size() == pts.size()) {
        qreal minD = 0.0;
        for (qreal d : diameters) {
            if (d > 0.0)
                minD = (minD <= 0.0) ? d : qMin(minD, d);
        }
        if (minD > 0.0) {
            // Finger reference: the median of a short recent window. A median (not the
            // minimum) keeps a normal finger "normal" even when pressure makes its
            // contact 1.5-2x bigger. Freezing the window while the palm is erasing
            // keeps the reference at finger size.
            if (!state.hasFingerDiam
                || minD <= state.fingerDiam * kPalmSizeRatio) {   // never feed a palm
                state.fingerWindow[state.fingerPos] = minD;
                state.fingerPos = (state.fingerPos + 1) % TouchClassState::kFingerWindow;
                if (state.fingerCount < TouchClassState::kFingerWindow)
                    ++state.fingerCount;
                qreal vals[TouchClassState::kFingerWindow];
                for (int i = 0; i < state.fingerCount; ++i)
                    vals[i] = state.fingerWindow[i];
                std::sort(vals, vals + state.fingerCount);
                state.fingerDiam = vals[state.fingerCount / 2];
                state.hasFingerDiam = true;
            }
            // The reference is fed by any single contact too (a pen/finger press is the
            // main calibration source); only the PALM DECISION is skipped for one
            // contact - a felt pen pressing harder must never become an eraser.
            const bool canJudgePalm = (pts.size() >= 2);
            const qreal ratio = state.palmActive ? kPalmSizeStayRatio : kPalmSizeRatio;
            const qreal palmMin = state.fingerDiam * ratio;
            int palmCount = 0;
            int fingerCount = 0;
            QPointF palmAt;
            QPointF fingerAt;
            qreal palmDiam = 0.0;
            for (int i = 0; i < pts.size(); ++i) {
                if (diameters.at(i) >= palmMin) {
                    ++palmCount;
                    palmAt += pts.at(i);
                    palmDiam = qMax(palmDiam, diameters.at(i));
                } else {
                    ++fingerCount;
                    fingerAt += pts.at(i);
                }
            }
            if (canJudgePalm && palmCount > 0 && fingerCount == 0) {
                // 只有手掌/手背：擦。半径直接取接触椭圆的半径（再平滑），比用
                // 簇心到最远点的距离更接近“手掌有多大就擦多大”。
                if (!palmEraserEnabled) {
                    resetPalmClass(state);
                    r.mode = TouchClass::Writing;
                    r.hasWritePos = false;
                    return r;
                }
                if (++state.palmFrames >= kPalmEnterFrames) {
                    state.palmActive = true;
                    r.mode = TouchClass::PalmEraser;
                    r.radiusPx = smoothPalmRadius(state, palmDiam * 0.5 + kPalmSizePadPx,
                                                  palmAt / qreal(palmCount));
                } else {
                    r.mode = TouchClass::Writing;   // 幽灵帧：先看清，不动手
                    r.hasWritePos = false;
                }
                return r;
            }
            if (palmCount > 0) {
                // 手掌 + 若干手指：一根手指就写字（掌托防误触）；多根交给双指逻辑。
                if (canJudgePalm && fingerCount == 1) {
                    resetPalmClass(state);
                    r.mode = TouchClass::Writing;
                    r.writePos = fingerAt;
                    r.hasWritePos = true;
                    return r;
                }
            } else {
                // 全是手指大小的接触：清掉掌擦状态，走原来的点数/聚簇规则。
                resetPalmClass(state);
            }
        }
    }

    const QVector<QVector<QPointF>> clusters = clusterTouchPoints(pts);

    if (clusters.size() >= 2) {
        int palmClusters = 0;
        bool othersSingle = true;
        for (const QVector<QPointF> &c : clusters) {
            if (c.size() >= kPalmEnterPoints)
                ++palmClusters;
            else if (c.size() > 1)
                othersSingle = false;
        }
        if (palmClusters == 1 && othersSingle) {
            // 手掌搁在屏上、手指在旁边写字：这一帧不是掌擦，把书写点交给落笔路径。
            QVector<QPointF> writing;
            for (const QVector<QPointF> &c : clusters) {
                if (c.size() < kPalmEnterPoints)
                    writing += c;
            }
            r.mode = TouchClass::Writing;
            r.writePos = pointsCentroid(writing);
            r.hasWritePos = !writing.isEmpty();
        } else {
            r.mode = TouchClass::Pinch;
        }
        resetPalmClass(state);
        return r;
    }

    const QVector<QPointF> &cluster = clusters.first();
    const QPointF centroid = pointsCentroid(cluster);

    // 开关关的是"掌擦"，不是手指书写：只有点数够得上手掌的那些簇（>= 2 点）才
    // 走"什么都不做"；单点必须继续落到下面的书写分支，否则关掉开关会连手指
    // 书写一起关掉。
    if (!palmEraserEnabled && cluster.size() >= kPalmStayPoints) {
        // 开关关闭：这一簇本来会被判成掌擦，现在什么都不做 —— 不擦、不画、不缩放。
        // 状态一并清掉，免得重新打开时还带着旧的滞回。
        resetPalmClass(state);
        r.mode = TouchClass::Writing;
        r.hasWritePos = false;
        return r;
    }

    if (cluster.size() < kPalmStayPoints) {
        // 单点 = 书写/点击；<= 1 点也意味着掌擦该结束了。
        resetPalmClass(state);
        r.mode = TouchClass::Writing;
        r.writePos = centroid;
        r.hasWritePos = true;
        return r;
    }

    if (state.palmActive) {
        // 迟滞：点数在 4、5、3、2 之间抖动时一直是掌擦，不来回翻。
        r.mode = TouchClass::PalmEraser;
        r.radiusPx = updatePalmRadius(state, cluster, centroid);
        return r;
    }

    if (cluster.size() >= kPalmEnterPoints) {
        if (++state.palmFrames >= kPalmEnterFrames) {
            state.palmActive = true;
            state.hasRadius = false;           // 进入时按真实大小初始化
            state.hasRest = false;
            r.mode = TouchClass::PalmEraser;
            r.radiusPx = updatePalmRadius(state, cluster, centroid);
            return r;
        }
        // 第一帧只记账：幽灵帧不能开擦。
        r.mode = TouchClass::Writing;
        r.hasWritePos = false;
        return r;
    }

    // 单簇 2-3 点、还没进入掌擦：既不够点开擦，也不是单点书写。
    state.palmFrames = 0;
    r.mode = TouchClass::Writing;
    r.hasWritePos = false;
    return r;
}

bool PdfCanvas::touchBelongsToOverlay(const QVector<QPointF> &viewportPts) const
{
    // A gesture that is already running owns the touch. The island, palette and
    // page grid are children of the viewport, so anything written across them is
    // covered by them on screen - the ink goes underneath, which is exactly what
    // "keep writing" means. Refusing these samples mid-stroke used to set the ink
    // lock, and endInput() then cancelled the WHOLE stroke: one line dragged over
    // the island made the part before it disappear too.
    if (m_drawing || m_erasing || m_palmEraseActive || m_pinchActive || m_moveDragActive)
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
    // Clear even when the sequence is ignored below (no document): leaving the flag set
    // would block left-button input for the rest of the session.
    if (te->type() == QEvent::TouchEnd || te->type() == QEvent::TouchCancel) {
        m_touchActive = false;
        m_palmSequence = false;
        m_sequenceWrote = false;
    }

    // Free move pans the view, so it works on a blank canvas too.
    if (m_tool != InkTool::Move && (!m_doc || m_geom.isEmpty()))
        return false;
    // The only path that feeds real finger input: a finger tap may still be noise, so it
    // keeps the "too short = discard" rule in endInput(). A mouse/stylus tap leaves a dot.
    m_strokeFromTouch = true;
    // Set only for a touch we actually handle, so the flag can never stick on a sequence
    // that was passed on to the base class.
    if (te->type() == QEvent::TouchBegin) {
        m_touchActive = true;
        m_touchSmooth.reset();     // every finger stroke starts from its own raw sample
        m_touchWidthValid = false; // contact-area width starts fresh per stroke
    }

    QVector<QPointF> pts;
    QVector<qreal> diams;       // per-contact size, 0 when the panel does not report it
    for (const QEventPoint &p : te->points()) {
        // Cancelled touches arrive as a TouchCancel event; here a point is
        // either still down or released.
        if (p.state() != QEventPoint::Released) {
            pts.append(p.position());
            const QSizeF e = p.ellipseDiameters();
            diams.append(qMax(e.width(), e.height()));
        }
    }
    InputProbe::instance().addEvent(pts.size());

    // A sequence can only turn into a palm eraser if it STARTED as a palm press. A pen
    // tip / finger sequence must never do that: its own contact can look palm-sized
    // when pressed harder (felt pen == finger on this panel).
    if (te->type() == QEvent::TouchBegin) {
        m_palmSequence = palmLikeBegin(pts, diams, m_touchClass);
        m_sequenceWrote = false;
    }

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

    // The system can take the touch stream away mid-stroke (its own press-and-hold
    // gesture, an edge swipe, a panel hiccup). Throwing the stroke away there is
    // exactly what made handwriting lose chunks - see endCancelledTouch().
    if (te->type() == QEvent::TouchCancel) {
        endCancelledTouch();
        return true;
    }

    // --- 先分类这一帧：掌擦 / 双指 / 书写三条路互斥 -------------------------
    // 分类器只看点集和它自己的跨帧状态（见 classifyTouch），既有双指语义
    // 一分不动；点数抖动的单簇不再被误当成双指。
    TouchClassResult cls = classifyTouch(pts, diams, m_touchClass, m_palmEraserEnabled);
    // 自由移动模式完全不做掌擦（那是"拖动画面"模式）：手掌帧沿用原来的
    // 双指路径，该模式的行为不变。
    if (m_tool == InkTool::Move && cls.mode == TouchClass::PalmEraser)
        cls.mode = TouchClass::Pinch;

    if (cls.mode == TouchClass::PalmEraser) {
        // A sequence that began with a pen/finger, or that already wrote ink, must never
        // turn into an eraser: the writing contact itself can look palm-sized when the
        // felt tip is pressed harder, and a resting palm must not delete handwriting.
        if (!m_palmSequence || m_sequenceWrote || m_drawing || m_erasing)
            return true;
        palmEraseTo(pointsCentroid(pts), cls.radiusPx);
        return true;
    }
    if (m_palmEraseActive)
        endPalmErase();                    // 手掌抬起 / 簇散开 / 点数不够

    if (cls.mode == TouchClass::Pinch) {
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
        // The smoother holds the newest sample back by one to keep its window centred;
        // hand it over so the ink reaches the finger.
        if (m_drawing && m_strokeFromTouch && m_touchSmooth.hasPending())
            moveInputTo(m_touchSmooth.flush());
        endInput();
        // Keep the lock briefly: synthesized mouse events may still trail in.
        if (m_touchRelease)
            m_touchRelease->start();
        m_pinchActive = false;
        m_pinchPts.clear();
        return true;
    }

    // 书写 / 单点：writePos 是"该落笔的那一个点"（手掌 + 手指时是手指，
    // 不是手掌簇）。
    if (cls.hasWritePos) {
        // Finger samples carry a couple of pixels of panel noise; the panel cannot send
        // more of them, so filter the ones it sends (see InkSmooth.h). Free-move panning
        // and the mouse/stylus paths stay raw - a pan has no ink to make hairy and a
        // stylus is precise enough that filtering would only add lag.
        //
        // Contact area -> brush width: the pure-touch stand-in for pressure. The
        // reference is the smallest contact seen this session, so no absolute size or
        // DPI is assumed; a panel that reports nothing leaves the stroke uniform.
        qreal widthFactor = 1.0;
        if (diams.size() == pts.size() && m_touchClass.hasFingerDiam
            && m_touchClass.fingerDiam > 0.0) {
            qreal picked = 0.0;
            qreal best = 1e9;
            for (int i = 0; i < pts.size(); ++i) {
                if (diams.at(i) <= 0.0)
                    continue;
                const qreal dist = QLineF(pts.at(i), cls.writePos).length();
                if (dist < best) {
                    best = dist;
                    picked = diams.at(i);
                }
            }
            if (picked > 0.0) {
                const qreal target = qBound(kInkMinWidthFactor,
                                            picked / m_touchClass.fingerDiam,
                                            kInkMaxWidthFactor);
                if (!m_touchWidthValid) {
                    m_touchWidth = target;
                    m_touchWidthValid = true;
                } else {
                    m_touchWidth += (target - m_touchWidth) * 0.25;
                }
                widthFactor = m_touchWidth;
            }
        }
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
                m_moveLastPos = cls.writePos;
                return true;
            }
            // Eraser tool + a panel that reports contact size: the wipe follows the
            // contact (Seewo-style adaptive eraser). The selected step is the floor.
            qreal eraseRadius = -1.0;
            if (m_tool == InkTool::Eraser && diams.size() == pts.size()) {
                qreal widest = 0.0;
                for (qreal v : diams)
                    widest = qMax(widest, v);
                if (widest > 0.0) {
                    const qreal *steps = eraserRadiusSteps();
                    eraseRadius = adaptiveEraseRadius(m_eraserRadius, widest,
                                                     steps[0], steps[2] * 2.0);
                }
            }
            beginInputAt(m_touchSmooth.push(cls.writePos), false, widthFactor, eraseRadius);
            if (m_drawing)
                m_sequenceWrote = true;
            return true;
        }
        if (m_touchInkBlocked)
            return true;                   // residual finger of a pinch gesture
        if (m_tool == InkTool::Move) {
            freePanTo(cls.writePos);
            return true;
        }
        moveInputTo(m_touchSmooth.push(cls.writePos), widthFactor);
        if (m_drawing)
            m_sequenceWrote = true;
        return true;
    }

    // 没有可落笔的点（手掌预判帧、点数不够的簇）：这一帧什么都不做 ——
    // 不落笔、不缩放、也不擦除，等下一帧分类清楚。误判的代价因此只有
    // 一帧的延迟，不会留下笔迹或跳动画面。
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
    // Mirrors handleTouch's TouchBegin: a fresh sequence clears the trailing lock and
    // owns the pointer (synthesized mouse events stay out).
    if (m_touchRelease)
        m_touchRelease->stop();
    m_touchActive = true;
    beginInputAt(viewportPos);
}

void PdfCanvas::testTouchMove(const QPointF &viewportPos) { moveInputTo(viewportPos); }

void PdfCanvas::testTouchCancel() { endCancelledTouch(); }

void PdfCanvas::testTouchEnd()
{
    m_touchActive = false;
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

void PdfCanvas::testPalmFrame(const QVector<QPointF> &viewportPts)
{
    // Same classify -> execute route as handleTouch's palm branch, minus the
    // QTouchEvent plumbing (see the header comment).
    TouchClassResult cls = classifyTouch(viewportPts, m_touchClass, m_palmEraserEnabled);
    if (cls.mode == TouchClass::PalmEraser && m_tool != InkTool::Move) {
        palmEraseTo(pointsCentroid(viewportPts), cls.radiusPx);
        return;
    }
    if (m_palmEraseActive)
        endPalmErase();
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
