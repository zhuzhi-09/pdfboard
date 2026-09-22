#include "PageGrid.h"

#include "IconPainter.h"
#include "OverlayDismiss.h"
#include "PdfCanvas.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QtAlgorithms>
#include <QtMath>

namespace {

// Raster cache budget: whichever cap is reached first wins. 80 tiles at the
// default size is roughly 4 MB, so a 300 page document never grows past a few
// screenfuls of bitmaps.
constexpr int    kMaxThumbEntries  = 80;
constexpr qint64 kThumbBudgetBytes = 8LL * 1024 * 1024;

// Column count bounds: one column would waste the panel width, more than eight
// makes the tiles too small to recognise across a classroom.
constexpr int kMinColumns = 2;
constexpr int kMaxColumns = 8;

// Share of the page viewport height the card may occupy at most.
constexpr qreal kHeightShare = 0.55;

// The card, its hairline and the slim scroll bar, all from the Theme tokens -
// the same surface / edge / radius recipe PenPalette uses.
QString cardSheet(const Theme::Palette &c, const Theme::Metrics &m)
{
    QColor handle = c.text;
    handle.setAlpha(0x3A);
    QColor handleHover = c.text;
    handleHover.setAlpha(0x5C);
    QColor handlePressed = c.accent;
    handlePressed.setAlpha(0xB0);

    const QString sheet = QStringLiteral(
                              "QFrame#pageGridCard {"
                              " background: %1;"
                              " border: %2 solid %3;"
                              " border-radius: %4; }"
                              "QFrame#pageGridLine {"
                              " background: %5;"
                              " border: none; }"
                              "QLabel#pageGridTitle { color: %6; background: transparent; }"
                              "QLabel#pageGridCount { color: %7; background: transparent; }"
                              "QScrollArea { background: transparent; border: none; }"
                              "QScrollArea > QWidget > QWidget { background: transparent; }"
                              "QFrame#pageGridCard QScrollBar:vertical {"
                              " background: transparent;"
                              " width: %8;"
                              " margin: %9 2px %9 2px; }")
                              .arg(Theme::rgba(c.surface))
                              .arg(Theme::px(m.divider))
                              .arg(Theme::rgba(c.surfaceEdge))
                              .arg(Theme::px(qMax(int(Theme::RadiusPopover), m.radiusChip + 2)))
                              .arg(Theme::rgba(c.divider))
                              .arg(Theme::rgba(c.text))
                              .arg(Theme::rgba(c.textMuted))
                              .arg(Theme::px(Theme::Space2 + 4))
                              .arg(Theme::px(Theme::Space1));

    return sheet + QStringLiteral(
                       "QFrame#pageGridCard QScrollBar::handle:vertical {"
                       " background: %1;"
                       " min-height: %2;"
                       " border-radius: %3; }"
                       "QFrame#pageGridCard QScrollBar::handle:vertical:hover { background: %4; }"
                       "QFrame#pageGridCard QScrollBar::handle:vertical:pressed { background: %5; }"
                       "QFrame#pageGridCard QScrollBar::add-line:vertical,"
                       " QFrame#pageGridCard QScrollBar::sub-line:vertical {"
                       " height: 0; background: transparent; }"
                       "QFrame#pageGridCard QScrollBar::add-page:vertical,"
                       " QFrame#pageGridCard QScrollBar::sub-page:vertical {"
                       " background: transparent; }")
                       .arg(Theme::rgba(handle))
                       .arg(Theme::px(48))
                       .arg(Theme::px(Theme::Space1))
                       .arg(Theme::rgba(handleHover))
                       .arg(Theme::rgba(handlePressed));
}

}   // namespace

// One tile of the grid. The raster is requested from the owner inside
// paintEvent, i.e. only for the cells the viewport really exposes - that is what
// keeps opening a long document instant.
class PageGrid::PageCell : public QAbstractButton
{
public:
    PageCell(PageGrid *owner, int page, QWidget *parent)
        : QAbstractButton(parent)
        , m_owner(owner)
        , m_page(page)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover, true);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setToolTip(QStringLiteral("第 %1 页").arg(page + 1));
        setFixedSize(m_owner->m_cellSize);
    }

    int page() const { return m_page; }

