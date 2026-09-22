#include "DocumentTabs.h"

#include "IconPainter.h"
#include "Theme.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QtMath>

namespace {

// A press that travels further than this turns from a tap into a scroll drag.
constexpr int kDragSlop = 6;

// Close "x": two crossed lines, drawn rather than an image asset.
void drawCloseMark(QPainter &p, const QRectF &box, const QColor &color, qreal stroke)
{
    const qreal inset = box.width() * 0.30;
    const QRectF r = box.adjusted(inset, inset, -inset, -inset);
    p.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(r.topLeft(), r.bottomRight());
    p.drawLine(r.topRight(), r.bottomLeft());
}

}   // namespace

DocumentTabs::DocumentTabs(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("documentTabs"));
    setMouseTracking(true);                 // hover feedback for chips / "+"
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    // Slightly smaller than the island chrome, like the status bar.
    setFont(Theme::scaledFont(font(), 0.95, QFont::Medium));

    // All geometry derives from the current font: on a 150 % / 200 % display
    // the font metrics grow and so does every box below.
    const Theme::Metrics m = Theme::metrics(font());
    const QFontMetrics fm(font());
    m_stripPad = Theme::Space1;
    m_chipH    = qMax(int(m.touch * 0.8), fm.height() + 2 * Theme::Space1 + 4);
    m_padX     = qMax(int(Theme::Space3), m.padX - 2);
    m_gap      = qMax(int(Theme::Space2), m.gap);
    m_closeBox = qMax(m.icon, int(m.touch * 0.5));
    // The trailing action is a TEXT tag, not a "+" glyph: a plus reads as
    // "add a tab" and misleads users about what it does.
    m_addW     = qMax(m.touch, fm.horizontalAdvance(QStringLiteral("打开")) + 4 * m_padX);
    m_textMinW = int(m.touch * 1.4);
    m_textMaxW = int(m.touch * 4.5);
    m_iconBox  = qMin(qMax(m.icon, int(m_chipH * 0.55)), int(m_chipH * 0.62));

    // The leading "+" home chip: a square-ish touch target, its own pinned
    // zone, never part of the document chip list (see relayout).
    m_homeW    = qMax(m_chipH, int(m.touch * 0.9));

    // The Settings chip (gear glyph + 「设置」) is pinned between the scrolling
    // document chips and "+", so it is reachable with zero documents open. Its
    // zone must budget the strip padding, the inner padding, the glyph and the
    // label, or the label gets clipped.
    m_settingsW = 2 * m_stripPad + 2 * m_padX + m_iconBox + Theme::Space2
                  + fm.horizontalAdvance(QStringLiteral("设置"));
    m_settingsW = qMax(m_settingsW, int(m.touch * 1.6));
}

QSize DocumentTabs::sizeHint() const
{
    return QSize(m_homeW + m_gap + m_settingsW + m_addW + int(Theme::Space6 * 6),
                 m_chipH + 2 * m_stripPad);
}

QSize DocumentTabs::minimumSizeHint() const
{
    return QSize(m_homeW + m_gap + m_settingsW + m_addW + Theme::Space6,
                 m_chipH + 2 * m_stripPad);
}

void DocumentTabs::addTab(const QString &title)
{
    m_titles.append(title);
    relayout();
    update();
}

void DocumentTabs::removeTab(int index)
{
    if (index < 0 || index >= m_titles.size())
        return;

    m_titles.removeAt(index);

    // While the settings page or the home page is up no document page is
    // visible: the selection is adjusted silently so closing chips cannot pull
    // the host off that page.
    const bool overlayActive = m_settingsActive || m_homeActive;
    if (m_titles.isEmpty()) {
        const bool hadCurrent = (m_current >= 0);
        m_current = -1;
        m_hoverChip = -1;
        m_hoverClose = false;
        relayout();
        update();
        if (hadCurrent && !overlayActive)
            emit currentChanged(-1);
        return;
    }

    if (index < m_current) {
        // The same document stays active, one chip to the left.
        --m_current;
        relayout();
        update();
        if (!overlayActive)
            emit currentChanged(m_current);
    } else if (index == m_current) {
        // Activate the right neighbour (or the new last chip).
        m_current = qMin(index, int(m_titles.size()) - 1);
        ensureChipVisible(m_current);
        relayout();
        update();
        if (!overlayActive)
            emit currentChanged(m_current);
    } else {
        relayout();
        update();
    }
}

