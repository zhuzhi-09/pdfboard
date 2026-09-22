#include "MainWindow.h"
#include "AnnotationBundle.h"
#include "AppLog.h"
#include "AppSettings.h"
#include "HomePage.h"
#include "InkToolbar.h"
#include "MemProbe.h"
#include "PdfCanvas.h"
#include "Theme.h"
#include "WordConvert.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMouseEvent>
#include <QScreen>
#include <QElapsedTimer>
#include <QImage>
#include <QPdfDocument>
#include <QPointF>
#include <QScrollBar>
#include <QSettings>
#include <QSharedPointer>
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

// Headless home-page self test:  pdfboard.exe --selftest-home
// Pure greeting / quote-filter helpers plus the recent-files registry
// round-trip. No network calls, and the real machine state (the env var and
// the stored recent list) is snapshotted and restored.
static int runHomeSelfTest()
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

    // --- greeting buckets ---------------------------------------------------
    check("greeting 2h is late night",
          HomePage::greetingForHour(2, QStringLiteral("老师"))
                  == QStringLiteral("夜深了，老师，该睡了") ? 1 : 0, 1);
    check("greeting 4h is late night",
          HomePage::greetingForHour(4, QStringLiteral("老师"))
                  == QStringLiteral("夜深了，老师，该睡了") ? 1 : 0, 1);
    check("greeting 5h is morning",
          HomePage::greetingForHour(5, QStringLiteral("老师"))
                  == QStringLiteral("早上好，老师，今天看些什么？") ? 1 : 0, 1);
    check("greeting 6h is morning",
          HomePage::greetingForHour(6, QStringLiteral("老师"))
                  == QStringLiteral("早上好，老师，今天看些什么？") ? 1 : 0, 1);
    check("greeting 12h is noon",
          HomePage::greetingForHour(12, QStringLiteral("老师"))
                  == QStringLiteral("中午好，老师，今天看些什么？") ? 1 : 0, 1);
    check("greeting 15h is afternoon",
          HomePage::greetingForHour(15, QStringLiteral("老师"))
                  == QStringLiteral("下午好，老师，今天看些什么？") ? 1 : 0, 1);
    check("greeting 21h is evening",
          HomePage::greetingForHour(21, QStringLiteral("老师"))
                  == QStringLiteral("晚上好，老师，今天看些什么？") ? 1 : 0, 1);

    // %USERNAME% is used when set, 老师 when empty.
    const QByteArray savedUser = qgetenv("USERNAME");
    qputenv("USERNAME", QByteArray());
    check("empty USERNAME -> 老师",
          HomePage::userName() == QStringLiteral("老师") ? 1 : 0, 1);
    if (savedUser.isEmpty())
        qunsetenv("USERNAME");
    else
        qputenv("USERNAME", savedUser);
    check("USERNAME restored", qgetenv("USERNAME") == savedUser ? 1 : 0, 1);

    // --- classroom quote filter ---------------------------------------------
    check("filter: classical line ok",
          HomePage::isQuoteAcceptable(
              QStringLiteral("书山有路勤为径，学海无涯苦作舟")) ? 1 : 0, 1);
    check("filter: >60 chars rejected",
          HomePage::isQuoteAcceptable(QString(61, QChar(0x597D))) ? 1 : 0, 0);
    check("filter: http rejected",
          HomePage::isQuoteAcceptable(QStringLiteral("详见 http://example.com")) ? 1 : 0, 0);
    check("filter: blocked word rejected",
          HomePage::isQuoteAcceptable(QStringLiteral("某某赌博平台欢迎你")) ? 1 : 0, 0);
    // 死 occurs in perfectly appropriate classical poetry: no single-character
    // filtering may reject it.
    check("filter: 死 is not blocked",
          HomePage::isQuoteAcceptable(
              QStringLiteral("人生自古谁无死，留取丹心照汗青")) ? 1 : 0, 1);

    // --- recent files (real registry: snapshot, test, restore) --------------
    const QStringList savedRecent = AppSettings::recentFiles();

    AppSettings::clearRecentFiles(nullptr);
    check("recent: clear empties",
          int(AppSettings::recentFiles().size()), 0);

    const QString a = QStringLiteral("D:\\dev\\selftest-home\\A.pdf");
    const QString b = QStringLiteral("D:\\dev\\selftest-home\\B.pdf");
    AppSettings::addRecentFile(a, nullptr);
    AppSettings::addRecentFile(b, nullptr);
    AppSettings::addRecentFile(a, nullptr);          // A,B,A -> [A,B]
    check("recent: dedupe keeps 2",
          int(AppSettings::recentFiles().size()), 2);
    check("recent: A moved to front",
          AppSettings::recentFiles().value(0) == a ? 1 : 0, 1);
    check("recent: B stays second",
          AppSettings::recentFiles().value(1) == b ? 1 : 0, 1);

    // Windows paths compare case-insensitively.
    const QString aUpper = QStringLiteral("d:\\DEV\\SELFTEST-HOME\\a.PDF");
    AppSettings::addRecentFile(aUpper, nullptr);
    check("recent: case-insensitive dedupe",
          int(AppSettings::recentFiles().size()), 2);
    check("recent: new spelling wins",
          AppSettings::recentFiles().value(0) == aUpper ? 1 : 0, 1);

    for (int i = 1; i <= 10; ++i) {
        AppSettings::addRecentFile(
            QStringLiteral("D:\\dev\\selftest-home\\P%1.pdf").arg(i), nullptr);
    }
    check("recent: capped at 8", int(AppSettings::recentFiles().size()), 8);
    check("recent: newest first",
          AppSettings::recentFiles().value(0)
                  == QStringLiteral("D:\\dev\\selftest-home\\P10.pdf") ? 1 : 0, 1);
    check("recent: oldest evicted",
          AppSettings::recentFiles().contains(
              QStringLiteral("D:\\dev\\selftest-home\\P1.pdf")) ? 0 : 1, 1);

    const QString p10 = QStringLiteral("D:\\dev\\selftest-home\\P10.pdf");
    check("recent: remove succeeds",
          AppSettings::removeRecentFile(p10, nullptr) ? 1 : 0, 1);
    check("recent: removed entry gone",
          AppSettings::recentFiles().contains(p10) ? 0 : 1, 1);
    check("recent: size after remove",
          int(AppSettings::recentFiles().size()), 7);

    // Put the machine back exactly as we found it (an empty list stays empty).
    AppSettings::clearRecentFiles(nullptr);
    for (qsizetype i = savedRecent.size() - 1; i >= 0; --i)
        AppSettings::addRecentFile(savedRecent.at(i), nullptr);
    check("recent: stored list restored",
          AppSettings::recentFiles() == savedRecent ? 1 : 0, 1);

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Headless Word-conversion self test:  pdfboard.exe --selftest-docx
// Pure pieces of the .docx path (extension filter, cache key) plus the export
// failure paths. No Word/WPS is needed and no automation is ever started, so
// this passes on every machine and in CI; when a converter IS installed the
// failure checks still prove nothing is launched and nothing is left behind.
static int runDocxSelfTest()
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

    // --- extension filter ---------------------------------------------------
    check("isWordDoc: .docx",
          int(WordConvert::isWordDoc(QStringLiteral("D:\\课件\\第1课.docx"))), 1);
    check("isWordDoc: .doc",
          int(WordConvert::isWordDoc(QStringLiteral("D:\\课件\\第1课.doc"))), 1);
    check("isWordDoc: .DOCX",
          int(WordConvert::isWordDoc(QStringLiteral("D:\\课件\\第1课.DOCX"))), 1);
    check("isWordDoc: .pdf rejected",
          int(WordConvert::isWordDoc(QStringLiteral("D:\\课件\\第1课.pdf"))), 0);
    check("isWordDoc: .dpz rejected",
          int(WordConvert::isWordDoc(QStringLiteral("D:\\课件\\第1课.dpz"))), 0);
    check("isWordDoc: .txt rejected",
          int(WordConvert::isWordDoc(QStringLiteral("D:\\课件\\第1课.txt"))), 0);
    check("isWordDoc: no extension",
          int(WordConvert::isWordDoc(QStringLiteral("D:\\课件\\第一课"))), 0);

    // --- cache key ----------------------------------------------------------
    const QString src = QStringLiteral("D:\\课件\\第1课.docx");
    const QString cached = WordConvert::tempPdfPathFor(src, 1234, 5000);
    check("cache key: deterministic",
          int(cached == WordConvert::tempPdfPathFor(src, 1234, 5000)), 1);
    check("cache key: size changes it",
          int(cached != WordConvert::tempPdfPathFor(src, 1235, 5000)), 1);
    check("cache key: mtime changes it",
          int(cached != WordConvert::tempPdfPathFor(src, 1234, 5001)), 1);
    check("cache key: path changes it",
          int(cached != WordConvert::tempPdfPathFor(QStringLiteral("D:\\课件\\第2课.docx"),
                                                    1234, 5000)), 1);
    check("cache key: temp .pdf name",
          int(cached.contains(QStringLiteral("pdfboard"))
              && cached.endsWith(QStringLiteral(".pdf"))), 1);

    // --- export failure paths -----------------------------------------------
    // Snapshot the cache first, so the assertion holds even when a real
    // converter exists on this machine and other Word PDFs are cached there.
    const QDir cache(QDir(QDir::tempPath()).filePath(QStringLiteral("pdfboard")));
    const QStringList before = cache.entryList({QStringLiteral("word-*")}, QDir::Files);

    // Non-Word input is rejected immediately, before any converter check.
    QString pdfOut = QStringLiteral("sentinel");
    QString err;
    const QString notWord = QDir(QDir::tempPath())
                                .filePath(QStringLiteral("pdfboard-selftest-docx.pdf"));
    const bool pdfRejected = WordConvert::convertToPdf(notWord, &pdfOut, &err);
    check("convert: .pdf rejected", int(pdfRejected), 0);
    check("convert: .pdf error reported", int(!err.isEmpty()), 1);
    check("convert: .pdf no output", int(pdfOut.isEmpty()), 1);

    // A missing .docx fails with a reason and leaves nothing in the cache.
    const QString missing = QDir(QDir::tempPath())
                                .filePath(QStringLiteral("pdfboard-selftest-docx-missing.docx"));
    QFile::remove(missing);
    pdfOut = QStringLiteral("sentinel");
    err.clear();
    const bool converted = WordConvert::convertToPdf(missing, &pdfOut, &err);
    check("convert: missing input fails", int(converted), 0);
    check("convert: error reported", int(!err.isEmpty()), 1);
    check("convert: no output on failure", int(pdfOut.isEmpty()), 1);
    const QStringList after = cache.entryList({QStringLiteral("word-*")}, QDir::Files);
    check("convert: nothing left in temp", int(after == before), 1);

    // --- probe --------------------------------------------------------------
    // May be true or false depending on the machine; it must answer, be cached
    // and never crash.
    const bool has = WordConvert::hasConverter();
    check("hasConverter: returns a bool", int(has || !has), 1);
    check("hasConverter: cached second call", int(WordConvert::hasConverter() == has), 1);

    // A bundle must never be written anywhere but a .dpz. That guard is what
    // stops 保存 from overwriting the document it was made from - the failure
    // has to be visible instead of destructive.
    {
        const QString guard = QDir(QDir::tempPath())
                                  .filePath(QStringLiteral("pdfboard-guard-source.docx"));
        const QByteArray original = QByteArrayLiteral("ORIGINAL SOURCE BYTES");
        {
            QFile seed(guard);
            seed.open(QIODevice::WriteOnly | QIODevice::Truncate);
            seed.write(original);
        }
        QString guardErr;
        const bool wrote = AnnotationBundle::write(
            guard, QByteArrayLiteral("%PDF-1.4 fake"), QJsonObject(), &guardErr);
        check("guard: refuses non-.dpz target", wrote ? 0 : 1, 1);
        check("guard: error explains why", guardErr.isEmpty() ? 0 : 1, 1);
        QFile back(guard);
        back.open(QIODevice::ReadOnly);
        check("guard: source file untouched", back.readAll() == original ? 1 : 0, 1);
        back.close();
        QFile::remove(guard);

        const QString dpz = QDir(QDir::tempPath())
                                .filePath(QStringLiteral("pdfboard-guard-target.dpz"));
        QFile::remove(dpz);
        QString okErr;
        const bool okWrote = AnnotationBundle::write(
            dpz, QByteArrayLiteral("%PDF-1.4 fake"), QJsonObject(), &okErr);
        check("guard: .dpz target still works", okWrote ? 1 : 0, 1);
        check("guard: .dpz file created", QFileInfo::exists(dpz) ? 1 : 0, 1);
        QFile::remove(dpz);
    }

    // --- working copies under %TEMP%\pdfboard -------------------------------
    // A document without a bundle of its own is 保存d into a per-source working
    // copy in the temp directory. The path function is pure, so it is asserted
    // without ever touching the disk.
    const QString sourceA = QStringLiteral("D:\\dev\\tmp\\selftest-docx-a.pdf");
    const QString sourceB = QStringLiteral("D:\\dev\\tmp\\selftest-docx-b.pdf");
    const QString workA = AnnotationBundle::workingBundlePathFor(sourceA);
    check("working path: deterministic",
          int(workA == AnnotationBundle::workingBundlePathFor(sourceA)), 1);
    check("working path: per source",
          int(workA != AnnotationBundle::workingBundlePathFor(sourceB)), 1);
    check("working path: .dpz suffix",
          int(workA.endsWith(QStringLiteral(".dpz"))), 1);
    {
        QString temp = QDir::fromNativeSeparators(QDir::tempPath());
        while (temp.endsWith(QLatin1Char('/')))
            temp.chop(1);
        const QString normalized = QDir::fromNativeSeparators(workA);
        check("working path: under temp",
              int(normalized.startsWith(temp + QLatin1Char('/'), Qt::CaseInsensitive)), 1);
        check("working path: inside pdfboard",
              int(normalized.contains(QStringLiteral("/pdfboard/work-"))), 1);
    }

    // --- content-identity fingerprint ---------------------------------------
    // The restore gate: a working copy may only come back when the file behind
    // the same PATH is still the same document content.
    const QString fpPath =
        QDir(QDir::tempPath()).filePath(QStringLiteral("pdfboard-selftest-fp.bin"));
    QFile::remove(fpPath);
    {
        QFile seed(fpPath);
        seed.open(QIODevice::WriteOnly | QIODevice::Truncate);
        seed.write(QByteArray(200, 'A'));
    }

    const QJsonObject fpA = AnnotationBundle::sourceFingerprintFor(fpPath);
    check("fingerprint: non-empty", int(!fpA.isEmpty()), 1);
    check("fingerprint: stable",
          int(fpA == AnnotationBundle::sourceFingerprintFor(fpPath)), 1);
    check("fingerprint: matches itself",
          int(AnnotationBundle::fingerprintMatches(fpA, fpA)), 1);
    check("fingerprint: head is sha1 hex",
          int(fpA.value(QStringLiteral("head")).toString().size() == 40), 1);
    check("fingerprint: missing file empty",
          int(AnnotationBundle::sourceFingerprintFor(
                  fpPath + QStringLiteral(".missing")).isEmpty()), 1);

    // One more byte: the size changes, so the fingerprint must not match.
    {
        QFile appender(fpPath);
        appender.open(QIODevice::Append);
        appender.write(QByteArrayLiteral("B"));
    }
    const QJsonObject fpB = AnnotationBundle::sourceFingerprintFor(fpPath);
    check("fingerprint: append breaks match",
          int(AnnotationBundle::fingerprintMatches(fpA, fpB)), 0);

    // Same length, DIFFERENT content, original mtime restored: only the head
    // hash can tell the two apart. This is the "not just a name match" proof.
    {
        QFile replacer(fpPath);
        replacer.open(QIODevice::WriteOnly | QIODevice::Truncate);
        replacer.write(QByteArray(200, 'B'));
    }
    {
        const qint64 mtimeMs = qint64(fpA.value(QStringLiteral("mtimeMs")).toDouble());
        constexpr qint64 kEpochOffsetMs = 11644473600000LL;   // 1601 -> 1970
        ULARGE_INTEGER ticks;
        ticks.QuadPart = static_cast<ULONGLONG>((mtimeMs + kEpochOffsetMs) * 10000LL);
        FILETIME stamp;
        stamp.dwLowDateTime = ticks.LowPart;
        stamp.dwHighDateTime = ticks.HighPart;
        const QString native = QDir::toNativeSeparators(fpPath);
        HANDLE handle = CreateFileW(
            reinterpret_cast<const wchar_t *>(native.utf16()),
            FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            SetFileTime(handle, nullptr, nullptr, &stamp);
            CloseHandle(handle);
        }
    }
    const QJsonObject fpC = AnnotationBundle::sourceFingerprintFor(fpPath);
    check("fingerprint: same size kept",
          int(fpC.value(QStringLiteral("size")) == fpA.value(QStringLiteral("size"))), 1);
    check("fingerprint: same mtime restored",
          int(fpC.value(QStringLiteral("mtimeMs"))
              == fpA.value(QStringLiteral("mtimeMs"))), 1);
    check("fingerprint: head differs",
          int(fpC.value(QStringLiteral("head")) != fpA.value(QStringLiteral("head"))), 1);
    check("fingerprint: name+size+mtime same -> no match",
          int(AnnotationBundle::fingerprintMatches(fpA, fpC)), 0);
    QFile::remove(fpPath);

    // Round trip: a working copy written with the fingerprint must carry it
    // back through annotations.json, and the restore gate must accept the SAME
    // file while refusing an edited one. MainWindow::openPath relies on exactly
    // this contract.
    {
        const QString workFile = AnnotationBundle::workingBundlePathFor(
            QStringLiteral("D:\\dev\\tmp\\selftest-docx-source.pdf"));
        QDir().mkpath(QFileInfo(workFile).absolutePath());
        QFile::remove(workFile);

        const QString rtSrc =
            QDir(QDir::tempPath()).filePath(QStringLiteral("pdfboard-selftest-rt.pdf"));
        QFile::remove(rtSrc);
        {
            QFile seed(rtSrc);
            seed.open(QIODevice::WriteOnly | QIODevice::Truncate);
            seed.write(QByteArrayLiteral("%PDF-1.4 selftest"));
        }

        const QJsonObject recorded = AnnotationBundle::sourceFingerprintFor(rtSrc);
        QString rtErr;
        check("round trip: write working copy",
              int(AnnotationBundle::write(workFile, QByteArrayLiteral("%PDF-1.4 fake"),
                                          QJsonObject(), &rtErr, recorded)), 1);
        QByteArray rtPdf;
        QJsonObject rtInk;
        check("round trip: read working copy",
              int(AnnotationBundle::read(workFile, &rtPdf, &rtInk, &rtErr)), 1);
        check("round trip: fingerprint stored",
              int(rtInk.value(QStringLiteral("source")).toObject() == recorded), 1);
        check("round trip: gate accepts same file",
              int(AnnotationBundle::fingerprintMatches(
                      rtInk.value(QStringLiteral("source")).toObject(),
                      AnnotationBundle::sourceFingerprintFor(rtSrc))), 1);
        {
            QFile appender(rtSrc);
            appender.open(QIODevice::Append);
            appender.write(QByteArrayLiteral("!"));
        }
        check("round trip: gate refuses edited file",
              int(AnnotationBundle::fingerprintMatches(
                      rtInk.value(QStringLiteral("source")).toObject(),
                      AnnotationBundle::sourceFingerprintFor(rtSrc))), 0);

        QFile::remove(workFile);
        QFile::remove(rtSrc);
    }

    // --- 最近项目 refuses temp paths ----------------------------------------
    // The stored list is restored through the raw registry afterwards, so an
    // odd pre-existing value cannot make the restore itself fail.
    {
        QSettings rawRecent(QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard"),
                            QSettings::NativeFormat);
        const QVariant savedRecent = rawRecent.value(QStringLiteral("RecentFiles"));

        const QStringList recentBefore = AppSettings::recentFiles();
        const QString tempCandidate =
            QDir(QDir::tempPath()).filePath(QStringLiteral("pdfboard/work-selftest.dpz"));
        QString recentErr;
        const bool added = AppSettings::addRecentFile(tempCandidate, &recentErr);
        check("recent: temp path refused", int(added), 0);
        check("recent: refusal explains", int(!recentErr.isEmpty()), 1);
        check("recent: list untouched",
              int(AppSettings::recentFiles() == recentBefore), 1);

        const QString normal = QStringLiteral("D:\\dev\\tmp\\selftest-docx-recent.pdf");
        AppSettings::addRecentFile(normal, nullptr);
        check("recent: normal path recorded",
              int(AppSettings::recentFiles().value(0) == normal), 1);

        if (savedRecent.isValid())
            rawRecent.setValue(QStringLiteral("RecentFiles"), savedRecent);
        else
            rawRecent.remove(QStringLiteral("RecentFiles"));
        rawRecent.sync();
        const QVariant afterRecent = rawRecent.value(QStringLiteral("RecentFiles"));
        check("recent: stored list restored",
              int(savedRecent.isValid() ? afterRecent == savedRecent
                                        : !afterRecent.isValid()), 1);
    }

    // --- Word 打开方式 preference ------------------------------------------
    {
        QSettings rawWord(QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard"),
                          QSettings::NativeFormat);
        const QVariant savedWord = rawWord.value(QStringLiteral("WordOpenMode"));

        rawWord.remove(QStringLiteral("WordOpenMode"));
        rawWord.sync();
        check("word mode: default 0", AppSettings::wordOpenMode(), 0);

        QString wordErr;
        check("word mode: set 1", int(AppSettings::setWordOpenMode(1, &wordErr)), 1);
        check("word mode: stored 1", AppSettings::wordOpenMode(), 1);
        check("word mode: set 2", int(AppSettings::setWordOpenMode(2, &wordErr)), 1);
        check("word mode: stored 2", AppSettings::wordOpenMode(), 2);

        rawWord.setValue(QStringLiteral("WordOpenMode"), 7);
        rawWord.sync();
        check("word mode: out of range -> 0", AppSettings::wordOpenMode(), 0);

        if (savedWord.isValid())
            rawWord.setValue(QStringLiteral("WordOpenMode"), savedWord);
        else
            rawWord.remove(QStringLiteral("WordOpenMode"));
        rawWord.sync();
        const QVariant afterWord = rawWord.value(QStringLiteral("WordOpenMode"));
        check("word mode: stored value restored",
              int(savedWord.isValid() ? afterWord == savedWord : !afterWord.isValid()), 1);
    }

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Single-instance plumbing: the first process owns a named local socket and
// every later launch hands its document paths over before exiting, so opening a
// file never pops up a second window. QtNetwork backs this; no WinAPI needed.

