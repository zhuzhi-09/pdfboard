#include "SettingsPage.h"

#include "AppSettings.h"
#include "AppLog.h"
#include "IconPainter.h"
#include "Theme.h"
#include "UpdateCheck.h"
#include "WordConvert.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QByteArray>
#include <QAction>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImage>
#include <QMenu>
#include <QInputDevice>
#include <QLabel>
#include <QPropertyAnimation>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QStackedWidget>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleFactory>
#include <QToolButton>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <functional>
#include <utility>

#include <windows.h>

namespace {

// The project's licence, shown verbatim in 关于. Keep in sync with LICENSE.
const char *kMitLicenseText =
    "Copyright (c) 2026 zhuzhi-09\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy "
    "of this software and associated documentation files (the \"Software\"), to deal "
    "in the Software without restriction, including without limitation the rights "
    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
    "copies of the Software, and to permit persons to whom the Software is "
    "furnished to do so, subject to the following conditions:\n\n"
    "The above copyright notice and this permission notice shall be included in all "
    "copies or substantial portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR "
    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, "
    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE "
    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER "
    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, "
    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE "
    "SOFTWARE.";

// The GitHub release page the 更新 card opens in the browser. The app itself
// never goes online: it only hands this URL to the system browser.
const char *kReleasesLatest =
    "https://github.com/zhuzhi-09/pdfboard/releases/latest";

// 关于 shows the version baked into the executable's own version resource
// (assets/app.rc), so the installer, the file properties and this page can
// never disagree about what is running.
QString appVersion()
{
    const QString path = QCoreApplication::applicationFilePath();
    const auto *wide = reinterpret_cast<const wchar_t *>(path.utf16());

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(wide, &ignored);
    if (size == 0)
        return QStringLiteral("-");

    QByteArray buffer(int(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(wide, 0, size, buffer.data()))
        return QStringLiteral("-");

    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.constData(), L"\\",
                        reinterpret_cast<LPVOID *>(&info), &length)
        || !info) {
        return QStringLiteral("-");
    }
    return QStringLiteral("%1.%2.%3")
        .arg(HIWORD(info->dwFileVersionMS))
        .arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS));
}

// The on/off control: an MD3 switch painted with the Theme tokens. Doing it by
// hand - rather than through a style sheet - keeps the app free of image
// assets, and the metrics follow the font, so the switch grows with the
// display DPI and the touch panel's hit targets.
//
// MD3 anatomy (32 x 52 at 100 %): off = surfaceContainerHighest track with a
// 2 px outline and a small outline-coloured handle; on = primary track with a
// large onPrimary handle; pressed = the handle grows to its full size. The
// handle slides and grows with the MD3 standard curve - one small widget, so
// the animation costs a few tiny repaints and stops by itself.
class ToggleSwitch : public QAbstractButton
{
public:
    explicit ToggleSwitch(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setAttribute(Qt::WA_Hover, true);

        m_anim = new QVariantAnimation(this);
        m_anim->setEasingCurve(Theme::Motion::curve(Theme::Motion::Easing::Standard));
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_pos = v.toReal();
            update();
        });
        // Programmatic setChecked (loading the stored setting) fires toggled
        // too; it animates the same short way, which is harmless.
        connect(this, &QAbstractButton::toggled, this, [this](bool on) {
            const qreal target = on ? 1.0 : 0.0;
            const int ms = Theme::Motion::duration(Theme::Motion::Medium1);
            m_anim->stop();
            if (ms <= 0 || !isVisible()) {
                m_pos = target;
                update();
                return;
            }
            m_anim->setDuration(ms);
            m_anim->setStartValue(m_pos);
            m_anim->setEndValue(target);
            m_anim->start();
        });
    }

    QSize sizeHint() const override
    {
        const int h = trackHeight();
        return QSize(int(h * 52.0 / 32.0 + 0.5), h);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        const QSize hint = sizeHint();
        const qreal h = hint.height();
        const qreal unit = h / 32.0;                 // 1 MD3 dp at this size
        const QRectF track((width() - hint.width()) / 2.0, (height() - h) / 2.0,
                           hint.width(), h);
        const qreal radius = h / 2.0;
        const bool enabled = isEnabled();
        const qreal t = m_pos;                       // 0 = off, 1 = on

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Track: blend between the off and on containers while sliding.
        const QColor offTrack = pal.surfaceContainerHighest;
        const QColor onTrack  = pal.primary;
        QColor trackFill = Theme::stateLayer(offTrack, onTrack, t);
        if (!enabled)
            trackFill = Theme::withAlpha(t > 0.5 ? pal.onSurface : offTrack,
                                         t > 0.5 ? Theme::State::DisabledContainer : 1.0);
        const qreal border = 2.0 * unit * (1.0 - t);
        p.setPen(border > 0.05 ? QPen(enabled ? pal.outline
                                              : Theme::withAlpha(pal.onSurface,
                                                                 Theme::State::DisabledContainer),
                                      border)
                               : QPen(Qt::NoPen));
        p.setBrush(trackFill);
        const qreal inset = border / 2.0;
        p.drawRoundedRect(track.adjusted(inset, inset, -inset, -inset),
                          radius - inset, radius - inset);

        // Handle: 16 dp off, 24 dp on, 28 dp while pressed.
        qreal handle = (16.0 + 8.0 * t) * unit;
        if (isDown())
            handle = 28.0 * unit;
        const qreal travelLeft  = track.left() + 16.0 * unit;     // centre when off
        const qreal travelRight = track.right() - 16.0 * unit;    // centre when on
        const QPointF centre(travelLeft + (travelRight - travelLeft) * t,
                             track.center().y());

        // Hover / focus / press state layer: a 40 dp disc behind the handle.
        // MD3 shows focus only for KEYBOARD focus: a click also focuses the
        // switch, and the disc used to stay on after the pointer had left. Hover
        // is for a real pointer only: after a tap Windows parks the synthesized
        // cursor on the switch, which would keep the disc on as well.
        const bool hovered = underMouse() && !m_touchPress;
        const bool keyFocus = hasFocus() && m_keyboardFocus;
        if (enabled && (hovered || keyFocus || isDown())) {
            const qreal opacity = isDown() ? Theme::State::Pressed
                                : keyFocus ? Theme::State::Focus
                                           : Theme::State::Hover;
            const QColor layer = t > 0.5 ? pal.primary : pal.onSurface;
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::withAlpha(layer, opacity));
            p.drawEllipse(centre, 20.0 * unit, 20.0 * unit);
        }

        QColor handleFill = Theme::stateLayer(pal.outline, pal.onPrimary, t);
        if (!enabled)
            handleFill = t > 0.5 ? pal.surface
                                 : Theme::withAlpha(pal.onSurface, Theme::State::DisabledContent);
        p.setPen(Qt::NoPen);
        p.setBrush(handleFill);
        p.drawEllipse(centre, handle / 2.0, handle / 2.0);
    }

    void showEvent(QShowEvent *e) override
    {
        QAbstractButton::showEvent(e);
        m_anim->stop();
        m_pos = isChecked() ? 1.0 : 0.0;   // never replay a slide that happened while hidden
    }

    void enterEvent(QEnterEvent *e) override
    {
        QAbstractButton::enterEvent(e);
        update();
    }

    void leaveEvent(QEvent *e) override
    {
        QAbstractButton::leaveEvent(e);
        m_touchPress = false;
        update();
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        m_touchPress = e->source() != Qt::MouseEventNotSynthesized
                       || (e->device() && e->device()->type() != QInputDevice::DeviceType::Mouse);
        QAbstractButton::mousePressEvent(e);
    }

    void focusInEvent(QFocusEvent *e) override
    {
        QAbstractButton::focusInEvent(e);
        m_keyboardFocus = e->reason() == Qt::TabFocusReason
                          || e->reason() == Qt::BacktabFocusReason
                          || e->reason() == Qt::ShortcutFocusReason;
        update();
    }

    void focusOutEvent(QFocusEvent *e) override
    {
        QAbstractButton::focusOutEvent(e);
        m_keyboardFocus = false;
        update();
    }

private:
    int trackHeight() const
    {
        const Theme::Metrics m = Theme::metrics(font());
        return qMax(int(m.touch * 0.62), qMax(24, fontMetrics().height()));
    }

    QVariantAnimation *m_anim = nullptr;
    qreal m_pos = 0.0;
    bool  m_keyboardFocus = false;   // focus came from Tab, not from a click
    bool  m_touchPress = false;      // last press was a tap (synthesized mouse)
};

// 「主题色」的一枚种子色板：一个实心圆，颜色取该种子在默认风格下的 primary
// （fidelity 保留种子本身彩度，是这个种子最有代表性的颜色）。选中的一枚画一圈
// 实时调色板的 primary 描边；悬停 / 按下按 MD3 状态层铺一层文本色圆晕。全自绘，
// 无图片资源，颜色随主题实时变化，尺寸跟随字体缩放（和 ToggleSwitch 同一套做法）。
class SeedSwatch : public QAbstractButton
{
public:
    explicit SeedSwatch(int seed, QWidget *parent = nullptr)
        : QAbstractButton(parent)
        , m_seed(seed)
    {
        setCheckable(true);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        // Every swatch is a touch target: the same floor as every button row.
        const int side = Theme::metrics(font()).touch;
        return QSize(side, side);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        const QColor fill = Theme::makeLightPalette(m_seed, Theme::kDefaultStyle).primary;

        const qreal r     = qMin(width(), height()) / 2.0 - 1.0;   // 1 px AA slack
        const qreal ring  = qMax(2.0, r * 0.14);                   // selected ring
        const qreal gap   = ring * 0.9;                            // surface gap
        const qreal fillR = r - ring - gap;
        const QPointF centre(width() / 2.0, height() / 2.0);

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Hover / press: an MD3 state-layer disc in the text colour, under the
        // fill so the swatch colour always stays true.
        if (isDown() || underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::withAlpha(pal.text,
                                        isDown() ? Theme::State::Pressed
                                                 : Theme::State::Hover));
            p.drawEllipse(centre, r, r);
        }

        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawEllipse(centre, fillR, fillR);

        // Selected: a ring in the live primary, one surface-gap clear of the fill
        // (MD3 selection never touches the selected content).
        if (isChecked()) {
            QPen ringPen(pal.primary, ring);
            ringPen.setCapStyle(Qt::RoundCap);
            p.setPen(ringPen);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(centre, fillR + gap + ring / 2.0, fillR + gap + ring / 2.0);
        }
    }

    void enterEvent(QEnterEvent *e) override
    {
        QAbstractButton::enterEvent(e);
        update();
    }

    void leaveEvent(QEvent *e) override
    {
        QAbstractButton::leaveEvent(e);
        update();
    }

private:
    int m_seed = 0;
};

