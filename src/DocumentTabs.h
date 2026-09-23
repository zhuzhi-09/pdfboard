#pragma once

#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

// Slim, docked document tab strip pinned under the page area (below the
// floating toolbar island, above the status bar).
//
// One chip per open document: the file name (middle elided when long) plus a
// close "x". The active chip is a surface card with an accent underline. Two
// non-document chips are pinned OUTSIDE the scrolling document-chip list, so
// the chip index -> document index mapping stays exact: a leading house chip
// that returns to the home page, and a trailing 「打开」 chip that opens a new
// document, with the non-closeable Settings chip (gear glyph + 「设置」) right
// before it. When the chips overflow the strip scrolls horizontally (wheel or
// drag) instead of squashing them; both pinned chips stay reachable with zero
// documents open.
//
// The whole strip is painted in one widget - no child buttons - so it stays a
// single slim row and scrolling is just an offset. Every metric derives from
// the widget font through Theme::metrics, so the strip follows 150 % / 200 %
// display scaling and the touch panel's larger hit targets.
class DocumentTabs : public QWidget
{
    Q_OBJECT
public:
    explicit DocumentTabs(QWidget *parent = nullptr);

    int count() const { return int(m_titles.size()); }
    int currentIndex() const { return m_current; }

    void addTab(const QString &title);          // appends, keeps the selection
    void removeTab(int index);                  // keeps the selection sensible
    void setTabTitle(int index, const QString &title);
    void setCurrentIndex(int index);            // emits currentChanged on a move

    // The Settings chip highlights like a chip but has no close "x"; the host
    // toggles it from outside (see MainWindow::showSettingsPage).
    void setSettingsActive(bool on);
    bool isSettingsActive() const { return m_settingsActive; }

    // The leading house (主页) chip highlights the same way while the home page
    // is the visible page.
    void setHomeActive(bool on);
    bool isHomeActive() const { return m_homeActive; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void currentChanged(int index);
    void closeRequested(int index);
    void addRequested();
    void settingsRequested();   // the Settings chip was activated
    void homeRequested();       // the leading house chip was activated

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    struct Chip {
        QString title;      // full file name (tooltip)
        QString elided;     // middle-elided text that is actually painted
        int     textW = 0;  // painted text box width
        QRect   rect;       // chip box in widget coordinates (scroll applied)
        QRect   closeRect;  // close "x" hit target
    };

    void relayout();                    // widths, elision, scroll clamp
    int  maxScroll() const;
    void setScroll(int value);
    void ensureChipVisible(int index);
    int  chipAt(const QPoint &pos) const;   // -1 when not on a chip
    QRect addRect() const { return m_addRect; }
    QRect settingsInner() const;            // painted box of the Settings chip
    QRect homeInner() const;                // painted box of the leading house chip

    QVector<Chip> m_chips;
    QStringList   m_titles;
    int m_current = -1;

    int  m_scroll = 0;
    int  m_hoverChip = -1;
    bool m_hoverClose = false;
    bool m_hoverAdd = false;
    bool m_hoverSettings = false;
    bool m_hoverHome = false;

    bool   m_pressed = false;
    bool   m_dragging = false;
    QPoint m_pressPos;
    int    m_pressScroll = 0;

    // Geometry, all derived from the widget font (see the constructor).
    int m_stripPad  = 4;
    int m_chipH     = 38;
    int m_padX      = 10;
    int m_gap       = 6;
    int m_closeBox  = 22;
    int m_addW      = 48;
    int m_textMinW  = 48;
    int m_textMaxW  = 180;
    int m_iconBox   = 24;
    int m_settingsW = 96;
    int m_homeW     = 48;
    int m_content   = 0;        // total scrolling content width (last relayout)
    int m_chipsLeft = 0;        // left edge of the scrolling chip area
    int m_chipsRight = 0;       // right edge of the scrolling chip area
    QRect m_homeRect;           // pinned house (主页) chip (leading)
    QRect m_addRect;
    QRect m_settingsRect;       // pinned zone between the chips and "+"
    bool m_settingsActive = false;
    bool m_homeActive = false;
};
