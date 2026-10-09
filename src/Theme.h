#pragma once

#include <QColor>
#include <QEasingCurve>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPointF>
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

// Material Design 3 colour roles plus the legacy names the chrome was written
// against. The roles are the source of truth; every legacy field is DERIVED
// from them in finishPalette(), so a call site that still says `accent` or
// `surfaceHover` already paints with the MD3 scheme.
//
// The two schemes are the MD3 "fidelity" variant (spec 2021) generated with
// material-color-utilities from the brand seed #1A73E8: fidelity keeps the
// seed's chroma, so the primary stays a clear blue on a washed-out classroom
// projector instead of the greyish blue the tonal-spot variant produces.
struct Palette {
    // ---- MD3 colour roles -------------------------------------------------
    QColor primary;
    QColor onPrimary;
    QColor primaryContainer;
    QColor onPrimaryContainer;
    QColor secondary;
    QColor onSecondary;
    QColor secondaryContainer;      // active indicator, tonal buttons, selected chips
    QColor onSecondaryContainer;
    QColor tertiary;
    QColor onTertiary;
    QColor tertiaryContainer;
    QColor onTertiaryContainer;
    QColor error;
    QColor onError;
    QColor errorContainer;
    QColor onErrorContainer;

    QColor surfaceDim;
    QColor surfaceBright;
    QColor surfaceContainerLowest;
    QColor surfaceContainerLow;
    QColor surfaceContainer;
    QColor surfaceContainerHigh;
    QColor surfaceContainerHighest;
    QColor onSurface;
    QColor onSurfaceVariant;
    QColor outline;
    QColor outlineVariant;
    QColor inverseSurface;
    QColor inverseOnSurface;
    QColor inversePrimary;
    QColor scrim;

    // ---- Legacy names (derived, see finishPalette) ------------------------
    // Brand / selection.
    QColor accent;          // = primary
    QColor accentHover;     // = primary + onPrimary hover layer
    QColor accentSoft;      // translucent primary wash (selection, focus ring)
    QColor onAccent;        // = onPrimary

    // Canvas ("paper on a desk").
    QColor desk;            // the flat plane the pages rest on
    QColor deskShade;       // deeper tone: top edge + gutter behind the scroll bars
    QColor paper;
    QColor pageEdge;        // hairline around a page
    QColor pageShadow;      // base colour; the alpha comes from PageShadow

    // Floating chrome.
    QColor surface;         // the container colour of floating chrome
    QColor surfaceEdge;     // = outlineVariant
    QColor surfaceHover;    // onSurface hover state layer (8 %)
    QColor surfacePressed;  // onSurface pressed state layer (10 %)
    QColor chipTint;        // inset pill (page counter, status chips, off tracks)

    // Text.
    QColor text;            // = onSurface
    QColor textMuted;       // = onSurfaceVariant
    QColor textDisabled;    // onSurface at 38 %
    QColor divider;         // = outlineVariant

    // Drop shadow of the floating chrome.
    QColor shadow;
};

// ---------------------------------------------------------------------------
// State layers
// ---------------------------------------------------------------------------

// MD3 state-layer opacities: an interactive element shows its state by laying
// its CONTENT colour over its container at this opacity.
namespace State {
constexpr qreal Hover             = 0.08;
constexpr qreal Focus             = 0.10;
constexpr qreal Pressed           = 0.10;
constexpr qreal Dragged           = 0.16;
constexpr qreal DisabledContent   = 0.38;
constexpr qreal DisabledContainer = 0.12;
}   // namespace State

// `c` with its alpha replaced by `opacity` (0..1).
inline QColor withAlpha(QColor c, qreal opacity)
{
    c.setAlphaF(float(qBound<qreal>(0.0, opacity, 1.0)));
    return c;
}

