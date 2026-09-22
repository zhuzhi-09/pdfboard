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

QString paletteSheet(const Theme::Palette &c, const Theme::Metrics &m)
{
    return QStringLiteral(
               "QFrame#penPalette {"
               " background: %1;"
               " border: %2 solid %3;"
               " border-radius: %4; }"
               "QLabel#paletteCaption { color: %5; background: transparent; }"
               "QFrame#penPalette QToolButton {"
               " color: %6;"
               " background: transparent;"
               " border: none;"
               " border-radius: %7;"
               " padding: %8 %9 %10 %9; }"
               "QFrame#penPalette QToolButton:hover { background: %11; }"
               "QFrame#penPalette QToolButton:pressed { background: %12; }"
               "QFrame#penPalette QToolButton:checked {"
               " background: %13; color: %14; }")
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
        const QFontMetrics fm(font());
        const int s = qMax(40, fm.height() * 2 + 10);
        return QSize(s, s);
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

// Pen colour + width palette: a child overlay above the toolbar (never a
// top-level popup, see the class comment below).
class PenPalette : public QWidget
{
    Q_OBJECT
public:
    explicit PenPalette(QWidget *parent = nullptr);

    void syncSelection(const QColor &color, qreal width);

signals:
    void colorPicked(const QColor &color);
    void widthPicked(qreal width);

protected:
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;   // close on outside click

private:
    void buildUi();
    void refreshWidthIcons();

    QWidget                *m_toolbar = nullptr;   // clicking here must not auto-hide
    QColor                 m_color{ 0xD3, 0x2F, 0x2F };
    QVector<ColorSwatch *> m_swatches;
    QVector<QToolButton *> m_widthButtons;
};

// The palette is a plain CHILD overlay, not a top-level popup: on Windows a
// frameless window gets a square native (DWM) shadow around its bounds, which
// looked broken next to the rounded card. As a child it is composited by Qt, so
// only the card and its own soft shadow are visible.
PenPalette::PenPalette(QWidget *toolbar)
    : QWidget(toolbar ? toolbar->parentWidget() : nullptr)
    , m_toolbar(toolbar)
{
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    if (qApp)
        qApp->installEventFilter(this);   // dismiss on a click outside
    buildUi();
}

void PenPalette::buildUi()
{
    const Theme::Palette &pal = Theme::light();

    setFont(Theme::chromeFont(font()));
    const Theme::Metrics m = Theme::metrics(font());

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(m.shadowRoom, m.shadowRoom, m.shadowRoom, m.shadowRoom);
    outer->setSpacing(0);

    auto *chip = new QFrame(this);
    chip->setObjectName(QStringLiteral("penPalette"));
    chip->setAttribute(Qt::WA_StyledBackground, true);
    chip->setStyleSheet(paletteSheet(pal, m));
    auto *glow = new QGraphicsDropShadowEffect(chip);
    glow->setBlurRadius(m.shadowBlur);
    glow->setOffset(0.0, m.shadowOffsetY);
    QColor shadow = pal.shadow;
    shadow.setAlpha(0x46);
    glow->setColor(shadow);
    chip->setGraphicsEffect(glow);
    outer->addWidget(chip);

    auto *content = new QVBoxLayout(chip);
    content->setContentsMargins(m.chipPad + Theme::Space1, m.chipPad + Theme::Space1,
                                m.chipPad + Theme::Space1, m.chipPad + Theme::Space1);
    content->setSpacing(m.gap);

    const QFont captionFont = Theme::captionFont(font());
    auto addCaption = [&](const QString &text) {
        auto *caption = new QLabel(text, chip);
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
        auto *swatch = new ColorSwatch(colors[i], chip);
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
        auto *button = new QToolButton(chip);
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
    const Theme::Metrics m = Theme::metrics(font());
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

void PenPalette::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    refreshWidthIcons();   // pick up the current screen's device pixel ratio
}

bool PenPalette::eventFilter(QObject *watched, QEvent *e)
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

InkToolbar::InkToolbar(PdfCanvas *canvas)
    : QWidget(canvas ? canvas->viewport() : nullptr)
    , m_canvas(canvas)
{
    buildUi();

    if (m_canvas) {
        connect(m_canvas, &PdfCanvas::toolChanged,      this, &InkToolbar::syncFromCanvas);
        connect(m_canvas, &PdfCanvas::penChanged,       this, &InkToolbar::syncFromCanvas);
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

    // Slightly larger, touch friendly type; every metric below derives from it.
    setFont(Theme::chromeFont(font()));
    setCursor(Qt::ArrowCursor);
    m_metrics = Theme::metrics(font());
    const Theme::Metrics m = m_metrics;

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(m.shadowRoom, m.shadowRoom, m.shadowRoom, m.shadowRoom);
    outer->setSpacing(0);

    m_chip = new QFrame(this);
    m_chip->setObjectName(QStringLiteral("inkChip"));
    m_chip->setAttribute(Qt::WA_StyledBackground, true);
    m_chip->setStyleSheet(chipSheet(pal, m));
    auto *glow = new QGraphicsDropShadowEffect(m_chip);
    glow->setBlurRadius(m.shadowBlur);
    glow->setOffset(0.0, m.shadowOffsetY);
    QColor shadow = pal.shadow;
    shadow.setAlpha(0x46);
    glow->setColor(shadow);
    m_chip->setGraphicsEffect(glow);
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
    auto *toolGroup = new QButtonGroup(this);
    toolGroup->setExclusive(true);
    toolGroup->addButton(m_penButton);
    toolGroup->addButton(m_eraserButton);

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
        if (m_palette)
            m_palette->hide();
        dismissPageGrid();
        if (m_canvas)
            m_canvas->setTool(PdfCanvas::InkTool::Eraser);
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
        if (m_palette)
            m_palette->hide();
        emit saveRequested();
    });
    connect(m_saveAsButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        if (m_palette)
            m_palette->hide();
        emit saveAsRequested();
    });
    connect(m_settingsButton, &QToolButton::clicked, this, [this] {
        dismissPageGrid();
        if (m_palette)
            m_palette->hide();
        emit settingsRequested();
    });
    // The chevron IS the collapse / expand affordance: a direct click toggles
    // the island, there is no intermediate menu any more.
    connect(m_moreButton, &QToolButton::clicked, this, &InkToolbar::toggleCollapsed);

    m_palette = new PenPalette(this);
    connect(m_palette, &PenPalette::colorPicked, this, &InkToolbar::onPenColorPicked);
    connect(m_palette, &PenPalette::widthPicked, this, &InkToolbar::onPenWidthPicked);
    // Child overlays are shown together with their parent unless they were
    // explicitly hidden once, so hide them here or they pop up on startup.
    m_palette->hide();

    // The page-thumbnail picker is another viewport child overlay, owned by the
    // canvas like the palette: the "n / N" chip toggles it.
    m_pageGrid = new PageGrid(m_canvas, this);
    m_pageGrid->hide();

    refreshIcons();
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
        const QColor badge = (glyph == IconPainter::Glyph::Pen) ? pen : QColor();
        it.key()->setIcon(IconPainter::makeIcon(glyph, m_metrics.icon, states, dpr, 1.0, badge));
    }

    m_iconPenColor = pen;
    m_iconDpr = dpr;
}

