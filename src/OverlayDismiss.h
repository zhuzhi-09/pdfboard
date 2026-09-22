#pragma once

#include <QEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QTouchEvent>
#include <QWidget>

// ---------------------------------------------------------------------------
// Shared helper for the floating overlays (pen palette, page grid).
//
// Dismissal must be decided by the event's GLOBAL POSITION, not by the `watched`
// widget of an app-wide event filter: PdfCanvas accepts touch events, so a touch
// that lands on an overlay is still reported to the filter with the viewport as
// `watched`. Testing the widget chain there made the overlay close on every tap
// - including taps on the overlay itself, which made it unusable.
// ---------------------------------------------------------------------------
namespace Overlay {

// Global position of a press-like event, if it carries one.
inline bool pressGlobalPos(const QEvent *e, QPoint *out)
{
    switch (e->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        *out = static_cast<const QMouseEvent *>(e)->globalPosition().toPoint();
        return true;
    case QEvent::TouchBegin: {
        const auto *te = static_cast<const QTouchEvent *>(e);
        if (!te->points().isEmpty()) {
            *out = te->points().first().globalPosition().toPoint();
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

// True when the press happened inside `w`, or inside `alsoAllow` (typically the
// toolbar that owns the overlay).
inline bool pressInside(const QEvent *e, const QWidget *w, const QWidget *alsoAllow = nullptr)
{
    QPoint gp;
    if (!pressGlobalPos(e, &gp))
        return false;
    if (w && w->isVisible() && w->rect().contains(w->mapFromGlobal(gp)))
        return true;
    if (alsoAllow && alsoAllow->isVisible()
        && alsoAllow->rect().contains(alsoAllow->mapFromGlobal(gp)))
        return true;
    return false;
}

}   // namespace Overlay