// `layer` composited over the opaque `base` at `opacity`: the flattened colour
// of a container under a state layer. Opaque, so it is cheap to paint and can
// be used where a translucent fill would let the page show through.
inline QColor stateLayer(const QColor &base, const QColor &layer, qreal opacity)
{
    const qreal a = qBound<qreal>(0.0, opacity, 1.0);
    return QColor::fromRgbF(float(base.redF()   * (1.0 - a) + layer.redF()   * a),
                            float(base.greenF() * (1.0 - a) + layer.greenF() * a),
                            float(base.blueF()  * (1.0 - a) + layer.blueF()  * a),
                            base.alphaF());
}

// Fills every legacy field from the MD3 roles. `dark` only picks the
// scheme-specific desk / chrome container steps.
inline void finishPalette(Palette &c, bool dark)
{
    c.accent      = c.primary;
    c.accentHover = stateLayer(c.primary, c.onPrimary, State::Hover);
    c.accentSoft  = withAlpha(c.primary, dark ? 0.18 : 0.12);
    c.onAccent    = c.onPrimary;

    // The desk is a mid container step; floating chrome sits one step brighter
    // (light) or higher (dark) so it reads as a separate surface without
    // relying on a heavy shadow - MD3 expresses elevation with tone.
    c.desk      = dark ? c.surfaceDim             : c.surfaceContainer;
    c.deskShade = dark ? c.surfaceContainerLowest : c.surfaceContainerHighest;
    c.surface   = dark ? c.surfaceContainerHigh   : c.surfaceContainerLow;

    c.paper      = QColor(0xFF, 0xFF, 0xFF);   // pages stay paper white in both modes
    c.pageEdge   = dark ? c.outlineVariant : withAlpha(c.onSurface, 0.14);
    c.pageShadow = QColor(0x00, 0x00, 0x00);

    c.surfaceEdge    = c.outlineVariant;
    c.surfaceHover   = withAlpha(c.onSurface, State::Hover);
    c.surfacePressed = withAlpha(c.onSurface, State::Pressed);
    c.chipTint       = c.surfaceContainerHighest;

    c.text         = c.onSurface;
    c.textMuted    = c.onSurfaceVariant;
    c.textDisabled = withAlpha(c.onSurface, State::DisabledContent);
    c.divider      = c.outlineVariant;

    c.shadow = QColor(0x00, 0x00, 0x00);
}

inline Palette makeLightPalette()
{
    Palette c;
    c.primary                 = QColor(0x00, 0x5B, 0xBF);
    c.onPrimary               = QColor(0xFF, 0xFF, 0xFF);
    c.primaryContainer        = QColor(0x1A, 0x73, 0xE8);
    c.onPrimaryContainer      = QColor(0xFF, 0xFF, 0xFF);
    c.secondary               = QColor(0x47, 0x5E, 0x8C);
    c.onSecondary             = QColor(0xFF, 0xFF, 0xFF);
    c.secondaryContainer      = QColor(0xB2, 0xC9, 0xFE);
    c.onSecondaryContainer    = QColor(0x3D, 0x54, 0x81);
    c.tertiary                = QColor(0x9E, 0x43, 0x00);
    c.onTertiary              = QColor(0xFF, 0xFF, 0xFF);
    c.tertiaryContainer       = QColor(0xC5, 0x55, 0x00);
    c.onTertiaryContainer     = QColor(0x0E, 0x02, 0x00);
    c.error                   = QColor(0xBA, 0x1A, 0x1A);
    c.onError                 = QColor(0xFF, 0xFF, 0xFF);
    c.errorContainer          = QColor(0xFF, 0xDA, 0xD6);
    c.onErrorContainer        = QColor(0x93, 0x00, 0x0A);

    c.surfaceDim              = QColor(0xD8, 0xD9, 0xE3);
    c.surfaceBright           = QColor(0xF9, 0xF9, 0xFF);
    c.surfaceContainerLowest  = QColor(0xFF, 0xFF, 0xFF);
    c.surfaceContainerLow     = QColor(0xF2, 0xF3, 0xFD);
    c.surfaceContainer        = QColor(0xEC, 0xED, 0xF7);
    c.surfaceContainerHigh    = QColor(0xE6, 0xE8, 0xF2);
    c.surfaceContainerHighest = QColor(0xE0, 0xE2, 0xEC);
    c.onSurface               = QColor(0x19, 0x1C, 0x23);
    c.onSurfaceVariant        = QColor(0x41, 0x47, 0x54);
    c.outline                 = QColor(0x72, 0x77, 0x85);
    c.outlineVariant          = QColor(0xC1, 0xC6, 0xD6);
    c.inverseSurface          = QColor(0x2D, 0x30, 0x38);
    c.inverseOnSurface        = QColor(0xEF, 0xF0, 0xFA);
    c.inversePrimary          = QColor(0xAD, 0xC7, 0xFF);
    c.scrim                   = QColor(0x00, 0x00, 0x00);

    finishPalette(c, false);
    return c;
}