// MD3 common buttons, all fully rounded at the touch height:
//   plain QPushButton          -> Outlined button (outline border, primary label)
//   #settingsPrimary           -> Filled button  (primary / onPrimary)
// Hover / press are the MD3 state layers flattened onto the container, so a
// button never turns see-through over the card.
QString buttonSheet(const Theme::Palette &c, const Theme::Metrics &m)
{
    const QString radius = Theme::px(Theme::shapeFull(m.touch));
    const QColor card = c.surfaceContainerHigh;   // the list items the buttons sit on

    return QStringLiteral(
               "QPushButton {"
               " color: %1;"
               " background: transparent;"
               " border: 1px solid %2;"
               " border-radius: %3;"
               " padding: %4 %5; }"
               "QPushButton:hover { background: %6; }"
               "QPushButton:pressed { background: %7; }"
               "QPushButton:disabled { color: %8; border: 1px solid %9; }")
               .arg(Theme::rgba(c.primary))
               .arg(Theme::rgba(c.outline))
               .arg(radius)
               .arg(Theme::px(m.padY))
               .arg(Theme::px(Theme::Space5))           // MD3: 24 dp side padding
               .arg(Theme::rgba(Theme::stateLayer(card, c.primary, Theme::State::Hover)))
               .arg(Theme::rgba(Theme::stateLayer(card, c.primary, Theme::State::Pressed)))
               .arg(Theme::rgba(c.textDisabled))
               .arg(Theme::rgba(Theme::withAlpha(c.onSurface, Theme::State::DisabledContainer)))
        + QStringLiteral(
              "QPushButton#settingsPrimary {"
              " color: %1; background: %2; border-radius: %7; }"
              "QPushButton#settingsPrimary:hover { background: %3; }"
              "QPushButton#settingsPrimary:pressed { background: %4; }"
              "QPushButton#settingsPrimary:disabled { color: %5; background: %6; }")
              .arg(Theme::rgba(c.onPrimary))
              .arg(Theme::rgba(c.primary))
              .arg(Theme::rgba(Theme::stateLayer(c.primary, c.onPrimary, Theme::State::Hover)))
              .arg(Theme::rgba(Theme::stateLayer(c.primary, c.onPrimary, Theme::State::Pressed)))
              .arg(Theme::rgba(c.textDisabled))
              .arg(Theme::rgba(Theme::withAlpha(c.onSurface, Theme::State::DisabledContainer)))
               // `border: none` drops the base rule's radius, so the filled button went
               // square; restate it (the pill radius is the same all buttons share).
               .arg(radius)
        ;
}

// The sheet of the page. Applied to the inner root (never to the page itself)
// so the guidance message boxes spawned from here keep their native look.
QString pageSheet(const QFont &font)
{
    const Theme::Palette &c = Theme::light();
    const Theme::Metrics m = Theme::metrics(font);

    return QStringLiteral(
               "QWidget#settingsRoot, QWidget#settingsBody, QWidget#settingsRail"
               " { background: transparent; }"
               "QScrollArea#settingsScroll { background: transparent; border: none; }"
               "QLabel { color: %1; background: transparent; }"
               "QLabel#settingsRowBody, QLabel#settingsCrumbRoot,"
               " QLabel#settingsIdentityBody { color: %2; }"
               "QLabel#settingsSectionHeader { color: %3; }"
               // MD3 (Expressive) segmented list: the items of a group are
               // separate containers 2 dp apart; the group's outer corners are
               // large, the corners between items extra-small. No outline.
               "QFrame#settingsCard {"
               " background: %4;"
               " border: none;"
               " border-radius: %5; }"
               "QFrame#settingsCard[seg=\"first\"] {"
               " border-top-left-radius: %6; border-top-right-radius: %6; }"
               "QFrame#settingsCard[seg=\"last\"] {"
               " border-bottom-left-radius: %6; border-bottom-right-radius: %6; }"
               "QFrame#settingsCard[seg=\"single\"] { border-radius: %6; }")
        .arg(Theme::rgba(c.text))
        .arg(Theme::rgba(c.textMuted))
        .arg(Theme::rgba(c.primary))             // MD3 list subheader
        .arg(Theme::rgba(c.surfaceContainerHigh))
        .arg(Theme::px(Theme::ShapeExtraSmall))
        .arg(Theme::px(Theme::ShapeLarge))
        + buttonSheet(c, m);
}

// A glyph painted from the LIVE palette on every paint, so a theme switch needs
// no pixmap rebuild. Transparent for the mouse: a click on a card's icon is a
// click on the card.
class GlyphBox : public QWidget
{
public:
    enum class Role { Text, Muted };

    GlyphBox(IconPainter::Glyph glyph, int side, Role role, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_glyph(glyph)
        , m_role(role)
    {
        setFixedSize(side, side);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    void setGlyph(IconPainter::Glyph glyph)
    {
        m_glyph = glyph;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        QPainter p(this);
        IconPainter::paintGlyph(p, m_glyph, QRectF(rect()),
                                m_role == Role::Muted ? pal.textMuted : pal.text, 1.8);
    }

private:
    IconPainter::Glyph m_glyph;
    Role m_role;
};

// The app's own icon, read from the executable's icon resource (IDI_ICON1 in
// assets/app.rc - the one Explorer and the taskbar show), at `px` device pixels.
// Windows picks the closest of the .ico's sizes (16 .. 256) and scales it, so
// the badge stays sharp at any DPI. Null if the resource is missing.
QImage appIconImage(int px)
{
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), L"IDI_ICON1",
                                               IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
    if (!icon)
        return {};
    QImage image = QImage::fromHICON(icon);
    DestroyIcon(icon);
    return image;
}

// The app identity on top of the rail (where Windows shows the account) and on
// the 关于 page: the application icon itself. Without the resource (never in a
// shipped build) it falls back to a primaryContainer disc with the ink swash.
class AppBadge : public QWidget
{
public:
    explicit AppBadge(int side, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(side, side);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const qreal dpr = devicePixelRatioF();
        const int px = qCeil(width() * dpr);
        if (m_iconPx != px) {          // first paint, or the window changed screens
            m_iconPx = px;
            m_icon = appIconImage(px);
            if (!m_icon.isNull())
                m_icon.setDevicePixelRatio(qreal(m_icon.width()) / width());
        }
        if (!m_icon.isNull()) {
            p.drawImage(QRectF(rect()), m_icon);
            return;
        }

        const Theme::Palette &pal = Theme::light();
        p.setPen(Qt::NoPen);
        p.setBrush(pal.primaryContainer);
        p.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
        const qreal inset = width() * 0.24;
        IconPainter::paintGlyph(p, IconPainter::Glyph::Swash,
                                QRectF(rect()).adjusted(inset, inset, -inset, -inset),
                                pal.onPrimaryContainer, 1.6);
    }

private:
    QImage m_icon;
    int    m_iconPx = 0;
};

// MD3 measures are in dp; this app's dp is the touch target / 48.
qreal dpUnit(const QFont &font)
{
    return Theme::metrics(font).touch / 48.0;
}

// The container of the navigation. Wide: an MD3 standard navigation drawer
// (surfaceContainerLow, large END corners). Compact: an MD3 navigation rail,
// which sits on the page surface without a container of its own.
class RailPane : public QWidget
{
public:
    explicit RailPane(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("settingsRail"));
    }

    void setCompact(bool compact)
    {
        m_compact = compact;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_compact)
            return;
        const Theme::Palette &pal = Theme::light();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(pal.surfaceContainerLow);
        const qreal r = Theme::ShapeLarge;
        const QRectF box(rect());
        QPainterPath path;   // square start edge (it meets the window), round end
        path.moveTo(box.topLeft());
        path.lineTo(box.right() - r, box.top());
        path.quadTo(box.topRight(), QPointF(box.right(), box.top() + r));
        path.lineTo(box.right(), box.bottom() - r);
        path.quadTo(box.bottomRight(), QPointF(box.right() - r, box.bottom()));
        path.lineTo(box.bottomLeft());
        path.closeSubpath();
        p.drawPath(path);
    }

private:
    bool m_compact = false;
};

// One destination, in the two MD3 forms the navigation takes:
//   drawer item - 56 dp pill, 24 dp icon, labelLarge; the active indicator is a
//                 full-width secondaryContainer pill;
//   rail item   - 80 dp column, a 56 x 32 dp indicator pill around the icon and
//                 a labelMedium label under it.
// Idle icon and label are onSurfaceVariant; hover / press are state layers on
// the indicator shape.
class NavItem : public QAbstractButton
{
public:
    NavItem(IconPainter::Glyph glyph, const QString &text, QWidget *parent = nullptr)
        : QAbstractButton(parent)
        , m_glyph(glyph)
    {
        setText(text);
        setCheckable(true);
        setAutoExclusive(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setCompact(bool compact)
    {
        if (m_compact == compact)
            return;
        m_compact = compact;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        const qreal dp = dpUnit(font());
        if (m_compact) {
            const int label = QFontMetrics(railFont()).height();
            return QSize(qRound(80 * dp), qRound((4 + 32 + 4 + 12) * dp) + label);
        }
        const QFontMetrics fm(drawerFont());
        return QSize(qRound((16 + 24 + 12 + 24) * dp) + fm.horizontalAdvance(text()),
                     qRound(56 * dp));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        const qreal dp = dpUnit(font());
        const qreal icon = 24 * dp;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);

        // The indicator shape: the whole item in the drawer, a 56 x 32 pill
        // around the icon on the rail.
        const QRectF indicator = m_compact
            ? QRectF((width() - 56 * dp) / 2.0, 4 * dp, 56 * dp, 32 * dp)
            : QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const qreal radius = indicator.height() / 2.0;

        QColor fill = isChecked() ? pal.secondaryContainer : QColor(Qt::transparent);
        const QColor layerOn = isChecked() ? pal.onSecondaryContainer : pal.onSurface;
        if (isDown())
            fill = Theme::stateLayer(isChecked() ? fill : pal.surfaceContainerLow, layerOn,
                                     Theme::State::Pressed);
        else if (underMouse())
            fill = Theme::stateLayer(isChecked() ? fill : pal.surfaceContainerLow, layerOn,
                                     Theme::State::Hover);
        if (fill.alpha() > 0) {
            p.setBrush(fill);
            p.drawRoundedRect(indicator, radius, radius);
        }

        const QColor iconColor = isChecked() ? pal.onSecondaryContainer : pal.textMuted;
        if (m_compact) {
            IconPainter::paintGlyph(p, m_glyph,
                                    QRectF(indicator.center().x() - icon / 2.0,
                                           indicator.center().y() - icon / 2.0, icon, icon),
                                    iconColor, 1.8);
            p.setFont(railFont());
            p.setPen(isChecked() ? pal.text : pal.textMuted);
            const QRectF label(0, indicator.bottom() + 4 * dp, width(),
                               height() - indicator.bottom() - 4 * dp);
            p.drawText(label, Qt::AlignHCenter | Qt::AlignTop,
                       QFontMetrics(railFont()).elidedText(text(), Qt::ElideRight, width()));
            return;
        }

        IconPainter::paintGlyph(p, m_glyph,
                                QRectF(16 * dp, (height() - icon) / 2.0, icon, icon),
                                iconColor, 1.8);
        p.setFont(drawerFont());
        p.setPen(isChecked() ? pal.onSecondaryContainer : pal.textMuted);
        const qreal textX = (16 + 24 + 12) * dp;
        const QRectF textRect(textX, 0, qMax<qreal>(0, width() - textX - 24 * dp), height());
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(drawerFont()).elidedText(text(), Qt::ElideRight,
                                                         int(textRect.width())));
    }

    void enterEvent(QEnterEvent *e) override
    {
        QAbstractButton::enterEvent(e);
        update();
    }

    void leaveEvent(QEvent *e) override
    {
        QAbstractButton::leaveEvent(e);
        update();
    }

private:
    // labelLarge (drawer) / labelMedium (rail): medium weight, never bold - the
    // indicator alone marks the active destination.
    QFont drawerFont() const { return Theme::scaledFont(font(), 1.0, QFont::Medium); }
    QFont railFont() const { return Theme::scaledFont(font(), 0.86, QFont::Medium); }

    IconPainter::Glyph m_glyph;
    bool m_compact = false;
};

QPainterPath segmentPath(const QRectF &r, const QString &seg);

// MD3 "fade through" between two settings categories (sibling destinations
// without a spatial relationship): the outgoing frame fades out over the first
// 30 % with an accelerating curve, then the incoming one fades in while it
// scales 92 % -> 100 % with the emphasized-decelerate curve. Both frames are
// snapshots of the content area, so the animation costs two grabs and a few
// blits; the real page underneath is already the new one and gets the clicks.
class FadeThrough : public QWidget
{
public:
    explicit FadeThrough(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        hide();
    }

