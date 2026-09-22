#pragma once

#include "Theme.h"

#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class PdfCanvas;
class QFrame;
class QGraphicsDropShadowEffect;
class QGridLayout;
class QLabel;
class QScrollArea;

// Page-thumbnail picker: the island's "n / N" chip opens this card instead of
// asking for a number in a modal spin box.
//
// Like PenPalette it is a plain CHILD overlay of PdfCanvas::viewport(), never a
// top-level popup: on Windows a frameless window gets a hard square DWM shadow
// around its bounds, which looks broken next to the rounded card. It sits in
// parent coordinates and dismisses on an outside click (app-level event filter
// that ignores the grid itself and the toolbar), on Esc, or when a page is
// picked.
//
// Thumbnails are rendered lazily: a tile asks for its raster while it is being
// painted, so a 300 page document opens instantly and only the cells the
// viewport really exposes are rasterized. Rasters live in a bounded LRU (entry
// count + byte budget), and the whole cache is dropped when the document
// changes so a new file can never show a stale page.
class PageGrid : public QWidget
{
    Q_OBJECT
public:
    PageGrid(PdfCanvas *canvas, QWidget *toolbar);
    ~PageGrid() override;

    bool isOpen() const { return isVisible(); }

    void open();          // refresh, place, show and centre the current page
    void close();         // hide the overlay (it is a child, never a window)

    // Drops every cached raster and repaints the tiles. Called when the
    // document changes.
    void invalidateThumbnails();

    // Re-applies the card sheet, shadow and header glyph after a theme change.
    void applyTheme();

protected:
    void showEvent(QShowEvent *e) override;
    void changeEvent(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;   // dismiss + resize

private:
    class PageCell;

    void buildUi();
    void refreshGlyph();
    void onCanvasPageChanged(int page, int count);
    void refreshPages(int count);       // (re)build the tiles of a document
    void updateCellSize();              // tile box for the current aspect ratio
    void applyGeometry();               // width / height / columns / placement
    void layoutCells(int columns);      // re-flow the tiles for a column count
    void syncCurrent(bool centerOnCurrent);
    QImage thumbnailFor(int page);      // cache-aware raster for one tile
    void touchThumbLru(int page);
    void trimThumbnails();

    Theme::Metrics m_metrics;

    PdfCanvas *m_canvas  = nullptr;
    QWidget   *m_toolbar = nullptr;     // clicks here must not dismiss the grid

    QFrame      *m_card     = nullptr;
    QGraphicsDropShadowEffect *m_glow = nullptr;
    QLabel      *m_glyph    = nullptr;
    QLabel      *m_title    = nullptr;
    QLabel      *m_count    = nullptr;
    QScrollArea *m_scroll   = nullptr;
    QWidget     *m_gridHost = nullptr;
    QGridLayout *m_grid     = nullptr;
    QVector<PageCell *> m_cells;

    // Tile metrics, all derived from the font (see Theme::metrics), so the grid
    // survives 150 % / 200 % display scaling without special cases.
    int   m_thumbW     = 96;
    int   m_cellPad    = 4;
    int   m_captionH   = 14;
    int   m_captionGap = 4;
    int   m_cellRadius = 14;
    int   m_sheetRadius = 4;
    qreal m_ringW      = 2.0;
    QSize m_cellSize{ 112, 160 };

    int     m_columns     = 0;          // 0 forces the first re-flow
    int     m_currentPage = -1;
    qreal   m_thumbAspect = 297.0 / 210.0;   // ISO A4 portrait until a page says otherwise
    bool    m_aspectKnown = false;
    qreal   m_dpr         = 1.0;
    QString m_docPath;

    QHash<int, QImage> m_thumbs;        // page -> raster, kept at device pixels
    QVector<int>       m_thumbLru;      // least recent first
    qint64 m_thumbBytes = 0;
};