protected:
    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::HoverEnter || e->type() == QEvent::HoverLeave)
            update();
        return QAbstractButton::event(e);
    }

    // Dragging a tile scrolls the grid. On a classroom panel the touch to mouse
    // translation lands here, and a flick across the tiles must never jump to a
    // random page - so a press only counts as a click while it stays put.
    void mousePressEvent(QMouseEvent *e) override
    {
        m_dragging  = false;
        m_pressPos  = e->globalPosition();
        m_pressValue = m_owner->m_scroll->verticalScrollBar()->value();
        QAbstractButton::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons().testFlag(Qt::LeftButton)) {
            const QPointF delta = e->globalPosition() - m_pressPos;
            if (!m_dragging && qAbs(delta.y()) > QApplication::startDragDistance())
                m_dragging = true;
            if (m_dragging) {
                setDown(false);
                m_owner->m_scroll->verticalScrollBar()->setValue(
                    m_pressValue - int(qRound(delta.y())));
                return;
            }
        }
        QAbstractButton::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (m_dragging) {
            m_dragging = false;
            setDown(false);
            e->accept();
            return;                 // a scroll flick is not a page jump
        }
        QAbstractButton::mouseReleaseEvent(e);
    }

    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        const bool current = (m_owner->m_currentPage == m_page);
        const int pad = m_owner->m_cellPad;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF full = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QRectF sheet = QRectF(rect()).adjusted(
            pad, pad, -pad, -(pad + m_owner->m_captionGap + m_owner->m_captionH));

        // Hover / press wash behind the whole tile: the two tones the island
        // buttons use.
        if (!current && (underMouse() || isDown())) {
            p.setPen(Qt::NoPen);
            p.setBrush(isDown() ? pal.surfacePressed : pal.surfaceHover);
            p.drawRoundedRect(full, m_owner->m_cellRadius, m_owner->m_cellRadius);
        }

        // The sheet well: a quiet inset, with an accent ring while this is the
        // page the canvas shows.
        const qreal radius = m_owner->m_sheetRadius;
        QPainterPath well;
        well.addRoundedRect(sheet, radius, radius);
        p.setPen(QPen(current ? pal.accent : pal.pageEdge,
                      current ? m_owner->m_ringW : 1.0));
        p.setBrush(current ? pal.accentSoft : pal.chipTint);
        p.drawPath(well);

        p.save();
        p.setClipPath(well);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QImage img = m_owner->thumbnailFor(m_page);
        if (!img.isNull()) {
            const qreal dpr = img.devicePixelRatio() > 0.0 ? img.devicePixelRatio() : 1.0;
            QSizeF fit(qreal(img.width()) / dpr, qreal(img.height()) / dpr);
            // Aspect fit inside the well, so mixed page sizes keep a square grid
            // instead of stretching a landscape page into a portrait tile.
            fit.scale(sheet.size(), Qt::KeepAspectRatio);
            p.drawImage(QRectF(sheet.center().x() - fit.width() / 2.0,
                               sheet.center().y() - fit.height() / 2.0,
                               fit.width(), fit.height()),
                        img);
        } else {
            // No raster available (no document): a quiet glyph, not a hole.
            const int g = qMax(12, int(sheet.width() * 0.42));
            IconPainter::paintGlyph(p, IconPainter::Glyph::Pages,
                                    QRectF(sheet.center().x() - g / 2.0,
                                           sheet.center().y() - g / 2.0, g, g),
                                    pal.textDisabled, 2.0);
        }
        p.restore();

        // Page number under the sheet; an accent pill while this is the current
        // page, so "where am I" needs no second look.
        const QString label = QString::number(m_page + 1);
        const QFont labelFont = current
            ? Theme::scaledFont(m_owner->font(), Theme::FontCaption, QFont::Bold)
            : Theme::captionFont(m_owner->font());
        const QFontMetrics fm(labelFont);
        const QRectF caption(sheet.left(), sheet.bottom() + m_owner->m_captionGap,
                             sheet.width(), m_owner->m_captionH);
        if (current) {
            const qreal pillW = qMin(caption.width(),
                                     qreal(fm.horizontalAdvance(label) + 2 * m_owner->m_metrics.chipPad));
            const QRectF pill(caption.center().x() - pillW / 2.0, caption.top(),
                              pillW, caption.height());
            p.setPen(Qt::NoPen);
            p.setBrush(pal.accent);
            p.drawRoundedRect(pill, pill.height() / 2.0, pill.height() / 2.0);
            p.setPen(pal.onAccent);
        } else {
            p.setPen(pal.textMuted);
        }
        p.setFont(labelFont);
        p.drawText(caption, Qt::AlignCenter, label);
    }