    // `background` is the page's own fill, so the fade never shows a seam.
    void setFrames(const QPixmap &from, const QPixmap &to, const QColor &background)
    {
        m_from = from;
        m_to = to;
        m_background = background;
    }

    void setProgress(qreal t)
    {
        m_t = qBound<qreal>(0.0, t, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        // The snapshots of the transparent content area carry no background.
        p.fillRect(rect(), m_background);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);

        constexpr qreal kOut = 0.3;
        if (m_t < kOut) {
            static const QEasingCurve out =
                Theme::Motion::curve(Theme::Motion::Easing::StandardAccelerate);
            p.setOpacity(1.0 - out.valueForProgress(m_t / kOut));
            p.drawPixmap(0, 0, m_from);
            return;
        }
        static const QEasingCurve in =
            Theme::Motion::curve(Theme::Motion::Easing::EmphasizedDecelerate);
        const qreal e = in.valueForProgress((m_t - kOut) / (1.0 - kOut));
        const qreal scale = 0.92 + 0.08 * e;
        p.setOpacity(e);
        p.translate(width() / 2.0, height() / 2.0);
        p.scale(scale, scale);
        p.translate(-width() / 2.0, -height() / 2.0);
        p.drawPixmap(0, 0, m_to);
    }

private:
    QPixmap m_from;
    QPixmap m_to;
    QColor  m_background;
    qreal   m_t = 0.0;
};

// A card the whole of which is a button (the 许可证 expander header): pointer
// cursor, a hover state layer laid over the card, and a callback on click.
class ClickCard : public QFrame
{
public:
    explicit ClickCard(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
    }

    std::function<void()> onClick;

protected:
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())
            && onClick) {
            onClick();
        }
        QFrame::mouseReleaseEvent(e);
    }

    void paintEvent(QPaintEvent *e) override
    {
        QFrame::paintEvent(e);       // the style sheet card
        if (!underMouse())
            return;
        const Theme::Palette &pal = Theme::light();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::withAlpha(pal.onSurface, Theme::State::Hover));
        p.drawPath(segmentPath(QRectF(rect()), property("seg").toString()));
    }

    void enterEvent(QEnterEvent *e) override
    {
        QFrame::enterEvent(e);
        update();
    }

    void leaveEvent(QEvent *e) override
    {
        QFrame::leaveEvent(e);
        update();
    }
};

// One setting = one card (the Windows 11 layout): leading glyph, title over a
// muted description, trailing control. Content that does not fit the trailing
// slot (button rows, swatches) goes UNDER the text column via addUnderText(),
// so it lines up with the title instead of the card edge.
struct SettingCard {
    QFrame      *frame = nullptr;
    QVBoxLayout *outer = nullptr;   // top row + anything added under the text
    QVBoxLayout *texts = nullptr;   // title, description (+ extra state lines)
    QLabel      *title = nullptr;
    QLabel      *body  = nullptr;   // hidden while empty
    int          textIndent = 0;    // x of the text column inside the card
};

int cardIconSide(const Theme::Metrics &m)
{
    return qMax(20, int(m.icon * 0.9));
}

QLabel *makeBodyLabel(QWidget *parent, const QString &text = QString())
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("settingsRowBody"));
    label->setFont(Theme::scaledFont(parent->font(), 0.95, QFont::Normal));
    label->setWordWrap(true);
    return label;
}

SettingCard makeSettingCard(QWidget *parent, IconPainter::Glyph glyph, const QString &title,
                            const QString &body, QWidget *trailing = nullptr,
                            QFrame *frame = nullptr)
{
    const Theme::Metrics m = Theme::metrics(parent->font());

    SettingCard card;
    card.frame = frame ? frame : new QFrame(parent);
    card.frame->setObjectName(QStringLiteral("settingsCard"));
    card.frame->setAttribute(Qt::WA_StyledBackground, true);
    card.frame->setMinimumHeight(m.touch + Theme::Space4);

    card.outer = new QVBoxLayout(card.frame);
    card.outer->setContentsMargins(Theme::Space4, Theme::Space3, Theme::Space4, Theme::Space3);
    card.outer->setSpacing(Theme::Space3);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(Theme::Space4);

    const int side = cardIconSide(m);
    top->addWidget(new GlyphBox(glyph, side, GlyphBox::Role::Muted, card.frame), 0,
                   Qt::AlignVCenter);
    card.textIndent = side + Theme::Space4;

    card.texts = new QVBoxLayout;
    card.texts->setContentsMargins(0, 0, 0, 0);
    card.texts->setSpacing(Theme::Space1 / 2);

    card.title = new QLabel(title, card.frame);
    card.title->setFont(Theme::scaledFont(card.frame->font(), 1.05, QFont::Normal));   // MD3 list headline: bodyLarge
    card.title->setWordWrap(true);
    card.texts->addWidget(card.title);

    card.body = makeBodyLabel(card.frame, body);
    card.body->setVisible(!body.isEmpty());
    card.texts->addWidget(card.body);

    top->addLayout(card.texts, 1);
    if (trailing) {
        trailing->setParent(card.frame);
        top->addWidget(trailing, 0, Qt::AlignVCenter);
    }
    card.outer->addLayout(top);
    return card;
}

void addUnderText(SettingCard &card, QWidget *content)
{
    auto *row = new QHBoxLayout;
    row->setContentsMargins(card.textIndent, 0, 0, 0);
    row->setSpacing(0);
    content->setParent(card.frame);
    row->addWidget(content, 0, Qt::AlignLeft);
    row->addStretch(1);
    card.outer->addLayout(row);
}

// ---------------------------------------------------------------------------
// 按钮行：设置页里每一行按钮都由下面这一套构造，行内间距、最小高度、字体与
// 状态配色因此完全一致——曾经每一行各写各的 QHBoxLayout（分段行的 spacing 还是
// 0），深色主题下相邻按钮的底色连成一片，看起来像「贴在一起」。
// ---------------------------------------------------------------------------

struct ButtonRow {
    QWidget     *box = nullptr;   // 放进卡片行的容器
    QHBoxLayout *row = nullptr;   // 按钮按加入顺序横排
};

// 行内按钮的用途：普通按钮 / 主操作按钮。
enum class RowButton { Plain, Primary };

ButtonRow makeButtonRow(QWidget *parent, const QFont &font)
{
    const Theme::Metrics m = Theme::metrics(font);

    ButtonRow row;
    row.box = new QWidget(parent);
    row.row = new QHBoxLayout(row.box);
    row.row->setContentsMargins(0, 0, 0, 0);
    // 8 逻辑像素起步（Space2），并跟随字号同比放大：全页每行都取同一个值，
    // 100 % 与 250 % 下都留出清晰可见的间隔，绝不会退化成 0。
    row.row->setSpacing(qMax(int(Theme::Space2), m.gap * 2));
    return row;
}

QPushButton *addRowButton(ButtonRow &row, const QFont &font, const QString &text,
                          RowButton kind = RowButton::Plain)
{
    const Theme::Metrics m = Theme::metrics(font);

    auto *button = new QPushButton(text, row.box);
    button->setCursor(Qt::PointingHandCursor);
    button->setFont(Theme::chromeFont(font));
    button->setMinimumHeight(m.touch);         // 触控目标：整页按钮行统一高度
    if (kind == RowButton::Primary)
        button->setObjectName(QStringLiteral("settingsPrimary"));
    row.row->addWidget(button);
    return button;
}

// A single trailing button (full touch height: Qt's style sheet only keeps the
// pill radius while it fits inside the button, radius <= height / 2).
QPushButton *makeButton(QWidget *parent, const QFont &font, const QString &text,
                        RowButton kind = RowButton::Plain)
{
    auto *button = new QPushButton(text, parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setFont(Theme::chromeFont(font));
    button->setMinimumHeight(Theme::metrics(font).touch);
    if (kind == RowButton::Primary)
        button->setObjectName(QStringLiteral("settingsPrimary"));
    return button;
}

// A group header inside a category page: muted, semibold, with the group gap
// above it (none for the first group of a page).
// MD3 list subheader (titleSmall, primary), indented to the items' content.
void addGroupHeader(QVBoxLayout *col, QWidget *parent, const QString &text, bool first)
{
    if (!first)
        col->addSpacing(Theme::Space5);
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("settingsSectionHeader"));
    label->setFont(Theme::scaledFont(parent->font(), 0.95, QFont::DemiBold));
    label->setIndent(Theme::Space4);
    col->addWidget(label);
    col->addSpacing(Theme::Space2);
}

// Marks every run of consecutive cards in a page column as one segmented group:
// seg = first / middle / last / single picks the corner radii in pageSheet().
void segmentGroups(QVBoxLayout *col)
{
    QVector<QWidget *> run;
    auto close = [&run] {
        for (int i = 0; i < run.size(); ++i) {
            const char *seg = run.size() == 1 ? "single"
                              : i == 0 ? "first"
                              : i == run.size() - 1 ? "last" : "middle";
            run.at(i)->setProperty("seg", QString::fromLatin1(seg));
        }
        run.clear();
    };
    for (int i = 0; i < col->count(); ++i) {
        QWidget *w = col->itemAt(i)->widget();
        if (w && w->objectName() == QLatin1String("settingsCard"))
            run.append(w);
        else if (w || col->itemAt(i)->spacerItem()) {
            // A header or a group gap ends the run; the 2 dp layout spacing
            // between items is not an item, so it does not.
            if (w || col->itemAt(i)->spacerItem()->sizeHint().height() > Theme::Space2)
                close();
        }
    }
    close();
}

// The outline of a segmented card, for painting over it (hover layer).
QPainterPath segmentPath(const QRectF &r, const QString &seg)
{
    const qreal wide = Theme::ShapeLarge;
    const qreal tight = Theme::ShapeExtraSmall;
    const bool top = seg == QLatin1String("first") || seg == QLatin1String("single");
    const bool bottom = seg == QLatin1String("last") || seg == QLatin1String("single");
    const qreal tr = top ? wide : tight;
    const qreal br = bottom ? wide : tight;
    QPainterPath path;
    path.moveTo(r.left() + tr, r.top());
    path.lineTo(r.right() - tr, r.top());
    path.quadTo(r.right(), r.top(), r.right(), r.top() + tr);
    path.lineTo(r.right(), r.bottom() - br);
    path.quadTo(r.right(), r.bottom(), r.right() - br, r.bottom());
    path.lineTo(r.left() + br, r.bottom());
    path.quadTo(r.left(), r.bottom(), r.left(), r.bottom() - br);
    path.lineTo(r.left(), r.top() + tr);
    path.quadTo(r.left(), r.top(), r.left() + tr, r.top());
    return path;
}

// ---------------------------------------------------------------------------
// SettingsSelect 的弹出层：原生 QMenu（见下方 SelectMenu）—— 宽度按内容算、
// 文本不省略、键盘 / 点外部 / Esc 都由 Qt 管；皮肤走 selectMenuSheet()，当前项
// 由菜单自己的对勾标出。
// ---------------------------------------------------------------------------

// 箭头与对勾共用的字形尺寸：跟随字体，最小 16 逻辑像素（= Space4）。
int selectGlyphSize(const Theme::Metrics &m)
{
    return qMax(int(Theme::Space4), int(m.icon * 0.80));
}

