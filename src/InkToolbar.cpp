#include "InkToolbar.h"

#include "IconPainter.h"
#include "OverlayDismiss.h"
#include "PageGrid.h"
#include "PdfCanvas.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QEvent>
#include <QKeyEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

namespace {

// ---------------------------------------------------------------------------
// Pen palette contents: 6 colours x 3 widths.
// ---------------------------------------------------------------------------

const QColor *penColors()
{
    static const QColor colors[6] = {
        QColor(0xD3, 0x2F, 0x2F),   // 红
        QColor(0x19, 0x76, 0xD2),   // 蓝
        QColor(0x21, 0x21, 0x21),   // 黑
        QColor(0x38, 0x8E, 0x3C),   // 绿
        QColor(0xF5, 0x7C, 0x00),   // 橙
        QColor(0xFB, 0xC0, 0x2D),   // 黄
    };
    return colors;
}

const qreal *penWidths()
{
    static const qreal widths[3] = { 2.0, 4.0, 8.0 };
    return widths;
}

QString penColorLabel(int index)
{
    switch (index) {
    case 0: return QStringLiteral("红");
    case 1: return QStringLiteral("蓝");
    case 2: return QStringLiteral("黑");
    case 3: return QStringLiteral("绿");
    case 4: return QStringLiteral("橙");
    default: return QStringLiteral("黄");
    }
}

QString penWidthLabel(int index)
{
    switch (index) {
    case 0: return QStringLiteral("细");
    case 1: return QStringLiteral("中");
    default: return QStringLiteral("粗");
    }
}

// ---------------------------------------------------------------------------
// Style sheets, built from the Theme tokens.
// ---------------------------------------------------------------------------

QString chipSheet(const Theme::Palette &c, const Theme::Metrics &m)
{
    QString sheet = QStringLiteral(
                        "QFrame#inkChip {"
                        " background: %1;"
                        " border: %2 solid %3;"
                        " border-radius: %4; }"
                        "QFrame#inkSep {"
                        " background: %5;"
                        " border: none;"
                        " border-radius: %6; }")
                        .arg(Theme::rgba(c.surface))
                        .arg(Theme::px(m.divider))
                        .arg(Theme::rgba(c.surfaceEdge))
                        .arg(Theme::px(m.radiusChip))
                        .arg(Theme::rgba(c.divider))
                        .arg(Theme::px(m.divider));

    sheet += QStringLiteral(
                 "QFrame#inkChip QToolButton {"
                 " color: %1;"
                 " background: transparent;"
                 " border: none;"
                 " border-radius: %2;"
                 " padding: %3 %4 %5 %4; }"
                 "QFrame#inkChip QToolButton:hover { background: %6; }"
                 "QFrame#inkChip QToolButton:pressed { background: %7; }"
                 "QFrame#inkChip QToolButton:checked { background: %8; color: %9; }")
                 .arg(Theme::rgba(c.text))
                 .arg(Theme::px(m.radiusButton))
                 .arg(Theme::px(m.padY))
                 .arg(Theme::px(m.padX))
                 .arg(Theme::px(m.padY + 2))
                 .arg(Theme::rgba(c.surfaceHover))
                 .arg(Theme::rgba(c.surfacePressed))
                 .arg(Theme::rgba(c.accent))
                 .arg(Theme::rgba(c.onAccent));

    sheet += QStringLiteral(
                 "QFrame#inkChip QToolButton:checked:hover { background: %1; }"
                 "QFrame#inkChip QToolButton:disabled { color: %2; }"
                 "QFrame#inkChip QToolButton:checked:disabled {"
                 " background: %3; color: %2; }"
                 "QFrame#inkChip QToolButton#inkPage {"
                 " background: %3; color: %4;"
                 " border-radius: %9;"
                 " padding: %5 %6 %5 %6; }"
                 "QFrame#inkChip QToolButton#inkPage:hover { background: %7; }"
                 "QFrame#inkChip QToolButton#inkPage:pressed { background: %8; }"
                 "QFrame#inkChip QToolButton#inkPage:disabled { color: %2; }")
                 .arg(Theme::rgba(c.accentHover))
                 .arg(Theme::rgba(c.textDisabled))
                 .arg(Theme::rgba(c.chipTint))
                 .arg(Theme::rgba(c.text))
                 .arg(Theme::px(m.padY))
                 .arg(Theme::px(m.padX))
                 .arg(Theme::rgba(c.surfaceHover))
                 .arg(Theme::rgba(c.surfacePressed))
                 .arg(Theme::px(m.radiusButton));
    return sheet;
}

QString paletteSheet(const Theme::Palette &c, const Theme::Metrics &m, const QString &cardName)
{
    // `cardName` is the object name of the card frame (penPalette / eraserPalette):
    // both floating panels share this one sheet, so the eraser size popup looks
    // exactly like the pen palette it mirrors.
    return QStringLiteral(
               "QFrame#%1 {"
               " background: %2;"
               " border: %3 solid %4;"
               " border-radius: %5; }"
               "QLabel#paletteCaption { color: %6; background: transparent; }"
               "QFrame#%1 QToolButton {"
               " color: %7;"
               " background: transparent;"
               " border: none;"
               " border-radius: %8;"
               " padding: %9 %10 %11 %10; }"
               "QFrame#%1 QToolButton:hover { background: %12; }"
               "QFrame#%1 QToolButton:pressed { background: %13; }"
               "QFrame#%1 QToolButton:checked {"
               " background: %14; color: %15; }")
        .arg(cardName)
        .arg(Theme::rgba(c.surface))
        .arg(Theme::px(m.divider))
        .arg(Theme::rgba(c.surfaceEdge))
        .arg(Theme::px(qMax(int(Theme::RadiusPopover), m.radiusChip + 2)))
        .arg(Theme::rgba(c.textMuted))
        .arg(Theme::rgba(c.text))
        .arg(Theme::px(m.radiusButton))
        .arg(Theme::px(m.padY))
        .arg(Theme::px(m.padX))
        .arg(Theme::px(m.padY + 2))
        .arg(Theme::rgba(c.surfaceHover))
        .arg(Theme::rgba(c.surfacePressed))
        .arg(Theme::rgba(c.accentSoft))
        .arg(Theme::rgba(c.accent));
}

// Stroke sample of the given width, used as the icon of a width button. The
// faint casing keeps pale pen colours (黄) visible on the light surface.
QIcon widthIcon(qreal width, const QColor &color, const QColor &casing,
                const QSize &logical, qreal dpr)
{
    QPixmap pm(qMax(1, int(logical.width() * dpr)), qMax(1, int(logical.height() * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal y = logical.height() / 2.0;
    const qreal inset = width / 2.0 + 1.0;
    const QPointF a(inset, y);
    const QPointF b(qMax<qreal>(inset + 1.0, logical.width() - inset), y);

    p.setPen(QPen(casing, width + 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, b);
    p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(a, b);
    return QIcon(pm);
}

// 橡皮档位的文字标签（面板与按钮提示共用，所以只有一个定义处）。
QString eraserSizeLabel(qreal radiusPx)
{
    const qreal *steps = PdfCanvas::eraserRadiusSteps();
    if (qFuzzyCompare(radiusPx, steps[0]))
        return QStringLiteral("小");
    if (qFuzzyCompare(radiusPx, steps[1]))
        return QStringLiteral("中");
    return QStringLiteral("大");
}

// 橡皮档位的图标：一个空心圆环，直径按该档在最大档里的占比画出来，三档一眼
// 能分出大小；浅色包边保证深色卡片上也有边界（和 widthIcon 同一个做法）。
QIcon eraserSizeIcon(qreal radiusPx, const QColor &line, const QColor &casing,
                     const QSize &logical, qreal dpr)
{
    QPixmap pm(qMax(1, int(logical.width() * dpr)), qMax(1, int(logical.height() * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    const int count = PdfCanvas::eraserRadiusStepCount();
    const qreal maxRadius = PdfCanvas::eraserRadiusSteps()[qMax(0, count - 1)];
    const QPointF centre(logical.width() / 2.0, logical.height() / 2.0);
    const qreal room = qMax<qreal>(4.0, qMin(logical.width(), logical.height()) / 2.0 - 2.0);
    const qreal r = qBound<qreal>(2.5, room * (radiusPx / maxRadius), room);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(casing, 3.0, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(centre, r, r);
    p.setPen(QPen(line, 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawEllipse(centre, r, r);
    return QIcon(pm);
}

}   // namespace

// Circular colour swatch. The selected one carries a clear accent ring, the
// hovered one a faint halo, so the current pen colour is never ambiguous.
class ColorSwatch : public QAbstractButton
{
public:
    explicit ColorSwatch(const QColor &color, QWidget *parent = nullptr)
        : QAbstractButton(parent), m_color(color)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    QColor color() const { return m_color; }

    QSize sizeHint() const override
    {
        // The palette rides on the island metrics, so its swatches scale with
        // the island instead of keeping a 100% floor.
        const QFontMetrics fm(font());
        const qreal s = qMax<qreal>(40.0, fm.height() * 2.0 + 10.0) * Theme::IslandScale;
        return QSize(int(s), int(s));
    }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)
            update();
        return QAbstractButton::event(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal side = qMin(width(), height());
        const qreal ring = qMax<qreal>(2.0, side * 0.085);
        const qreal gap  = qMax<qreal>(1.5, side * 0.050);
        const QRectF outer = QRectF(rect()).adjusted(ring / 2.0, ring / 2.0,
                                                      -ring / 2.0, -ring / 2.0);
        const QRectF disc = outer.adjusted(ring + gap, ring + gap, -ring - gap, -ring - gap);

        // Selection ring, or a quiet halo on hover.
        if (isChecked()) {
            p.setPen(QPen(pal.accent, ring));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(outer);
        } else if (underMouse()) {
            QColor halo = pal.text;
            halo.setAlpha(0x38);
            p.setPen(QPen(halo, ring * 0.8));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(outer);
        }

        // The colour itself, with a hairline so pale colours keep an edge.
        p.setPen(QPen(pal.pageEdge, qMax<qreal>(1.0, side * 0.035)));
        p.setBrush(m_color);
        p.drawEllipse(disc);

        p.setPen(Qt::NoPen);
        p.setBrush(Qt::NoBrush);
    }

private:
    QColor m_color;
};

// 浮层卡片的公共骨架：圆角卡片 + 柔和投影 + 跟随主题 + "点外部 / Esc 关闭"。
// 笔调色板和橡皮大小面板都建在它上面，所以两块卡片看起来、用起来是同一个
// 东西（同一张 paletteSheet、同一套关闭规则、同一个 DPI 处理）。
//
// 卡片是普通子浮层，不是顶层弹窗：Windows 给无边框窗口加的是方形原生
// （DWM）投影，贴在圆角卡片旁边很难看；作为子部件由 Qt 合成，只剩卡片和
// 它自己的柔和投影。
class PaletteCard : public QWidget
{
public:
    PaletteCard(QWidget *toolbar, const QFont &font, const Theme::Metrics &metrics,
                const QString &cardName)
        : QWidget(toolbar ? toolbar->parentWidget() : nullptr)
        , m_toolbar(toolbar)
        , m_font(font)
        , m_metrics(metrics)
        , m_cardName(cardName)
    {
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setFont(m_font);
        if (qApp)
            qApp->installEventFilter(this);   // dismiss on a click outside
    }

    // Re-applies the card sheet, its shadow and the DPI/themed artwork.
    virtual void applyTheme()
    {
        if (m_chip)
            m_chip->setStyleSheet(paletteSheet(Theme::light(), m_metrics, m_cardName));
        refreshGlow();
        refreshArtwork();
        update();
    }

protected:
    // Builds the rounded card + shadow and returns the layout the content goes
    // into. Called once by each subclass constructor.
    QVBoxLayout *buildCard()
    {
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(m_metrics.shadowRoom, m_metrics.shadowRoom,
                                  m_metrics.shadowRoom, m_metrics.shadowRoom);
        outer->setSpacing(0);

        m_chip = new QFrame(this);
        m_chip->setObjectName(m_cardName);
        m_chip->setAttribute(Qt::WA_StyledBackground, true);
        m_chip->setStyleSheet(paletteSheet(Theme::light(), m_metrics, m_cardName));
        m_glow = new QGraphicsDropShadowEffect(m_chip);
        m_glow->setBlurRadius(m_metrics.shadowBlur);
        m_glow->setOffset(0.0, m_metrics.shadowOffsetY);
        refreshGlow();
        m_chip->setGraphicsEffect(m_glow);
        outer->addWidget(m_chip);

        auto *content = new QVBoxLayout(m_chip);
        content->setContentsMargins(m_metrics.chipPad + Theme::Space1,
                                    m_metrics.chipPad + Theme::Space1,
                                    m_metrics.chipPad + Theme::Space1,
                                    m_metrics.chipPad + Theme::Space1);
        content->setSpacing(m_metrics.gap);
        return content;
    }

    void showEvent(QShowEvent *e) override
    {
        QWidget::showEvent(e);
        refreshArtwork();   // pick up the current screen's device pixel ratio
    }

    bool eventFilter(QObject *watched, QEvent *e) override
    {
        if (!isVisible())
            return QWidget::eventFilter(watched, e);

        switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::TouchBegin:
            // Decide by POSITION: the canvas accepts touch events, so a tap on the
            // palette still reaches this filter with the viewport as `watched`.
            if (!Overlay::pressInside(e, this, m_toolbar))
                hide();
            break;
        case QEvent::KeyPress:
            if (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
                hide();
                return true;
            }
            break;
        default:
            break;
        }
        return QWidget::eventFilter(watched, e);
    }

    // Subclass hook: rebuild the icons/labels whose colours or DPI changed.
    virtual void refreshArtwork() {}

    QFrame *chip() const { return m_chip; }
    const Theme::Metrics &metrics() const { return m_metrics; }

private:
    void refreshGlow()
    {
        if (!m_glow)
            return;
        QColor shadow = Theme::light().shadow;
        shadow.setAlpha(0x46);
        m_glow->setColor(shadow);
    }

    QWidget                *m_toolbar = nullptr;   // clicking here must not auto-hide
    QFont                   m_font;
    Theme::Metrics          m_metrics{};
    QString                 m_cardName;
    QFrame                 *m_chip = nullptr;
    QGraphicsDropShadowEffect *m_glow = nullptr;
};

// Pen colour + width palette: a child overlay above the toolbar.
class PenPalette : public PaletteCard
{
    Q_OBJECT
public:
    // The island hands down its scaled font and metrics: the palette is part of
    // the same piece of chrome, so it shrinks with it.
    PenPalette(QWidget *toolbar, const QFont &font, const Theme::Metrics &metrics)
        : PaletteCard(toolbar, font, metrics, QStringLiteral("penPalette"))
    {
        buildUi();
    }

    void syncSelection(const QColor &color, qreal width);

signals:
    void colorPicked(const QColor &color);
    void widthPicked(qreal width);

protected:
    void refreshArtwork() override { refreshWidthIcons(); }

private:
    void buildUi();
    void refreshWidthIcons();

    QColor                  m_color{ 0xD3, 0x2F, 0x2F };
    QVector<ColorSwatch *> m_swatches;
    QVector<QToolButton *> m_widthButtons;
};

// 橡皮大小面板：三档（小 / 中 / 大）。和笔调色板是同一张卡片 —— 同一个
// paletteSheet、同一个关闭规则、同一个定位方式，内容换成三个圆环图标。
class EraserPalette : public PaletteCard
{
    Q_OBJECT
public:
    EraserPalette(QWidget *toolbar, const QFont &font, const Theme::Metrics &metrics)
        : PaletteCard(toolbar, font, metrics, QStringLiteral("eraserPalette"))
    {
        buildUi();
    }

    // 高亮当前档（值一定吸附到某一档，所以总能对上）。
    void syncSelection(qreal radius);

signals:
    void sizePicked(qreal radiusPx);

protected:
    void refreshArtwork() override { refreshIcons(); }

private:
    void buildUi();
    void refreshIcons();

    QVector<QToolButton *> m_sizeButtons;
};

void PenPalette::buildUi()
{
    const Theme::Metrics m = metrics();
    QVBoxLayout *content = buildCard();
    QFrame *card = chip();

    const QFont captionFont = Theme::captionFont(font());
    auto addCaption = [&](const QString &text) {
        auto *caption = new QLabel(text, card);
        caption->setObjectName(QStringLiteral("paletteCaption"));
        caption->setFont(captionFont);
        content->addWidget(caption);
    };

    addCaption(QStringLiteral("颜色"));
    auto *colorRow = new QHBoxLayout;
    colorRow->setSpacing(Theme::Space2);
    const QColor *colors = penColors();
    auto *colorGroup = new QButtonGroup(this);
    colorGroup->setExclusive(true);
    for (int i = 0; i < 6; ++i) {
        auto *swatch = new ColorSwatch(colors[i], card);
        swatch->setToolTip(QStringLiteral("%1  %2").arg(penColorLabel(i), colors[i].name()));
        colorGroup->addButton(swatch);
        connect(swatch, &QAbstractButton::clicked, this, [this, swatch] {
            emit colorPicked(swatch->color());
        });
        m_swatches.append(swatch);
        colorRow->addWidget(swatch);
    }
    content->addLayout(colorRow);

    content->addSpacing(Theme::Space1);
    addCaption(QStringLiteral("粗细"));
    auto *widthRow = new QHBoxLayout;
    widthRow->setSpacing(m.gap);
    auto *widthGroup = new QButtonGroup(this);
    widthGroup->setExclusive(true);
    const qreal *widths = penWidths();
    for (int i = 0; i < 3; ++i) {
        auto *button = new QToolButton(card);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setText(penWidthLabel(i));
        button->setToolTip(QStringLiteral("%1 · %2 px").arg(penWidthLabel(i), QString::number(widths[i])));
        button->setMinimumHeight(m.touch);
        connect(button, &QToolButton::clicked, this, [this, i] {
            emit widthPicked(penWidths()[i]);
        });
        widthGroup->addButton(button);
        m_widthButtons.append(button);
        widthRow->addWidget(button, 1);
    }
    content->addLayout(widthRow);

    refreshWidthIcons();
}

void PenPalette::refreshWidthIcons()
{
    const Theme::Palette &pal = Theme::light();
    const Theme::Metrics m = metrics();
    const QSize box(qMax(30, m.icon + 12), qMax(14, int(m.icon * 0.62)));
    const qreal dpr = devicePixelRatioF();
    const qreal *widths = penWidths();
    const int count = int(m_widthButtons.size());
    for (int i = 0; i < count; ++i) {
        QToolButton *button = m_widthButtons.at(i);
        button->setIconSize(box);
        button->setIcon(widthIcon(widths[i], m_color, pal.pageEdge, box, dpr));
    }
}

void PenPalette::syncSelection(const QColor &color, qreal width)
{
    m_color = color;

    const QColor *colors = penColors();
    const int swatchCount = int(m_swatches.size());
    for (int i = 0; i < swatchCount; ++i) {
        if (colors[i] == color) {
            m_swatches.at(i)->setChecked(true);   // the group unchecks the rest
            break;
        }
    }

    const qreal *widths = penWidths();
    const int widthCount = int(m_widthButtons.size());
    for (int i = 0; i < widthCount; ++i) {
        if (qFuzzyCompare(widths[i], width)) {
            m_widthButtons.at(i)->setChecked(true);
            break;
        }
    }

    refreshWidthIcons();
}

// --- 橡皮大小面板 -----------------------------------------------------------

void EraserPalette::buildUi()
{
    const Theme::Metrics m = metrics();
    QVBoxLayout *content = buildCard();
    QFrame *card = chip();

    const QFont captionFont = Theme::captionFont(font());
    auto *caption = new QLabel(QStringLiteral("大小"), card);
    caption->setObjectName(QStringLiteral("paletteCaption"));
    caption->setFont(captionFont);
    content->addWidget(caption);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->setSpacing(m.gap);
    auto *sizeGroup = new QButtonGroup(this);
    sizeGroup->setExclusive(true);
    const qreal *steps = PdfCanvas::eraserRadiusSteps();
    const int count = PdfCanvas::eraserRadiusStepCount();
    for (int i = 0; i < count; ++i) {
        auto *button = new QToolButton(card);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setText(eraserSizeLabel(steps[i]));
        // 提示给出实际擦除直径（圆环直径 = 两倍半径），和按钮上的圆环图标对得上。
        button->setToolTip(QStringLiteral("%1 · 直径 %2 px")
                               .arg(eraserSizeLabel(steps[i]),
                                    QString::number(qRound(steps[i] * 2.0))));
        button->setMinimumHeight(m.touch);
        connect(button, &QToolButton::clicked, this, [this, i] {
            emit sizePicked(PdfCanvas::eraserRadiusSteps()[i]);
        });
        sizeGroup->addButton(button);
        m_sizeButtons.append(button);
        sizeRow->addWidget(button, 1);
    }
    content->addLayout(sizeRow);

    refreshIcons();
}

void EraserPalette::refreshIcons()
{
    const Theme::Palette &pal = Theme::light();
    const Theme::Metrics m = metrics();
    // 方形略宽的音符盒：三个圆环按档位占比排开，高度一致才好比较。
    const QSize box(qMax(30, m.icon + 12), qMax(20, m.icon));
    const qreal dpr = devicePixelRatioF();
    const qreal *steps = PdfCanvas::eraserRadiusSteps();
    const int count = int(m_sizeButtons.size());
    for (int i = 0; i < count; ++i) {
        QToolButton *button = m_sizeButtons.at(i);
        button->setIconSize(box);
        button->setIcon(eraserSizeIcon(steps[i], pal.text, pal.pageEdge, box, dpr));
    }
}

void EraserPalette::syncSelection(qreal radius)
{
    const qreal *steps = PdfCanvas::eraserRadiusSteps();
    const int count = int(m_sizeButtons.size());
    for (int i = 0; i < count; ++i) {
        if (qFuzzyCompare(steps[i], radius)) {
            m_sizeButtons.at(i)->setChecked(true);   // the group unchecks the rest
            break;
        }
    }
    refreshIcons();
}

InkToolbar::InkToolbar(PdfCanvas *canvas)
    : QWidget(canvas ? canvas->viewport() : nullptr)
    , m_canvas(canvas)
{
    buildUi();

    if (m_canvas) {
        connect(m_canvas, &PdfCanvas::toolChanged,      this, &InkToolbar::syncFromCanvas);
        connect(m_canvas, &PdfCanvas::penChanged,       this, &InkToolbar::syncFromCanvas);
        connect(m_canvas, &PdfCanvas::eraserChanged,    this, &InkToolbar::syncFromCanvas);
        connect(m_canvas, &PdfCanvas::undoStateChanged, this, &InkToolbar::syncFromCanvas);
        connect(m_canvas, &PdfCanvas::inkChanged,       this, &InkToolbar::syncFromCanvas);
        connect(m_canvas, &PdfCanvas::pageChanged,      this, &InkToolbar::syncFromCanvas);
    }

    syncFromCanvas();
    reposition();
}

InkToolbar::~InkToolbar() = default;

void InkToolbar::buildUi()
{
    const Theme::Palette &pal = Theme::light();

    // The island ships at 80% of the chrome type scale: font and metrics shrink
    // together, so the bar stays one proportional piece.
    const QFont base = font();
    setFont(Theme::islandFont(base));
    setCursor(Qt::OpenHandCursor);
    m_metrics = Theme::metrics(Theme::chromeFont(base), Theme::IslandScale);
    const Theme::Metrics m = m_metrics;

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(m.shadowRoom, m.shadowRoom, m.shadowRoom, m.shadowRoom);
    outer->setSpacing(0);

    m_chip = new QFrame(this);
    m_chip->setObjectName(QStringLiteral("inkChip"));
    m_chip->setAttribute(Qt::WA_StyledBackground, true);
    m_chip->setStyleSheet(chipSheet(pal, m));
    m_glow = new QGraphicsDropShadowEffect(m_chip);
    m_glow->setBlurRadius(m.shadowBlur);
    m_glow->setOffset(0.0, m.shadowOffsetY);
    QColor shadow = pal.shadow;
    shadow.setAlpha(0x46);
    m_glow->setColor(shadow);
    m_chip->setGraphicsEffect(m_glow);
    outer->addWidget(m_chip);

    auto *chipRow = new QHBoxLayout(m_chip);
    chipRow->setContentsMargins(m.chipPad, m.chipPad, m.chipPad, m.chipPad);
    chipRow->setSpacing(m.gap);

    m_body = new QWidget(m_chip);
    auto *row = new QHBoxLayout(m_body);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(m.gap);
    chipRow->addWidget(m_body, 1);

    auto addButton = [this, &m](QHBoxLayout *into, IconPainter::Glyph glyph,
                                const QString &text, const QString &tip,
                                bool checkable) -> QToolButton * {
        auto *button = new QToolButton(m_chip);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(checkable);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(font());
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(m.icon, m.icon));
        button->setMinimumSize(QSize(m.touch, m.buttonHeight));
        m_glyphs.insert(button, glyph);
        into->addWidget(button);
        return button;
    };

    auto addSeparator = [this, &m](QHBoxLayout *into) -> QFrame * {
        auto *sep = new QFrame(m_chip);
        sep->setObjectName(QStringLiteral("inkSep"));
        sep->setAttribute(Qt::WA_StyledBackground, true);
        sep->setFrameShape(QFrame::NoFrame);
        sep->setFixedWidth(m.divider);
        sep->setFixedHeight(int(m.buttonHeight * 0.55));
        sep->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        into->addWidget(sep);
        return sep;
    };

    // Tools, one of them is always the active tool.
    m_penButton = addButton(row, IconPainter::Glyph::Pen, QStringLiteral("笔"),
                            QStringLiteral("画笔：选择颜色与粗细"), true);
    m_eraserButton = addButton(row, IconPainter::Glyph::Eraser, QStringLiteral("橡皮"),
                               QStringLiteral("点擦：擦除碰到的整条笔迹"), true);
    m_moveButton = addButton(row, IconPainter::Glyph::Move, QStringLiteral("自由移动"),
                             QStringLiteral("自由移动：拖动画面（不批注）"), true);
    auto *toolGroup = new QButtonGroup(this);
    toolGroup->setExclusive(true);
    toolGroup->addButton(m_penButton);
    toolGroup->addButton(m_eraserButton);
    toolGroup->addButton(m_moveButton);

    addSeparator(row);

    // Group 3: history and page clearing.
    m_undoButton = addButton(row, IconPainter::Glyph::Undo, QStringLiteral("撤销"),
                             QStringLiteral("撤销上一步"), false);
    m_redoButton = addButton(row, IconPainter::Glyph::Redo, QStringLiteral("重做"),
                             QStringLiteral("重做上一步"), false);
    m_clearButton = addButton(row, IconPainter::Glyph::Trash, QStringLiteral("清空"),
                              QStringLiteral("清空本页批注"), false);

    addSeparator(row);

    // Group 4: view controls.
    m_fitButton = addButton(row, IconPainter::Glyph::FitWidth, QStringLiteral("适配宽度"),
                            QStringLiteral("适配宽度：按页面宽度自动缩放"), false);

    m_pageButton = new QToolButton(m_chip);
    m_pageButton->setObjectName(QStringLiteral("inkPage"));
    m_pageButton->setText(QStringLiteral("- / -"));
    m_pageButton->setToolTip(QStringLiteral("跳转页码"));
    m_pageButton->setFocusPolicy(Qt::NoFocus);
    m_pageButton->setCursor(Qt::PointingHandCursor);
    m_pageButton->setFont(font());
    m_pageButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_pageButton->setMinimumSize(QSize(m.touch + Theme::Space5, m.buttonHeight));
    row->addWidget(m_pageButton);

    // Save actions sit between the page counter and the settings gear.
    m_saveButton = addButton(row, IconPainter::Glyph::Save, QStringLiteral("保存"),
                             QStringLiteral("保存批注（打包为 .dpz）"), false);
    m_saveAsButton = addButton(row, IconPainter::Glyph::SaveAs, QStringLiteral("另存为"),
                               QStringLiteral("另存为：打包保存或注入 PDF"), false);

    // Settings sits between the page counter and the collapse chevron.
    m_settingsButton = addButton(row, IconPainter::Glyph::Gear, QStringLiteral("设置"),
                                 QStringLiteral("设置：开机自启与文件关联"), false);

    // Fullscreen sits right after settings; the window state belongs to the host.
    m_fullscreenButton = addButton(row, IconPainter::Glyph::Fullscreen, QStringLiteral("全屏"),
                                   QStringLiteral("全屏显示（F11）"), false);

    // Group 5: the collapse / expand chevron, kept outside the collapsible body
    // so it is always reachable.
    m_moreSep = addSeparator(chipRow);
    m_moreButton = new QToolButton(m_chip);
    m_moreButton->setToolTip(QStringLiteral("收起工具栏"));
    m_moreButton->setAccessibleName(QStringLiteral("收起工具栏"));
    m_moreButton->setFocusPolicy(Qt::NoFocus);
    m_moreButton->setCursor(Qt::PointingHandCursor);
    m_moreButton->setFont(font());
    m_moreButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_moreButton->setIconSize(QSize(m.icon, m.icon));
    m_moreButton->setMinimumSize(QSize(m.touch, m.buttonHeight));
    m_glyphs.insert(m_moreButton, IconPainter::Glyph::ChevronUp);
    chipRow->addWidget(m_moreButton);

    // The page counter pill needs its object name before the sheet is polished.
    m_pageButton->style()->unpolish(m_pageButton);
    m_pageButton->style()->polish(m_pageButton);

    connect(m_penButton, &QToolButton::clicked, this, [this] {
        if (m_canvas)
            m_canvas->setTool(PdfCanvas::InkTool::Pen);
        showPenPalette();
    });
    connect(m_eraserButton, &QToolButton::clicked, this, [this] {
        if (m_canvas)
            m_canvas->setTool(PdfCanvas::InkTool::Eraser);
        showEraserPalette();
    });
    connect(m_moveButton, &QToolButton::clicked, this, [this] {
        hidePalettes();
        dismissPageGrid();
        if (m_canvas)
            m_canvas->setTool(PdfCanvas::InkTool::Move);
    });
    connect(m_undoButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        if (m_canvas) m_canvas->undo();
    });
    connect(m_redoButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        if (m_canvas) m_canvas->redo();
    });
    connect(m_clearButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        if (m_canvas) m_canvas->clearCurrentPage();
    });
    connect(m_fitButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        if (m_canvas) m_canvas->setFitWidth(true);
    });
    connect(m_pageButton, &QToolButton::clicked, this, &InkToolbar::togglePageGrid);
    connect(m_saveButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        hidePalettes();
        emit saveRequested();
    });
    connect(m_saveAsButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        hidePalettes();
        emit saveAsRequested();
    });
    connect(m_settingsButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        hidePalettes();
        emit settingsRequested();
    });
    connect(m_fullscreenButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        hidePalettes();
        emit fullscreenRequested();
    });
    // The chevron IS the collapse / expand affordance: a direct click toggles
    // the island, there is no intermediate menu any more.
    connect(m_moreButton, &QToolButton::clicked, this, &InkToolbar::toggleCollapsed);

    m_palette = new PenPalette(this, font(), m_metrics);
    connect(m_palette, &PenPalette::colorPicked, this, &InkToolbar::onPenColorPicked);
    connect(m_palette, &PenPalette::widthPicked, this, &InkToolbar::onPenWidthPicked);
    // Child overlays are shown together with their parent unless they were
    // explicitly hidden once, so hide them here or they pop up on startup.
    m_palette->hide();

    // 橡皮按钮的大小面板：和笔调色板同一张卡片、同一套关闭/定位规则，
    // 只是一次点开（不是长按 —— 长按在这里没人会用）。
    m_eraserPalette = new EraserPalette(this, font(), m_metrics);
    connect(m_eraserPalette, &EraserPalette::sizePicked, this, &InkToolbar::onEraserSizePicked);
    m_eraserPalette->hide();

    // The page-thumbnail picker is another viewport child overlay, owned by the
    // canvas like the palette: the "n / N" chip toggles it.
    m_pageGrid = new PageGrid(m_canvas, this);
    m_pageGrid->hide();

    // Every press on the island may start a drag, the ones landing on a button
    // included: the filter arms on the press and only takes over once the
    // pointer has travelled far enough, so a tap still clicks the button.
    for (QWidget *child : findChildren<QWidget *>())
        child->installEventFilter(this);

    refreshIcons();
}

