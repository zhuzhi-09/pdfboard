#pragma once

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QString>
#include <QStyleHints>
#include <QtGlobal>

// Central design tokens for the application chrome: the canvas backdrop, the
// floating island toolbar, the pen palette popover and the status bar.
//
// Colours are plain QColor values. Every geometric value is a logical unit
// derived from the widget font (see Theme::metrics) so the chrome scales with
// the display DPI and with the user's font settings instead of assuming 96 dpi.
//
// The palette is runtime-switchable: 系统 follows the Windows colour scheme,
// 浅色 / 深色 pin it. The pages stay paper white in both modes - a document on
// a classroom projector must keep looking like paper.

namespace Theme {

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

struct Palette {
    // Brand / selection.
    QColor accent;
    QColor accentHover;
    QColor accentSoft;      // translucent accent wash
    QColor onAccent;        // icon + text drawn on top of the accent

    // Canvas ("paper on a desk").
    QColor desk;            // the flat plane the pages rest on
    QColor deskShade;       // deeper tone: top edge + gutter behind the scroll bars
    QColor paper;
    QColor pageEdge;        // hairline around a page
    QColor pageShadow;      // base colour; the alpha comes from PageShadow

    // Floating chrome.
    QColor surface;
    QColor surfaceEdge;
    QColor surfaceHover;
    QColor surfacePressed;
    QColor chipTint;        // inset pill (page counter, status chips)

    // Text.
    QColor text;
    QColor textMuted;
    QColor textDisabled;
    QColor divider;

    // Drop shadow of the floating chrome.
    QColor shadow;
};

inline Palette makeLightPalette()
{
    Palette c;
    c.accent         = QColor(0x1A, 0x73, 0xE8);
    c.accentHover    = QColor(0x15, 0x63, 0xC8);
    c.accentSoft     = QColor(0x1A, 0x73, 0xE8, 0x22);
    c.onAccent       = QColor(0xFF, 0xFF, 0xFF);

    c.desk           = QColor(0xEB, 0xED, 0xF2);
    c.deskShade      = QColor(0xDF, 0xE3, 0xEA);
    c.paper          = QColor(0xFF, 0xFF, 0xFF);
    c.pageEdge       = QColor(0x10, 0x18, 0x28, 0x24);
    c.pageShadow     = QColor(0x10, 0x18, 0x28);

    c.surface        = QColor(0xFC, 0xFC, 0xFD);
    c.surfaceEdge    = QColor(0x10, 0x18, 0x28, 0x20);
    c.surfaceHover   = QColor(0x10, 0x18, 0x28, 0x14);
    c.surfacePressed = QColor(0x10, 0x18, 0x28, 0x26);
    c.chipTint       = QColor(0x10, 0x18, 0x28, 0x0F);

    c.text           = QColor(0x1F, 0x24, 0x30);
    c.textMuted      = QColor(0x5C, 0x66, 0x75);
    c.textDisabled   = QColor(0x1F, 0x24, 0x30, 0x48);
    c.divider        = QColor(0x10, 0x18, 0x28, 0x18);

    c.shadow         = QColor(0x10, 0x18, 0x28);
    return c;
}

inline Palette makeDarkPalette()
{
    Palette c;
    c.accent         = QColor(0x4C, 0x95, 0xFF);
    c.accentHover    = QColor(0x6B, 0xA8, 0xFF);
    c.accentSoft     = QColor(0x4C, 0x95, 0xFF, 0x2E);
    c.onAccent       = QColor(0xFF, 0xFF, 0xFF);

    c.desk           = QColor(0x1B, 0x1B, 0x1F);
    c.deskShade      = QColor(0x14, 0x14, 0x17);
    c.paper          = QColor(0xFF, 0xFF, 0xFF);
    c.pageEdge       = QColor(0x3A, 0x3A, 0x42);
    c.pageShadow     = QColor(0x00, 0x00, 0x00);

    c.surface        = QColor(0x2C, 0x2C, 0x31);
    c.surfaceEdge    = QColor(0x3A, 0x3A, 0x42);
    c.surfaceHover   = QColor(0x38, 0x38, 0x3F);
    c.surfacePressed = QColor(0x44, 0x44, 0x4C);
    c.chipTint       = QColor(0x4C, 0x95, 0xFF, 0x1F);

    c.text           = QColor(0xE8, 0xE8, 0xEC);
    c.textMuted      = QColor(0xA0, 0xA0, 0xAA);
    c.textDisabled   = QColor(0x6A, 0x6A, 0x74);
    c.divider        = QColor(0x3A, 0x3A, 0x42);

    c.shadow         = QColor(0x00, 0x00, 0x00);
    return c;
}

// How the appearance is chosen.
enum class Mode {
    System,     // follow the Windows colour scheme
    Light,
    Dark,
};

// One palette for the whole program. It is an inline VARIABLE, not a
// function-local static: inline variables are guaranteed to be a single object
// across translation units, whereas a local static in an inline function is
// duplicated by some toolchains - and a duplicated palette means the unit that
// paints reads a different theme than the one main() selected.
inline Palette g_palette = makeLightPalette();

inline const Palette &light() { return g_palette; }

// The selected mode (modifiable reference: setMode writes through it).
inline Mode g_mode = Mode::System;

inline Mode &modeRef() { return g_mode; }

inline Mode mode() { return modeRef(); }

// What the OS asks for right now (Qt 6.5+ colour scheme).
inline Mode systemMode()
{
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
               ? Mode::Dark : Mode::Light;
}

inline Mode resolvedMode()
{
    const Mode m = mode();
    return m == Mode::System ? systemMode() : m;
}

// Swaps the palette to match resolvedMode(). Call at startup before any widget
// is built, and again whenever the setting or the OS scheme changes.
inline void applyTheme()
{
    g_palette = (resolvedMode() == Mode::Dark) ? makeDarkPalette()
                                               : makeLightPalette();
}

inline void setMode(Mode m)
{
    modeRef() = m;
    applyTheme();
}

// Applies the STORED preference (0 = 系统, 1 = 浅色, 2 = 深色). Every path that
// re-applies the theme must go through this: the settings page only persists the
// value, so the runtime mode has to be re-synced from it - otherwise the swap
// recomputes from a stale mode and clicking 深色 / 浅色 does nothing.
inline void applyStoredMode(int stored)
{
    setMode(static_cast<Mode>(qBound(0, stored, 2)));
}

// ---------------------------------------------------------------------------
// Style sheet helpers
// ---------------------------------------------------------------------------

// Qt style sheet colour for `c`. The alpha is emitted as a 0..1 float, the
// form Qt's CSS parser is happiest with.
inline QString rgba(const QColor &c)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(c.red())
        .arg(c.green())
        .arg(c.blue())
        .arg(c.alphaF(), 0, 'f', 3);
}

