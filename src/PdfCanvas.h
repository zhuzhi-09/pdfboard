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
    // On-screen (device px) thickness of a stored stroke at the current zoom.
    qreal testStrokeDeviceWidth(int page, int index) const;
    void  testZoomAt(const QPointF &viewportAnchor, qreal factor);
    qreal testZoom() const { return m_zoom; }
    int   testPageAtViewportY(qreal y) const;
    qreal testFracAtViewportY(qreal y) const;
    qreal testFracX(int page, qreal x) const;
    QSize testViewportSize() const { return viewport()->size(); }
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
    int    strokeCount() const;
    void   clearInk();                  // all pages
    void   clearCurrentPage();          // page under the viewport, undoable

signals:
    void pageChanged(int page, int count);
    void renderMeasured(qint64 ms, QSize size);
    void inkChanged(int strokes);
    void toolChanged();
    void penChanged();
    void undoStateChanged();

protected:
    bool event(QEvent *e) override;
    bool viewportEvent(QEvent *e) override;   // touch: 1 finger = ink, 2 = zoom+pan
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent *e) override;
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
    // anything changed.
    bool eraseAtPointer(int page, const QPointF &viewportPos);
    // Sweeps the eraser from `from` to `to` so fast drags do not skip ink
    // between successive pointer samples.
    bool eraseSweep(int pageHint, const QPointF &from, const QPointF &to);
    // Shared input entry points (used by both mouse and touch handlers).
    void beginInputAt(const QPointF &viewportPos);   // pen stroke / erase start
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

    bool m_erasing     = false;         // point-eraser drag in progress
    bool m_erasePushed = false;         // this drag already took a snapshot
    QPointF m_lastErasePos;             // previous eraser sample (for sweeping)
    bool    m_hasLastErasePos = false;
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
};