void DocumentTabs::setTabTitle(int index, const QString &title)
{
    if (index < 0 || index >= m_titles.size() || m_titles.at(index) == title)
        return;
    m_titles[index] = title;
    relayout();
    update();
}

void DocumentTabs::setCurrentIndex(int index)
{
    if (m_titles.isEmpty()) {
        if (m_current != -1) {
            m_current = -1;
            update();
            emit currentChanged(-1);
        }
        return;
    }

    index = qBound(0, index, int(m_titles.size()) - 1);
    const bool wasOverlay = m_settingsActive || m_homeActive;
    m_settingsActive = false;           // activating a document clears the gear
    m_homeActive = false;               // ... and the leading "+" highlight
    if (index == m_current && !wasOverlay) {
        update();
        return;
    }

    const bool moved = (index != m_current);
    m_current = index;
    if (moved)
        ensureChipVisible(m_current);
    relayout();
    update();
    emit currentChanged(m_current);
}

void DocumentTabs::setSettingsActive(bool on)
{
    if (m_settingsActive == on)
        return;
    m_settingsActive = on;
    update();
}

void DocumentTabs::setHomeActive(bool on)
{
    if (m_homeActive == on)
        return;
    m_homeActive = on;
    update();
}

void DocumentTabs::relayout()
{
    const QFontMetrics fm(font());

    struct Pending {
        QString title;
        QString elided;
        int     textW = 0;
        int     chipW = 0;
    };

    QVector<Pending> pending;
    pending.reserve(m_titles.size());
    int content = 2 * m_stripPad;
    for (const QString &title : m_titles) {
        Pending item;
        item.title = title;
        item.textW = qBound(m_textMinW, fm.horizontalAdvance(title), m_textMaxW);
        item.elided = fm.elidedText(title, Qt::ElideMiddle, item.textW);
        item.chipW = 2 * m_padX + item.textW + m_gap + m_closeBox;
        content += item.chipW;
        pending.append(item);
    }
    if (!pending.isEmpty())
        content += m_gap * int(pending.size() - 1);

    // The leading "+" home chip is pinned to the far LEFT; the 「打开」 chip and
    // the Settings chip are pinned to the right. Only the document chips in
    // between scroll. m_chipsLeft / m_chipsRight border that scrolling window:
    // the home chip enters the geometry ONLY here, so m_chips still holds
    // exactly one item per m_titles and every index stays a document index.
    const int homeW = qMin(m_homeW, qMax(0, width()));
    m_homeRect = QRect(0, 0, homeW, height());

    const int addW = qMin(m_addW, qMax(0, width() - m_homeRect.width()));
    m_addRect = QRect(width() - addW, 0, addW, height());

    const int settingsW = qMin(m_settingsW, qMax(0, m_addRect.left() - m_gap));
    m_settingsRect = QRect(m_addRect.left() - settingsW, 0, settingsW, height());

    m_chipsLeft = m_homeRect.right() + 1 + m_gap;
    m_chipsRight = qMax(m_chipsLeft, m_settingsRect.left() - m_gap);
    const int avail = qMax(0, m_chipsRight - m_chipsLeft);
    m_content = content;
    m_scroll = qBound(0, m_scroll, qMax(0, m_content - avail));

    m_chips.clear();
    m_chips.reserve(pending.size());
    int x = m_chipsLeft + m_stripPad - m_scroll;
    for (const Pending &item : pending) {
        Chip chip;
        chip.title = item.title;
        chip.elided = item.elided;
        chip.textW = item.textW;
        chip.rect = QRect(x, m_stripPad, item.chipW, m_chipH);
        chip.closeRect = QRect(chip.rect.right() - m_padX - m_closeBox,
                               chip.rect.top() + (m_chipH - m_closeBox) / 2,
                               m_closeBox, m_closeBox);
        m_chips.append(chip);
        x += item.chipW + m_gap;
    }
}

int DocumentTabs::maxScroll() const
{
    return qMax(0, m_content - (m_chipsRight - m_chipsLeft));
}

void DocumentTabs::setScroll(int value)
{
    const int clamped = qBound(0, value, maxScroll());
    if (clamped == m_scroll)
        return;
    m_scroll = clamped;
    relayout();
    update();
}