// Qt style sheet length in logical px (device independent, so it scales with
// the display DPI on its own).
inline QString px(int value)
{
    return QStringLiteral("%1px").arg(value);
}

// ---------------------------------------------------------------------------
// Spacing scale (logical px at 100 %)
// ---------------------------------------------------------------------------

enum Space {
    Space1 = 4,
    Space2 = 8,
    Space3 = 12,
    Space4 = 16,
    Space5 = 24,
    Space6 = 32,
};

// ---------------------------------------------------------------------------
// Corner radii (logical px at 100 %)
// ---------------------------------------------------------------------------

enum Radius {
    RadiusPage    = 4,     // paper corners
    RadiusPill    = 10,    // status / page counter pills
    RadiusButton  = 14,    // island toolbar buttons
    RadiusPopover = 18,    // pen palette card
    RadiusChip    = 22,    // island chip
};

// ---------------------------------------------------------------------------
// Font scales (relative to the application font)
// ---------------------------------------------------------------------------

constexpr qreal FontChrome  = 1.06;   // toolbar / palette labels
constexpr qreal FontBody    = 1.10;   // empty state hint
constexpr qreal FontTitle   = 1.85;   // empty state title
constexpr qreal FontCaption = 0.86;   // section captions, status bar

// ---------------------------------------------------------------------------
// Shadow specs
// ---------------------------------------------------------------------------

// A page lift is faked with a few stacked translucent rounded rectangles: one
// per layer, each a little larger and further down than the previous one. It is
// far cheaper than a real blur and cannot hurt the scroll repaint budget.
struct PageShadow {
    static constexpr int layers = 3;
    int grow[layers]  = { 2, 5, 11 };    // extra size per side, logical px
    int dy[layers]    = { 2, 6, 12 };    // vertical offset, logical px
    int alpha[layers] = { 30, 20, 13 };  // 0..255
};

// ---------------------------------------------------------------------------
// Metrics derived from the widget font
// ---------------------------------------------------------------------------

struct Metrics {
    // Touch / hit targets.
    int touch;          // minimum touch target (>= 48 logical px)
    int buttonHeight;   // icon + label + padding: what a chip button needs
    int icon;           // glyph box

    // Toolbar layout.
    int gap;            // spacing between adjacent buttons
    int padX;           // horizontal padding inside a button
    int padY;           // vertical padding inside a button
    int chipPad;        // chip inner padding
    int barBottom;      // chip distance above the viewport bottom
    int shadowRoom;     // space reserved around a chip for its drop shadow
    int divider;        // separator thickness
    int menuGap;        // gap between the "⋯" button and its menu

