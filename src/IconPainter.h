#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

// Hand drawn vector glyphs (design grid 24 x 24, stroke based) plus the QIcon
// builder used by the island toolbar, the pen palette and the empty state.
//
// Everything is painted with QPainter: no icon font, no image asset, no extra
// dependency. The returned QIcon carries pixmaps for several device pixel
// ratios, so it stays crisp when the window moves between screens without any
// per-screen cache to invalidate.
namespace IconPainter {

enum class Glyph {
    Open,       // folder with an up arrow
    Pen,        // marker with an optional colour badge
    Eraser,
    Undo,
    Redo,
    Trash,
    FitWidth,
    ChevronUp,   // collapse/expand affordance: shown while expanded
    ChevronDown, // collapse/expand affordance: shown while collapsed
    Gear,        // settings: ring with teeth
    House,       // home: roof over a wall with a door
    Pages,      // page picker: four thumbnail tiles
    Save,       // floppy disk: save annotations
    SaveAs,     // floppy disk + export arrow: save as / inject
    Fullscreen,     // four brackets opening outwards: enter fullscreen
    FullscreenExit, // four brackets opening inwards: leave fullscreen
    Move,           // four-way arrow: free move
    Document,   // empty state: a page
    Swash,      // empty state: a hand drawn ink stroke
};

// Colors of the button states the toolbar uses.
struct States {
    QColor normal;
    QColor selected;    // checked
    QColor disabled;
};

// Paints `glyph` inside `box` (square) with `color`. `stroke` is a stroke width
// in design units (2.0 is the standard weight); it is scaled to `box` together
// with the coordinates. `badge`, when valid, paints a small filled dot at the
// bottom right (used by the pen button to show the current ink colour).
void paintGlyph(QPainter &p, Glyph glyph, const QRectF &box,
                const QColor &color, qreal stroke = 2.0,
                const QColor &badge = QColor());

// Builds a QIcon for one device pixel ratio. The pixmaps are rendered at
// `logicalPx * dpr` device pixels with that ratio attached, so Qt blits them
// 1:1 into the icon rect (a QIcon carrying several ratios at once is blitted
// unscaled and comes out cropped, which is why the caller rebuilds the icon
// when the widget's screen changes - see InkToolbar::changeEvent).
QIcon makeIcon(Glyph glyph, int logicalPx, const States &colors, qreal dpr,
               qreal strokeScale = 1.0, const QColor &badge = QColor());

}   // namespace IconPainter