private:
    PageGrid *m_owner = nullptr;
    int       m_page  = 0;
    bool      m_dragging = false;
    QPointF   m_pressPos;
    int       m_pressValue = 0;
};

PageGrid::PageGrid(PdfCanvas *canvas, QWidget *toolbar)
    : QWidget(canvas ? canvas->viewport() : nullptr)
    , m_canvas(canvas)
    , m_toolbar(toolbar)
{
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    buildUi();
    hide();
}

PageGrid::~PageGrid() = default;

void PageGrid::buildUi()
{
    const Theme::Palette &pal = Theme::light();

    setFont(Theme::chromeFont(font()));
    setCursor(Qt::ArrowCursor);
    m_metrics = Theme::metrics(font());
    m_dpr = devicePixelRatioF();

    const Theme::Metrics m = m_metrics;
    const QFontMetrics fm(font());

    // Tile metrics. The sheet is a little over two touch targets wide, the page
    // number sits under it, and every value follows the font so 150 % / 200 %
    // display scaling needs no special case.
    m_thumbW      = int(qRound(m.touch * 2.2));
    m_cellPad     = qMax(int(Theme::Space1), m.gap);
    m_captionH    = fm.height();
    m_captionGap  = qMax(int(Theme::Space1) - 1, m.gap - 1);
    m_cellRadius  = qMax(int(Theme::RadiusButton), m.radiusButton);
    m_sheetRadius = qMax(int(Theme::RadiusPage), m.icon / 6);
    m_ringW       = qMax(2.0, qreal(m.divider) * 2.0);
    updateCellSize();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(m.shadowRoom, m.shadowRoom, m.shadowRoom, m.shadowRoom);
    outer->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("pageGridCard"));
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    m_card->setStyleSheet(cardSheet(pal, m));
    m_glow = new QGraphicsDropShadowEffect(m_card);
    m_glow->setBlurRadius(m.shadowBlur);
    m_glow->setOffset(0.0, m.shadowOffsetY);
    QColor shadow = pal.shadow;
    shadow.setAlpha(0x46);
    m_glow->setColor(shadow);
    m_card->setGraphicsEffect(m_glow);
    outer->addWidget(m_card);

    const int pad = m.chipPad + Theme::Space1;
    auto *content = new QVBoxLayout(m_card);
    content->setContentsMargins(pad, pad, pad, pad);
    content->setSpacing(m.gap);

    // Header: glyph, 「页面」 and the document's page count.
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(m.gap);

    m_glyph = new QLabel(m_card);
    m_glyph->setObjectName(QStringLiteral("pageGridGlyph"));
    m_glyph->setFixedSize(QSize(m.icon, m.icon));
    header->addWidget(m_glyph);

    m_title = new QLabel(QStringLiteral("页面"), m_card);
    m_title->setObjectName(QStringLiteral("pageGridTitle"));
    m_title->setFont(font());
    header->addWidget(m_title);
    header->addStretch(1);

    m_count = new QLabel(m_card);
    m_count->setObjectName(QStringLiteral("pageGridCount"));
    m_count->setFont(Theme::captionFont(font()));
    header->addWidget(m_count);
    content->addLayout(header);

    auto *line = new QFrame(m_card);
    line->setObjectName(QStringLiteral("pageGridLine"));
    line->setAttribute(Qt::WA_StyledBackground, true);
    line->setFrameShape(QFrame::NoFrame);
    line->setFixedHeight(m.divider);
    content->addWidget(line);

    // The tiles live in a plain widget inside a scroll area: the scroll area
    // owns the scrolling, the tiles only paint themselves.
    m_scroll = new QScrollArea(m_card);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scroll->viewport()->setAutoFillBackground(false);

    m_gridHost = new QWidget;
    m_gridHost->setAutoFillBackground(false);
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setSpacing(m.gap);
    m_scroll->setWidget(m_gridHost);

    content->addWidget(m_scroll, 0, Qt::AlignHCenter);

    refreshGlyph();

    if (m_canvas)
        connect(m_canvas, &PdfCanvas::pageChanged, this, &PageGrid::onCanvasPageChanged);
    if (qApp)
        qApp->installEventFilter(this);
}