// 弹出的菜单卡片皮肤：surface 卡片 + outlineVariant 发丝线 + ShapeMedium 圆角；
// 行高向触控目标看齐，悬停 / 按下是压到卡片上的 MD3 状态层。当前项用强调色文字
// —— Qt 把行的文字色同时交给左侧的对勾，勾与字因此同为强调色，一眼看清选了哪个。
QString selectMenuSheet(const Theme::Palette &c, const QFont &font)
{
    const Theme::Metrics m = Theme::metrics(font);
    const int textH = QFontMetrics(font).height();
    const int padY = qMax(int(Theme::Space2), (int(m.touch) - textH + 1) / 2);
    const int rowRadius = qMax(int(Theme::ShapeSmall), m.radiusPill);

    return QStringLiteral(
               "QMenu {"
               " background-color: %1;"
               " border: 1px solid %2;"
               " border-radius: %3;"
               " padding: %4; }"
               "QMenu::item {"
               " padding: %5 %6 %5 %7;"
               " border-radius: %8;"
               " color: %9; }"
               "QMenu::item:selected { background-color: %10; }"
               "QMenu::item:pressed { background-color: %11; }"
               "QMenu::item:disabled { color: %12; }"
               "QMenu::item:checked { color: %13; }")
        .arg(Theme::rgba(c.surface))
        .arg(Theme::rgba(c.outlineVariant))
        .arg(Theme::px(Theme::ShapeMedium))
        .arg(Theme::px(Theme::Space1))
        .arg(Theme::px(padY))
        .arg(Theme::px(Theme::Space3))     // 右侧内边距
        .arg(Theme::px(Theme::Space4))     // 左侧给对勾留位
        .arg(Theme::px(rowRadius))
        .arg(Theme::rgba(c.text))
        .arg(Theme::rgba(Theme::stateLayer(c.surface, c.onSurface, Theme::State::Hover)))
        .arg(Theme::rgba(Theme::stateLayer(c.surface, c.onSurface, Theme::State::Pressed)))
        .arg(Theme::rgba(c.textDisabled))
        .arg(Theme::rgba(c.primary));
}

// QMenu 子类：补两件 Qt 不管的事 —— 去掉 Windows 给弹出窗口配的方形系统投影，
// 以及打开时一段 130 ms 的淡入（「减少动效」下直接到位）。窗口透明度由窗口系统
// 合成，不进任何部件的重绘路径。
class SelectMenu : public QMenu
{
public:
    explicit SelectMenu(QWidget *parent = nullptr)
        : QMenu(parent)
    {
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
        // 圆角卡片画在一个方形的弹出窗口里：窗口本身不透明时，圆角外的四个角会
        // 露出方形底色。透明窗口背景让角外真正透出下面的页面。
        setAttribute(Qt::WA_TranslucentBackground, true);
        // Qt 的 Windows 11 风格（qmodernwindowsstyle）画菜单面板时会在窗口右下角
        // 自带一块阴影，样式表的圆角盖不住它，圆角外就露出一个深色直角。菜单的
        // 外观全部来自 selectMenuSheet()，所以换成不画阴影的 Fusion 作为底层样式。
        static QStyle *const plainStyle = QStyleFactory::create(QStringLiteral("Fusion"));
        if (plainStyle)
            setStyle(plainStyle);
        setToolTipsVisible(true);   // 「调色板风格」每项的说明挂在 action tooltip 上
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QMenu::showEvent(event);

        const int ms = Theme::Motion::reduced() ? 0 : kFadeMs;
        if (ms <= 0) {
            setWindowOpacity(1.0);
            return;
        }
        setWindowOpacity(0.0);
        auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
        fade->setDuration(ms);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->setEasingCurve(QEasingCurve::OutCubic);
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void hideEvent(QHideEvent *event) override
    {
        setWindowOpacity(1.0);      // 收起即复位：下次打开都从 0 重新淡入
        QMenu::hideEvent(event);
    }

private:
    static constexpr int kFadeMs = 130;
};

}   // namespace

// ---------------------------------------------------------------------------
// 设置行上的下拉选择。收起态是自绘 QToolButton（无框：右对齐当前值 + 右缘一枚
// ChevronDown，悬停 / 按下铺圆角状态层，宽度按最宽选项算所以换值不跳行）；弹出层
// 是原生 QMenu —— 宽度永远按内容算（文本不会被省略）、键盘可导航、点外部与 Esc 都
// 归 Qt 管，换主题只换皮肤。只有用户真的从菜单里选定（QAction::triggered）才发
// changed(int)，程序性的 setCurrentIndex 不打扰外面。
// ---------------------------------------------------------------------------
class SettingsSelect : public QToolButton
{
    Q_OBJECT
public:
    explicit SettingsSelect(const QStringList &options, QWidget *parent = nullptr)
        : QToolButton(parent)
        , m_options(options)
    {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

        m_menu = new SelectMenu(this);
        auto *group = new QActionGroup(m_menu);
        group->setExclusive(true);
        for (int i = 0; i < m_options.size(); ++i) {
            QAction *action = m_menu->addAction(m_options.at(i));
            action->setCheckable(true);
            action->setActionGroup(group);
            connect(action, &QAction::triggered, this, [this, i] { emit changed(i); });
        }
        m_menu->setStyleSheet(selectMenuSheet(Theme::light(), font()));
        // 菜单收起后重画一次：清掉打开时被冻结的悬停层（见 paintEvent 的说明）。
        connect(m_menu, &QMenu::aboutToHide, this, [this] { update(); });

        setCurrentIndex(0);
    }

    int currentIndex() const { return m_index; }

    // 程序性同步：挡住 action 的信号，外部 connect 到 changed 的槽不会被打扰。
    void setCurrentIndex(int index)
    {
        const int n = m_options.size();
        if (n <= 0)
            return;
        const int i = qBound(0, index, n - 1);
        m_index = i;
        // 不挡信号：QActionGroup 是靠 action 的 toggled 信号取消旧选中项的，挡住它
        // 会让旧项一直保持勾选（菜单里出现两个对勾）。这里也不会误发 changed ——
        // changed 只从 QAction::triggered（真实点击）发出，程序性 setChecked 不触发它。
        const QList<QAction *> acts = m_menu->actions();
        if (i < acts.size())
            acts.at(i)->setChecked(true);
        syncTip();
        update();
    }

    // 每项的悬停提示（调色板风格行用它解释四种算法）；收起态显示当前项的。
    void setOptionTips(const QStringList &tips)
    {
        m_tips = tips;
        const QList<QAction *> acts = m_menu->actions();
        for (int i = 0; i < acts.size() && i < tips.size(); ++i)
            acts.at(i)->setToolTip(tips.at(i));
        syncTip();
    }

    void refreshTheme()
    {
        m_menu->setStyleSheet(selectMenuSheet(Theme::light(), font()));
        update();
    }

signals:
    void changed(int index);

protected:
    QSize sizeHint() const override
    {
        const Theme::Metrics m = Theme::metrics(font());
        const QFontMetrics fm(font());
        int text = 0;
        for (const QString &s : m_options)
            text = qMax(text, fm.horizontalAdvance(s));
        return QSize(text + 2 * Theme::Space3 + Theme::Space2 + selectGlyphSize(m),
                     qMax(int(m.touch), fm.height() + Theme::Space2));
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        const Theme::Metrics m = Theme::metrics(font());

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // 悬停：整块一个圆角状态层，和行内按钮同一套语言。用显式的 hover 标记
        // 而不是 underMouse()：点开菜单后弹出层抢走鼠标，underMouse() 会一直停在
        // true，状态层就「粘」在控件上（要滚动才消）；这里在打开菜单时清掉它。
        if (isEnabled() && m_hover) {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::stateLayer(pal.surfaceContainerHigh, pal.onSurface, Theme::State::Hover));
            const int radius = qMax(int(Theme::ShapeSmall), m.radiusPill);
            p.drawRoundedRect(rect(), radius, radius);
        }

        // 右侧箭头（比正文淡一档 = onSurfaceVariant），当前值右对齐贴着它。
        const int box = selectGlyphSize(m);
        const QRectF arrow(width() - Theme::Space3 - box,
                           (height() - box) / 2.0, box, box);
        IconPainter::paintGlyph(p, IconPainter::Glyph::ChevronDown, arrow,
                                isEnabled() ? pal.textMuted : pal.textDisabled);
        p.setFont(font());
        p.setPen(isEnabled() ? pal.text : pal.textDisabled);
        const QRect textRect(0, 0, qMax(0, int(arrow.left()) - Theme::Space2), height());
        p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, currentLabel());
    }

    // 弹出的菜单由 Qt 拥有（能越出滚动区域），这里只把它的位置对齐到收起态的
    // 右缘；超出屏幕时钳回可用区域。
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QToolButton::mousePressEvent(event);
            return;
        }
        const QSize hint = m_menu->sizeHint();
        const QPoint below = mapToGlobal(QPoint(width() - hint.width(), height() + 2));
        QPoint pos = below;
        if (QScreen *scr = screen()) {
            const QRect avail = scr->availableGeometry();
            const int x = qBound(avail.left(), below.x(), avail.right() - hint.width() + 1);
            int y = below.y();
            if (y + hint.height() > avail.bottom())
                y = mapToGlobal(QPoint(0, 0)).y() - hint.height() - 2;   // 下方放不下就翻到上方
            pos = QPoint(x, y);
        }
        m_hover = false;        // 菜单接管鼠标：先收掉悬停层，别把它冻在控件上
        update();
        m_menu->popup(pos);
        event->accept();
    }

    // 设置页是滚动的：滚轮划过控件时不能顺手把值改了 —— 选择只由点击触发。
    void wheelEvent(QWheelEvent *event) override { event->ignore(); }

    void enterEvent(QEnterEvent *event) override
    {
        QToolButton::enterEvent(event);
        m_hover = true;
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        QToolButton::leaveEvent(event);
        m_hover = false;
        update();
    }

    // 字体（DPI / 系统字号）变了，弹出菜单的字体与行尺寸跟上。
    void changeEvent(QEvent *event) override
    {
        QToolButton::changeEvent(event);
        if (event->type() == QEvent::FontChange
            || event->type() == QEvent::ApplicationFontChange) {
            refreshTheme();
        }
    }

private:
    QString currentLabel() const
    {
        return (m_index >= 0 && m_index < m_options.size()) ? m_options.at(m_index)
                                                            : QString();
    }

    void syncTip()
    {
        setToolTip(m_index >= 0 && m_index < m_tips.size() ? m_tips.at(m_index)
                                                           : currentLabel());
    }

    QStringList  m_options;
    QStringList  m_tips;
    SelectMenu  *m_menu = nullptr;
    int          m_index = 0;
    bool         m_hover = false;
};

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("settingsPage"));
    setFocusPolicy(Qt::NoFocus);

    // The page background is the same desk the pages rest on: no floating
    // island, the settings tab reads as one more surface of the app.
    const Theme::Palette &pal = Theme::light();
    QPalette pagePal = palette();
    pagePal.setColor(QPalette::Window, pal.surfaceContainerLowest);   // MD3 page under the drawer and the list
    setPalette(pagePal);
    setAutoFillBackground(true);

    buildUi();
    showCategory(0);
    layoutForWidth();
}

QWidget *SettingsPage::addCategory(const QString &name, int glyph)
{
    Category c;
    c.name = name;
    c.glyph = glyph;

    c.scroll = new QScrollArea(m_stack);
    c.scroll->setObjectName(QStringLiteral("settingsScroll"));
    c.scroll->setFrameShape(QFrame::NoFrame);
    c.scroll->setWidgetResizable(true);
    c.scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    c.scroll->setFocusPolicy(Qt::NoFocus);
    c.scroll->viewport()->setAutoFillBackground(false);
    c.scroll->viewport()->installEventFilter(this);

    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("settingsBody"));
    auto *row = new QHBoxLayout(body);
    row->setContentsMargins(Theme::Space5, 0, Theme::Space5, Theme::Space6);
    row->setSpacing(0);
    row->addStretch(1);
    c.column = new QWidget(body);
    c.column->setMaximumWidth(m_maxColumn);
    row->addWidget(c.column, 0);
    row->addStretch(1);

    auto *col = new QVBoxLayout(c.column);
    col->setContentsMargins(0, 0, 0, 0);
    // MD3 segmented list: the items of a group sit 2 dp apart.
    col->setSpacing(Theme::Space1 / 2);

    c.scroll->setWidget(body);
    m_stack->addWidget(c.scroll);

    auto *item = new NavItem(IconPainter::Glyph(glyph), name, m_rail);
    item->setFont(font());
    auto *railCol = static_cast<QVBoxLayout *>(m_rail->layout());
    railCol->insertWidget(railCol->count() - 1, item);   // above the closing stretch
    const int index = m_categories.size();
    connect(item, &QAbstractButton::clicked, this, [this, index] { showCategory(index); });
    c.navItem = item;

    m_categories.append(c);
    return c.column;
}

