#pragma once

#include "Theme.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QPointF>
#include <QSizeF>
#include <QVector>

class QPdfDocument;
class QPainter;
class QTouchEvent;

class QTabletEvent;
class QWheelEvent;
class QTimer;
class InkToolbar;

// Continuous (Word-like) vertical PDF viewer with a per-page ink overlay.
//
// All pages are laid out in a single vertical strip; only pages intersecting
// the viewport are rasterized, and rendered pages are held in a byte-budgeted
// LRU cache. Ink strokes are stored per page in normalized (0..1) page
// coordinates, so they survive resizes and zoom changes.
class PdfCanvas : public QAbstractScrollArea
{
    Q_OBJECT
public:
    enum class InkTool {
        Pen,
        Eraser,
        // Free move: a one-finger / left-button drag pans the document view on
        // both axes (a hand tool); it never starts a stroke, erases or touches
        // the undo stack. The toolbar island keeps its own separate drag.
        Move,
    };
    Q_ENUM(InkTool)

    explicit PdfCanvas(QWidget *parent = nullptr);
    ~PdfCanvas() override;

    bool openPdf(const QString &path, QString *errorOut = nullptr);
    void closePdf();

    // Drops every cached page bitmap (used when another document becomes
    // visible, so only one document holds rendered pages at a time).
    void releaseCachedPages();

    // Re-applies the theme-derived pieces: the canvas scroll sheet and every
    // overlay that hangs off this canvas (the toolbar island owns the pen
    // palette and the page-grid picker).
    void applyTheme();

    int pageCount() const;
    int currentPage() const;            // topmost visible page
    QString pdfPath() const { return m_path; }

    // --- Annotation (de)serialization + page geometry ----------------------
    // The whole ink model as `{"version":1,"pages":{"<pageIndex>":[...]}}`,
    // ready to be stored as `annotations.json` inside a `.dpz` bundle.
    QJsonObject exportInk() const;
    // Replaces the whole ink model with the one in `obj`, clears undo/redo,
    // emits inkChanged/undoStateChanged and repaints.
    void        importInk(const QJsonObject &obj);
    // Page box in points (forwarder over QPdfDocument), used by PDF export.
    QSizeF      pagePointSize(int page) const;

    // Renders one page at the requested pixel size (for thumbnails). The width
    // of `size` drives the raster and the height keeps the page's own aspect
    // ratio (QPdfDocument::pagePointSize), so a tile never squashes the sheet.
    // Returns a null image when the page or document is unavailable. This is a
    // side path: it does not touch the whole-page render cache or stats.
    QImage pageThumbnail(int page, const QSize &size) const;

    void goToPage(int index);
    void nextPage();                    // scroll to next page
    void prevPage();                    // scroll to previous page

    // Fit-width on/off and zoom relative to fit-width.
    void setFitWidth(bool on);
    void zoomIn();
    void zoomOut();
    // Jump to an absolute zoom (1.0 == 适配宽度), keeping the viewport centre
    // fixed. Used by the status-bar zoom control; pinch / Ctrl+wheel have their
    // own anchor-at-the-pointer paths.
    void setZoomLevel(qreal zoom);
    // Zoom by `factor`, keeping the content point under `viewportAnchor` fixed.
    void  zoomAt(qreal factor, const QPointF &viewportAnchor);
    qreal zoomFactor() const { return m_zoom; }

    // Active tool (pen / point eraser) and pen appearance.
    InkTool tool() const { return m_tool; }
    void    setTool(InkTool tool);
    QColor  penColor() const { return m_penColor; }
    void    setPenColor(const QColor &color);
    qreal   penWidth() const { return m_penWidth; }
    void    setPenWidth(qreal width);