void PageGrid::refreshGlyph()
{
    if (!m_glyph)
        return;
    const Theme::Palette &pal = Theme::light();
    const IconPainter::States states{ pal.textMuted, pal.accent, pal.textDisabled };
    const QIcon icon = IconPainter::makeIcon(IconPainter::Glyph::Pages, m_metrics.icon,
                                             states, m_dpr);
    m_glyph->setPixmap(icon.pixmap(QSize(m_metrics.icon, m_metrics.icon), m_dpr));
}

void PageGrid::applyTheme()
{
    const Theme::Palette &pal = Theme::light();
    if (m_card)
        m_card->setStyleSheet(cardSheet(pal, m_metrics));
    if (m_glow) {
        QColor shadow = pal.shadow;
        shadow.setAlpha(0x46);
        m_glow->setColor(shadow);
    }
    refreshGlyph();
    for (PageCell *cell : m_cells)
        cell->update();   // the tiles read Theme::light() themselves
    update();
}

void PageGrid::open()
{
    if (!m_canvas || m_canvas->pageCount() <= 0)
        return;

    // One probe render: the page the canvas is on is the cell the picker opens
    // on, so its raster is needed a moment later anyway. It also tells the tiles
    // the document's real aspect ratio, which keeps the grid square for anything
    // that is not A4.
    if (!m_aspectKnown) {
        const int page = qBound(0, m_canvas->currentPage(), m_canvas->pageCount() - 1);
        const QImage probe = thumbnailFor(page);
        if (!probe.isNull() && probe.width() > 0) {
            m_thumbAspect = qreal(probe.height()) / qreal(probe.width());
            m_aspectKnown = true;
        }
    }

    updateCellSize();
    applyGeometry();
    show();
    raise();
    syncCurrent(true);
}

void PageGrid::close()
{
    hide();
}

void PageGrid::invalidateThumbnails()
{
    m_thumbs.clear();
    m_thumbLru.clear();
    m_thumbBytes = 0;
}

void PageGrid::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);

    const qreal dpr = devicePixelRatioF();
    if (!qFuzzyCompare(dpr, m_dpr)) {
        m_dpr = dpr;
        refreshGlyph();
        invalidateThumbnails();     // rasters are rendered for one pixel ratio
    }

    // The scroll bar range is only final after the first layout pass, so the
    // centring is repeated once the event loop comes back around.
    QTimer::singleShot(0, this, [this] { syncCurrent(true); });
}

void PageGrid::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::DevicePixelRatioChange
        || e->type() == QEvent::ScreenChangeInternal) {
        const qreal dpr = devicePixelRatioF();
        if (!qFuzzyCompare(dpr, m_dpr)) {
            m_dpr = dpr;
            refreshGlyph();
            invalidateThumbnails();
        }
    }
}