QWidget *InkToolbar::testEraserPalette() const
{
    return m_eraserPalette;
}

void InkToolbar::refreshIcons()
{
    const Theme::Palette &pal = Theme::light();
    const IconPainter::States states{ pal.text, pal.onAccent, pal.textDisabled };
    const QColor pen = m_canvas ? m_canvas->penColor() : pal.accent;
    const qreal dpr = devicePixelRatioF();

    for (auto it = m_glyphs.constBegin(); it != m_glyphs.constEnd(); ++it) {
        IconPainter::Glyph glyph = it.value();
        // The chevron points up while expanded and down while collapsed, so the
        // toggle always shows what the next click will do.
        if (it.key() == m_moreButton && m_collapsed)
            glyph = IconPainter::Glyph::ChevronDown;
        else if (it.key() == m_fullscreenButton && m_fullscreenActive)
            glyph = IconPainter::Glyph::FullscreenExit;   // show the way out
        const QColor badge = (glyph == IconPainter::Glyph::Pen) ? pen : QColor();
        it.key()->setIcon(IconPainter::makeIcon(glyph, m_metrics.icon, states, dpr, 1.0, badge));
    }

    m_iconPenColor = pen;
    m_iconDpr = dpr;
}

void InkToolbar::applyTheme()
{
    if (m_chip)
        m_chip->setStyleSheet(chipSheet(Theme::light(), m_metrics));
    if (m_glow) {
        QColor shadow = Theme::light().shadow;
        shadow.setAlpha(0x46);
        m_glow->setColor(shadow);
    }
    if (m_palette)
        m_palette->applyTheme();
    if (m_eraserPalette)
        m_eraserPalette->applyTheme();
    if (m_pageGrid)
        m_pageGrid->applyTheme();   // owned by the island, drawn on the canvas
    refreshIcons();
    update();
}

