#include "MainWindow.h"
#include "AppLog.h"
#include "AppSettings.h"
#include "InkToolbar.h"
#include "MemProbe.h"
#include "PdfCanvas.h"
#include "Theme.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QScreen>
#include <QElapsedTimer>
#include <QImage>
#include <QPdfDocument>
#include <QPointF>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QTranslator>
#include <QLibraryInfo>
#include <QLocale>
#include <QtGlobal>

#include <cstdio>
#include <windows.h>

// A GUI-subsystem process owns no console, so `qInfo`/`fprintf` output from
// --bench and --selftest-* would vanish. Attach to the launching terminal; this
// is a no-op when the caller redirected stdout, because then the handle is
// already valid and must not be clobbered.
static void attachConsoleForCli()
{
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE)
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;                                  // launched from Explorer: fine
    FILE *f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

// Raw stdout write + flush: qInfo() output proved unreliable here (Qt's default
// category logging can be compiled out), so the bench uses plain stdio.
static void out(const QString &s)
{
    std::fputs(qPrintable(s), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Headless self test:  pdfboard.exe --bench <file.pdf>
// Prints page count, per-page render time and process memory, then exits.
// This is the M0 evidence collector for the memory / render-time budget.
static int runBench(const QString &path)
{
    out(QStringLiteral("[bench] start"));

    QPdfDocument doc;
    const QPdfDocument::Error err = doc.load(path);
    if (err != QPdfDocument::Error::None) {
        out(QStringLiteral("[bench] load FAILED error=%1 file=%2")
                .arg(int(err)).arg(path));
        return 2;
    }

    const int pc = doc.pageCount();
    out(QStringLiteral("[bench] file: %1").arg(path));
    out(QStringLiteral("[bench] pages: %1").arg(pc));
    if (pc <= 0)
        return 3;

    {
        const MemInfo m = currentMemInfo();
        out(QStringLiteral("[bench] baseline     : WS %1 MB | Private %2 MB")
                .arg(m.workingSetMB, 0, 'f', 1)
                .arg(m.privateMB, 0, 'f', 1));
    }

    const int pagesToRender = qMin(pc, 3);
    for (int i = 0; i < pagesToRender; ++i) {
        const QSizeF ps = doc.pagePointSize(i);
        // ~1.5x page point size: a sensible "screen reading" raster target
        const QSize target(qMax(1, int(ps.width()  * 1.5)),
                           qMax(1, int(ps.height() * 1.5)));
        QElapsedTimer t;
        t.start();
        const QImage img = doc.render(i, target);
        const qint64 ms = t.elapsed();
        out(QStringLiteral("[bench] page %1: pt %2x%3 -> %4x%5 | %6 ms | %7 MB")
                .arg(i)
                .arg(ps.width(), 0, 'f', 0).arg(ps.height(), 0, 'f', 0)
                .arg(img.width()).arg(img.height())
                .arg(ms)
                .arg(double(img.sizeInBytes()) / 1048576.0, 0, 'f', 2));
    }

    {
        const MemInfo m = currentMemInfo();
        out(QStringLiteral("[bench] after render : WS %1 MB | Private %2 MB")
                .arg(m.workingSetMB, 0, 'f', 1)
                .arg(m.privateMB, 0, 'f', 1));
    }

    doc.close();
    out(QStringLiteral("[bench] done"));
    return 0;
}

// Headless ink-model self test:  pdfboard.exe --selftest-ink <file.pdf>
// Drives the ink model without any mouse input: draw -> partial erase ->
// undo -> redo -> clear. Guards the eraser/undo behaviour against regressions.
static int runInkSelfTest(const QString &path)
{
    PdfCanvas canvas;
    canvas.setAttribute(Qt::WA_DontShowOnScreen, true);
    canvas.resize(1000, 800);
    canvas.show();

    QString err;
    if (!canvas.openPdf(path, &err)) {
        out(QStringLiteral("[selftest] open FAILED: %1").arg(err));
        return 2;
    }
    out(QStringLiteral("[selftest] file: %1").arg(path));
    out(QStringLiteral("[selftest] pages: %1").arg(canvas.pageCount()));

    int failed = 0;
    auto check = [&failed](const char *what, int got, int want) {
        const bool ok = (got == want);
        if (!ok)
            ++failed;
        out(QStringLiteral("[selftest] %1: got %2 want %3 -> %4")
                .arg(QString::fromLatin1(what), -28)
                .arg(got).arg(want)
                .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
    };

    canvas.clearInk();
    check("clearInk", canvas.strokeCount(), 0);

    canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                         QColor(0xD3, 0x2F, 0x2F), 4.0);
    check("draw: strokes", canvas.strokeCount(), 1);
    check("draw: undoDepth", canvas.undoDepth(), 1);
    check("draw: redoDepth", canvas.redoDepth(), 0);

    canvas.testEraseAtNormalized(0, QPointF(0.5, 0.5));
    check("partial erase: strokes", canvas.strokeCount(), 2);
    check("partial erase: undoDepth", canvas.undoDepth(), 2);
    check("partial erase: redoDepth", canvas.redoDepth(), 0);

    canvas.undo();
    check("undo erase: strokes", canvas.strokeCount(), 1);
    check("undo erase: undoDepth", canvas.undoDepth(), 1);
    check("undo erase: redoDepth", canvas.redoDepth(), 1);

    canvas.redo();
    check("redo erase: strokes", canvas.strokeCount(), 2);

    canvas.undo();
    check("undo again: strokes", canvas.strokeCount(), 1);
    canvas.undo();
    check("undo draw: strokes", canvas.strokeCount(), 0);
    check("undo draw: undoDepth", canvas.undoDepth(), 0);

    // --- Regression: a fast drag must not skip over the stroke -------------
    // Endpoints are far apart relative to the eraser radius, mimicking the
    // sparse pointer samples you get during a quick vertical drag.
    canvas.clearInk();
    canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                         QColor(0xD3, 0x2F, 0x2F), 4.0);
    check("sweep setup: strokes", canvas.strokeCount(), 1);
    // A short, continuous sweep across the line cuts it.
    canvas.testEraseSweepNormalized(0, QPointF(0.5, 0.47), QPointF(0.5, 0.53));
    check("short sweep cuts line", canvas.strokeCount(), 2);
    out(QStringLiteral("[selftest] after short sweep: %1").arg(canvas.testStrokeSummary(0)));

    // Regression: a huge pointer jump (dropped/coalesced events) must NOT wipe
    // the span between the two samples.
    canvas.clearInk();
    canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                         QColor(0xD3, 0x2F, 0x2F), 4.0);
    canvas.testEraseSweepNormalized(0, QPointF(0.5, 0.25), QPointF(0.5, 0.75));
    check("jump guard: line kept", canvas.strokeCount(), 1);

    // Regression: erasing a SPARSELY sampled stroke (a line drawn fast) must not
    // delete a whole side. The point at the far end of the segment that leaves
    // the eraser circle used to be dropped, taking the whole tail with it.
    canvas.clearInk();
    canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                         QColor(0xD3, 0x2F, 0x2F), 4.0, 3, /*densify=*/false);
    check("sparse setup: strokes", canvas.strokeCount(), 1);
    canvas.testEraseAtNormalized(0, QPointF(0.5, 0.5));
    check("sparse cut keeps both sides", canvas.strokeCount(), 2);
    out(QStringLiteral("[selftest] after sparse cut: %1").arg(canvas.testStrokeSummary(0)));

    // Densification: the real input path interpolates so no segment is longer
    // than kInkMaxStepNorm, which keeps erasing precise for sparse input.
    canvas.clearInk();
    canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                         QColor(0xD3, 0x2F, 0x2F), 4.0, 3, /*densify=*/true);
    const int densePoints = canvas.testStrokePoints(0, 0);
    out(QStringLiteral("[selftest] densified stroke points: %1").arg(densePoints));
    check("densify: points > 100", densePoints > 100 ? 1 : 0, 1);

    // Pinch / anchored zoom must keep the content point under the anchor.
    canvas.clearInk();
    canvas.goToPage(1);
    {
        const QPointF anchor(400, 300);
        const int pageBefore = canvas.testPageAtViewportY(anchor.y());
        const qreal fracBefore = canvas.testFracAtViewportY(anchor.y());
        const qreal fracXBefore = canvas.testFracX(pageBefore, anchor.x());
        canvas.testZoomAt(anchor, 2.0);
        const int pageAfter = canvas.testPageAtViewportY(anchor.y());
        const qreal fracAfter = canvas.testFracAtViewportY(anchor.y());
        const qreal fracXAfter = canvas.testFracX(pageAfter, anchor.x());
        out(QStringLiteral("[selftest] zoom=%1 anchorPage %2->%3 frac %4->%5 fracX %6->%7")
                .arg(canvas.testZoom(), 0, 'f', 2)
                .arg(pageBefore).arg(pageAfter)
                .arg(fracBefore, 0, 'f', 3).arg(fracAfter, 0, 'f', 3)
                .arg(fracXBefore, 0, 'f', 3).arg(fracXAfter, 0, 'f', 3));
        check("zoom: zoomed in", canvas.testZoom() > 1.2 ? 1 : 0, 1);
        check("zoom: anchor page kept", pageAfter == pageBefore ? 1 : 0, 1);
        check("zoom: anchor frac kept", qAbs(fracAfter - fracBefore) < 0.03 ? 1 : 0, 1);
        check("zoom: anchor fracX kept", qAbs(fracXAfter - fracXBefore) < 0.03 ? 1 : 0, 1);
    }

    // "閫傞厤瀹藉害" must keep the focal position instead of jumping.
    {
        const QSize vs = canvas.testViewportSize();
        const QPointF centre(vs.width() / 2.0, vs.height() / 2.0);
        canvas.goToPage(1);
        canvas.testZoomAt(centre, 2.0);
        const int pageMid = canvas.testPageAtViewportY(centre.y());
        const qreal fracMid = canvas.testFracAtViewportY(centre.y());
        canvas.setFitWidth(true);
        const int pageBack = canvas.testPageAtViewportY(centre.y());
        const qreal fracBack = canvas.testFracAtViewportY(centre.y());
        out(QStringLiteral("[selftest] fitWidth: zoom=%1 page %2->%3 frac %4->%5")
                .arg(canvas.testZoom(), 0, 'f', 2)
                .arg(pageMid).arg(pageBack)
                .arg(fracMid, 0, 'f', 3).arg(fracBack, 0, 'f', 3));
        check("fitWidth: zoom is 1", qAbs(canvas.testZoom() - 1.0) < 0.001 ? 1 : 0, 1);
        check("fitWidth: page kept", pageBack == pageMid ? 1 : 0, 1);
        check("fitWidth: focus kept", qAbs(fracBack - fracMid) < 0.03 ? 1 : 0, 1);
    }

    // Ink thickness must scale with zoom: writing zoomed-in and then fitting the
    // width shrinks the strokes together with the page.
    {
        canvas.clearInk();
        canvas.goToPage(0);
        canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                             QColor(0xD3, 0x2F, 0x2F), 4.0);
        const qreal wFit = canvas.testStrokeDeviceWidth(0, 0);
        const QSize vs = canvas.testViewportSize();
        canvas.testZoomAt(QPointF(vs.width() / 2.0, vs.height() / 2.0), 2.0);
        const qreal wZoom = canvas.testStrokeDeviceWidth(0, 0);
        canvas.setFitWidth(true);
        const qreal wBack = canvas.testStrokeDeviceWidth(0, 0);
        out(QStringLiteral("[selftest] ink width px: fit=%1 zoom2x=%2 fitAgain=%3")
                .arg(wFit, 0, 'f', 2).arg(wZoom, 0, 'f', 2).arg(wBack, 0, 'f', 2));
        check("ink width scales up with zoom", wZoom > wFit * 1.9 ? 1 : 0, 1);
        check("ink width back at fit", qAbs(wBack - wFit) < 0.6 ? 1 : 0, 1);
    }

    // A sweep that never reaches the line must leave it intact.
    canvas.clearInk();
    canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                         QColor(0xD3, 0x2F, 0x2F), 4.0);
    canvas.testEraseSweepNormalized(0, QPointF(0.5, 0.02), QPointF(0.5, 0.15));
    check("sweep off-line: strokes", canvas.strokeCount(), 1);

    // Simulate the real drag: many small swept moves down through the line.
    canvas.clearInk();
    canvas.testAddStroke(0, QPointF(0.2, 0.5), QPointF(0.8, 0.5),
                         QColor(0xD3, 0x2F, 0x2F), 4.0);
    {
        QPointF prev(0.5, 0.30);
        for (int i = 1; i <= 30; ++i) {
            const QPointF cur(0.5, 0.30 + 0.40 * (qreal(i) / 30.0));
            canvas.testEraseSweepNormalized(0, prev, cur);
            prev = cur;
        }
    }
    check("drag sweep: strokes", canvas.strokeCount(), 2);
    out(QStringLiteral("[selftest] after drag sweep: %1").arg(canvas.testStrokeSummary(0)));

    // Island drag (a separate feature): InkToolbar::moveBy moves the island and
    // must be inert for ink - no stroke, no undo entry. The island moves by
    // exactly the delta while the target stays inside the viewport, and is
    // clamped otherwise. 自由移动's own pan semantics are covered right after.
    {
        InkToolbar *bar = canvas.toolbar();
        check("island drag: toolbar exists", bar ? 1 : 0, 1);
        if (bar) {
            canvas.setTool(PdfCanvas::InkTool::Move);
            check("move: tool is Move",
                  canvas.tool() == PdfCanvas::InkTool::Move ? 1 : 0, 1);

            // Anchor at the top-left clamp so the +40/+30 delta below lands far
            // away from every edge and cannot be absorbed by the clamp.
            bar->moveBy(QPoint(-100000, -100000));
            const QPoint before = bar->pos();
            const int strokesBefore = canvas.strokeCount();
            const int undoBefore = canvas.undoDepth();
            bar->moveBy(QPoint(40, 30));
            out(QStringLiteral("[selftest] island drag: toolbar %1,%2 -> %3,%4 (viewport %5x%6)")
                    .arg(before.x()).arg(before.y())
                    .arg(bar->pos().x()).arg(bar->pos().y())
                    .arg(canvas.viewport()->width()).arg(canvas.viewport()->height()));
            check("island drag: delta applied",
                  (bar->pos() - before) == QPoint(40, 30) ? 1 : 0, 1);
            check("island drag: no ink", canvas.strokeCount(), strokesBefore);
            check("island drag: no undo entry", canvas.undoDepth(), undoBefore);

            bar->moveBy(QPoint(100000, 100000));
            const QSize host = canvas.viewport()->size();
            const QRect island(bar->pos(), bar->size());
            out(QStringLiteral("[selftest] island drag: huge move -> %1,%2 island %3x%4")
                    .arg(island.x()).arg(island.y())
                    .arg(island.width()).arg(island.height()));
            check("island drag: clamped inside host",
                  (island.left() >= 0 && island.top() >= 0
                   && island.right() < host.width()
                   && island.bottom() < host.height()) ? 1 : 0, 1);

            canvas.setTool(PdfCanvas::InkTool::Pen);
            check("move: back to pen",
                  canvas.tool() == PdfCanvas::InkTool::Pen ? 1 : 0, 1);
        }
    }

    // Free move (自由移动) mode: a left-button drag pans the view on BOTH axes
    // and must stay inert for ink and for the toolbar island.
    {
        canvas.setTool(PdfCanvas::InkTool::Move);
        check("free move: tool is Move",
              canvas.tool() == PdfCanvas::InkTool::Move ? 1 : 0, 1);

        // Zoom in so both scroll bars get a usable range: at fit width the
        // horizontal range is zero and the X assertion would be vacuous.
        const QSize vs = canvas.testViewportSize();
        canvas.testZoomAt(QPointF(vs.width() / 2.0, vs.height() / 2.0), 2.0);

        QScrollBar *vb = canvas.verticalScrollBar();
        QScrollBar *hb = canvas.horizontalScrollBar();
        check("free move: v-scroll range", vb->maximum() > vb->minimum() ? 1 : 0, 1);
        check("free move: h-scroll range", hb->maximum() > hb->minimum() ? 1 : 0, 1);
        // Park both in the middle so the drag has room in either direction.
        vb->setValue((vb->minimum() + vb->maximum()) / 2);
        hb->setValue((hb->minimum() + hb->maximum()) / 2);

        const int vBefore = vb->value();
        const int hBefore = hb->value();
        const QPoint barBefore = canvas.toolbar()->pos();
        const int strokesBefore = canvas.strokeCount();
        const int undoBefore = canvas.undoDepth();

        const QPointF from(vs.width() / 2.0, vs.height() / 2.0);
        canvas.testFreeMoveDrag(from, from + QPointF(60, 40));

        out(QStringLiteral("[selftest] free move: scroll v %1->%2 h %3->%4")
                .arg(vBefore).arg(vb->value()).arg(hBefore).arg(hb->value()));
        check("free move: pan X by delta", hb->value() - hBefore, -60);
        check("free move: pan Y by delta", vb->value() - vBefore, -40);
        check("free move: no ink", canvas.strokeCount(), strokesBefore);
        check("free move: no undo entry", canvas.undoDepth(), undoBefore);
        check("free move: island unmoved",
              (canvas.toolbar()->pos() == barBefore) ? 1 : 0, 1);

        canvas.setTool(PdfCanvas::InkTool::Pen);
    }

    // The island can be dragged from anywhere on it, its buttons included: a
    // press only arms a drag, a move shorter than startDragDistance() is still a
    // tap, and once it becomes a drag the release must not click the button.
    if (InkToolbar *bar = canvas.toolbar()) {
        canvas.setTool(PdfCanvas::InkTool::Pen);
        const PdfCanvas::InkTool toolBefore = canvas.tool();

        QToolButton *eraser = nullptr;
        for (QToolButton *b : bar->findChildren<QToolButton *>()) {
            if (b->text() == QStringLiteral("橡皮"))
                eraser = b;
        }
        check("drag anywhere: eraser found", eraser ? 1 : 0, 1);

        if (eraser) {
            // Park the island away from the clamp edges: the checks above left it
            // pinned to the bottom-right corner, where nothing can move further.
            bar->moveBy(QPoint(-2000, -2000));      // clamps to (0, 0)

            const QPoint start = eraser->rect().center();
            const QPoint barBefore = bar->pos();
            const int strokesBefore = canvas.strokeCount();

            auto send = [eraser](QEvent::Type type, const QPoint &pos,
                                 Qt::MouseButton button, Qt::MouseButtons buttons) {
                QMouseEvent ev(type, QPointF(pos), QPointF(pos), button, buttons,
                               Qt::NoModifier);
                QApplication::sendEvent(eraser, &ev);
            };

            // NB: not "small"/"far" - windows.h defines both as legacy macros.
            const QPoint tinyMove(qMax(2, QApplication::startDragDistance() / 3), 0);
            send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
            send(QEvent::MouseMove, start + tinyMove, Qt::NoButton, Qt::LeftButton);
            check("drag anywhere: tap is not a drag", (bar->pos() == barBefore) ? 1 : 0, 1);

            const int beyond = QApplication::startDragDistance() + Theme::Space5;
            send(QEvent::MouseMove, start + QPoint(beyond, beyond), Qt::NoButton, Qt::LeftButton);
            check("drag anywhere: far move drags", (bar->pos() != barBefore) ? 1 : 0, 1);

            send(QEvent::MouseButtonRelease, start + QPoint(beyond, beyond),
                 Qt::LeftButton, Qt::NoButton);
            check("drag anywhere: click swallowed", (canvas.tool() == toolBefore) ? 1 : 0, 1);
            check("drag anywhere: no ink", canvas.strokeCount(), strokesBefore);
        }
    }

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Headless self test:  pdfboard.exe --selftest-log
// Locks the rule that once made the settings switch dead: PDFBOARD_LOG decides
// the startup default and the file's location, but never whether the user may
// turn logging off again.
static int runLogSelfTest()
{
    int failed = 0;
    auto check = [&failed](const char *what, int got, int want) {
        const bool ok = (got == want);
        if (!ok)
            ++failed;
        out(QStringLiteral("[selftest] %1: got %2 want %3 -> %4")
                .arg(QString::fromLatin1(what), -28)
                .arg(got).arg(want)
                .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
    };

    // Leave the machine exactly as we found it.
    const bool savedPref = AppSettings::debugLogEnabled();
    const QByteArray savedEnv = qgetenv("PDFBOARD_LOG");

    const QString tmp = QDir(QDir::tempPath())
                            .filePath(QStringLiteral("pdfboard-selftest-log.log"));
    QFile::remove(tmp);
    qputenv("PDFBOARD_LOG", tmp.toLocal8Bit());

    AppLog::applySettings();
    check("env var: startup is ON", AppLog::isEnabled() ? 1 : 0, 1);
    check("env var: path is used", AppLog::logFilePath() == tmp ? 1 : 0, 1);

    // The reported bug: this used to stay ON, so the switch never moved.
    AppLog::setEnabled(false, nullptr);
    check("switch OFF beats env", AppLog::isEnabled() ? 1 : 0, 0);
    check("switch OFF is stored", AppSettings::debugLogEnabled() ? 1 : 0, 0);

    const qint64 before = QFileInfo(tmp).size();
    AppLog::write(QStringLiteral("selftest"), QStringLiteral("must not be written"));
    check("switch OFF: no writes", QFileInfo(tmp).size() > before ? 1 : 0, 0);

    AppLog::setEnabled(true, nullptr);
    check("switch ON resumes", AppLog::isEnabled() ? 1 : 0, 1);
    check("switch ON: writes again", QFileInfo(tmp).size() > before ? 1 : 0, 1);

    if (savedEnv.isEmpty())
        qunsetenv("PDFBOARD_LOG");
    else
        qputenv("PDFBOARD_LOG", savedEnv);
    AppSettings::setDebugLogEnabled(savedPref, nullptr);
    AppLog::applySettings();
    check("preference restored", AppSettings::debugLogEnabled() == savedPref ? 1 : 0, 1);

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Headless UI-geometry self test:  pdfboard.exe --selftest-ui
// Pure maths, no widgets: the island metrics must shrink to 80% (font and
// floors together), and the drag clamp must keep the bar inside the viewport
// even when the host is smaller than the island.
static int runUiSelfTest()
{
    int failed = 0;
    auto check = [&failed](const char *what, int got, int want) {
        const bool ok = (got == want);
        if (!ok)
            ++failed;
        out(QStringLiteral("[selftest] %1: got %2 want %3 -> %4")
                .arg(QString::fromLatin1(what), -28)
                .arg(got).arg(want)
                .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
    };

    const QFont f = QApplication::font();
    const Theme::Metrics full = Theme::metrics(f, 1.0);
    const Theme::Metrics island = Theme::metrics(f, Theme::IslandScale);

    check("island scale: touch shrinks", full.touch > island.touch ? 1 : 0, 1);
    check("island scale: touch ~80%",
          qAbs(island.touch * 100 / full.touch - 80) <= 3 ? 1 : 0, 1);
    check("clamp: negative -> 0,0",
          (InkToolbar::clampToolbarPos(QPoint(-50, -50), QSize(800, 600), QSize(100, 50))
               == QPoint(0, 0)) ? 1 : 0, 1);
    check("clamp: overflow -> max",
          (InkToolbar::clampToolbarPos(QPoint(900, 900), QSize(800, 600), QSize(100, 50))
               == QPoint(700, 550)) ? 1 : 0, 1);
    check("clamp: host smaller -> 0,0",
          (InkToolbar::clampToolbarPos(QPoint(50, 50), QSize(800, 600), QSize(900, 700))
               == QPoint(0, 0)) ? 1 : 0, 1);

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Headless theme self test:  pdfboard.exe --selftest-theme
// Locks the runtime-switchable appearance: the stored preference round-trips,
// out-of-range registry values fall back to 系统, the dark palette is really
// darker than the light one, and 系统 resolves to the live OS scheme.
static int runThemeSelfTest()
{
    int failed = 0;
    auto check = [&failed](const char *what, int got, int want) {
        const bool ok = (got == want);
        if (!ok)
            ++failed;
        out(QStringLiteral("[selftest] %1: got %2 want %3 -> %4")
                .arg(QString::fromLatin1(what), -28)
                .arg(got).arg(want)
                .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
    };

    // Leave the machine exactly as we found it: the registry value and the
    // in-process theme mode are both snapshotted and restored.
    QSettings raw(QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard"),
                  QSettings::NativeFormat);
    const QVariant savedValue = raw.value(QStringLiteral("ThemeMode"));
    const Theme::Mode savedMode = Theme::mode();

    raw.remove(QStringLiteral("ThemeMode"));
    raw.sync();
    check("default mode is System", AppSettings::themeMode(), 0);

    raw.setValue(QStringLiteral("ThemeMode"), 7);
    raw.sync();
    check("out-of-range reads as System", AppSettings::themeMode(), 0);

    check("set 1 succeeds", AppSettings::setThemeMode(1, nullptr) ? 1 : 0, 1);
    check("stored 1 reads back", AppSettings::themeMode(), 1);
    check("set 2 succeeds", AppSettings::setThemeMode(2, nullptr) ? 1 : 0, 1);
    check("stored 2 reads back", AppSettings::themeMode(), 2);

    // The dark palette must really invert the two ends of the scale.
    const Theme::Palette lightPal = Theme::makeLightPalette();
    Theme::setMode(Theme::Mode::Dark);
    check("dark mode is applied", Theme::mode() == Theme::Mode::Dark ? 1 : 0, 1);
    check("dark: desk is darker",
          qGray(Theme::light().desk.rgb()) < qGray(lightPal.desk.rgb()) ? 1 : 0, 1);
    check("dark: text is lighter",
          qGray(Theme::light().text.rgb()) > qGray(lightPal.text.rgb()) ? 1 : 0, 1);

    // 系统 resolves to whatever this machine asks for, and the palette matches
    // the matching factory - honest on both light and dark Windows.
    Theme::setMode(Theme::Mode::System);
    check("system resolves to OS scheme",
          Theme::resolvedMode() == Theme::systemMode() ? 1 : 0, 1);
    const Theme::Palette expected = (Theme::systemMode() == Theme::Mode::Dark)
                                        ? Theme::makeDarkPalette()
                                        : Theme::makeLightPalette();
    check("system: palette matches factory",
          Theme::light().desk == expected.desk ? 1 : 0, 1);

    // The settings-page click path: persist the value, then re-apply through
    // applyStoredMode. Both directions must actually switch - the palette used to
    // be recomputed from the mode that was set at startup, so a click did nothing.
    AppSettings::setThemeMode(1, nullptr);
    Theme::applyStoredMode(AppSettings::themeMode());
    check("click path: light applies",
          qGray(Theme::light().desk.rgb()) > qGray(Theme::makeDarkPalette().desk.rgb()) ? 1 : 0, 1);
    AppSettings::setThemeMode(2, nullptr);
    Theme::applyStoredMode(AppSettings::themeMode());
    check("click path: dark applies",
          qGray(Theme::light().desk.rgb()) < qGray(Theme::makeLightPalette().desk.rgb()) ? 1 : 0, 1);

    if (savedValue.isValid())
        raw.setValue(QStringLiteral("ThemeMode"), savedValue);
    else
        raw.remove(QStringLiteral("ThemeMode"));
    raw.sync();
    Theme::setMode(savedMode);

    const bool restored = savedValue.isValid()
        ? (raw.value(QStringLiteral("ThemeMode")) == savedValue)
        : !raw.contains(QStringLiteral("ThemeMode"));
    check("stored preference restored", restored ? 1 : 0, 1);
    check("theme mode restored", Theme::mode() == savedMode ? 1 : 0, 1);

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("PDFBoard"));

    // Qt's own dialogs (message boxes) should follow the system language.
    {
        auto *translator = new QTranslator(&app);
        if (translator->load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                             QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
            app.installTranslator(translator);
    }

    const QStringList args = app.arguments();

    // Opt-in diagnostics: records the startup environment, which is the first
    // thing you want when a scaling or rendering complaint comes in.
    AppLog::applySettings();

    // The appearance preference must be in place before any widget is built:
    // the chrome bakes its colours at construction.
    Theme::setMode(static_cast<Theme::Mode>(AppSettings::themeMode()));

    AppLog::write(QStringLiteral("app"),
                  QStringLiteral("外观：设置 %1 → 解析 %2（系统 %3）pal=%4 desk=%5,%6,%7")
                      .arg(AppSettings::themeMode())
                      .arg(int(Theme::resolvedMode()))
                      .arg(int(Theme::systemMode()))
                      .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(&Theme::light())))
                      .arg(Theme::light().desk.red())
                      .arg(Theme::light().desk.green())
                      .arg(Theme::light().desk.blue()));

    AppLog::write(QStringLiteral("app"),
                  QStringLiteral("启动：Qt %1 | 程序 %2 | 参数 [%3]")
                      .arg(QString::fromLatin1(qVersion()),
                           QCoreApplication::applicationFilePath(),
                           args.mid(1).join(QLatin1Char(' '))));
    if (QScreen *scr = QGuiApplication::primaryScreen())
        AppLog::write(QStringLiteral("app"),
                      QStringLiteral("屏幕：%1×%2 @DPR %3")
                          .arg(scr->geometry().width())
                          .arg(scr->geometry().height())
                          .arg(scr->devicePixelRatio()));
    const int benchIdx = args.indexOf(QStringLiteral("--bench"));
    const int stIdx = args.indexOf(QStringLiteral("--selftest-ink"));
    const int logIdx = args.indexOf(QStringLiteral("--selftest-log"));
    const int uiIdx = args.indexOf(QStringLiteral("--selftest-ui"));
    const int themeIdx = args.indexOf(QStringLiteral("--selftest-theme"));

    // The shipping build is a GUI executable (no console window when the user
    // double-clicks it). The console-based modes still need their output, so
    // attach to the launching terminal - but only when stdout was NOT already
    // redirected, otherwise we would clobber the caller's capture.
    if ((benchIdx >= 0 && benchIdx + 1 < args.size())
        || (stIdx >= 0 && stIdx + 1 < args.size())
        || logIdx >= 0
        || uiIdx >= 0
        || themeIdx >= 0) {
        attachConsoleForCli();
    }

    if (benchIdx >= 0 && benchIdx + 1 < args.size())
        return runBench(args.at(benchIdx + 1));

    if (stIdx >= 0 && stIdx + 1 < args.size())
        return runInkSelfTest(args.at(stIdx + 1));

    if (logIdx >= 0)
        return runLogSelfTest();

    if (uiIdx >= 0)
        return runUiSelfTest();

    if (themeIdx >= 0)
        return runThemeSelfTest();

    MainWindow w;
    w.show();

    // Optional: open a document passed on the command line - either a plain PDF
    // or a .dpz annotation bundle (this is also the "double-click to open" path
    // used by the file association).
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a.startsWith(QLatin1Char('-')))
            continue;
        if (a.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
            || a.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive)) {
            w.openPath(a);
            break;
        }
    }

    return app.exec();
}