void SettingsPage::buildUi()
{
    using G = IconPainter::Glyph;
    const Theme::Metrics m = Theme::metrics(font());
    m_maxColumn = m.touch * 16;

    m_root = new QWidget(this);
    m_root->setObjectName(QStringLiteral("settingsRoot"));
    auto *pageLayout = new QHBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(m_root);

    auto *split = new QHBoxLayout(m_root);
    split->setContentsMargins(0, 0, 0, 0);
    split->setSpacing(0);

    // --- navigation rail ----------------------------------------------------
    m_rail = new RailPane(m_root);
    auto *railCol = new QVBoxLayout(m_rail);
    railCol->setContentsMargins(Theme::Space3, Theme::Space5, Theme::Space3, Theme::Space4);
    railCol->setSpacing(Theme::Space1);

    m_identity = new QWidget(m_rail);
    {
        auto *h = new QHBoxLayout(m_identity);
        h->setContentsMargins(Theme::Space2, 0, Theme::Space2, Theme::Space4);
        h->setSpacing(Theme::Space3);
        h->addWidget(new AppBadge(int(m.touch * 1.25), m_identity), 0, Qt::AlignVCenter);

        m_identityText = new QWidget(m_identity);
        auto *v = new QVBoxLayout(m_identityText);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(Theme::Space1 / 2);
        auto *name = new QLabel(QStringLiteral("落墨·大屏批注"), m_identityText);
        name->setFont(Theme::scaledFont(font(), 1.05, QFont::DemiBold));
        v->addWidget(name);
        auto *version = new QLabel(QStringLiteral("版本 %1").arg(appVersion()), m_identityText);
        version->setObjectName(QStringLiteral("settingsIdentityBody"));
        version->setFont(Theme::scaledFont(font(), 0.92, QFont::Normal));
        v->addWidget(version);
        h->addWidget(m_identityText, 1, Qt::AlignVCenter);
    }
    railCol->addWidget(m_identity);
    railCol->addStretch(1);
    split->addWidget(m_rail, 0);

    // --- content: fixed breadcrumb over the category's own scrolling page ----
    auto *content = new QWidget(m_root);
    auto *contentCol = new QVBoxLayout(content);
    contentCol->setContentsMargins(0, 0, 0, 0);
    contentCol->setSpacing(0);

    m_header = new QWidget(content);
    {
        auto *h = new QHBoxLayout(m_header);
        h->setContentsMargins(Theme::Space5, Theme::Space5, Theme::Space5, Theme::Space4);
        h->setSpacing(Theme::Space3);
        const QFont crumbFont = Theme::scaledFont(font(), 2.0, QFont::Normal);   // MD3 headline
        auto *root = new QLabel(QStringLiteral("设置"), m_header);
        root->setObjectName(QStringLiteral("settingsCrumbRoot"));
        root->setFont(crumbFont);
        h->addWidget(root, 0, Qt::AlignVCenter);
        const int chevron = qMax(16, int(QFontMetrics(crumbFont).height() * 0.62));
        h->addWidget(new GlyphBox(G::ChevronRight, chevron, GlyphBox::Role::Muted, m_header),
                     0, Qt::AlignVCenter);
        m_crumbLeaf = new QLabel(m_header);
        m_crumbLeaf->setFont(crumbFont);
        h->addWidget(m_crumbLeaf, 0, Qt::AlignVCenter);
        h->addStretch(1);
    }
    contentCol->addWidget(m_header);

    m_stack = new QStackedWidget(content);
    contentCol->addWidget(m_stack, 1);
    split->addWidget(content, 1);
    m_content = content;

    // Category switches: MD3 fade through (see FadeThrough). The duration comes
    // from Theme::Motion, which is 0 while 减少动效 is on - then the switch is
    // instant, and the toggle takes effect on the very next switch.
    m_transition = new FadeThrough(content);
    m_transitionAnim = new QVariantAnimation(this);
    m_transitionAnim->setStartValue(0.0);
    m_transitionAnim->setEndValue(1.0);   // linear: FadeThrough applies the curves
    connect(m_transitionAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        static_cast<FadeThrough *>(m_transition)->setProgress(v.toReal());
    });
    connect(m_transitionAnim, &QAbstractAnimation::finished, m_transition, &QWidget::hide);

    // === 通用 =================================================================
    {
        QWidget *page = addCategory(QStringLiteral("通用"), int(G::Gear));
        auto *col = static_cast<QVBoxLayout *>(page->layout());

        addGroupHeader(col, page, QStringLiteral("启动"), true);
        m_autoStart = new ToggleSwitch;
        m_autoStart->setToolTip(QStringLiteral("登录 Windows 后自动打开本程序"));
        m_autoStart->setChecked(AppSettings::isAutoStartEnabled());
        connect(m_autoStart, &QAbstractButton::toggled, this, &SettingsPage::onAutoStartToggled);
        col->addWidget(makeSettingCard(page, G::Power, QStringLiteral("开机自动启动"),
                                       QStringLiteral("登录 Windows 后自动打开本程序"),
                                       m_autoStart).frame);

        addGroupHeader(col, page, QStringLiteral("书写"), false);
        m_palmEraser = new ToggleSwitch;
        m_palmEraser->setToolTip(QStringLiteral("默认关：开启后手掌/手背大面积接触才触发擦除；用笔或手指书写不受影响"));
        m_palmEraser->setChecked(AppSettings::palmEraserEnabled());
        connect(m_palmEraser, &QAbstractButton::toggled, this, &SettingsPage::onPalmEraserToggled);
        col->addWidget(makeSettingCard(page, G::Hand, QStringLiteral("手掌/手背当橡皮"),
                                       QStringLiteral("默认关。开启后大面积接触自动擦除（可撤销）；笔/手指书写不受影响"),
                                       m_palmEraser).frame);

        addGroupHeader(col, page, QStringLiteral("性能"), false);
        m_reduceMotion = new ToggleSwitch;
        m_reduceMotion->setToolTip(QStringLiteral("界面动画直接跳到结果，适合配置较低的一体机"));
        m_reduceMotion->setChecked(AppSettings::reduceMotion());
        connect(m_reduceMotion, &QAbstractButton::toggled, this, &SettingsPage::onReduceMotionToggled);
        col->addWidget(makeSettingCard(page, G::Sparkle, QStringLiteral("减少动效"),
                                       QStringLiteral("关闭界面动画，配置较低的一体机上更省资源"),
                                       m_reduceMotion).frame);
        col->addStretch(1);
    }

    // === 外观 =================================================================
    {
        QWidget *page = addCategory(QStringLiteral("外观"), int(G::Palette));
        auto *col = static_cast<QVBoxLayout *>(page->layout());

        addGroupHeader(col, page, QStringLiteral("主题"), true);
        m_themeSelect = new SettingsSelect(
            { QStringLiteral("系统"), QStringLiteral("浅色"), QStringLiteral("深色") });
        m_themeSelect->setFont(Theme::chromeFont(font()));
        connect(m_themeSelect, &SettingsSelect::changed, this, &SettingsPage::onThemePicked);
        syncThemeSelect();
        // The description line IS the note on what 系统 currently resolves to.
        const SettingCard mode = makeSettingCard(page, G::Contrast, QStringLiteral("颜色模式"),
                                                 QString(), m_themeSelect);
        m_themeNote = mode.body;
        m_themeNote->setVisible(true);
        col->addWidget(mode.frame);

        // 主题色：8 个种子色的圆形色板。色板自带触控目标宽度，挂在行右侧会把
        // 说明文字挤成一条缝，所以放在文字下方。
        SettingCard seeds = makeSettingCard(
            page, G::Palette, QStringLiteral("主题色"),
            QStringLiteral("选择界面配色由哪个种子色推导（PDF 纸面始终保持白色）"));
        {
            auto *seedStrip = new QWidget;
            auto *seedRow = new QHBoxLayout(seedStrip);
            seedRow->setContentsMargins(0, 0, 0, 0);
            seedRow->setSpacing(Theme::Space2);

            m_themePresetGroup = new QButtonGroup(this);
            m_themePresetGroup->setExclusive(true);
            const QString seedNames[ThemePresets::kSeedCount] = {
                QStringLiteral("蓝（默认）"), QStringLiteral("青绿"), QStringLiteral("绿"),
                QStringLiteral("琥珀"), QStringLiteral("橙"), QStringLiteral("红"),
                QStringLiteral("紫"), QStringLiteral("灰蓝") };
            for (int i = 0; i < ThemePresets::kSeedCount; ++i) {
                auto *swatch = new SeedSwatch(i, seedStrip);
                swatch->setToolTip(seedNames[i]);
                m_themePresetGroup->addButton(swatch, i);
                m_themePresetSwatches[i] = swatch;
                seedRow->addWidget(swatch, 0, Qt::AlignVCenter);
            }
            connect(m_themePresetGroup, &QButtonGroup::idClicked,
                    this, &SettingsPage::onThemePresetPicked);
            syncThemePreset();
            addUnderText(seeds, seedStrip);
        }
        col->addWidget(seeds.frame);

        // 调色板风格：种子色 → 配色的推导算法（MD3 的四个变体）；每种算法的解释
        // 挂在列表项和当前值的提示上。
        const QString paletteLabels[ThemePresets::kStyleCount] = {
            QStringLiteral("保真"), QStringLiteral("均衡"),
            QStringLiteral("中性"), QStringLiteral("单色") };
        const QString paletteTips[ThemePresets::kStyleCount] = {
            QStringLiteral("保留种子本身彩度、最贴品牌色（默认）"),
            QStringLiteral("Material 3 默认"),
            QStringLiteral("接近灰、最低调"),
            QStringLiteral("纯灰阶") };
        m_paletteStyleSelect = new SettingsSelect(
            { paletteLabels[0], paletteLabels[1], paletteLabels[2], paletteLabels[3] });
        m_paletteStyleSelect->setFont(Theme::chromeFont(font()));
        m_paletteStyleSelect->setOptionTips(
            { paletteTips[0], paletteTips[1], paletteTips[2], paletteTips[3] });
        connect(m_paletteStyleSelect, &SettingsSelect::changed,
                this, &SettingsPage::onPaletteStylePicked);
        syncPaletteStyleSelect();
        col->addWidget(makeSettingCard(page, G::Pages, QStringLiteral("调色板风格"),
                                       QStringLiteral("由种子色推导配色的算法"),
                                       m_paletteStyleSelect).frame);

        addGroupHeader(col, page, QStringLiteral("工具栏"), false);
        // 工具栏风格：两种外观、同一组按钮（简约 = 纯图标，文字 = 图标+名称）。
        m_toolbarStyleSelect = new SettingsSelect(
            { QStringLiteral("简约"), QStringLiteral("文字") });
        m_toolbarStyleSelect->setFont(Theme::chromeFont(font()));
        connect(m_toolbarStyleSelect, &SettingsSelect::changed,
                this, &SettingsPage::onToolbarStylePicked);
        syncToolbarStyleSelect();
        col->addWidget(makeSettingCard(page, G::Toolbar, QStringLiteral("工具栏风格"),
                                       QStringLiteral("简约（纯图标）/ 文字（图标+名称）"),
                                       m_toolbarStyleSelect).frame);
        col->addStretch(1);
        refreshThemeNote();
    }

    // === Word 文档 ============================================================
    {
        QWidget *page = addCategory(QStringLiteral("Word 文档"), int(G::Document));
        auto *col = static_cast<QVBoxLayout *>(page->layout());

        addGroupHeader(col, page, QStringLiteral("打开"), true);
        m_wordSelect = new SettingsSelect(
            { QStringLiteral("每次询问"), QStringLiteral("批注"),
              QStringLiteral("用 Word 打开") });
        m_wordSelect->setFont(Theme::chromeFont(font()));
        connect(m_wordSelect, &SettingsSelect::changed, this, &SettingsPage::onWordModePicked);
        syncWordSelect();
        col->addWidget(makeSettingCard(page, G::Document, QStringLiteral("打开 Word 文档时"),
                                       QStringLiteral("每次询问 / 批注 / 用 Word 打开"),
                                       m_wordSelect).frame);

        addGroupHeader(col, page, QStringLiteral("转换"), false);
        // 转换引擎：自动（优先 Word，缺失时降级 LibreOffice）/ 仅 Word / 仅 LibreOffice。
        m_wordEngineSelect = new SettingsSelect(
            { QStringLiteral("自动（优先 Word）"), QStringLiteral("仅 Word"),
              QStringLiteral("仅 LibreOffice") });
        m_wordEngineSelect->setFont(Theme::chromeFont(font()));
        m_wordEngineSelect->setToolTip(QStringLiteral("没有 Word 时自动改用 LibreOffice"));
        connect(m_wordEngineSelect, &SettingsSelect::changed,
                this, &SettingsPage::onWordEnginePicked);
        syncWordEngineSelect();
        col->addWidget(makeSettingCard(
            page, G::Convert, QStringLiteral("转换引擎"),
            QStringLiteral("自动（优先 Word，缺失时用 LibreOffice）/ 仅 Word / 仅 LibreOffice"),
            m_wordEngineSelect).frame);

        // LibreOffice 可选附加包：检测状态 + 选择 / 清除 soffice.exe + 下载页。
        // 查找顺序见 WordConvert::libreOfficePath()：用户指定 → 程序目录便携包 →
        // 环境变量 → PATH → 标准安装目录。
        SettingCard lo = makeSettingCard(page, G::Package,
                                         QStringLiteral("LibreOffice（可选附加包）"), QString());
        m_libreOfficeStatus = lo.body;
        m_libreOfficeStatus->setVisible(true);
        {
            ButtonRow buttons = makeButtonRow(lo.frame, font());
            m_chooseSoffice = addRowButton(buttons, font(), QStringLiteral("选择 soffice.exe…"));
            m_clearSoffice = addRowButton(buttons, font(), QStringLiteral("清除"));
            m_openSofficePage = addRowButton(buttons, font(), QStringLiteral("打开下载页"));
            addUnderText(lo, buttons.box);
        }
        connect(m_chooseSoffice, &QPushButton::clicked, this, &SettingsPage::onChooseSoffice);
        connect(m_clearSoffice, &QPushButton::clicked, this, &SettingsPage::onClearSoffice);
        connect(m_openSofficePage, &QPushButton::clicked, this, &SettingsPage::onOpenSofficePage);
        refreshLibreOfficeStatus();
        col->addWidget(lo.frame);

        // The nag fix: a toggle that persists AppSettings::wordNagFixEnabled,
        // with the 恢复 button under the text. The state line and the button's
        // tooltip follow WordConvert::nagRestoreMode(), so the restore always
        // matches what this machine actually has.
        m_wordNagFix = new ToggleSwitch;
        m_wordNagFix->setToolTip(QStringLiteral("转换前关闭 Word 启动时的「不是默认程序」提醒"));
        {
            const QSignalBlocker block(m_wordNagFix);
            m_wordNagFix->setChecked(AppSettings::wordNagFixEnabled());
        }
        SettingCard nag = makeSettingCard(
            page, G::BellOff, QStringLiteral("关闭 Word 的「不是默认程序」提醒"),
            QStringLiteral("转换 Word 文档需要它：会写入 Word 自己的两个开关值"
                           "（HKCU\\Software\\Microsoft\\Office\\<版本>\\Word\\Options）。"),
            m_wordNagFix);
        m_wordNagNote = makeBodyLabel(nag.frame);
        nag.texts->addWidget(m_wordNagNote);
        m_restoreWordNag = makeButton(nag.frame, font(), QStringLiteral("恢复 Word 设置"));
        addUnderText(nag, m_restoreWordNag);
        connect(m_wordNagFix, &QAbstractButton::toggled, this, &SettingsPage::onWordNagFixToggled);
        connect(m_restoreWordNag, &QPushButton::clicked, this, &SettingsPage::onRestoreWordNag);
        refreshWordNag();
        col->addWidget(nag.frame);
        col->addStretch(1);
    }

    // === 保存与文件 ============================================================
    {
        QWidget *page = addCategory(QStringLiteral("保存与文件"), int(G::Save));
        auto *col = static_cast<QVBoxLayout *>(page->layout());

        addGroupHeader(col, page, QStringLiteral("保存"), true);
        ButtonRow saveButtons = makeButtonRow(nullptr, font());
        m_chooseSaveDir = addRowButton(saveButtons, font(), QStringLiteral("选择文件夹…"));
        m_resetSaveDir  = addRowButton(saveButtons, font(), QStringLiteral("跟随源文件"));
        SettingCard save = makeSettingCard(page, G::Open, QStringLiteral("默认保存位置"),
                                           QString(), saveButtons.box);
        m_savePathLabel = save.body;
        m_savePathLabel->setVisible(true);
        save.texts->addWidget(makeBodyLabel(
            save.frame, QStringLiteral("选择「跟随源文件」则批注与源文件保存在同一目录。")));
        connect(m_chooseSaveDir, &QPushButton::clicked, this, &SettingsPage::onChooseSaveDir);
        connect(m_resetSaveDir, &QPushButton::clicked, this, &SettingsPage::onResetSaveDir);
        refreshSavePath();
        col->addWidget(save.frame);

        addGroupHeader(col, page, QStringLiteral("文件关联"), false);
        m_register = makeButton(nullptr, font(), QStringLiteral("注册…"), RowButton::Primary);
        m_register->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        connect(m_register, &QPushButton::clicked, this, &SettingsPage::onRegisterPdf);
        col->addWidget(makeSettingCard(
            page, G::Link, QStringLiteral("设为 PDF 默认打开方式"),
            QStringLiteral("Windows 不允许程序直接抢占默认应用：注册后还需要在「默认应用」中"
                           "选择「落墨·大屏批注」。"),
            m_register).frame);
        col->addStretch(1);
    }

    // === 调试 =================================================================
    {
        QWidget *page = addCategory(QStringLiteral("调试"), int(G::Bug));
        auto *col = static_cast<QVBoxLayout *>(page->layout());

        addGroupHeader(col, page, QStringLiteral("诊断日志"), true);
        m_debugLog = new ToggleSwitch;
        m_debugLog->setToolTip(QStringLiteral("写入诊断日志（默认关闭）"));
        {
            const QSignalBlocker block(m_debugLog);
            m_debugLog->setChecked(AppSettings::debugLogEnabled() || AppLog::isEnabled());
        }
        col->addWidget(makeSettingCard(
            page, G::Bug, QStringLiteral("写入诊断日志"),
            QStringLiteral("用于排查问题。日志只记录运行状态与耗时，不包含文档内容或批注坐标。默认关闭。"),
            m_debugLog).frame);

        m_openLogDir = makeButton(nullptr, font(), QStringLiteral("打开日志文件夹"));
        SettingCard logs = makeSettingCard(page, G::Open, QStringLiteral("日志位置"), QString(),
                                           m_openLogDir);
        m_logPathLabel = logs.body;
        m_logPathLabel->setVisible(true);
        // Why the switch can start out ON, and why that is not the setting.
        m_logEnvNote = makeBodyLabel(logs.frame);
        m_logEnvNote->setVisible(false);
        logs.texts->addWidget(m_logEnvNote);
        col->addWidget(logs.frame);
        col->addStretch(1);

        connect(m_debugLog, &QAbstractButton::toggled, this, &SettingsPage::onDebugLogToggled);
        connect(m_openLogDir, &QPushButton::clicked, this, &SettingsPage::onOpenLogDir);
        refreshLogPath();
    }

    // === 关于 =================================================================
    {
        QWidget *page = addCategory(QStringLiteral("关于"), int(G::Info));
        auto *col = static_cast<QVBoxLayout *>(page->layout());

        // Identity card: the badge, the name and what the app is for.
        {
            auto *card = new QFrame(page);
            card->setObjectName(QStringLiteral("settingsCard"));
            card->setAttribute(Qt::WA_StyledBackground, true);
            auto *h = new QHBoxLayout(card);
            h->setContentsMargins(Theme::Space4, Theme::Space4, Theme::Space4, Theme::Space4);
            h->setSpacing(Theme::Space4);
            h->addWidget(new AppBadge(int(m.touch * 1.4), card), 0, Qt::AlignVCenter);
            auto *v = new QVBoxLayout;
            v->setSpacing(Theme::Space1);
            auto *name = new QLabel(QStringLiteral("落墨·大屏批注"), card);
            name->setFont(Theme::scaledFont(font(), 1.25, QFont::DemiBold));
            v->addWidget(name);
            v->addWidget(makeBodyLabel(card, QStringLiteral(
                "版本 %1 · 面向教室大屏一体机的 PDF 查看与手写批注"
                "（原生 C++/Qt6，触控与低内存优化）").arg(appVersion())));
            h->addLayout(v, 1);
            col->addWidget(card);
        }

        addGroupHeader(col, page, QStringLiteral("更新"), false);
        m_updateButton = makeButton(nullptr, font(), QStringLiteral("检查更新"),
                                    RowButton::Primary);
        m_updateButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        SettingCard upd = makeSettingCard(page, G::Download, QStringLiteral("检查更新"),
                                          QString(), m_updateButton);
        m_updateNote = upd.body;
        m_updateNote->setVisible(true);
        connect(m_updateButton, &QPushButton::clicked, this, &SettingsPage::onCheckUpdate);
        resetUpdateRow();
        col->addWidget(upd.frame);

        QPushButton *releases = makeButton(nullptr, font(), QStringLiteral("打开"));
        connect(releases, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(UpdateCheck::releasesUrl());
        });
        col->addWidget(makeSettingCard(page, G::Pages, QStringLiteral("发布页面"),
                                       QStringLiteral("所有版本的更新说明，安装版与便携版都在这里下载"),
                                       releases).frame);

        addGroupHeader(col, page, QStringLiteral("项目"), false);
        QPushButton *repo = makeButton(nullptr, font(), QStringLiteral("打开"));
        connect(repo, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(UpdateCheck::repositoryUrl());
        });
        col->addWidget(makeSettingCard(page, G::Link, QStringLiteral("项目主页"),
                                       UpdateCheck::repositoryUrl().toString(), repo).frame);

        // 许可证: an expander card - the full MIT text only when asked for.
        auto *chevron = new GlyphBox(G::ChevronDown, selectGlyphSize(m), GlyphBox::Role::Muted);
        auto *licenseCard = new ClickCard(page);
        SettingCard lic = makeSettingCard(
            page, G::Document, QStringLiteral("许可证"),
            QStringLiteral("MIT License · 第三方组件：Qt 6（LGPL-3.0）、PDFium（BSD-3-Clause）"),
            chevron, licenseCard);
        auto *licenseBody = new QWidget;
        {
            auto *v = new QVBoxLayout(licenseBody);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(Theme::Space2);
            auto *text = makeBodyLabel(licenseBody, QString::fromLatin1(kMitLicenseText));
            text->setFont(Theme::scaledFont(font(), 0.92, QFont::Normal));
            text->setTextInteractionFlags(Qt::TextSelectableByMouse);
            v->addWidget(text);
            auto *thirdParty = makeBodyLabel(licenseBody, QStringLiteral(
                "第三方组件：Qt 6（LGPL-3.0，动态链接使用）· PDFium（BSD-3-Clause，随 Qt PDF 模块）"));
            thirdParty->setFont(Theme::scaledFont(font(), 0.92, QFont::Normal));
            v->addWidget(thirdParty);
        }
        licenseBody->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        {
            // Under the text, stretched: the licence wraps to the card width.
            auto *row = new QHBoxLayout;
            row->setContentsMargins(lic.textIndent, 0, 0, 0);
            licenseBody->setParent(lic.frame);
            row->addWidget(licenseBody, 1);
            lic.outer->addLayout(row);
        }
        licenseBody->setVisible(false);
        licenseCard->onClick = [licenseBody, chevron] {
            const bool open = !licenseBody->isVisible();
            licenseBody->setVisible(open);
            chevron->setGlyph(open ? IconPainter::Glyph::ChevronUp
                                   : IconPainter::Glyph::ChevronDown);
        };
        col->addWidget(licenseCard);
        col->addStretch(1);
    }

    for (const Category &c : std::as_const(m_categories))
        segmentGroups(static_cast<QVBoxLayout *>(c.column->layout()));
    m_root->setStyleSheet(pageSheet(font()));
}

