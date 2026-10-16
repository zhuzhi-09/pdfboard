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

#include "ThemePresets.generated.h"

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

// 「主题色」种子与「调色板风格」预设。索引对应 ThemePresets.generated.h 里的表
// （见 tools/md3-presets），改表或加种子时两边一起改。
enum class PaletteStyle { Fidelity = 0, TonalSpot = 1, Neutral = 2, Monochrome = 3 };
constexpr int kDefaultPreset = 0;                        // 品牌蓝
constexpr int kDefaultStyle  = int(PaletteStyle::Fidelity);   // 投影最优的风格

// MD3 调色板由 (种子, 风格, 明暗) 三者决定；这里只做查表 + 派生旧语义名。
inline Palette makePalette(bool dark, int preset, int style)
{
    preset = qBound(0, preset, ThemePresets::kSeedCount - 1);
    style  = qBound(0, style,  ThemePresets::kStyleCount - 1);
    const ThemePresets::Scheme &d = ThemePresets::kSchemes[preset][style][dark ? 1 : 0];
    const auto col = [](const ThemePresets::Rgb &v) { return QColor(v.r, v.g, v.b); };

    Palette c;
    c.primary              = col(d.primary);
    c.onPrimary            = col(d.onPrimary);
    c.primaryContainer     = col(d.primaryContainer);
    c.onPrimaryContainer   = col(d.onPrimaryContainer);
    c.secondary            = col(d.secondary);
    c.onSecondary          = col(d.onSecondary);
    c.secondaryContainer   = col(d.secondaryContainer);
    c.onSecondaryContainer = col(d.onSecondaryContainer);
    c.tertiary             = col(d.tertiary);
    c.onTertiary           = col(d.onTertiary);
    c.tertiaryContainer    = col(d.tertiaryContainer);
    c.onTertiaryContainer  = col(d.onTertiaryContainer);
    c.error                = col(d.error);
    c.onError              = col(d.onError);
    c.errorContainer       = col(d.errorContainer);
    c.onErrorContainer     = col(d.onErrorContainer);
    c.surfaceDim              = col(d.surfaceDim);
    c.surfaceBright           = col(d.surfaceBright);
    c.surfaceContainerLowest  = col(d.surfaceContainerLowest);
    c.surfaceContainerLow     = col(d.surfaceContainerLow);
    c.surfaceContainer        = col(d.surfaceContainer);
    c.surfaceContainerHigh    = col(d.surfaceContainerHigh);
    c.surfaceContainerHighest = col(d.surfaceContainerHighest);
    c.onSurface          = col(d.onSurface);
    c.onSurfaceVariant   = col(d.onSurfaceVariant);
    c.outline            = col(d.outline);
    c.outlineVariant     = col(d.outlineVariant);
    c.inverseSurface     = col(d.inverseSurface);
    c.inverseOnSurface   = col(d.inverseOnSurface);
    c.inversePrimary     = col(d.inversePrimary);
    c.scrim              = col(d.scrim);

    finishPalette(c, dark);
    return c;
}

// The factories keep (preset, style) defaulted so every existing caller compiles
// unchanged and starts on 品牌蓝 / fidelity - the look the app shipped with.
inline Palette makeLightPalette(int preset = kDefaultPreset, int style = kDefaultStyle)
{
    return makePalette(false, preset, style);
}

inline Palette makeDarkPalette(int preset = kDefaultPreset, int style = kDefaultStyle)
{
    return makePalette(true, preset, style);
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

// 「主题色」种子 / 「调色板风格」的运行时状态 (indexes into the generated table,
// see makePalette above). The setters re-apply immediately, like setMode does.
inline int g_preset = kDefaultPreset;
inline int g_style  = kDefaultStyle;

inline int preset() { return g_preset; }
inline int style()  { return g_style; }

// applyTheme is defined just below; declared first so the setters can re-apply.
inline void applyTheme();

inline void setPreset(int p) { g_preset = qBound(0, p, ThemePresets::kSeedCount - 1); applyTheme(); }
inline void setStyle(int s)  { g_style  = qBound(0, s,  ThemePresets::kStyleCount - 1); applyTheme(); }

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

// Swaps the palette to match resolvedMode() with the current (preset, style).
// Call at startup before any widget is built, and again whenever the setting or
// the OS scheme changes.
inline void applyTheme()
{
    g_palette = (resolvedMode() == Mode::Dark) ? makeDarkPalette(g_preset, g_style)
                                               : makeLightPalette(g_preset, g_style);
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

// Applies the STORED preferences (mode 0/1/2, preset, style) in one shot. Every
// path that re-applies the theme must go through a function like this: the
// settings page only persists the values, so the runtime state has to be re-synced.
inline void applyStoredTheme(int storedMode, int storedPreset, int storedStyle)
{
    modeRef() = static_cast<Mode>(qBound(0, storedMode, 2));
    g_preset  = qBound(0, storedPreset, ThemePresets::kSeedCount - 1);
    g_style   = qBound(0, storedStyle,  ThemePresets::kStyleCount - 1);
    applyTheme();
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

// The canvas reports each stroke's start and end exactly once (on the
// transition of its own drawing flag), so several open documents can share
// the counter.
inline void inkBegan() { ++g_inkActive; }
inline void inkEnded() { g_inkActive = qMax(0, g_inkActive - 1); }

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

// Alpha (0..255) of the soft drop shadow under floating chrome (island,
// palettes, page grid). MD3 carries elevation mostly in the container tone, so
// the shadow only has to lift the chrome off a white page - kept light.
constexpr int ChromeShadowAlpha = 0x30;

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