void InkToolbar::syncFromCanvas()
{
    if (!m_canvas)
        return;

    const PdfCanvas::InkTool tool = m_canvas->tool();
    const bool penTool    = (tool == PdfCanvas::InkTool::Pen);
    const bool eraserTool = (tool == PdfCanvas::InkTool::Eraser);
    const bool moveTool   = (tool == PdfCanvas::InkTool::Move);
    if (m_penButton->isChecked() != penTool)
        m_penButton->setChecked(penTool);
    if (m_eraserButton->isChecked() != eraserTool)
        m_eraserButton->setChecked(eraserTool);
    if (m_moveButton->isChecked() != moveTool)
        m_moveButton->setChecked(moveTool);

    m_undoButton->setEnabled(m_canvas->canUndo());
    m_redoButton->setEnabled(m_canvas->canRedo());
    m_clearButton->setEnabled(m_canvas->strokeCount() > 0);

    const int count = m_canvas->pageCount();
    const QString pageText = count > 0
        ? QStringLiteral("%1 / %2").arg(m_canvas->currentPage() + 1).arg(count)
        : QStringLiteral("- / -");
    if (m_pageButton->text() != pageText)
        m_pageButton->setText(pageText);
    m_pageButton->setEnabled(count > 0);

    // The pen button carries the current ink colour as a badge.
    const QColor pen = m_canvas->penColor();
    if (pen != m_iconPenColor)
        refreshIcons();

    if (m_palette)
        m_palette->syncSelection(pen, m_canvas->penWidth());

    // 橡皮按钮的提示带上当前大小（圆环直径 = 2 倍半径），面板高亮同一档。
    if (m_eraserButton) {
        const qreal radius = m_canvas->eraserRadius();
        const QString tip = QStringLiteral("点擦：擦除碰到的整条笔迹 · 当前大小：%1（直径 %2 px）")
                                .arg(eraserSizeLabel(radius),
                                     QString::number(qRound(radius * 2.0)));
        if (m_eraserButton->toolTip() != tip)
            m_eraserButton->setToolTip(tip);
    }
    if (m_eraserPalette)
        m_eraserPalette->syncSelection(m_canvas->eraserRadius());
}

