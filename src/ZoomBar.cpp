#include "ZoomBar.h"

#include "Theme.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QSlider>
#include <QToolButton>
#include <QtMath>
#include <cmath>

namespace {

// 80 % of Theme's touch target: this bar shares the one-line status strip.
int stepButtonSize(const Theme::Metrics &m)
{
    return int(m.touch * 0.8);
}

// The percentage pill is sized for the widest value (400 %) so stepping the zoom
// never nudges its neighbours around in the status bar.
int percentWidth(const QFont &font)
{
    return QFontMetrics(font).horizontalAdvance(QStringLiteral("400%"))
           + 2 * Theme::Space2;
}

// One sheet, rebuilt on every theme switch: it is the only place the colours
// live, so refreshTheme() is just this function called with the new palette.
QString zoomSheet(const Theme::Palette &c, const Theme::Metrics &m)
{
    const int side  = stepButtonSize(m);
    const int track = qMax(int(Theme::Space1), m.divider * 4);
    const int knob  = qMax(int(Theme::Space4), int(m.touch * 0.34));
    const int inset = (knob - track) / 2;   // groove overhang that centres the knob

    return QStringLiteral(
               "QToolButton#zoomMinus, QToolButton#zoomPlus {"
               " color: %1; background: transparent;"
               " border: %2 solid %3; border-radius: %4; }"
               "QToolButton#zoomMinus:hover, QToolButton#zoomPlus:hover { background: %5; }"
               "QToolButton#zoomMinus:pressed, QToolButton#zoomPlus:pressed { background: %6; }"
               "QToolButton#zoomPercent {"
               " color: %7; background: %8; border: none; border-radius: %9; }"
               "QToolButton#zoomPercent:hover { background: %5; color: %1; }"
               "QToolButton#zoomPercent:pressed { background: %6; }"
               "QSlider#zoomSlider { background: transparent; }"
               "QSlider#zoomSlider::groove:horizontal {"
               " height: %10; background: %8; border-radius: %11; }"
               "QSlider#zoomSlider::sub-page:horizontal { background: transparent; }"
               "QSlider#zoomSlider::handle:horizontal {"
               " width: %12; height: %12; margin: -%13 0;"
               " background: %14; border: none; border-radius: %15; }"
               // Keyboard focus must stay visible: the groove tints on focus.
               "QSlider#zoomSlider::groove:horizontal:focus { background: %16; }")
        .arg(Theme::rgba(c.text)).arg(Theme::px(m.divider))
        .arg(Theme::rgba(c.surfaceEdge)).arg(Theme::px(side / 2))
        .arg(Theme::rgba(c.surfaceHover)).arg(Theme::rgba(c.surfacePressed))
        .arg(Theme::rgba(c.textMuted)).arg(Theme::rgba(c.chipTint))
        .arg(Theme::px(Theme::RadiusPill)).arg(Theme::px(track))
        .arg(Theme::px(track / 2)).arg(Theme::px(knob))
        .arg(Theme::px(inset)).arg(Theme::rgba(c.accent))
        .arg(Theme::px(knob / 2)).arg(Theme::rgba(c.accentSoft));
}

}   // namespace

namespace ZoomBarMath {

qreal zoomForSlider(int value)
{
    const int v = qBound(0, value, kSliderMax);
    // The ratio is 16^(v/1000). exp2() instead of pow() keeps the anchors
    // (0.25 / 0.5 / 1 / 2 / 4) bit-exact; pow() may be one ulp off.
    return kMinZoom
           * std::exp2(qreal(v) * std::log2(kMaxZoom / kMinZoom) / qreal(kSliderMax));
}

int sliderForZoom(qreal zoom)
{
    const qreal z = qBound(kMinZoom, zoom, kMaxZoom);
    const qreal unitsPerDoubling = qreal(kSliderMax) / std::log2(kMaxZoom / kMinZoom);
    return qRound(std::log2(z / kMinZoom) * unitsPerDoubling);
}

}   // namespace ZoomBarMath

