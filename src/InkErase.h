#pragma once

// Adaptive eraser size from the finger's contact area.
//
// A pure-touch panel has no pressure and no separate eraser: the contact itself is
// the size signal. Seewo Whiteboard 5's "adaptive" eraser works this way - a wider
// contact wipes a wider swath. The manually selected step stays a FLOOR: choosing
// 粗 must never be shrunk by a small fingertip, while a palm/fist contact is allowed
// to grow the wipe. Returns `selectedPx` unchanged when the panel reports no area.

#include <QtGlobal>

inline qreal adaptiveEraseRadius(qreal selectedPx, qreal contactDiameterPx,
                                 qreal minPx, qreal maxPx)
{
    if (contactDiameterPx <= 0.0)
        return selectedPx;
    const qreal fromContact = contactDiameterPx * 0.5 + 4.0;   // contact radius + margin
    return qBound(minPx, qMax(selectedPx, fromContact), maxPx);
}