void InkToolbar::showPenPalette()
{
    if (!m_palette)
        return;
    if (m_collapsed)
        setCollapsed(false);
    // Only one card floats at a time.
    dismissPageGrid();
    if (m_eraserPalette)
        m_eraserPalette->hide();
    if (m_palette->isVisible()) {
        m_palette->hide();
        return;
    }
    if (m_canvas)
        m_palette->syncSelection(m_canvas->penColor(), m_canvas->penWidth());
    positionPaletteAbove(m_penButton, m_palette);
    m_palette->show();
    m_palette->raise();
}

// 橡皮按钮的点击路径：和笔按钮完全对称 —— 普通一点开面板，再点收起；
// 选中一档后关掉面板。没有任何"按住"行为。
void InkToolbar::showEraserPalette()
{
    if (!m_eraserPalette)
        return;
    if (m_collapsed)
        setCollapsed(false);
    dismissPageGrid();
    if (m_palette)
        m_palette->hide();
    if (m_eraserPalette->isVisible()) {
        m_eraserPalette->hide();
        return;
    }
    if (m_canvas)
        m_eraserPalette->syncSelection(m_canvas->eraserRadius());
    positionPaletteAbove(m_eraserButton, m_eraserPalette);
    m_eraserPalette->show();
    m_eraserPalette->raise();
}