ZoomBar::ZoomBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("zoomBar"));
    setAccessibleName(QStringLiteral("缩放"));
    // The status bar labels' type scale: this bar is chrome, not paper.
    setFont(Theme::scaledFont(font(), 0.95, QFont::Medium));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    const Theme::Metrics m = Theme::metrics(font());
    const int side = stepButtonSize(m);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(qMax(int(Theme::Space2), m.gap));

    // The two step buttons differ only in glyph, tip and direction.
    auto makeStepButton = [&](const QString &name, const QString &glyph, const QString &tip) {
        auto *button = new QToolButton(this);
        button->setObjectName(name);
        button->setText(glyph);
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);   // the slider owns the keyboard
        button->setFont(Theme::scaledFont(font(), 1.25, QFont::DemiBold));
        button->setFixedSize(side, side);
        return button;
    };
    m_minus = makeStepButton(QStringLiteral("zoomMinus"), QStringLiteral("−"),
                             QStringLiteral("缩小"));
    m_plus  = makeStepButton(QStringLiteral("zoomPlus"), QStringLiteral("+"),
                             QStringLiteral("放大"));

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setObjectName(QStringLiteral("zoomSlider"));
    m_slider->setAccessibleName(QStringLiteral("缩放滑块"));
    m_slider->setToolTip(QStringLiteral("拖动调整缩放"));
    m_slider->setRange(0, ZoomBarMath::kSliderMax);
    m_slider->setSingleStep(1);
    m_slider->setPageStep(ZoomBarMath::kSliderMax / 20);   // quiet PageUp / PageDown
    m_slider->setFixedWidth(int(m.touch * 3.4));
    m_slider->setFocusPolicy(Qt::StrongFocus);

    m_percent = new QToolButton(this);
    m_percent->setObjectName(QStringLiteral("zoomPercent"));
    m_percent->setAccessibleName(QStringLiteral("缩放比例"));
    m_percent->setToolTip(QStringLiteral("适配宽度（点击恢复到 100%）"));
    m_percent->setToolButtonStyle(Qt::ToolButtonTextOnly);   // no icon column
    m_percent->setCursor(Qt::PointingHandCursor);
    m_percent->setFocusPolicy(Qt::NoFocus);
    m_percent->setFixedSize(percentWidth(font()), side);

    row->addWidget(m_minus);
    row->addWidget(m_slider);
    row->addWidget(m_plus);
    row->addWidget(m_percent);

    connect(m_minus, &QToolButton::clicked, this, [this] {
        emit zoomRequested(qBound(ZoomBarMath::kMinZoom, m_zoom / 1.25,
                                  ZoomBarMath::kMaxZoom));
    });
    connect(m_plus, &QToolButton::clicked, this, [this] {
        emit zoomRequested(qBound(ZoomBarMath::kMinZoom, m_zoom * 1.25,
                                  ZoomBarMath::kMaxZoom));
    });
    connect(m_percent, &QToolButton::clicked, this, &ZoomBar::fitWidthRequested);

    // Dragging reports through sliderMoved. Qt emits it only while the handle is
    // held, so the keyboard and the wheel - real user input too - arrive as
    // valueChanged; that path is guarded so setZoom()'s own setValue() can never
    // bounce back and the value a drag already reported is not sent twice.
    connect(m_slider, &QSlider::sliderMoved, this, [this](int value) {
        if (m_syncing)
            return;
        m_reported = value;
        emit zoomRequested(ZoomBarMath::zoomForSlider(value));
    });
    connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
        if (m_syncing || value == m_reported)
            return;
        m_reported = value;
        emit zoomRequested(ZoomBarMath::zoomForSlider(value));
    });

    setZoom(1.0);       // where the canvas starts: 1.0 == 适配宽度
    refreshTheme();
}

void ZoomBar::setZoom(qreal zoom)
{
    m_zoom = qBound(ZoomBarMath::kMinZoom, zoom, ZoomBarMath::kMaxZoom);
    const int value = ZoomBarMath::sliderForZoom(m_zoom);

    // While the handle is HELD, the slider is the source of truth. Writing back to it
    // here fights the drag in two ways: the handle gets yanked out from under the
    // finger, and - because a programmatic setValue during a drag re-emits
    // sliderMoved - the canvas -> bar -> slider -> canvas loop can nest one level
    // deeper on every mouse move. That is a stack overflow (0xC00000FD), and it only
    // shows up on a slow machine where more moves arrive mid-flight.
    if (!m_slider->isSliderDown()) {
        m_syncing = true;                  // silent write: emitting here would bounce
        m_slider->setValue(value);
        m_syncing = false;
        m_reported = value;                // the canvas already knows this zoom
    }
    syncLabel();
}

void ZoomBar::refreshTheme()
{
    setStyleSheet(zoomSheet(Theme::light(), Theme::metrics(font())));
}

void ZoomBar::syncLabel()
{
    const int percent = qRound(m_zoom * 100.0);
    m_percent->setText(QStringLiteral("%1%").arg(percent));

    // The tooltip is set ONCE in the constructor and deliberately never written here.
    // Doing it per zoom step drove QToolTip's nested event handling (the pill sits under
    // the pointer during a drag) and that is what overflowed the stack inside
    // Qt6Widgets once already. The current percentage is visible in the label itself, so
    // a dynamic tooltip buys nothing at all.
}
