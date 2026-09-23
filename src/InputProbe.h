#pragma once

// Per-second input/frame probe (opt-in through the diagnostics log switch).
//
// Purpose: settle "is handwriting lagging because of input sampling or because of
// frame rate?" with numbers taken from the real panel instead of impressions.
// One line per second is written to the log:
//
//   每秒：输入 63 事件（样本 63）| 重绘 58 帧 | 页面栅格化 2 | 缩放 1.00x
//
//   in     = touch/tablet events the platform actually delivered (the panel's
//            report rate after Qt's own merging) - if this is ~20 while the
//            panel promises 120 Hz, samples are still being dropped above us
//   pts    = sample points those events carried
//   paint  = viewport repaints, i.e. the frame rate the teacher actually sees
//   render = PDF page rasterisations (cache misses): the expensive part. A few
//            per second during a pinch means every zoom step re-renders pages.
//   zoom   = zoom factor at the end of the window
//
// Header-only and counter-based, so --selftest-ink can assert its arithmetic
// without a running event loop.

#include <QString>

class InputProbe
{
public:
    static InputProbe &instance()
    {
        static InputProbe probe;
        return probe;
    }

    void addEvent(int points)
    {
        ++m_events;
        m_points += points;
    }
    void addPaint() { ++m_paints; }
    void addRender() { ++m_renders; }
    void setZoom(qreal zoom) { m_zoom = zoom; }

    // Returns one formatted line once `intervalMs` has passed, then resets the
    // counters. Empty string while the interval is still running (so the caller
    // does not pay for a log write every frame).
    QString take(qint64 nowMs, qint64 intervalMs = 1000)
    {
        if (!m_started) {              // first call only starts the clock
            m_started = true;
            m_lastMs = nowMs;
            return QString();
        }
        if (nowMs - m_lastMs < intervalMs)
            return QString();

        const QString line =
            QStringLiteral("每秒：输入 %1 事件（样本 %2）| 重绘 %3 帧 | 页面栅格化 %4 | 缩放 %5x")
                .arg(m_events)
                .arg(m_points)
                .arg(m_paints)
                .arg(m_renders)
                .arg(m_zoom, 0, 'f', 2);
        m_events = 0;
        m_points = 0;
        m_paints = 0;
        m_renders = 0;
        m_lastMs = nowMs;
        return line;
    }

private:
    bool m_started = false;
    qint64 m_lastMs = 0;
    int m_events = 0;
    int m_points = 0;
    int m_paints = 0;
    int m_renders = 0;
    qreal m_zoom = 1.0;
};