inline Palette makeDarkPalette()
{
    Palette c;
    c.primary                 = QColor(0xAD, 0xC7, 0xFF);
    c.onPrimary               = QColor(0x00, 0x2E, 0x68);
    c.primaryContainer        = QColor(0x1A, 0x73, 0xE8);
    c.onPrimaryContainer      = QColor(0xFF, 0xFF, 0xFF);
    c.secondary               = QColor(0xAF, 0xC7, 0xFB);
    c.onSecondary             = QColor(0x15, 0x30, 0x5B);
    c.secondaryContainer      = QColor(0x2E, 0x46, 0x73);
    c.onSecondaryContainer    = QColor(0xD6, 0xE2, 0xFF);   // lifted from 9EB5E8 for projector contrast
    c.tertiary                = QColor(0xFF, 0xB6, 0x91);
    c.onTertiary              = QColor(0x55, 0x21, 0x00);
    c.tertiaryContainer       = QColor(0xC5, 0x55, 0x00);
    c.onTertiaryContainer     = QColor(0x0E, 0x02, 0x00);
    c.error                   = QColor(0xFF, 0xB4, 0xAB);
    c.onError                 = QColor(0x69, 0x00, 0x05);
    c.errorContainer          = QColor(0x93, 0x00, 0x0A);
    c.onErrorContainer        = QColor(0xFF, 0xDA, 0xD6);

    c.surfaceDim              = QColor(0x10, 0x13, 0x1A);
    c.surfaceBright           = QColor(0x36, 0x39, 0x41);
    c.surfaceContainerLowest  = QColor(0x0B, 0x0E, 0x15);
    c.surfaceContainerLow     = QColor(0x19, 0x1C, 0x23);
    c.surfaceContainer        = QColor(0x1D, 0x20, 0x27);
    c.surfaceContainerHigh    = QColor(0x27, 0x2A, 0x31);
    c.surfaceContainerHighest = QColor(0x32, 0x35, 0x3C);
    c.onSurface               = QColor(0xE0, 0xE2, 0xEC);
    c.onSurfaceVariant        = QColor(0xC1, 0xC6, 0xD6);
    c.outline                 = QColor(0x8B, 0x90, 0x9F);
    c.outlineVariant          = QColor(0x41, 0x47, 0x54);
    c.inverseSurface          = QColor(0xE0, 0xE2, 0xEC);
    c.inverseOnSurface        = QColor(0x2D, 0x30, 0x38);
    c.inversePrimary          = QColor(0x00, 0x5B, 0xC0);
    c.scrim                   = QColor(0x00, 0x00, 0x00);

    finishPalette(c, true);
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

// MD3 shape scale (logical px at 100 %). "Full" is not a number: a fully
// rounded shape uses half of its own height, see shapeFull().
enum Shape {
    ShapeNone        = 0,
    ShapeExtraSmall  = 4,
    ShapeSmall       = 8,
    ShapeMedium      = 12,
    ShapeLarge       = 16,
    ShapeExtraLarge  = 28,
};

inline int shapeFull(int height) { return height / 2; }

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

// MD3 motion tokens. Durations are in milliseconds; the curves are the MD3
// cubic beziers. Use Motion::duration() rather than the raw value so the
// "减少动效" switch and the pen-down gate are honoured everywhere.
//
// Performance contract (the canvas must never lose a frame to the chrome):
//  - animate small chrome only (buttons, indicators, popovers) - never the canvas;
//  - while a stroke is being drawn, chrome animations jump to their end value;
//  - every animation is a QVariantAnimation / QPropertyAnimation that stops
//    itself, never a free-running timer.
namespace Motion {

enum Duration {
    Short1  = 50,
    Short2  = 100,
    Short3  = 150,
    Short4  = 200,
    Medium1 = 250,
    Medium2 = 300,
    Medium3 = 350,
    Medium4 = 400,
    Long1   = 450,
    Long2   = 500,
};

enum class Easing {
    Standard,
    StandardDecelerate,
    StandardAccelerate,
    EmphasizedDecelerate,
    EmphasizedAccelerate,
};

inline QEasingCurve curve(Easing e)
{
    QPointF c1, c2;
    switch (e) {
    case Easing::Standard:             c1 = { 0.20, 0.00 }; c2 = { 0.00, 1.00 }; break;
    case Easing::StandardDecelerate:   c1 = { 0.00, 0.00 }; c2 = { 0.00, 1.00 }; break;
    case Easing::StandardAccelerate:   c1 = { 0.30, 0.00 }; c2 = { 1.00, 1.00 }; break;
    case Easing::EmphasizedDecelerate: c1 = { 0.05, 0.70 }; c2 = { 0.10, 1.00 }; break;
    case Easing::EmphasizedAccelerate: c1 = { 0.30, 0.00 }; c2 = { 0.80, 0.15 }; break;
    }
    QEasingCurve curve(QEasingCurve::BezierSpline);
    curve.addCubicBezierSegment(c1, c2, QPointF(1.0, 1.0));
    return curve;
}

// The user's 减少动效 preference (settings), and a counter the canvas raises
// while a pen / finger stroke is in progress.
inline bool g_reduced = false;
inline int g_inkActive = 0;

inline bool reduced() { return g_reduced; }
inline void setReduced(bool on) { g_reduced = on; }

// RAII guard the canvas holds for the length of a stroke.
struct InkGate {
    InkGate()  { ++g_inkActive; }
    ~InkGate() { --g_inkActive; }
    InkGate(const InkGate &) = delete;
    InkGate &operator=(const InkGate &) = delete;
};

inline bool inking() { return g_inkActive > 0; }

// The duration an animation should actually run: 0 (jump to the end) when
// motion is reduced or a stroke is in progress.
inline int duration(Duration d)
{
    return (reduced() || inking()) ? 0 : int(d);
}

}   // namespace Motion

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
//
// `compact` drops the reserved label row: a chip button that shows ONLY its
// glyph (the island's 简约 style) needs no room for a text line, so its height
// falls to the touch target itself. Without this the bar would stay as tall as
// the labelled one - the container asks for the same buttonHeight either way.
inline Metrics metrics(const QFont &font, qreal scale = 1.0, bool compact = false)
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

    if (compact) {
        // Glyph only: no label row is reserved, and the button IS the touch
        // target (no extra +6 breathing room), so the bar is as slim as the
        // classroom panel still allows.
        m.buttonHeight  = int(qMax<qreal>(qreal(m.touch),
                                          qreal(m.icon) + m.padY * 2.0 + 2.0));
    } else {
        // A chip button stacks its glyph over its label. The minimum height must
        // cover that stack plus the padding, otherwise the label is clipped.
        const qreal stack = m.icon + qMax<qreal>(6.0, h * 0.40) + h;
        m.buttonHeight  = int(qMax<qreal>(qreal(m.touch) + 6.0,
                                          stack + m.padY * 2.0 + 2.0));
    }
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