    // Radii that follow the font (the enum values above are the minimums).
    int radiusChip;
    int radiusButton;
    int radiusPill;

    // Shadows.
    qreal shadowBlur;
    qreal shadowOffsetY;

    // Canvas page layout.
    qreal pageGap;      // vertical gap between pages
    qreal pageMargin;   // horizontal gutter beside the pages
    int   pagePadTop;   // content padding above the first page
    int   pagePadBottom;// content padding below the last page

    // Empty state.
    int emptyCard;      // rounded card behind the empty state glyph
    int emptyGlyph;     // glyph box inside that card
};

// `scale` multiplies every FLOOR as well as the font-derived value, so a whole
// piece of chrome can shrink as one unit (the island ships at IslandScale).
// The default 1.0 keeps every existing caller bit-identical.
inline Metrics metrics(const QFont &font, qreal scale = 1.0)
{
    const QFontMetricsF fm(font);
    const qreal h = qMax<qreal>(12.0, fm.height());

    Metrics m;
    m.touch         = int(qMax<qreal>(48.0 * scale, h * 2.20));
    m.icon          = int(qMax<qreal>(22.0 * scale, h * 1.25));

    m.gap           = int(qMax<qreal>(qreal(Space1) * scale, h * 0.25));
    m.padX          = int(qMax<qreal>(10.0 * scale, h * 0.70));
    m.padY          = int(qMax<qreal>((qreal(Space1) + 2.0) * scale, h * 0.34));
    m.chipPad       = int(qMax<qreal>(qreal(Space2) * scale, h * 0.50));
    m.barBottom     = int(qMax<qreal>((qreal(Space5) - 6.0) * scale, h * 1.10));
    m.shadowRoom    = int(qMax<qreal>(14.0 * scale, h * 0.90));
    m.divider       = qMax(1, int(h / 14.0));
    m.menuGap       = int(qMax<qreal>(6.0 * scale, h * 0.40));

    m.radiusChip    = int(qMax<qreal>(qreal(RadiusChip) * scale, h * 1.30));
    m.radiusButton  = int(qMax<qreal>(qreal(RadiusButton) * scale, h * 0.85));
    m.radiusPill    = int(qMax<qreal>(qreal(RadiusPill) * scale, h * 0.60));

    m.shadowBlur    = qMax<qreal>(18.0 * scale, h * 1.10);
    m.shadowOffsetY = qMax<qreal>(3.0 * scale, h * 0.22);

    m.pageGap       = qMax<qreal>(14.0, h * 1.15);
    m.pageMargin    = qMax<qreal>(14.0, h * 1.15);
    m.pagePadTop    = int(qMax<qreal>(8.0, h * 0.60));
    m.pagePadBottom = int(qMax<qreal>(20.0, h * 1.40));

    m.emptyCard     = int(qMax<qreal>(104.0, h * 6.40));
    m.emptyGlyph    = int(m.emptyCard * 0.46);

    // A chip button stacks its glyph over its label. The minimum height must
    // cover that stack plus the padding, otherwise the label is clipped.
    const qreal stack = m.icon + qMax<qreal>(6.0, h * 0.40) + h;
    m.buttonHeight  = int(qMax<qreal>(qreal(m.touch) + 6.0,
                                      stack + m.padY * 2.0 + 2.0));
    return m;
}

// ---------------------------------------------------------------------------
// Themed fonts
// ---------------------------------------------------------------------------

// Returns `base` rescaled by `factor`, keeping whatever size unit the platform
// font uses (points or pixels) so nothing breaks at 150 % / 200 % scaling.
inline QFont scaledFont(const QFont &base, qreal factor,
                        QFont::Weight weight = QFont::Normal)
{
    QFont f = base;
    const qreal pt = base.pointSizeF();
    if (pt > 0.0)
        f.setPointSizeF(qMax<qreal>(6.0, pt * factor));
    else if (base.pixelSize() > 0)
        f.setPixelSize(qMax(8, int(base.pixelSize() * factor)));
    f.setWeight(weight);
    return f;
}

inline QFont chromeFont(const QFont &base)
{
    return scaledFont(base, FontChrome, QFont::DemiBold);
}

// The island ships at 80% of the type scale: it is one piece, so its font and
// its metrics shrink together.
inline constexpr qreal IslandScale = 0.80;

inline QFont islandFont(const QFont &base)
{
    return scaledFont(chromeFont(base), IslandScale, QFont::DemiBold);
}

inline QFont bodyFont(const QFont &base)
{
    return scaledFont(base, FontBody, QFont::Medium);
}

inline QFont titleFont(const QFont &base)
{
    return scaledFont(base, FontTitle, QFont::Bold);
}

inline QFont captionFont(const QFont &base)
{
    return scaledFont(base, FontCaption, QFont::Medium);
}

}   // namespace Theme