void DocumentTabs::ensureChipVisible(int index)
{
    if (index < 0 || index >= m_chips.size())
        return;
    const int left = m_chipsLeft + m_stripPad;
    const int right = m_chipsRight - m_stripPad;
    const QRect r = m_chips.at(index).rect;
    if (r.left() < left)
        m_scroll -= (left - r.left());
    else if (r.right() > right)
        m_scroll += (r.right() - right);
}

int DocumentTabs::chipAt(const QPoint &pos) const
{
    // The pinned home / Settings chips win over any document chip scrolled
    // under them: only the scrolling window between them is hit-testable.
    if (pos.x() < m_chipsLeft || pos.x() >= m_chipsRight)
        return -1;
    for (int i = 0; i < m_chips.size(); ++i) {
        if (m_chips.at(i).rect.contains(pos))
            return i;
    }
    return -1;
}

QRect DocumentTabs::settingsInner() const
{
    return QRect(m_settingsRect.left() + m_stripPad, m_stripPad,
                 qMax(0, m_settingsRect.width() - 2 * m_stripPad), m_chipH);
}

QRect DocumentTabs::homeInner() const
{
    return QRect(m_homeRect.left() + m_stripPad, m_stripPad,
                 qMax(0, m_homeRect.width() - 2 * m_stripPad), m_chipH);
}