void InkToolbar::hidePalettes()
{
    if (m_palette)
        m_palette->hide();
    if (m_eraserPalette)
        m_eraserPalette->hide();
}

// 把卡片锚在某个按钮上方，并钳在它所属的页面区域内。
void InkToolbar::positionPaletteAbove(QWidget *anchorButton, QWidget *card)
{
    if (!anchorButton || !card)
        return;

    const Theme::Metrics m = m_metrics;
    card->adjustSize();
    const QSize size = card->size();

    const QPoint anchor = anchorButton->mapToGlobal(QPoint(anchorButton->width() / 2, 0));
    int x = anchor.x() - size.width() / 2;
    int y = anchor.y() - size.height() - m.gap;

    // Keep it inside the page area it belongs to (the palette is a child of
    // that widget, so its position is parent-relative).
    QWidget *host = card->parentWidget();
    QRect bounds;
    if (host)
        bounds = QRect(host->mapToGlobal(QPoint(0, 0)), host->size());
    else if (QScreen *scr = screen())
        bounds = scr->availableGeometry();

    if (bounds.isValid()) {
        const int minX = bounds.left() + m.gap;
        const int maxX = qMax(minX, bounds.right() - size.width() - m.gap);
        const int minY = bounds.top() + m.gap;
        const int maxY = qMax(minY, bounds.bottom() - size.height() - m.gap);
        x = qBound(minX, x, maxX);
        y = qBound(minY, y, maxY);
    }

    if (host)
        card->move(host->mapFromGlobal(QPoint(x, y)));
    else
        card->move(x, y);
}

