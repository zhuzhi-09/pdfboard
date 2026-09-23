#pragma once

#include <QWidget>

// Word-like zoom control for the status bar: [−] ───●─── [+]  120%
//
// The zoom value is the canvas' own baseline: 1.0 == 适配宽度 (fit width), the
// range is 0.25 .. 4.0 (logarithmic on the slider). Clicking the percentage label
// goes back to 适配宽度.
//
// The slider<->zoom mapping lives in this header (ZoomBarMath) because it is a
// pure function that the self test asserts - keep the two directions consistent,
// and use them in the widget instead of duplicating the maths.
namespace ZoomBarMath {
constexpr qreal kMinZoom  = 0.25;
constexpr qreal kMaxZoom  = 4.0;    // 16x the minimum: 250 slider units == one doubling
constexpr int   kSliderMax = 1000;

// value 0 -> kMinZoom, value kSliderMax -> kMaxZoom, logarithmic in between
qreal zoomForSlider(int value);
// inverse of zoomForSlider, clamped to the slider range
int   sliderForZoom(qreal zoom);
}   // namespace ZoomBarMath

class QSlider;
class QToolButton;

class ZoomBar : public QWidget
{
    Q_OBJECT
public:
    explicit ZoomBar(QWidget *parent = nullptr);

    // Current canvas zoom (1.0 == 适配宽度). Updates slider + percentage, and does
    // NOT emit anything (no feedback loop).
    void setZoom(qreal zoom);
    // Re-apply the theme tokens (called on a light/dark switch).
    void refreshTheme();

signals:
    void zoomRequested(qreal zoom);   // slider dragged or −/+ clicked
    void fitWidthRequested();         // the percentage label was clicked

private:
    void syncLabel();

    QToolButton *m_minus   = nullptr;   // step out by 1.25x
    QSlider     *m_slider  = nullptr;
    QToolButton *m_plus    = nullptr;   // step in by 1.25x
    QToolButton *m_percent = nullptr;   // the clickable "120%" pill

    qreal m_zoom = 1.0;         // zoom the bar currently shows (1.0 == 适配宽度)
    int   m_reported = -1;      // last slider value reported as user input
    bool  m_syncing = false;    // true while setZoom() writes the slider itself
};