void SettingsPage::showCategory(int index)
{
    if (index < 0 || index >= m_categories.size())
        return;
    // A switch during a running transition first lands the old one.
    if (m_transitionAnim->state() == QAbstractAnimation::Running)
        m_transitionAnim->stop();
    m_transition->hide();

    const int ms = Theme::Motion::duration(Theme::Motion::Medium2);   // 0 under 减少动效
    const bool animate = ms > 0 && index != m_current && m_content->isVisible()
                         && !m_content->size().isEmpty();
    // Grab through the page itself: it paints the page background, while the
    // content area alone would come out on the default palette's light grey.
    const QRect area(m_content->mapTo(this, QPoint(0, 0)), m_content->size());
    const QPixmap from = animate ? grab(area) : QPixmap();

    m_current = index;
    const Category &c = m_categories.at(index);
    m_stack->setCurrentWidget(c.scroll);
    c.navItem->setChecked(true);
    m_crumbLeaf->setText(c.name);
    layoutForWidth();
    if (!animate)
        return;

    if (QLayout *l = m_content->layout())
        l->activate();
    auto *overlay = static_cast<FadeThrough *>(m_transition);
    overlay->setFrames(from, grab(area), palette().color(QPalette::Window));
    overlay->setGeometry(m_content->rect());
    overlay->setProgress(0.0);
    overlay->raise();
    overlay->show();
    m_transitionAnim->setDuration(ms);
    m_transitionAnim->start();
}