void InkToolbar::syncFromCanvas()
{
    if (!m_canvas)
        return;

    const bool penTool = (m_canvas->tool() == PdfCanvas::InkTool::Pen);
    if (m_penButton->isChecked() != penTool)
        m_penButton->setChecked(penTool);
    if (m_eraserButton->isChecked() == penTool)
        m_eraserButton->setChecked(!penTool);

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
}

void InkToolbar::showPenPalette()
{
    if (!m_palette)
        return;
    if (m_collapsed)
        setCollapsed(false);
    // Only one card floats at a time.
    dismissPageGrid();
    if (m_palette->isVisible()) {
        m_palette->hide();
        return;
    }
    if (m_canvas)
        m_palette->syncSelection(m_canvas->penColor(), m_canvas->penWidth());
    positionPalette();
    m_palette->show();
    m_palette->raise();
}

void InkToolbar::positionPalette()
{
    if (!m_palette)
        return;

    const Theme::Metrics m = m_metrics;
    m_palette->adjustSize();
    const QSize size = m_palette->size();

    // Anchored above the pen button, kept inside the screen it is shown on.
    const QPoint anchor = m_penButton->mapToGlobal(QPoint(m_penButton->width() / 2, 0));
    int x = anchor.x() - size.width() / 2;
    int y = anchor.y() - size.height() - m.gap;

    // Keep it inside the page area it belongs to (the palette is a child of
    // that widget, so its position is parent-relative).
    QWidget *host = m_palette->parentWidget();
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
        m_palette->move(host->mapFromGlobal(QPoint(x, y)));
    else
        m_palette->move(x, y);
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
    if (m_palette)
        m_palette->hide();
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
    if (on && m_palette)
        m_palette->hide();
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

    const int x = (host->width() - width()) / 2;
    const int y = host->height() - m.barBottom - height() + m.shadowRoom;
    move(qMax(0, x), qMax(0, y));
    raise();

    if (m_palette && m_palette->isVisible())
        positionPalette();   // keep the popup glued above the bar
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
