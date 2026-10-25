#pragma once

// Centred 3-sample average for finger input on the writing canvas.
//
// Why: a classroom IR panel reports its finger position ~100 times a second, but those
// positions carry a couple of pixels of noise (and integer quantisation at 4K), so a
// line written with a ruler still looks slightly hairy. More samples cannot be invented
// - the panel's report rate is the ceiling - but the samples it DOES send can be
// filtered.
//
// Why this filter: on quantised input, mild averaging is *more accurate* than the raw
// data (it cancels zero-mean quantisation noise), and a centred window gets that
// without displacing the path. Measured on the Windows pen pipeline (sevenpens devnotes,
// "Position Smoothing"): a centred 5-sample average had RMS error 0.130 px against
// 0.279 px raw, with 0.13 px of displacement, while an EMA of comparable smoothness
// displaced the path by 5.6 px. Three samples is the smallest window that removes the
// alternating pixel-grid jitter without eating a fast stroke.
//
// The cost is ONE sample of lookahead: the newest raw sample is held back until the
// NEXT sample arrives, so the point handed to the painter is centred on the previous
// one. At 100 Hz that is 10 ms - the same one-sample lag Krita's default Bezier
// interpolation accepts. flush() returns the held-back raw sample when the finger
// lifts, so the ink still reaches the finger.
//
// It needs no timestamps at all, which is deliberate: Qt's QEventPoint::timestamp()
// only advances on the 15.6 ms Windows timer tick, so any dt-based filter is wrong
// there (measured in the same devnotes).

#include <QPointF>
#include <QVector>
#include <QtMath>

class InkSmoother
{
public:
    // A sample farther than this from the newest one is not motion at all: the panel
    // swapped which contact it reports as the writing point, or a palm frame moved it.
    // Restarting there avoids dragging a long artificial curve across the page.
    static constexpr qreal kMaxJumpPx = 48.0;

    void reset()
    {
        m_pts.clear();
    }

    bool started() const { return !m_pts.isEmpty(); }

    // Feeds one raw sample (viewport/logical px) and returns the position to draw.
    // The first sample comes back unchanged; afterwards the result is the mean of up
    // to three raw samples ending at this one, i.e. centred one sample back.
    QPointF push(const QPointF &raw)
    {
        if (!m_pts.isEmpty()) {
            const QPointF d = raw - m_pts.last();
            if (qSqrt(QPointF::dotProduct(d, d)) > kMaxJumpPx) {
                m_pts.clear();
                m_pts.append(raw);
                return raw;
            }
        }
        m_pts.append(raw);
        while (m_pts.size() > 3)
            m_pts.removeFirst();

        if (m_pts.size() == 1)
            return raw;

        QPointF sum;
        for (const QPointF &p : m_pts)
            sum += p;
        return sum / qreal(m_pts.size());
    }

    // The newest raw sample (always the one `push` held back). Empty before any sample.
    QPointF flush() const { return m_pts.isEmpty() ? QPointF() : m_pts.last(); }

    bool hasPending() const { return !m_pts.isEmpty(); }

private:
    QVector<QPointF> m_pts;
};