bool SettingsPage::testTransitionRunning() const
{
    return m_transition && m_transition->isVisible();
}

void SettingsPage::testSetTransitionProgress(qreal t)
{
    if (m_transitionAnim->state() == QAbstractAnimation::Running)
        m_transitionAnim->pause();
    static_cast<FadeThrough *>(m_transition)->setProgress(t);
}

QString SettingsPage::testCategoryName(int index) const
{
    return (index >= 0 && index < m_categories.size()) ? m_categories.at(index).name
                                                      : QString();
}

QWidget *SettingsPage::testOpenThemeMenu()
{
    if (!m_themeSelect)
        return nullptr;
    const QPoint at = m_themeSelect->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, at, m_themeSelect->mapToGlobal(at),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(m_themeSelect, &press);
    return m_themeSelect->findChild<QMenu *>();
}

QScrollArea *SettingsPage::testCurrentScroll() const
{
    return m_categories.isEmpty() ? nullptr : m_categories.at(m_current).scroll;
}

bool SettingsPage::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::Resize)
        layoutForWidth();
    return false;
}

void SettingsPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The transition's frames were grabbed at the old size: land it at once.
    if (m_transitionAnim && m_transitionAnim->state() != QAbstractAnimation::Stopped) {
        m_transitionAnim->stop();
        m_transition->hide();
    }
    layoutForWidth();
}

// Folds the rail to icons on a narrow page, then centres every category column
// (at most m_maxColumn wide) and puts the breadcrumb over the visible one's
// left edge.
void SettingsPage::layoutForWidth()
{
    if (!m_rail || m_categories.isEmpty())
        return;
    const Theme::Metrics m = Theme::metrics(font());

    const bool compact = width() < m.touch * 18;
    if (compact != m_railCompact || m_rail->minimumWidth() == 0) {
        m_railCompact = compact;
        for (const Category &c : std::as_const(m_categories))
            static_cast<NavItem *>(c.navItem)->setCompact(compact);
        m_identity->setVisible(!compact);
        static_cast<RailPane *>(m_rail)->setCompact(compact);
        const qreal dp = dpUnit(font());
        // Rail: 80 dp. Drawer: items plus its 12 dp side padding.
        m_rail->setFixedWidth(compact ? qRound(80 * dp) : m.touch * 5 + 2 * Theme::Space3);
        m_rail->layout()->setContentsMargins(compact ? 0 : Theme::Space3, Theme::Space5,
                                             compact ? 0 : Theme::Space3, Theme::Space4);
        static_cast<QVBoxLayout *>(m_rail->layout())->setSpacing(compact ? Theme::Space3 : 0);
    }

    for (const Category &c : std::as_const(m_categories)) {
        const int avail = qMax(0, c.scroll->viewport()->width() - 2 * Theme::Space5);
        c.column->setMinimumWidth(qMin(avail, m_maxColumn));
    }

    // The breadcrumb sits on the column's left edge, so it uses the same centring
    // as the body (the viewport may be narrower than the stack by a scroll bar).
    const QScrollArea *scroll = m_categories.at(m_current).scroll;
    const int vw = scroll->viewport()->width();
    const int colW = qMin(qMax(0, vw - 2 * Theme::Space5), m_maxColumn);
    const int left = qMax(int(Theme::Space5), (vw - colW) / 2);
    if (auto *h = m_header->layout())
        h->setContentsMargins(left, Theme::Space5, Theme::Space5, Theme::Space4);
}

void SettingsPage::onChooseSaveDir()
{
    QString start = AppSettings::defaultSavePath();
    if (start.isEmpty() || !QFileInfo(start).isDir())
        start = QDir::homePath();

    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择默认保存位置"), start);
    if (dir.isEmpty())
        return;

    QString err;
    if (!AppSettings::setDefaultSavePath(dir, &err))
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
    refreshSavePath();
}

void SettingsPage::onResetSaveDir()
{
    QString err;
    if (!AppSettings::setDefaultSavePath(QString(), &err))
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
    refreshSavePath();
}

void SettingsPage::refreshSavePath()
{
    if (!m_savePathLabel)
        return;

    const QString dir = AppSettings::defaultSavePath();
    m_savePathLabel->setText(dir.isEmpty()
        ? QStringLiteral("跟随源文件所在目录（默认）")
        : QDir::toNativeSeparators(dir));
    if (m_resetSaveDir)
        m_resetSaveDir->setEnabled(!dir.isEmpty());
}

void SettingsPage::onDebugLogToggled(bool on)
{
    QString err;
    if (!AppLog::setEnabled(on, &err))
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);

    // Reflect the real state: the environment override can force it on.
    {
        const QSignalBlocker block(m_debugLog);
        m_debugLog->setChecked(AppLog::isEnabled());
    }
    refreshLogPath();
}

