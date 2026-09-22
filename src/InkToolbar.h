#pragma once

#include "IconPainter.h"
#include "Theme.h"

#include <QColor>
#include <QHash>
#include <QPoint>
#include <QSize>
#include <QWidget>

class PdfCanvas;
class QFrame;
class QGraphicsDropShadowEffect;
class QMouseEvent;
class QToolButton;
class PageGrid;
class PenPalette;

// Classroom-whiteboard style floating toolbar that hovers over the page area.
//
// It is a child of PdfCanvas::viewport(), re-centred at the bottom edge on
// every host resize until the user drags it elsewhere (session-only), and
// raised so it always floats above the page. All ink,
// zoom and page commands live here; the pen button opens a colour / width
// palette above the bar. When collapsed, only the chevron button stays visible
// so the toolbar can always be brought back - clicking that chevron toggles
// the collapse directly, there is no menu in between.
//
// Look: a light rounded chip with a soft shadow, every control showing a hand
// drawn vector icon over its label, and a filled accent pill marking the
// current tool. All geometry comes from Theme::metrics, so it scales with the
// display DPI instead of assuming 96 dpi.
class InkToolbar : public QWidget
{
    Q_OBJECT
public:
    explicit InkToolbar(PdfCanvas *canvas);
    ~InkToolbar() override;

    bool isCollapsed() const { return m_collapsed; }

    // Re-centre at the bottom of the host viewport and keep the bar on top.
    void reposition();

    // Moves the island by `delta` from its CURRENT position, clamped to the
    // host. This is the island's own drag path (grabbing its edge / chip
    // padding); the 自由移动 tool is unrelated - it pans the view instead.
    void moveBy(const QPoint &delta);

    // Clamps a drag position so the whole island stays inside the host. A host
    // smaller than the island yields (0, 0) instead of a negative bound.
    static QPoint clampToolbarPos(const QPoint &pos, const QSize &host, const QSize &self);

    // Re-applies the theme-derived pieces (chip sheet, shadow, icons) plus the
    // pen palette and the page-grid overlay this toolbar owns.
    void applyTheme();

    // --- Test hooks: used only by the `--selftest-ink` mode ----------------
    // Sends one synthetic left-button event to `watched` (a child of the
    // island) through QApplication::sendEvent, so the island's event filter and
    // drag handlers run exactly as they do for a real press / move / release.
    QToolButton *testEraserButton() const { return m_eraserButton; }
    void testPressOn(QWidget *watched, const QPoint &localPos);
    void testMoveOn(QWidget *watched, const QPoint &localPos);
    void testReleaseOn(QWidget *watched, const QPoint &localPos);

public slots:
    void setCollapsed(bool on);
    void toggleCollapsed();
    void showPenPalette();
    void setFullscreenActive(bool on);

signals:
    void hiddenChanged(bool hidden);
    void settingsRequested();      // 「设置」: the host switches to the settings page
    void saveRequested();          // 「保存」: bundle save (.dpz)
    void saveAsRequested();        // 「另存为」: bundle or inject-PDF export
    void fullscreenRequested();    // 「全屏」: the host toggles window fullscreen

protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    void buildUi();
    // One shared drag path: the island's own mouse handlers and the child
    // event filter both funnel through these, so the gesture behaves the same
    // no matter where on the island it was started.
    void beginDrag(const QPoint &posInIsland);
    void dragTo(const QPoint &posInIsland);
    void endDrag();
    void resetPress();             // drops the drag candidate / pressed child
    void refreshIcons();           // (re)build the button glyphs, incl. the pen badge
    void syncFromCanvas();
    void positionPalette();
    void onPenColorPicked(const QColor &color);
    void onPenWidthPicked(qreal width);
    void togglePageGrid();         // the "n / N" chip: page-thumbnail picker
    void dismissPageGrid();        // any command hides the picker it covers

    Theme::Metrics m_metrics;

    PdfCanvas   *m_canvas      = nullptr;
    QFrame      *m_chip        = nullptr;
    QGraphicsDropShadowEffect *m_glow = nullptr;
    QWidget     *m_body        = nullptr;
    QFrame      *m_moreSep     = nullptr;
    QToolButton *m_penButton   = nullptr;
    QToolButton *m_eraserButton = nullptr;
    QToolButton *m_moveButton  = nullptr;
    QToolButton *m_undoButton  = nullptr;
    QToolButton *m_redoButton  = nullptr;
    QToolButton *m_clearButton = nullptr;
    QToolButton *m_fitButton    = nullptr;
    QToolButton *m_pageButton  = nullptr;
    QToolButton *m_saveButton  = nullptr;
    QToolButton *m_saveAsButton = nullptr;
    QToolButton *m_settingsButton = nullptr;
    QToolButton *m_fullscreenButton = nullptr;
    QToolButton *m_moreButton  = nullptr;   // the collapse / expand chevron
    PenPalette  *m_palette     = nullptr;
    PageGrid    *m_pageGrid    = nullptr;

    // Glyph of every icon button, so the icons can be rebuilt (DPI change or a
    // new pen colour) without tracking each button separately.
    QHash<QToolButton *, IconPainter::Glyph> m_glyphs;
    QColor m_iconPenColor;         // pen colour baked into the current icons
    qreal  m_iconDpr = 1.0;        // device pixel ratio the icons were drawn for

    bool m_collapsed = false;
    bool m_fullscreenActive = false;

    // Drag state. m_userPos and m_dragged are session-only: the island returns
    // to its default spot on the next launch.
    bool   m_dragging = false;
    bool   m_dragged  = false;
    QPoint m_dragOffset;
    QPoint m_userPos;

    // A press anywhere on the island arms a drag, but it only turns into one
    // after the pointer travels startDragDistance(): a tap must keep clicking
    // the button under it. m_pressPos is in island coordinates.
    bool     m_pressArmed = false;
    QPoint   m_pressPos;
    QWidget *m_pressChild = nullptr;
};