static QString instanceServerName()
{
    return QStringLiteral("PDFBoard.SingleInstance");
}

// The .pdf/.dpz paths in argv, in order. Option-like arguments (--bench,
// --selftest-*) are never documents and are not forwarded.
static QStringList documentPathsFrom(const QStringList &args)
{
    QStringList paths;
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a.startsWith(QLatin1Char('-')))
            continue;
        if (a.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
            || a.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive)
            || WordConvert::isWordDoc(a)) {
            paths.append(a);
        }
    }
    return paths;
}

// A second launch: hand the paths to the running window and leave without ever
// constructing one. Returns false when nobody is listening on the name, which
// means this process is the first instance.
static bool forwardToRunningInstance(const QStringList &args)
{
    QLocalSocket socket;
    socket.connectToServer(instanceServerName());
    if (!socket.waitForConnected(500))
        return false;

    // Empty when there are no paths: the running window just comes to front.
    socket.write(documentPathsFrom(args).join(QLatin1Char('\n')).toUtf8());
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}

// The first launch answers later launches: their paths are opened as tabs in
// this window (openPath dedupes and focuses), then the window is raised. A
// failed listen() is not fatal - the app keeps its plain single-window startup.
static void serveLaterLaunches(MainWindow &w)
{
    const QString name = instanceServerName();

    // Only reached when nobody answered on the name: clear a socket a crashed
    // instance left behind so listen() starts clean.
    QLocalServer::removeServer(name);

    auto *server = new QLocalServer(&w);
    server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server->listen(name))
        return;

    // One buffer per connection: a payload may arrive in several chunks.
    auto buffers = QSharedPointer<QHash<QLocalSocket *, QByteArray>>::create();

    QObject::connect(server, &QLocalServer::newConnection, server,
                     [server, &w, buffers]() {
        while (QLocalSocket *socket = server->nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, socket,
                             [socket, buffers]() {
                (*buffers)[socket].append(socket->readAll());
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket,
                             [socket, &w, buffers]() {
                QByteArray payload = buffers->take(socket);
                payload.append(socket->readAll());   // any residue left unread
                const QList<QByteArray> lines = payload.split('\n');
                for (const QByteArray &line : lines) {
                    const QString path = QString::fromUtf8(line);
                    if (!path.isEmpty())
                        w.openPath(path);
                }
                // Windows may refuse a focus steal; alert() nudges the taskbar.
                w.raise();
                w.activateWindow();
                QApplication::alert(&w);
                socket->deleteLater();
            });
        }
    });
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
    const int homeIdx = args.indexOf(QStringLiteral("--selftest-home"));
    const int docxIdx = args.indexOf(QStringLiteral("--selftest-docx"));

    // The shipping build is a GUI executable (no console window when the user
    // double-clicks it). The console-based modes still need their output, so
    // attach to the launching terminal - but only when stdout was NOT already
    // redirected, otherwise we would clobber the caller's capture.
    if ((benchIdx >= 0 && benchIdx + 1 < args.size())
        || (stIdx >= 0 && stIdx + 1 < args.size())
        || logIdx >= 0
        || uiIdx >= 0
        || themeIdx >= 0
        || homeIdx >= 0
        || docxIdx >= 0) {
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

    if (homeIdx >= 0)
        return runHomeSelfTest();

    if (docxIdx >= 0)
        return runDocxSelfTest();

    // With a window already running, this process only hands its paths over and
    // exits: a file association, autostart or second command line must never
    // open another window. Done after the CLI modes so they stay independent.
    if (forwardToRunningInstance(args))
        return 0;

    MainWindow w;
    w.show();
    serveLaterLaunches(w);

    // Optional: open a document passed on the command line - a plain PDF, a
    // .dpz annotation bundle or a Word document (converted on the fly). This is
    // also the "double-click to open" path used by the file association.
    for (int i = 1; i < args.size(); ++i) {
        const QString &a = args.at(i);
        if (a.startsWith(QLatin1Char('-')))
            continue;
        if (a.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
            || a.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive)
            || WordConvert::isWordDoc(a)) {
            w.openPath(a);
            break;
        }
    }

    return app.exec();
}