void InkToolbar::onPenColorPicked(const QColor &color)
{
    if (color.isValid() && m_canvas)
        m_canvas->setPenColor(color);
    if (m_palette)
        m_palette->hide();
}

void InkToolbar::onPenWidthPicked(qreal width)
{
    if (m_canvas)
        m_canvas->setPenWidth(width);
    if (m_palette)
        m_palette->hide();
}

void InkToolbar::onEraserSizePicked(qreal radiusPx)
{
    if (m_canvas)
        m_canvas->setEraserRadius(radiusPx);
    if (m_eraserPalette)
        m_eraserPalette->hide();
}

// The page chip is a toggle: it opens the thumbnail picker, and clicking it
// again (or picking a page) closes it. The picker is the only way to jump to a
// page now that the modal number dialog is gone.
void InkToolbar::togglePageGrid()
{
    if (!m_pageGrid || !m_canvas || m_canvas->pageCount() <= 0)
        return;
    if (m_collapsed)
        setCollapsed(false);
    if (m_pageGrid->isOpen()) {
        m_pageGrid->close();
        return;
    }
    hidePalettes();
    m_pageGrid->open();
}

void InkToolbar::dismissPageGrid()
{
    if (m_pageGrid)
        m_pageGrid->close();
}

void InkToolbar::setCollapsed(bool on)
{
    if (m_collapsed == on)
        return;

    m_collapsed = on;
    if (on)
        hidePalettes();
    if (on)
        dismissPageGrid();
    if (m_body)
        m_body->setVisible(!on);
    if (m_moreSep)
        m_moreSep->setVisible(!on);
    if (m_moreButton) {
        m_moreButton->setToolTip(on ? QStringLiteral("展开工具栏") : QStringLiteral("收起工具栏"));
        m_moreButton->setAccessibleName(on ? QStringLiteral("展开工具栏")
                                           : QStringLiteral("收起工具栏"));
    }
    refreshIcons();     // the chevron flips direction with the state
    if (layout())
        layout()->activate();

    reposition();
    emit hiddenChanged(m_collapsed);
}