    // --- 橡皮大小（静态三档）------------------------------------------------
    // 每份画布记住自己的橡皮大小，默认就是第一档 = 旧版本唯一的固定值 14 px，
    // 所以没动过设置的安装擦除行为完全不变。档位表只此一处定义：工具栏面板、
    // 擦除命中、指示环、命中测试全都从这里取数。
    static const qreal *eraserRadiusSteps();
    static int eraserRadiusStepCount();
    qreal   eraserRadius() const { return m_eraserRadius; }
    void    setEraserRadius(qreal radiusPx);   // 吸附到最近的一档

    // 手掌擦除开关（设置页「手掌擦除」，默认开）。关掉后手掌帧永远不进入
    // 擦除，也结束正在进行的掌擦；双指与单指书写不受影响。
    void    setPalmEraserEnabled(bool on);
    bool    palmEraserEnabled() const { return m_palmEraserEnabled; }

    // --- 掌擦（手掌接触面橡皮）的纯分类器 ------------------------------------
    // 没有笔的教室大屏：手掌按上去时面板把它报成好几个接触点，而且点数逐帧
    // 抖动（3、4、5、2……）；真正的双指缩放同样是多点。这个函数把一帧接触点
    // 分到四类里，并给出掌擦半径与"该落笔的那一个点"。它只读入参和 state
    // （它唯一的记忆），不碰 QTouchEvent、不碰界面 —— 自测可以用合成点集
    // 逐帧驱动它，不需要一台触摸屏。
    enum class TouchClass {
        Idle,        // 空：没有接触点
        Writing,     // 单点，或"一个手掌簇 + 一个分离的书写点"
        Pinch,       // 两个及以上明确分离的簇（原有双指路径）
        PalmEraser,  // 恰好一簇、簇外没有别的点 -> 手掌擦除
    };
    struct TouchClassState {
        int     palmFrames = 0;     // 连续满足"一簇里 >= kPalmEnterPoints 点"的帧数
        bool    palmActive = false; // 迟滞：已进入掌擦，点数抖动不再退出
        qreal   radius = 0.0;       // 平滑后的掌擦半径
        bool    hasRadius = false;
        QPointF restCentroid;       // 静止检测用的慢平均簇心
        bool    hasRest = false;
    };
    struct TouchClassResult {
        TouchClass mode = TouchClass::Idle;
        qreal      radiusPx = 0.0;   // 仅 PalmEraser 有意义
        QPointF    writePos;         // 仅 Writing 且 hasWritePos 时有效
        bool       hasWritePos = false;
    };
    static TouchClassResult classifyTouch(const QVector<QPointF> &pts,
                                          TouchClassState &state,
                                          bool palmEraserEnabled = true);

    // Ink undo / redo: snapshot stack of the whole per-page ink model.
    void undo();
    void redo();
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