void SettingsPage::onOpenLogDir()
{
    const QString dir = AppLog::logDirectory();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void SettingsPage::refreshLogPath()
{
    if (!m_logPathLabel)
        return;

    const bool on = AppLog::isEnabled();
    m_logPathLabel->setText(on
        ? QDir::toNativeSeparators(AppLog::logFilePath())
        : QStringLiteral("已关闭（不写入任何日志）"));
    if (m_openLogDir)
        m_openLogDir->setEnabled(on);

    if (m_logEnvNote) {
        const QString env = AppLog::envOverridePath();
        m_logEnvNote->setVisible(!env.isEmpty());
        if (!env.isEmpty())
            m_logEnvNote->setText(QStringLiteral(
                "本次由环境变量 PDFBOARD_LOG 指定路径：%1（只决定启动时默认开启，"
                "上面的开关随时可以关闭）").arg(QDir::toNativeSeparators(env)));
    }
}

// The sheet and the page background bake theme colours, so a runtime theme
// switch re-applies them; every glyph (rail, cards, badge) paints from the live
// palette and only needs a repaint.
void SettingsPage::refreshTheme()
{
    const Theme::Palette &pal = Theme::light();

    QPalette pagePal = palette();
    pagePal.setColor(QPalette::Window, pal.surfaceContainerLowest);   // MD3 page under the drawer and the list
    setPalette(pagePal);

    if (m_root)
        m_root->setStyleSheet(pageSheet(font()));

    // 五个下拉：弹出层的卡片皮肤按新调色板重生成。
    for (SettingsSelect *select : { m_wordSelect, m_wordEngineSelect, m_themeSelect,
                                    m_toolbarStyleSelect, m_paletteStyleSelect }) {
        if (select)
            select->refreshTheme();
    }

    syncThemeSelect();
    syncWordSelect();
    syncThemePreset();
    syncPaletteStyleSelect();
    syncToolbarStyleSelect();
    refreshWordNag();
    refreshThemeNote();

    // 种子色板的选中圆环、导航项、卡片图标都取自实时调色板，换主题（含 主题色 /
    // 调色板风格）后必须重绘。
    const QList<QWidget *> children = findChildren<QWidget *>();
    for (QWidget *w : children)
        w->update();
    update();
}

// 检查更新 -------------------------------------------------------------------

void SettingsPage::resetUpdateRow()
{
    m_updatePage = QUrl();
    m_updateNote->setText(QStringLiteral(
        "当前版本 %1。只在点击时联网：仅访问 github.com，不上传任何数据。")
                              .arg(appVersion()));
    m_updateButton->setText(QStringLiteral("检查更新"));
    m_updateButton->setEnabled(true);
}

void SettingsPage::onCheckUpdate()
{
    // Second press after a newer release was found: hand it to the browser.
    if (m_updatePage.isValid()) {
        QDesktopServices::openUrl(m_updatePage);
        return;
    }
    if (!m_updater) {
        m_updater = new UpdateCheck::Checker(this);
        connect(m_updater, &UpdateCheck::Checker::finished,
                this, &SettingsPage::onUpdateResult);
    }
    if (m_updater->isRunning())
        return;
    m_updateButton->setEnabled(false);
    m_updateButton->setText(QStringLiteral("正在检查…"));
    m_updateNote->setText(QStringLiteral("正在连接 GitHub…"));
    m_updater->start(appVersion());
}

void SettingsPage::onUpdateResult(const UpdateCheck::Result &result)
{
    using Status = UpdateCheck::Result::Status;
    m_updateButton->setEnabled(true);
    switch (result.status) {
    case Status::NewerFound:
        m_updatePage = result.pageUrl;
        m_updateNote->setText(QStringLiteral(
            "发现新版本 %1（当前 %2）。点击「前往下载」在浏览器中打开发布页面。")
                                  .arg(result.latestTag, appVersion()));
        m_updateButton->setText(QStringLiteral("前往下载"));
        emit statusMessage(QStringLiteral("发现新版本 %1").arg(result.latestTag));
        break;
    case Status::UpToDate:
        m_updateNote->setText(QStringLiteral("已是最新版本（当前 %1，最新发布 %2）。")
                                  .arg(appVersion(), result.latestTag));
        m_updateButton->setText(QStringLiteral("再次检查"));
        break;
    case Status::Failed:
        m_updateNote->setText(QStringLiteral(
            "检查失败：%1。也可以打开下方的「发布页面」手动查看。").arg(result.error));
        m_updateButton->setText(QStringLiteral("重试"));
        break;
    }
}

void SettingsPage::refreshThemeNote()
{
    if (!m_themeNote)
        return;

    const int stored = AppSettings::themeMode();
    if (stored == 1) {
        m_themeNote->setText(QStringLiteral("已固定为浅色"));
    } else if (stored == 2) {
        m_themeNote->setText(QStringLiteral("已固定为深色"));
    } else {
        m_themeNote->setText(Theme::systemMode() == Theme::Mode::Dark
            ? QStringLiteral("当前跟随系统：深色")
            : QStringLiteral("当前跟随系统：浅色"));
    }
}

void SettingsPage::syncThemeSelect()
{
    if (!m_themeSelect)
        return;
    const QSignalBlocker block(m_themeSelect);
    m_themeSelect->setCurrentIndex(AppSettings::themeMode());
}

void SettingsPage::onThemePicked(int mode)
{
    QString err;
    if (!AppSettings::setThemeMode(mode, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncThemeSelect();       // show the value that is really stored
        return;
    }

    syncThemeSelect();
    emit themeChanged();         // MainWindow re-applies the theme everywhere
    refreshThemeNote();
}

void SettingsPage::syncToolbarStyleSelect()
{
    if (!m_toolbarStyleSelect)
        return;
    const QSignalBlocker block(m_toolbarStyleSelect);
    m_toolbarStyleSelect->setCurrentIndex(AppSettings::toolbarStyle());
}

void SettingsPage::onToolbarStylePicked(int style)
{
    QString err;
    if (!AppSettings::setToolbarStyle(style, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncToolbarStyleSelect();   // show the value that is really stored
        return;
    }
    syncToolbarStyleSelect();
    emit toolbarStyleChanged();      // MainWindow pushes it to every toolbar
}

void SettingsPage::syncThemePreset()
{
    if (!m_themePresetGroup)
        return;
    QAbstractButton *button = m_themePresetGroup->button(AppSettings::themePreset());
    if (!button || button->isChecked())
        return;
    const QSignalBlocker block(m_themePresetGroup);
    button->setChecked(true);
}

void SettingsPage::onThemePresetPicked(int preset)
{
    QString err;
    if (!AppSettings::setThemePreset(preset, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncThemePreset();       // show the value that is really stored
        return;
    }
    syncThemePreset();
    emit themeChanged();         // MainWindow re-applies the theme everywhere
}

void SettingsPage::syncPaletteStyleSelect()
{
    if (!m_paletteStyleSelect)
        return;
    const QSignalBlocker block(m_paletteStyleSelect);
    m_paletteStyleSelect->setCurrentIndex(AppSettings::themeStyle());
}

void SettingsPage::onPaletteStylePicked(int style)
{
    QString err;
    if (!AppSettings::setThemeStyle(style, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncPaletteStyleSelect();    // show the value that is really stored
        return;
    }
    syncPaletteStyleSelect();
    emit themeChanged();             // MainWindow re-applies the theme everywhere
}

void SettingsPage::syncWordSelect()
{
    if (!m_wordSelect)
        return;
    const QSignalBlocker block(m_wordSelect);
    m_wordSelect->setCurrentIndex(AppSettings::wordOpenMode());
}

void SettingsPage::syncWordEngineSelect()
{
    if (!m_wordEngineSelect)
        return;
    const QSignalBlocker block(m_wordEngineSelect);
    m_wordEngineSelect->setCurrentIndex(AppSettings::wordConvertEngine());
}

void SettingsPage::onWordEnginePicked(int engine)
{
    QString err;
    if (!AppSettings::setWordConvertEngine(engine, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncWordEngineSelect();       // show the value that is really stored
        return;
    }
    syncWordEngineSelect();
    emit statusMessage(QStringLiteral("Word 转换引擎已切换，重新打开文档后生效"));
}

void SettingsPage::refreshLibreOfficeStatus()
{
    if (!m_libreOfficeStatus)
        return;
    const QString detected = WordConvert::libreOfficePath();
    if (detected.isEmpty()) {
        m_libreOfficeStatus->setText(
            QStringLiteral("未检测到。可下载 LibreOffice（或便携版）后用「选择 soffice.exe…」指定；"
                           "也可把便携版解压到程序目录的 libreoffice\\ 下。"));
    } else {
        m_libreOfficeStatus->setText(QStringLiteral("已检测到：%1").arg(detected));
    }
    if (m_clearSoffice)
        m_clearSoffice->setEnabled(!AppSettings::libreOfficePath().isEmpty());
}

void SettingsPage::onChooseSoffice()
{
    const QString current = AppSettings::libreOfficePath();
    const QString start = current.isEmpty() ? QString() : QFileInfo(current).absolutePath();
    const QString chosen = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 LibreOffice 的 soffice.exe"), start,
        QStringLiteral("soffice.exe (soffice.exe);;可执行文件 (*.exe)"));
    if (chosen.isEmpty())
        return;
    if (QFileInfo(chosen).fileName().compare(QStringLiteral("soffice.exe"),
                                             Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, QStringLiteral("选择失败"),
                             QStringLiteral("请选择 LibreOffice 的 soffice.exe，通常位于 program 目录下"));
        return;
    }
    QString err;
    if (!AppSettings::setLibreOfficePath(chosen, &err)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        return;
    }
    refreshLibreOfficeStatus();
    emit statusMessage(QStringLiteral("LibreOffice 路径已保存"));
}

void SettingsPage::onClearSoffice()
{
    QString err;
    if (!AppSettings::setLibreOfficePath(QString(), &err)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        return;
    }
    refreshLibreOfficeStatus();
    emit statusMessage(QStringLiteral("已清除 LibreOffice 路径"));
}

void SettingsPage::onOpenSofficePage()
{
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.libreoffice.org/download/")));
}

void SettingsPage::onWordModePicked(int mode)
{
    QString err;
    if (!AppSettings::setWordOpenMode(mode, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncWordSelect();       // show the value that is really stored
        return;
    }
    syncWordSelect();
}

void SettingsPage::onWordNagFixToggled(bool on)
{
    QString err;
    if (!AppSettings::setWordNagFixEnabled(on, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        const QSignalBlocker block(m_wordNagFix);
        m_wordNagFix->setChecked(AppSettings::wordNagFixEnabled());
        return;
    }
    refreshWordNag();
}

// The restore always performs what nagRestoreMode() reports, so the
// confirmation text, the action and the status line cannot drift apart. With
// no backup at all the only thing worth restoring is Word's own default
// (AlertIfNotDefault = 1, no DoNotCheckIfWordIsDefaultApp), which brings the
// nag back - hence the explicit modal confirmation in both cases.
void SettingsPage::onRestoreWordNag()
{
    const WordConvert::NagRestoreMode mode = WordConvert::nagRestoreMode();
    if (mode == WordConvert::NagRestoreMode::None) {
        refreshWordNag();
        return;
    }

    const QString question =
        (mode == WordConvert::NagRestoreMode::FromBackup)
            ? QStringLiteral("用备份把 Word 的两个「不是默认程序」开关值恢复到本程序第一次"
                             "写入之前的状态：\n改动过的写回原值，原本不存在的删除。\n\n"
                             "是否继续？")
            : QStringLiteral("没有可用的备份（可能由旧版本或手动修改造成）：\n将把两个"
                             "开关值恢复为 Word 默认设置，Word 会重新弹出「不是默认程序」"
                             "提醒。\n\n是否继续？");

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("恢复 Word 设置"), question,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString message;
    if (!WordConvert::restoreWordNag(&message)) {
        QMessageBox::warning(this, QStringLiteral("恢复失败"), message);
        refreshWordNag();
        return;
    }
    refreshWordNag();
    // FromBackup clears the message, so the generic line is used there; the
    // ToDefaults path returns its own notice ("the nag will come back") which
    // is exactly what the status bar should say.
    emit statusMessage(message.isEmpty()
        ? QStringLiteral("已恢复 Word 的「不是默认程序」开关")
        : message);
}

// Keeps the button state, its tooltip and the row's state line on the same
// case: exact backup replay, restore-to-defaults, or nothing to do. Called at
// build time, after the toggle, after a restore and on every theme refresh.
void SettingsPage::refreshWordNag()
{
    if (!m_restoreWordNag || !m_wordNagNote)
        return;

    switch (WordConvert::nagRestoreMode()) {
    case WordConvert::NagRestoreMode::FromBackup:
        m_restoreWordNag->setEnabled(true);
        m_wordNagNote->setText(QStringLiteral("可用备份精确还原。"));
        m_restoreWordNag->setToolTip(
            QStringLiteral("可用备份精确还原：改动过的写回原值，原本不存在的删除"));
        break;
    case WordConvert::NagRestoreMode::ToDefaults:
        m_restoreWordNag->setEnabled(true);
        m_wordNagNote->setText(
            QStringLiteral("无备份记录：将恢复为 Word 默认（会重新提醒）。"));
        m_restoreWordNag->setToolTip(
            QStringLiteral("无备份记录：将恢复为 Word 默认（会重新提醒）"));
        break;
    case WordConvert::NagRestoreMode::None:
        m_restoreWordNag->setEnabled(false);
        m_wordNagNote->setText(QStringLiteral("Word 设置未被修改，无需恢复。"));
        m_restoreWordNag->setToolTip(QStringLiteral("Word 设置未被修改，无需恢复"));
        break;
    }
}

void SettingsPage::onAutoStartToggled(bool on)
{
    QString err;
    if (AppSettings::setAutoStart(on, &err))
        return;

    QMessageBox::warning(this, QStringLiteral("设置失败"), err);
    // Put the switch back to the registry state that actually exists.
    const QSignalBlocker block(m_autoStart);
    m_autoStart->setChecked(AppSettings::isAutoStartEnabled());
}

void SettingsPage::onPalmEraserToggled(bool on)
{
    QString err;
    if (!AppSettings::setPalmEraserEnabled(on, &err)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        // Put the switch back to the registry state that actually exists.
        const QSignalBlocker block(m_palmEraser);
        m_palmEraser->setChecked(AppSettings::palmEraserEnabled());
        return;
    }
    emit palmEraserChanged();
}

void SettingsPage::onReduceMotionToggled(bool on)
{
    QString err;
    if (!AppSettings::setReduceMotion(on, &err)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        const QSignalBlocker block(m_reduceMotion);
        m_reduceMotion->setChecked(AppSettings::reduceMotion());
        return;
    }
    Theme::Motion::setReduced(on);   // takes effect on the next animation, no restart
}

void SettingsPage::onRegisterPdf()
{
    QString err;
    if (!AppSettings::registerPdfHandler(&err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("已注册为打开方式"));
    box.setText(QStringLiteral(
        "已把本程序注册为 PDF、批注包 (.dpz) 以及 Word 文档 (.docx/.doc) 的打开方式。"));
    box.setInformativeText(QStringLiteral(
        "Windows 不允许程序直接抢占默认应用，需要你手动确认：\n"
        "接下来打开「默认应用」设置，在 .pdf（以及 .dpz）里选择「落墨·大屏批注」。\n\n"
        "Word 文档只注册为「打开方式」候选，默认仍由 Word 打开；\n"
        "需要时可右键 .docx / .doc →「打开方式」选择本程序。\n\n"
        "确认后，双击任意 PDF 或 .dpz 批注包都会用本程序打开。"));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("打开设置"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("稍后"));
    if (box.exec() == QMessageBox::Ok)
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")));
}

#include "SettingsPage.moc"
