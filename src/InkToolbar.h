#pragma once

#include "IconPainter.h"
#include "Theme.h"

#include <QColor>
#include <QHash>
#include <QWidget>

class PdfCanvas;
class QFrame;
class QToolButton;
class PageGrid;
class PenPalette;

// Classroom-whiteboard style floating toolbar that hovers over the page area.
//
// It is a child of PdfCanvas::viewport(), re-centred at the bottom edge on
// every host resize, and raised so it always floats above the page. All ink,
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

public slots:
    void setCollapsed(bool on);
    void toggleCollapsed();
    void showPenPalette();

signals:
    void hiddenChanged(bool hidden);
    void settingsRequested();      // 「设置」: the host switches to the settings page
    void saveRequested();          // 「保存」: bundle save (.dpz)
    void saveAsRequested();        // 「另存为」: bundle or inject-PDF export

protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;

private:
    void buildUi();
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
    QWidget     *m_body        = nullptr;
    QFrame      *m_moreSep     = nullptr;
    QToolButton *m_penButton   = nullptr;
    QToolButton *m_eraserButton = nullptr;
    QToolButton *m_undoButton  = nullptr;
    QToolButton *m_redoButton  = nullptr;
    QToolButton *m_clearButton = nullptr;
    QToolButton *m_fitButton    = nullptr;
    QToolButton *m_pageButton  = nullptr;
    QToolButton *m_saveButton  = nullptr;
    QToolButton *m_saveAsButton = nullptr;
    QToolButton *m_settingsButton = nullptr;
    QToolButton *m_moreButton  = nullptr;   // the collapse / expand chevron
    PenPalette  *m_palette     = nullptr;
    PageGrid    *m_pageGrid    = nullptr;

    // Glyph of every icon button, so the icons can be rebuilt (DPI change or a
    // new pen colour) without tracking each button separately.
    QHash<QToolButton *, IconPainter::Glyph> m_glyphs;
    QColor m_iconPenColor;         // pen colour baked into the current icons
    qreal  m_iconDpr = 1.0;        // device pixel ratio the icons were drawn for

    bool m_collapsed = false;
};