bool PageGrid::eventFilter(QObject *watched, QEvent *e)
{
    if (!isVisible())
        return QWidget::eventFilter(watched, e);

    switch (e->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TouchBegin:
        // Decide by POSITION: the canvas accepts touch events, so a tap on the
        // grid still reaches this filter with the viewport as `watched`.
        if (!Overlay::pressInside(e, this, m_toolbar))
            close();
        break;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape) {
            close();
            return true;
        }
        break;
    case QEvent::Resize:
        if (watched == parentWidget())
            applyGeometry();        // columns follow the page viewport width
        break;
    default:
        break;
    }
    return QWidget::eventFilter(watched, e);
}

void PageGrid::onCanvasPageChanged(int page, int count)
{
    const QString path = m_canvas ? m_canvas->pdfPath() : QString();
    if (count != int(m_cells.size()) || path != m_docPath) {
        // A different document (same page count included) must never show a
        // thumbnail of the previous file.
        m_docPath = path;
        m_thumbAspect = 297.0 / 210.0;
        m_aspectKnown = false;
        invalidateThumbnails();
        refreshPages(count);
    }

    m_currentPage = page;
    // Only the exposed tiles actually repaint; the rest are clipped away.
    for (PageCell *cell : m_cells)
        cell->update();

    if (count <= 0)
        close();
}

void PageGrid::refreshPages(int count)
{
    if (count == int(m_cells.size()))
        return;

    // Take the items out first: deleting a layout item does not delete the
    // widget it wrapped, so the cells can simply be destroyed afterwards.
    while (QLayoutItem *item = m_grid->takeAt(0))
        delete item;
    qDeleteAll(m_cells);
    m_cells.clear();

    m_cells.reserve(count);
    for (int i = 0; i < count; ++i) {
        auto *cell = new PageCell(this, i, m_gridHost);
        connect(cell, &QAbstractButton::clicked, this, [this, cell] {
            if (m_canvas)
                m_canvas->goToPage(cell->page());
            close();
        });
        m_cells.append(cell);
    }

    m_columns = 0;                  // force the next re-flow
    if (m_count)
        m_count->setText(count > 0 ? QStringLiteral("共 %1 页").arg(count) : QString());
}

void PageGrid::updateCellSize()
{
    const int thumbH = qMax(1, int(qRound(qreal(m_thumbW) * m_thumbAspect)));
    const QSize cell(m_thumbW + 2 * m_cellPad,
                     thumbH + m_captionGap + m_captionH + 2 * m_cellPad);
    if (cell == m_cellSize)
        return;

    m_cellSize = cell;
    for (PageCell *c : m_cells)
        c->setFixedSize(m_cellSize);
    if (m_grid)
        m_grid->invalidate();
    if (isVisible())
        applyGeometry();
}