void InkToolbar::toggleCollapsed()
{
    setCollapsed(!m_collapsed);
}

void InkToolbar::setFullscreenActive(bool on)
{
    if (m_fullscreenActive == on)
        return;

    m_fullscreenActive = on;
    if (m_fullscreenButton) {
        m_fullscreenButton->setToolTip(on ? QStringLiteral("退出全屏（Esc）")
                                          : QStringLiteral("全屏显示（F11）"));
    }
    refreshIcons();
}

QPoint InkToolbar::clampToolbarPos(const QPoint &pos, const QSize &host, const QSize &self)
{
    const int maxX = qMax(0, host.width() - self.width());
    const int maxY = qMax(0, host.height() - self.height());
    return QPoint(qBound(0, pos.x(), maxX), qBound(0, pos.y(), maxY));
}

void InkToolbar::moveBy(const QPoint &delta)
{
    QWidget *host = parentWidget();
    if (!host)
        return;

    m_userPos = clampToolbarPos(pos() + delta, host->size(), size());
    m_dragged = true;
    move(m_userPos);
    // 开着的面板跟着自己的按钮走
    if (m_palette && m_palette->isVisible())
        positionPaletteAbove(m_penButton, m_palette);
    if (m_eraserPalette && m_eraserPalette->isVisible())
        positionPaletteAbove(m_eraserButton, m_eraserPalette);
    raise();
}