void DocumentTabs::paintEvent(QPaintEvent *)
{
    const Theme::Palette &pal = Theme::light();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The strip continues the page desk, so the canvas area and the tab row
    // read as one surface; a hairline separates them.
    p.fillRect(rect(), pal.desk);
    p.setPen(QPen(pal.divider, 1.0));
    p.drawLine(QPointF(0.0, 0.5), QPointF(width(), 0.5));

    const qreal radius = qBound<qreal>(qreal(Theme::RadiusPill),
                                       m_chipH * 0.30,
                                       m_chipH / 2.0);
    const qreal markStroke = qMax<qreal>(1.6, m_chipH * 0.055);

    // --- chips (clipped to the scrolling area) ------------------------------
    p.save();
    p.setClipRect(QRect(m_chipsLeft, 0, qMax(0, m_chipsRight - m_chipsLeft), height()));
    for (int i = 0; i < m_chips.size(); ++i) {
        const Chip &chip = m_chips.at(i);
        if (chip.rect.right() < m_chipsLeft || chip.rect.left() > m_chipsRight)
            continue;

        // No document chip looks active while a non-document page (settings or
        // home) is the visible one.
        const bool active = (i == m_current) && !m_settingsActive && !m_homeActive;
        const bool hovered = (i == m_hoverChip);
        const QRectF box = QRectF(chip.rect).adjusted(0.5, 0.5, -0.5, -0.5);

        if (active) {
            // A surface card on the desk, exactly like the island chip.
            p.setPen(QPen(pal.surfaceEdge, 1.0));
            p.setBrush(pal.surface);
            p.drawRoundedRect(box, radius, radius);

            const qreal barH = qMax<qreal>(2.0, m_chipH * 0.07);
            const QRectF bar(box.left() + m_padX, box.bottom() - barH - m_stripPad * 0.5,
                             qMax<qreal>(1.0, box.width() - 2.0 * m_padX), barH);
            p.setPen(Qt::NoPen);
            p.setBrush(pal.accent);
            p.drawRoundedRect(bar, barH / 2.0, barH / 2.0);
        } else if (hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.surfaceHover);
            p.drawRoundedRect(box, radius, radius);
        }

        const QColor textColor = active ? pal.text : (hovered ? pal.text : pal.textMuted);
        p.setPen(textColor);
        p.setFont(font());
        p.drawText(QRect(chip.rect.left() + m_padX, chip.rect.top(),
                         chip.textW, chip.rect.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, chip.elided);

        // Close box: a "chipTint" disc appears under the pointer.
        const QRectF closeBox(chip.closeRect);
        if (m_hoverClose && hovered) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.chipTint);
            p.drawEllipse(closeBox.adjusted(1.0, 1.0, -1.0, -1.0));
        }
        const QColor markColor = (m_hoverClose && hovered) ? pal.text : pal.textMuted;
        drawCloseMark(p, closeBox, markColor, markStroke);
    }
    p.restore();

    // --- pinned leading "+" home chip ---------------------------------------
    const QRectF homeBox(homeInner());
    if (homeBox.width() > 4.0) {
        const QRectF box = homeBox.adjusted(0.5, 0.5, -0.5, -0.5);
        if (m_homeActive) {
            p.setPen(QPen(pal.surfaceEdge, 1.0));
            p.setBrush(pal.surface);
            p.drawRoundedRect(box, radius, radius);

            const qreal barH = qMax<qreal>(2.0, m_chipH * 0.07);
            const QRectF bar(box.left() + m_padX, box.bottom() - barH - m_stripPad * 0.5,
                             qMax<qreal>(1.0, box.width() - 2.0 * m_padX), barH);
            p.setPen(Qt::NoPen);
            p.setBrush(pal.accent);
            p.drawRoundedRect(bar, barH / 2.0, barH / 2.0);
        } else if (m_hoverHome) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.surfaceHover);
            p.drawRoundedRect(box, radius, radius);
        }

        // A plus glyph, not a document chip: "+ returns to the home page".
        const QColor ink = m_homeActive ? pal.accent
                                        : (m_hoverHome ? pal.text : pal.textMuted);
        const qreal plus = qMin<qreal>(m_iconBox, homeBox.height() * 0.42);
        const QPointF centre = homeBox.center();
        const QRectF glyphBox(centre.x() - plus / 2.0, centre.y() - plus / 2.0,
                              plus, plus);
        const qreal stroke = qMax<qreal>(1.8, plus * 0.16);
        p.setPen(QPen(ink, stroke, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(glyphBox.left(), glyphBox.center().y()),
                   QPointF(glyphBox.right(), glyphBox.center().y()));
        p.drawLine(QPointF(glyphBox.center().x(), glyphBox.top()),
                   QPointF(glyphBox.center().x(), glyphBox.bottom()));
    }

    // Hairline between the pinned home chip and the scrolling chips.
    if (m_homeRect.right() + 1 < m_chipsLeft) {
        p.setPen(QPen(pal.divider, 1.0));
        p.drawLine(QPointF(m_homeRect.right() + 0.5, m_stripPad + m_chipH * 0.18),
                   QPointF(m_homeRect.right() + 0.5,
                           height() - m_stripPad - m_chipH * 0.18));
    }

    // --- pinned Settings chip (no close "x") --------------------------------
    const QRectF sInner(settingsInner());
    if (sInner.width() > 4.0) {
        const QRectF sBox = sInner.adjusted(0.5, 0.5, -0.5, -0.5);
        if (m_settingsActive) {
            p.setPen(QPen(pal.surfaceEdge, 1.0));
            p.setBrush(pal.surface);
            p.drawRoundedRect(sBox, radius, radius);

            const qreal barH = qMax<qreal>(2.0, m_chipH * 0.07);
            const QRectF bar(sBox.left() + m_padX, sBox.bottom() - barH - m_stripPad * 0.5,
                             qMax<qreal>(1.0, sBox.width() - 2.0 * m_padX), barH);
            p.setPen(Qt::NoPen);
            p.setBrush(pal.accent);
            p.drawRoundedRect(bar, barH / 2.0, barH / 2.0);
        } else if (m_hoverSettings) {
            p.setPen(Qt::NoPen);
            p.setBrush(pal.surfaceHover);
            p.drawRoundedRect(sBox, radius, radius);
        }

        const QColor ink = m_settingsActive ? pal.accent
                                            : (m_hoverSettings ? pal.text : pal.textMuted);
        const int glyphPx = qMin(m_iconBox, int(sInner.height() * 0.62));
        const QRectF glyphBox(sInner.left() + m_padX,
                              sInner.center().y() - glyphPx / 2.0,
                              glyphPx, glyphPx);
        IconPainter::paintGlyph(p, IconPainter::Glyph::Gear, glyphBox, ink, 2.0);

        const int textX = int(sInner.left()) + m_padX + glyphPx + Theme::Space2;
        p.setPen(m_settingsActive ? pal.text : ink);
        p.setFont(font());
        p.drawText(QRect(textX, int(sInner.top()),
                         qMax(0, int(sInner.right()) - textX - m_padX),
                         int(sInner.height())),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("设置"));
    }

    // --- pinned "+" action --------------------------------------------------
    p.setPen(QPen(pal.divider, 1.0));
    p.drawLine(QPointF(m_addRect.left() + 0.5, m_stripPad + m_chipH * 0.18),
               QPointF(m_addRect.left() + 0.5, height() - m_stripPad - m_chipH * 0.18));

    const QRectF inner(m_addRect.left() + m_stripPad, m_stripPad,
                       qMax(0, m_addRect.width() - 2 * m_stripPad), m_chipH);
    if (m_hoverAdd && inner.isValid()) {
        p.setPen(Qt::NoPen);
        p.setBrush(pal.surfaceHover);
        p.drawRoundedRect(inner, radius, radius);
    }
    if (inner.width() > 4.0) {
        p.setPen(m_hoverAdd ? pal.accent : pal.textMuted);
        p.setFont(font());
        p.drawText(inner, Qt::AlignCenter, QStringLiteral("打开"));
    }
}