void PageGrid::applyGeometry()
{
    QWidget *host = parentWidget();
    if (!host || !m_scroll)
        return;

    const Theme::Metrics m = m_metrics;
    const int outer = m.shadowRoom;
    const int pad = m.chipPad + Theme::Space1;

    // Reserve the vertical scroll bar even while it is hidden: otherwise the
    // last column would end up underneath it the moment the grid overflows.
    const int barRoom = qMax(int(Theme::Space2) + 4,
                             m_scroll->verticalScrollBar()->sizeHint().width());
    const int available = qMax(m_cellSize.width(),
                               host->width() - 2 * outer - 2 * pad - barRoom);
    const int step = m_cellSize.width() + m.gap;
    const int columns = qBound(kMinColumns, (available + m.gap) / step, kMaxColumns);

    // A short document does not stretch the card: the grid hugs its tiles, so
    // four pages do not leave half a card of empty space to the right.
    const int used = m_cells.isEmpty() ? columns : qMin(columns, int(m_cells.size()));
    const int rows = m_cells.isEmpty() ? 1 : (int(m_cells.size()) + used - 1) / used;
    const int gridW = used * m_cellSize.width() + (used - 1) * m.gap;
    const int naturalH = rows * m_cellSize.height() + (rows - 1) * m.gap;

    // Card height is capped at ~55 % of the page viewport; the scroll area takes
    // whatever is left once the header and the card padding are paid for.
    const int headerH = qMax(m.icon, m_title ? m_title->sizeHint().height() : 0);
    const int chromeH = headerH + m.divider + 2 * pad + 2 * m.gap;
    const int cap = qMax(150, int(qreal(host->height()) * kHeightShare));
    const int scrollH = qMax(72, qMin(naturalH, cap - chromeH - 2 * outer));

    m_scroll->setFixedWidth(gridW);
    m_scroll->setFixedHeight(scrollH);

    layoutCells(used);

    if (QLayout *lay = layout()) {
        lay->invalidate();
        lay->activate();
    }
    resize(layout() ? layout()->sizeHint()
                    : QSize(gridW + 2 * pad + 2 * outer, scrollH + chromeH + 2 * outer));

    const int x = (host->width() - width()) / 2;
    const int y = (host->height() - height()) / 2;
    move(qBound(0, x, qMax(0, host->width() - width())),
         qBound(0, y, qMax(0, host->height() - height())));
    raise();
}

void PageGrid::layoutCells(int columns)
{
    if (!m_grid || columns == m_columns)
        return;

    // Re-flow the existing widgets for the new column count.
    while (QLayoutItem *item = m_grid->takeAt(0))
        delete item;
    m_columns = columns;
    for (int i = 0; i < m_cells.size(); ++i) {
        m_grid->addWidget(m_cells.at(i), i / columns, i % columns,
                          Qt::AlignTop | Qt::AlignHCenter);
    }
    m_grid->activate();
}

void PageGrid::syncCurrent(bool centerOnCurrent)
{
    const int page = m_canvas ? m_canvas->currentPage() : -1;
    if (page != m_currentPage) {
        m_currentPage = page;
        for (PageCell *cell : m_cells)
            cell->update();
    }

    if (!centerOnCurrent || !isVisible() || !m_scroll)
        return;
    PageCell *cell = m_cells.value(m_currentPage, nullptr);
    if (!cell)
        return;

    QScrollBar *bar = m_scroll->verticalScrollBar();
    const int centre = cell->y() + cell->height() / 2 - m_scroll->viewport()->height() / 2;
    bar->setValue(qBound(bar->minimum(), centre, bar->maximum()));
}

QImage PageGrid::thumbnailFor(int page)
{
    const auto it = m_thumbs.constFind(page);
    if (it != m_thumbs.constEnd()) {
        touchThumbLru(page);
        return it.value();
    }
    if (!m_canvas)
        return {};

    // Rendering happens here and only here: a tile calls this from its paint
    // event, so a cell that never became visible is never rasterized.
    QImage img = m_canvas->pageThumbnail(page, QSize(int(qRound(m_thumbW * m_dpr)), 0));
    if (img.isNull())
        return {};

    img.setDevicePixelRatio(m_dpr);
    m_thumbs.insert(page, img);
    m_thumbLru.append(page);
    m_thumbBytes += img.sizeInBytes();
    trimThumbnails();
    return img;
}

void PageGrid::touchThumbLru(int page)
{
    m_thumbLru.removeAll(page);
    m_thumbLru.append(page);
}

void PageGrid::trimThumbnails()
{
    // Evict least recently used first, and never the entry that was just added.
    while (m_thumbLru.size() > 1
           && (m_thumbLru.size() > kMaxThumbEntries || m_thumbBytes > kThumbBudgetBytes)) {
        const int oldest = m_thumbLru.takeFirst();
        m_thumbBytes -= m_thumbs.value(oldest).sizeInBytes();
        m_thumbs.remove(oldest);
    }
}