// One drag path: the island's own handlers and the child event filter both
// funnel through these, so the gesture behaves the same wherever it started.

void InkToolbar::beginDrag(const QPoint &posInIsland)
{
    m_dragging = true;
    m_dragOffset = posInIsland;
    setCursor(Qt::ClosedHandCursor);
}

void InkToolbar::dragTo(const QPoint &posInIsland)
{
    QWidget *host = parentWidget();
    if (!m_dragging || !host)
        return;

    // Keep the grab point under the pointer: move to that absolute target,
    // expressed as a delta for the clamped moveBy path.
    const QPoint target = mapToParent(posInIsland) - m_dragOffset;
    moveBy(target - pos());
}

void InkToolbar::endDrag()
{
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
}

void InkToolbar::resetPress()
{
    m_pressArmed = false;
    m_pressChild = nullptr;
}

// A press on any child - buttons included - arms a drag. The button sees the
// press first, so a tap keeps working; the drag only takes over once the
// pointer has travelled startDragDistance(), and from then on the release is
// swallowed so the button under it never fires clicked().
bool InkToolbar::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *w = qobject_cast<QWidget *>(watched);
    if (!w)
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton)
            break;
        m_pressArmed = true;
        m_pressPos = w->mapTo(this, me->position().toPoint());
        m_pressChild = w;
        break;                    // never consumed: the button needs the press
    }
    case QEvent::MouseMove: {
        if (!m_dragging && !m_pressArmed)
            break;
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint inIsland = w->mapTo(this, me->position().toPoint());
        if (!m_dragging) {
            if ((inIsland - m_pressPos).manhattanLength()
                < QApplication::startDragDistance()) {
                break;            // still a tap, not a drag
            }
            beginDrag(m_pressPos);
        }
        dragTo(inIsland);
        return true;              // the button must not track the pointer
    }
    case QEvent::MouseButtonRelease: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton)
            break;
        const bool dragged = m_dragging;
        if (dragged) {
            endDrag();
            // The release is swallowed, so clear the pressed look by hand.
            if (auto *button = qobject_cast<QAbstractButton *>(m_pressChild))
                button->setDown(false);
        }
        resetPress();
        if (dragged)
            return true;          // no clicked() for the button under it
        break;
    }
    default:
        break;
    }
    return QWidget::eventFilter(watched, event);
}

void InkToolbar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        // The island's own margin: no button under the pointer, so the drag
        // starts right away.
        beginDrag(e->position().toPoint());
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void InkToolbar::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging) {
        dragTo(e->position().toPoint());
        e->accept();
        return;
    }
    QWidget::mouseMoveEvent(e);
}

void InkToolbar::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_dragging && e->button() == Qt::LeftButton) {
        endDrag();
        resetPress();
        e->accept();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void InkToolbar::reposition()
{
    QWidget *host = parentWidget();
    if (!host)
        return;

    // The hint must be fresh: the chip's height depends on the current font
    // metrics, and collapsing hides the body. Invalidating only the top layout
    // is not enough - a hide() posts a LayoutRequest, so the nested chip and
    // body layouts would still answer with their old size for one event loop
    // turn, which is exactly what used to leave the bar full width (and its
    // buttons squeezed) after a collapse.
    QLayout *chain[3] = { layout(),
                          m_chip ? m_chip->layout() : nullptr,
                          m_body ? m_body->layout() : nullptr };
    for (QLayout *lay : chain) {
        if (!lay)
            continue;
        lay->invalidate();
        lay->activate();
    }

    const Theme::Metrics m = m_metrics;
    const QSize want = sizeHint();
    if (size() != want)
        resize(want);

    if (m_dragged) {
        // Once dragged, stay where the user put it, clamped to the viewport.
        move(clampToolbarPos(m_userPos, host->size(), size()));
    } else {
        const int x = (host->width() - width()) / 2;
        const int y = host->height() - m.barBottom - height() + m.shadowRoom;
        move(qMax(0, x), qMax(0, y));
    }
    raise();

    if (m_palette && m_palette->isVisible())
        positionPaletteAbove(m_penButton, m_palette);   // keep the popup glued above the bar
    if (m_eraserPalette && m_eraserPalette->isVisible())
        positionPaletteAbove(m_eraserButton, m_eraserPalette);
}

void InkToolbar::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    reposition();
}

void InkToolbar::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    // The icons are rendered for one device pixel ratio, so they have to be
    // re-drawn when the bar moves to a screen with a different scale factor.
    if (e->type() == QEvent::DevicePixelRatioChange
        || e->type() == QEvent::ScreenChangeInternal) {
        if (!qFuzzyCompare(m_iconDpr, devicePixelRatioF()))
            refreshIcons();
    }
}

#include "InkToolbar.moc"