    // --- Test hooks: used only by the `--selftest-ink` mode ----------------
    void testAddStroke(int page, const QPointF &aNorm, const QPointF &bNorm,
                       const QColor &color, qreal width, int steps = 120,
                       bool densify = true);
    int  testStrokePoints(int page, int index) const;
    // Number of points the sample path would keep for `raw` (dedupe + densify):
    // lets the self test prove repeated panel frames are dropped, not stored.
    int  testDensifiedCount(const QVector<QPointF> &raw) const;
    // Headless touch entry points + the overlay rule, so the "writing across the
    // island" behaviour is locked without a touchscreen.
    void  testTouchBegin(const QPointF &viewportPos);
    void  testTouchMove(const QPointF &viewportPos);
    void  testTouchEnd();
    bool  testTouchBelongsToOverlay(const QVector<QPointF> &viewportPts) const;
    // 掌擦的整帧驱动（自测用）：走和 handleTouch 掌擦分支同一段"分类 -> 执行"
    // 路径，只是不经过 QTouchEvent，所以没有触摸屏也能断言"多帧手掌真的擦掉了
    // 墨、而且不缩放不平移"。
    void  testPalmFrame(const QVector<QPointF> &viewportPts);
    bool  testPalmEraseActive() const { return m_palmEraseActive; }
    // Feed a real QWheelEvent to the viewport (headless), so the wheel rules are
    // asserted on the production path instead of on a reimplementation.
    void  testWheelAt(const QPointF &viewportPos, int angleDeltaY, bool ctrl);
    // On-screen (device px) thickness of a stored stroke at the current zoom.
    qreal testStrokeDeviceWidth(int page, int index) const;
    void  testZoomAt(const QPointF &viewportAnchor, qreal factor);
    qreal testZoom() const { return m_zoom; }
    int   testPageAtViewportY(qreal y) const;
    qreal testFracAtViewportY(qreal y) const;
    qreal testFracX(int page, qreal x) const;
    QSize testViewportSize() const { return viewport()->size(); }
    // 静态橡皮大小：设置/读取，供自测断言"更大的档真的擦掉更多墨"以及
    // "大小随文档走、切工具/缩放不重置"。
    qreal testEraserRadius() const { return m_eraserRadius; }
    void  testSetEraserRadius(qreal radiusPx) { setEraserRadius(radiusPx); }
    bool testEraseAtNormalized(int page, const QPointF &norm);
    bool testEraseSweepNormalized(int page, const QPointF &aNorm, const QPointF &bNorm);
    QString testStrokeSummary(int page) const;
    // Drives one full free-move drag (press / move / release) through the real
    // mouse handlers, so the pan semantics are tested end to end.
    void testFreeMoveDrag(const QPointF &from, const QPointF &to);
    int  undoDepth() const { return int(m_undoStack.size()); }
    int  redoDepth() const { return int(m_redoStack.size()); }

    qint64 lastRenderMs() const { return m_lastRenderMs; }
    InkToolbar *toolbar() const { return m_toolbar; }   // for host-level actions
    QSize  lastRenderSize() const { return m_lastRenderSize; }
    // A synchronous repaint started from inside a paint event is what overflowed the
    // stack (0xC00000FD, ~2200 nested frames, all of them re-entering paintEvent
    // through Qt6Widgets/Qt6Gui) while the zoom control was dragged. paintEvent()
    // now refuses to nest; the smoke test asserts the depth never exceeded 1.
    int  testMaxPaintDepth() const;
    void testResetMaxPaintDepth();
    // relayout() must not nest either: it used to toggle a scroll-bar policy, which
    // makes Qt resize the viewport, which re-enters resizeEvent -> relayout(). The two
    // bars then flipped each other's visibility forever - that is the resize
    // oscillation that exhausted the stack (0xC00000FD) while zooming.
    int  testMaxRelayoutDepth() const;
    void testResetMaxRelayoutDepth();
    int  testRelayoutRefusals() const;   // 0 means the loop is gone, not just capped