void DocumentTabs::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    relayout();
}

void DocumentTabs::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_pressed = true;
    m_dragging = false;
    m_pressPos = e->position().toPoint();
    m_pressScroll = m_scroll;
}

void DocumentTabs::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();

    if (m_pressed) {
        if (!m_dragging) {
            const QPoint d = pos - m_pressPos;
            if (qAbs(d.x()) > kDragSlop || qAbs(d.y()) > kDragSlop)
                m_dragging = true;
        }
        if (m_dragging) {
            setScroll(m_pressScroll - (pos.x() - m_pressPos.x()));
            return;
        }
    }

    const int chip = chipAt(pos);
    const bool close = (chip >= 0) && m_chips.at(chip).closeRect.contains(pos);
    const bool add = m_addRect.contains(pos);
    const bool settings = settingsInner().contains(pos);
    const bool home = homeInner().contains(pos);
    if (chip != m_hoverChip || close != m_hoverClose || add != m_hoverAdd
        || settings != m_hoverSettings || home != m_hoverHome) {
        m_hoverChip = chip;
        m_hoverClose = close;
        m_hoverAdd = add;
        m_hoverSettings = settings;
        m_hoverHome = home;
        update();
    }

    if (settings)
        setToolTip(QStringLiteral("设置"));
    else if (home)
        setToolTip(QStringLiteral("主页"));
    else if (chip >= 0)
        setToolTip(m_chips.at(chip).title);
    else if (add)
        setToolTip(QStringLiteral("新建标签"));
    else
        setToolTip(QString());
    setCursor((chip >= 0 || add || settings || home) ? Qt::PointingHandCursor
                                                     : Qt::ArrowCursor);
}

void DocumentTabs::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_pressed || e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    m_pressed = false;
    if (m_dragging) {
        m_dragging = false;       // the drag was a scroll, not a click
        return;
    }

    const QPoint pos = e->position().toPoint();
    if (homeInner().contains(pos)) {
        // The chip lights up through the host (MainWindow::showHomePage), which
        // also owns the page switch - identical to the Settings chip.
        emit homeRequested();
        return;
    }
    if (m_addRect.contains(pos)) {
        emit addRequested();
        return;
    }
    if (settingsInner().contains(pos)) {
        // The chip lights up immediately; the host owns the page switch (the
        // same chip also toggles the page back off).
        if (!m_settingsActive) {
            m_settingsActive = true;
            update();
        }
        emit settingsRequested();
        return;
    }
    const int chip = chipAt(pos);
    if (chip < 0)
        return;
    if (m_chips.at(chip).closeRect.contains(pos))
        emit closeRequested(chip);
    else
        setCurrentIndex(chip);
}

void DocumentTabs::wheelEvent(QWheelEvent *e)
{
    const int dy = e->angleDelta().y();
    const int dx = e->angleDelta().x();
    const int delta = (qAbs(dx) > qAbs(dy)) ? dx : dy;
    if (delta == 0) {
        e->ignore();
        return;
    }
    setScroll(m_scroll - delta / 2);
    e->accept();
}

void DocumentTabs::leaveEvent(QEvent *e)
{
    QWidget::leaveEvent(e);
    m_hoverChip = -1;
    m_hoverClose = false;
    m_hoverAdd = false;
    m_hoverSettings = false;
    m_hoverHome = false;
    update();
}