    // Eraser indicator: a ring whose diameter IS the wipe diameter, with the eraser glyph
    // in the middle so the tool is recognisable at a glance. Screen only - it is drawn in
    // paintEvent and both export paths rasterise through pageThumbnail, so it can never
    // reach a PDF or a PNG. Exposed so the self test can render it at two radii and prove
    // the size is actually readable from the drawing.
    void   testSetEraserHover(const QPointF &viewportPos);
    void   testClearEraserHover();
    // Pointer tracking: exposed so a test can prove a touch/stylus drag is never hijacked
    // by the global mouse cursor (the "dragging sometimes damages the wrong strokes" bug).
    void   testTrackPointer();
    QPointF testEraserHoverPos() const;
    // Two-finger smoothing: feed a synthetic gesture and watch the zoom / scrollbar.
    void   testPinchBegin(const QPointF &a, const QPointF &b);
    void   testPinchFrame(const QPointF &a1, const QPointF &b1);
    void   testPinchEnd();
    // The eraser end of a stylus (QTabletEvent::pointerType() == Eraser) must erase the stroke
    // instead of drawing ink; this drives that exact path without a tablet attached.
    void   testEraserTailStroke(const QPointF &from, const QPointF &to);
    QImage testRenderEraserIndicator(const QPointF &viewportPos, qreal radiusPx,
                                     const QSize &imageSize);
    int    strokeCount() const;
    void   clearInk();                  // all pages
    void   clearCurrentPage();          // page under the viewport, undoable

signals:
    void pageChanged(int page, int count);
    // Emitted whenever the zoom settles at a new value (pinch, Ctrl+wheel,
    // Ctrl+±, 适配宽度 or the status-bar zoom control). The status bar follows it.
    void zoomChanged(qreal zoom);
    void renderMeasured(qint64 ms, QSize size);
    void inkChanged(int strokes);
    void toolChanged();
    void penChanged();
    void eraserChanged();      // 橡皮大小变了：工具栏刷新提示与面板选中档
    void undoStateChanged();

protected:
    bool event(QEvent *e) override;
    bool viewportEvent(QEvent *e) override;   // touch: 1 finger = ink, 2 = zoom+pan
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *e) override;
    // Touch writing must never be interrupted by the system's "press and hold" right-click:
    // the canvas has no context menu, so the event is swallowed instead of propagating.
    void contextMenuEvent(QContextMenuEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    struct PageGeom {
        qreal y = 0;    // top edge in content coordinates
        qreal w = 0;    // display width
        qreal h = 0;    // display height
    };
    struct Stroke {
        QVector<QPointF> pts;   // normalized 0..1 inside the page rect
        QColor color{Qt::red};
        // Stroke thickness as a FRACTION of the page width (not device px), so
        // ink scales with zoom: writing zoomed in and then fitting the width
        // shrinks the strokes along with the page.
        qreal  width = 0.003;
    };
    using InkModel = QHash<int, QVector<Stroke>>;

    void relayout();
    // Eraser indicator: the ring shows the wipe size, the glyph shows the tool. Screen
    // only - see the block in the .cpp for why it cannot reach an export.
    bool   eraserIndicatorVisible() const;
    QRect  eraserIndicatorBounds(const QPointF &viewportPos, qreal radiusPx) const;
    void   updateEraserIndicator(const QPointF &from, bool hadFrom,
                                 const QPointF &to, bool hasTo);
    void   drawEraserIndicator(QPainter &p, const QPointF &viewportPos, qreal radiusPx);
    void   applyToolCursor();
    void   setEraserHover(const QPointF &viewportPos);
    void   clearEraserHover();
    // Pointer tracking for the areas the floating overlays swallow (see the .cpp).
    void   trackPointer();
    void invalidateRenders();           // drop cached bitmaps (on resize/zoom)
    // Applies an absolute zoom while keeping the content point under `anchor`
    // fixed on both axes. `immediate` re-renders at once instead of waiting for
    // the settle timer (used for discrete actions like "适配宽度").
    void applyZoom(qreal targetZoom, const QPointF &anchor, bool immediate);

    QImage imageFor(int page);          // cached render (renders on miss)
    void   trimCache();
    void   touchLru(int page);

    int    pageAtContentY(qreal contentY) const;
    int    pageAtViewportPos(const QPointF &viewportPos) const;  // -1 if off-page
    QRectF pageRectInViewport(int page) const;   // viewport coords
    QPointF toNormalized(int page, const QPointF &viewportPos) const;
    void   drawInk(QPainter &p, int page, const QRectF &rect);

    // Chrome painting (all of it token driven, see Theme.h).
    void   paintBackdrop(QPainter &p) const;
    void   paintEmptyState(QPainter &p) const;
    void   paintPageShadow(QPainter &p, const QRectF &page, qreal radius) const;

    // Ink mutation helpers (all snapshot the model before changing it).
    void         pushUndoSnapshot();
    void         cancelGesture();
    // Partial (segment-level) erase: cuts out only the touched part of a
    // stroke; the remaining pieces stay as separate strokes. Returns true if
    // anything changed. `radiusPx` is the wipe radius in logical px (the
    // selected static step, or the live palm radius).
    bool eraseAtPointer(int page, const QPointF &viewportPos, qreal radiusPx);
    // Sweeps the eraser from `from` to `to` so fast drags do not skip ink
    // between successive pointer samples.
    bool eraseSweep(int pageHint, const QPointF &from, const QPointF &to, qreal radiusPx);
    // 当前该用的擦除半径：掌擦手势中用动态半径，否则用静态选中档。
    qreal activeEraseRadius() const;
    // 掌擦（手掌接触面橡皮）的一帧：把指示环跟到簇心，并按动态半径沿路径擦过来。
    void  palmEraseTo(const QPointF &centroid, qreal radiusPx);
    void  endPalmErase();

    // True when a touch frame must be handed to a floating overlay (toolbar,
    // palette, page grid) instead of the page. Only a touch that would START
    // something is redirected; a stroke already running keeps every sample, so
    // writing across the island works and the ink simply goes underneath it.
    bool   touchBelongsToOverlay(const QVector<QPointF> &viewportPts) const;
    // Two-finger gesture: started, fed one frame at a time, ended. Extracted from
    // handleTouch so the smoothing below is testable, and so every frame goes through the
    // same maths (see applyPinchFrame for why it is drift-free).
    void   startPinch(const QPointF &a, const QPointF &b);
    void   applyPinchFrame(const QPointF &a1, const QPointF &b1);
    void   endPinch();

    // Wheel input: Ctrl+wheel zooms at the pointer; the plain wheel scrolls. A
    // running (or just finished) touch gesture owns the wheel entirely - see the
    // comment on the implementation for why that matters.
    bool handleWheel(QWheelEvent *we);
    // Shared input entry points (used by both mouse and touch handlers).
    // `asEraser` is for hardware that names its own eraser: a stylus used with its eraser end
    // erases this stroke without touching the toolbar's tool state.
    void beginInputAt(const QPointF &viewportPos, bool asEraser = false);
    void moveInputTo(const QPointF &viewportPos);
    void endInput();
    // Free-move mode: pans the view by the pointer movement since the previous
    // sample. Returns true while a move drag is in progress.
    bool freePanTo(const QPointF &viewportPos);
    bool handleTouch(QTouchEvent *te);               // 1 finger ink, 2 fingers zoom+pan
    // Pen/stylus input: a real pen reports several times the sample rate of the
    // synthesized mouse events, which is what smooth fast writing needs.
    bool handleTablet(QTabletEvent *te);
    void panBy(const QPointF &delta);

    QPdfDocument *m_doc = nullptr;
    QTimer       *m_zoomSettle = nullptr;   // re-render at full quality after a zoom
    QTimer       *m_touchRelease = nullptr; // delays clearing the touch ink lock
    QString       m_path;

    Theme::Metrics m_ui;                // chrome metrics derived from the font
    qreal m_pageRadius = 4.0;           // rounded paper corners (logical px)

    QVector<QSizeF> m_pageSizes;        // points, per page
    QVector<PageGeom> m_geom;           // content layout
    qreal m_contentH   = 0;
    qreal m_contentW   = 0;             // widest page (for horizontal panning)
    qreal m_maxPageWpt = 0;
    // relayout() memo - the layout is a pure function of these, so an identical
    // request (the resize/scroll-bar feedback loop issues thousands) is a no-op.
    QPdfDocument *m_layoutDoc = nullptr;
    qreal m_layoutZoom = -1.0;
    int   m_layoutW = -1;
    int   m_layoutH = -1;
    int   m_layoutPageCount = -1;
    qreal m_layoutMaxPageWpt = -1.0;
    qreal m_gap        = 14.0;          // vertical gap between pages
    qreal m_marginX    = 10.0;          // left/right gutter

    bool   m_fitWidth = true;
    qreal  m_zoom     = 1.0;            // multiplier on the fit-width scale
    qreal  m_scale    = 1.0;            // points -> device px

    QHash<int, QImage> m_cache;
    QVector<int>       m_lru;           // least recent first
    qint64 m_cacheBytes  = 0;
    qint64 m_cacheBudget = 128LL * 1024 * 1024;   // whole-page LRU budget (no tiling by design)

    InkModel m_ink;
    Stroke   m_current;
    bool     m_drawing   = false;
    int      m_drawPage  = -1;

    InkTool m_tool     = InkTool::Pen;
    QColor  m_penColor{0xD3, 0x2F, 0x2F};
    qreal   m_penWidth = 4.0;           // device px
    // 橡皮大小：静态三档里选中的那一档（逻辑像素半径），默认第一档 14。
    qreal   m_eraserRadius = eraserRadiusSteps()[0];

    bool m_erasing     = false;         // point-eraser drag in progress
    bool m_erasePushed = false;         // this drag already took a snapshot
    QPointF m_lastErasePos;             // previous eraser sample (for sweeping)
    bool    m_hasLastErasePos = false;
    // 掌擦：分类器（classifyTouch）负责防抖，这里只跟着它的结论走。
    // 开关默认开；主机把已保存的设置推给每份画布（见 MainWindow）。
    bool    m_palmEraserEnabled = true;
    qreal   m_palmRadius = 0.0;         // 当前动态半径（掌擦期间显示与命中都用它）
    bool    m_palmEraseActive = false;  // 掌擦手势进行中
    QPointF m_palmLastPos;              // 掌擦的上一帧簇心（沿路径扫擦）
    bool    m_hasPalmLastPos = false;
    TouchClassState m_touchClass;       // 分类器的跨帧记忆（点数迟滞 / 半径 EMA）
    // Eraser indicator: where the pointer is and whether it is on the canvas. The ring
    // follows this, so a pointer move only repaints two small circles (old + new).
    QPointF m_eraserHoverPos;
    bool    m_eraserHover = false;
    // Two-finger smoothing state (see applyPinchFrame): the raw gesture is accumulated, an
    // EMA follows it, and only the difference from what was applied is ever spent - so the
    // panel's contact noise averages out without the zoom drifting.
    QPointF m_pinchPanRaw;
    QPointF m_pinchPanSmooth;
    QPointF m_pinchPanApplied;
    QPointF m_pinchCentroidSmooth;
    qreal   m_pinchScaleRaw = 1.0;
    qreal   m_pinchScaleSmooth = 1.0;
    qreal   m_pinchScaleApplied = 1.0;
    // Slow average of the live gesture, used to tell "the fingers are held still" (which must
    // apply nothing at all) from "the teacher is actually pinching or panning".
    qreal   m_pinchRestSpan = 0.0;
    QPointF m_pinchRestCentroid;
    // Which input started the stroke in progress: only a finger keeps the "too short =
    // noise" rule, a mouse/stylus tap is committed as a dot.
    bool    m_strokeFromTouch = false;
    // Polls the global cursor so the indicator and mouse strokes survive crossing the
    // floating island (a child widget that would otherwise eat every mouse event).
    QTimer *m_pointerTrack = nullptr;
    QPointF m_lastTrackedPos;
    bool    m_hasTrackedPos = false;
    bool    m_pinchActive = false;      // two-finger zoom+pan in progress
    bool    m_touchInkBlocked = false;  // no inking until every finger lifts
    QVector<QPointF> m_pinchPts;        // last two touch points
    bool    m_moveDragActive = false;   // free-move drag in progress (mouse / one finger)
    QPointF m_moveLastPos;              // previous free-move sample (viewport coords)

    QVector<InkModel> m_undoStack;      // capped snapshot stack
    QVector<InkModel> m_redoStack;

    InkToolbar *m_toolbar = nullptr;

    qint64 m_lastRenderMs = 0;
    QSize  m_lastRenderSize;
    bool   m_renderNotifyPending = false;   // one queued status-bar update at a time
};
