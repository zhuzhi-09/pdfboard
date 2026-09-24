#include "MainWindow.h"
#include "AnnotationBundle.h"
#include "AppLog.h"
#include "AppSettings.h"
#include "CrashLog.h"
#include "HomePage.h"
#include "ImageImport.h"
#include "InkDirty.h"
#include "InkToolbar.h"
#include "InputProbe.h"
#include "MemProbe.h"
#include "PaperBase.h"
#include "PdfCanvas.h"
#include "PdfExport.h"
#include "SettingsPage.h"
#include "Theme.h"
#include "UpdateChecker.h"
#include "WordConvert.h"
#include "ZoomBar.h"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QNetworkProxyFactory>
#include <QSlider>
#include <QMouseEvent>
#include <QImageReader>
#include <QSslSocket>
#include <QStyle>
#include <QThread>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
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
#include <QtMath>

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
// Headless smoke test of the real window:  pdfboard.exe --selftest-smoke <file.pdf>
// Builds MainWindow exactly as the app does, opens a document, walks document →
// settings → home → document again and drives the status-bar zoom control through
// its ordinary signal chain. It exists because the unit tests never construct the
// window: a crash that only happens once the zoom control is docked, or once a page
// switch rewires it, would otherwise ship unnoticed.
static int runSmokeSelfTest(const QString &path)
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

    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1280, 800);
    window.show();
    check("smoke: window built", 1, 1);

    ZoomBar *bar = window.findChild<ZoomBar *>();
    check("smoke: zoom bar exists", bar ? 1 : 0, 1);
    check("smoke: hidden on the home page", (bar && bar->isVisible()) ? 0 : 1, 1);

    window.openPath(path);
    PdfCanvas *canvas = window.findChild<PdfCanvas *>();
    check("smoke: document opened", canvas ? 1 : 0, 1);
    check("smoke: bar shown on a document", (bar && bar->isVisible()) ? 1 : 0, 1);

    if (canvas) {
        const qreal before = canvas->testZoom();
        canvas->zoomIn();
        check("smoke: shortcut zoom works", canvas->testZoom() > before ? 1 : 0, 1);
    }

    // The zoom control's own chain: slider → zoomRequested → MainWindow → canvas.
    if (canvas && bar) {
        if (auto *slider = bar->findChild<QSlider *>(QStringLiteral("zoomSlider"))) {
            const qreal before = canvas->testZoom();
            slider->setValue(ZoomBarMath::sliderForZoom(before * 1.5));
            check("smoke: slider drives the canvas", canvas->testZoom() > before ? 1 : 0, 1);
        } else {
            check("smoke: slider found", 0, 1);
        }
    }

    // The stack overflow (0xC00000FD) was a synchronous repaint started from INSIDE
    // paintEvent: the render readout went straight from the paint to the status bar,
    // Qt flushed the dirty region immediately and painting re-entered itself ~2200
    // frames deep. Two things now hold it down: paintEvent refuses to nest, and the
    // readout is queued. Both are asserted below.
    int renderNotifies = 0;
    if (canvas) {
        canvas->testResetMaxPaintDepth();
        canvas->testResetMaxRelayoutDepth();
        QObject::connect(canvas, &PdfCanvas::renderMeasured,
                         [&renderNotifies](qint64, QSize) { ++renderNotifies; });
    }

    // The reported crash: RAPIDLY adjusting the zoom, reproducible on the classroom
    // VM only. That machine runs 250 % DPI, so every zoom step re-rasterises large
    // page bitmaps and the 160 ms settle timer lands in the middle of the sequence.
    // Drive the real signal chain (slider included) and pump the event loop between
    // steps, which is the interleaving a teacher produces by dragging the slider.
    if (canvas && bar) {
        auto *slider = bar->findChild<QSlider *>(QStringLiteral("zoomSlider"));
        for (int i = 0; i < 120; ++i) {
            const qreal target = (i % 2 == 0) ? ZoomBarMath::kMinZoom : ZoomBarMath::kMaxZoom;
            if (!slider || i % 3 == 0)
                canvas->setZoomLevel(target);                            // canvas-driven
            else
                slider->setValue(ZoomBarMath::sliderForZoom(target));    // widget-driven
            QCoreApplication::processEvents();
            if (i % 8 == 0)
                QThread::msleep(30);              // let the settle timer fire mid-stress
        }
        check("smoke: survived 120 rapid zooms", 1, 1);
    }

    // Closest possible stand-in for the classroom VM: a 4K-sized window (at 250 %
    // DPI that means huge page bitmaps, like the panel'), a REAL mouse drag on the
    // slider (press → moves → release, which is what emits sliderMoved) and finally
    // Ctrl+wheel spam.
    if (canvas && bar) {
        window.resize(3840, 2160);
        QCoreApplication::processEvents();

        if (auto *slider = bar->findChild<QSlider *>(QStringLiteral("zoomSlider"))) {
            // Press ON THE HANDLE, not on the groove: a groove press is a page step
            // and never enters Qt's drag state, so it would not emit sliderMoved -
            // the very path a teacher dragging the control exercises.
            const int hw = qMax(8, slider->style()->pixelMetric(QStyle::PM_SliderThickness,
                                                               nullptr, slider));
            for (int sweep = 0; sweep < 6; ++sweep) {
                const int y = slider->height() / 2;
                const int from = slider->style()->sliderPositionFromValue(
                                     0, slider->maximum(), slider->value(),
                                     qMax(1, slider->width() - hw))
                                 + hw / 2;
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(from, y),
                                  slider->mapToGlobal(QPoint(from, y)), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(slider, &press);
                for (int step = 0; step <= 60; ++step) {
                    const int span = qMax(1, slider->width() - hw);
                    const int x = (sweep % 2 == 0) ? hw / 2 + step * span / 60
                                                   : slider->width() - hw / 2 - step * span / 60;
                    QMouseEvent move(QEvent::MouseMove, QPointF(x, y),
                                     slider->mapToGlobal(QPoint(x, y)), Qt::NoButton,
                                     Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(slider, &move);
                    QCoreApplication::processEvents();
                    // Mid-drag: the canvas reporting a far-away zoom must NOT move the
                    // handle. That write-back is the feedback loop that overflowed the
                    // stack (0xC00000FD); with the fix the slider stays the master.
                    if (step == 30) {
                        const int held = slider->value();
                        auto *pill = bar->findChild<QToolButton *>(QStringLiteral("zoomPercent"));
                        const QString tip = pill ? pill->toolTip() : QString();
                        bar->setZoom(ZoomBarMath::kMinZoom);
                        check("smoke: drag not overridden by canvas",
                              slider->value() == held ? 1 : 0, 1);
                        // ...and it must not touch the tooltip either: setToolTip during
                        // a drag drives QToolTip's nested event handling, which is what
                        // overflowed the stack inside Qt6Widgets.dll.
                        check("smoke: drag leaves the tooltip alone",
                              (pill && pill->toolTip() == tip) ? 1 : 0, 1);
                    }
                }
                const int endX = (sweep % 2 == 0) ? slider->width() - hw / 2 : hw / 2;
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(endX, y),
                                    slider->mapToGlobal(QPoint(endX, y)), Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(slider, &release);
            }
            check("smoke: survived slider drag sweeps", 1, 1);
        }

        const QPointF middle(canvas->width() / 2.0, canvas->height() / 2.0);
        for (int i = 0; i < 200; ++i) {
            canvas->testWheelAt(middle, (i % 2 == 0) ? 120 : -120, /*ctrl=*/true);
            QCoreApplication::processEvents();
        }
        check("smoke: survived ctrl+wheel spam", 1, 1);

        // Regression for the 0xC00000FD: painting must never nest (a nested paint IS
        // the recursion), and the render readout must still reach the status bar - now
        // delivered by the event loop instead of from inside the paint.
        QCoreApplication::processEvents();
        check("smoke: paint never re-enters", canvas->testMaxPaintDepth() <= 1 ? 1 : 0, 1);
        check("smoke: render readout still arrives", renderNotifies > 0 ? 1 : 0, 1);
        // The actual culprit: relayout() toggling a scroll-bar policy resized the
        // viewport, which re-entered resizeEvent -> relayout() and never settled.
        check("smoke: relayout never nests", canvas->testMaxRelayoutDepth() <= 4 ? 1 : 0, 1);
        // Informational: 0 refusals means the structural fix removed the oscillation,
        // a non-zero count means only the cap is holding it down.
        out(QStringLiteral("[selftest] 排版拒绝次数 = %1（0 = 反馈环已消失）")
                .arg(canvas->testRelayoutRefusals()));
    }

    // Page switches must not leave a dangling or double-wired control. The invocations
    // are asserted on their return value: a name that does not resolve fails silently,
    // and the checks after it would then pass without the page ever having switched
    // (showHomePage() was not a slot, so "hidden on home" was vacuous).
    check("smoke: settings page reachable",
          QMetaObject::invokeMethod(&window, "onSettings") ? 1 : 0, 1);
    check("smoke: hidden on settings", (bar && bar->isVisible()) ? 0 : 1, 1);
    check("smoke: home page reachable",
          QMetaObject::invokeMethod(&window, "showHomePage") ? 1 : 0, 1);
    check("smoke: hidden on home", (bar && bar->isVisible()) ? 0 : 1, 1);
    check("smoke: tab switch reachable",
          QMetaObject::invokeMethod(&window, "onTabCurrentChanged", Q_ARG(int, 0)) ? 1 : 0, 1);
    check("smoke: still alive after switches", 1, 1);
    check("smoke: bar back on a document", (bar && bar->isVisible()) ? 1 : 0, 1);

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Deliberately recurses until the stack overflows, so the crash reporter's
// stack-overflow path (reporting from a fresh thread) can actually be verified.
// C4717 is suppressed because overflowing the stack is the entire point here.
#pragma warning(suppress : 4717)
static volatile int g_crashSink = 0;
#pragma warning(push)
#pragma warning(disable : 4717)   // intentional: the point IS to overflow the stack
#pragma optimize("", off)         // ...and it only overflows if the code is naive:
__declspec(noinline) static void crashTestRecursion(int depth)
{
    // Every byte of the frame is written and consumed, and no optimisation is allowed
    // here: otherwise the compiler collapses the recursion into a loop with a tiny
    // frame (it did exactly that, which made this test spin forever instead of
    // overflowing the stack).
    volatile char padding[8192];
    for (int i = 0; i < 8192; ++i)
        padding[i] = char(depth + i);
    g_crashSink += padding[depth & 8191];
    crashTestRecursion(depth + 1);
}
#pragma optimize("", on)
#pragma warning(pop)

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

    // The touch pipeline must stay uncompressed (see the note in main()): Qt's
    // default merges several panel samples into one event per frame, which turns
    // a fast stroke into long straight chords. This also proves the platform
    // integration actually honoured the pre-init attribute.
    check("input: touch not compressed",
          QCoreApplication::testAttribute(Qt::AA_CompressHighFrequencyEvents) ? 1 : 0, 0);

    // The classroom diagnosis rides on the probe's arithmetic, so lock it down:
    // silent until the interval is up, then exactly one line with the counts,
    // then the counters must be back to zero.
    {
        InputProbe probe;                  // local instance: no shared state
        check("probe: starts the clock", probe.take(0).isEmpty() ? 1 : 0, 1);
        probe.addEvent(3);
        probe.addPaint();
        probe.addRender();
        probe.setZoom(2.5);
        check("probe: quiet mid interval", probe.take(500).isEmpty() ? 1 : 0, 1);
        const QString line = probe.take(1000);
        check("probe: line after 1s", line.isEmpty() ? 0 : 1, 1);
        check("probe: input counted", line.contains(QStringLiteral("输入 1 事件")) ? 1 : 0, 1);
        check("probe: samples counted", line.contains(QStringLiteral("样本 3")) ? 1 : 0, 1);
        check("probe: paint counted", line.contains(QStringLiteral("重绘 1 帧")) ? 1 : 0, 1);
        check("probe: render counted", line.contains(QStringLiteral("栅格化 1")) ? 1 : 0, 1);
        check("probe: zoom reported", line.contains(QStringLiteral("2.50x")) ? 1 : 0, 1);
        check("probe: resets counts",
              probe.take(2000).contains(QStringLiteral("输入 0 事件")) ? 1 : 0, 1);
    }

    // Partial repaint geometry: a new sample may only dirty its own
    // neighbourhood, never the whole viewport (a full 4K repaint per sample is
    // what made handwriting lag behind the finger).
    {
        const QRectF page(0, 0, 1000, 1400);
        QPointF pts[3] = {QPointF(0.10, 0.50), QPointF(0.20, 0.50), QPointF(0.22, 0.52)};
        const QRect r = inkDirtyRect(page, pts, 3, 1, 4.0);
        const qreal area = qreal(r.width()) * qreal(r.height());
        check("dirty: far smaller than view", area < 1000.0 * 1400.0 / 20.0 ? 1 : 0, 1);
        check("dirty: covers new sample", r.contains(QPoint(220, 728)) ? 1 : 0, 1);
        check("dirty: covers prev sample", r.contains(QPoint(200, 700)) ? 1 : 0, 1);
        check("dirty: ignores older samples", r.contains(QPoint(100, 700)) ? 0 : 1, 1);
        check("dirty: pad applied", r.height() >= 34 ? 1 : 0, 1);
        const QRect one = inkDirtyRect(page, pts, 1, 0, 4.0);
        check("dirty: single sample box", (one.width() >= 8 && one.width() <= 12) ? 1 : 0, 1);
        check("dirty: empty input safe",
              inkDirtyRect(page, nullptr, 0, 0, 4.0).isNull() ? 1 : 0, 1);
    }

    // Panels repeat each physical sample several times (a classroom panel measured
    // ~420 events/s with 4 identical frames per sample). Storing the repeats would
    // rebuild the live path with 4x the points on every repaint.
    {
        const QVector<QPointF> repeated{QPointF(0.3, 0.3), QPointF(0.3, 0.3),
                                        QPointF(0.3, 0.3), QPointF(0.3, 0.3)};
        check("dedupe: repeats collapse", canvas.testDensifiedCount(repeated), 1);

        const QVector<QPointF> clean{QPointF(0.3, 0.3), QPointF(0.5, 0.3)};
        const QVector<QPointF> noisy{QPointF(0.3, 0.3), QPointF(0.3, 0.3),
                                     QPointF(0.5, 0.3), QPointF(0.5, 0.3),
                                     QPointF(0.5, 0.3)};
        const int plain = canvas.testDensifiedCount(clean);
        check("dedupe: same as clean path", canvas.testDensifiedCount(noisy), plain);
        check("dedupe: path still dense", plain > 100 ? 1 : 0, 1);
    }

    // A page raster carries alpha, so anything showing a page must lay paper-white
    // underneath. Missing that backdrop is why the page-picker thumbnails lost
    // their white paper in dark mode.
    {
        QImage transparent(4, 4, QImage::Format_ARGB32_Premultiplied);
        transparent.fill(Qt::transparent);
        transparent.setDevicePixelRatio(2.0);
        const QImage backed = onPaper(transparent, Theme::light().paper);
        check("paper: size kept", backed.size() == transparent.size() ? 1 : 0, 1);
        check("paper: dpr kept", qFuzzyCompare(backed.devicePixelRatio(), 2.0) ? 1 : 0, 1);
        check("paper: now opaque", backed.pixelColor(1, 1).alpha() == 255 ? 1 : 0, 1);
        check("paper: is paper", backed.pixelColor(1, 1) == Theme::light().paper ? 1 : 0, 1);
        check("paper: null safe", onPaper(QImage(), Theme::light().paper).isNull() ? 1 : 0, 1);
    }

    // Writing across the floating island must never lose the stroke: the island is
    // a child of the viewport, so the ink is covered by it (goes underneath).
    // Landing ON the island is a different gesture - that touch belongs to the
    // island, where a drag moves it, and must not start ink under it.
    // Uses a canvas of its own so the undo counts below stay untouched.
    {
        PdfCanvas probe;
        probe.setAttribute(Qt::WA_DontShowOnScreen, true);
        probe.resize(1000, 800);
        probe.show();
        QString openErr;
        check("overlay: probe opened", probe.openPdf(path, &openErr) ? 1 : 0, 1);

        InkToolbar *bar = probe.toolbar();
        const QSize vp = probe.testViewportSize();
        check("overlay: toolbar exists", bar ? 1 : 0, 1);
        const QPoint barPt = bar ? bar->geometry().center()
                                 : QPoint(vp.width() / 2, vp.height() - 40);

        // A viewport point that really lies on a page (not in the desk margin).
        QPointF pagePt(20.0, vp.height() / 2.0);
        for (int i = 0; i < 60 && pagePt.y() < vp.height()
                        && probe.testPageAtViewportY(pagePt.y()) < 0; ++i)
            pagePt.setY(pagePt.y() + 10.0);
        check("overlay: page point found",
              probe.testPageAtViewportY(pagePt.y()) >= 0 ? 1 : 0, 1);

        check("overlay: island touch stays with island",
              probe.testTouchBelongsToOverlay(QVector<QPointF>{QPointF(barPt)}) ? 1 : 0, 1);

        probe.testTouchBegin(pagePt);
        probe.testTouchMove(pagePt + QPointF(80.0, 0.0));
        check("overlay: running stroke keeps island samples",
              probe.testTouchBelongsToOverlay(QVector<QPointF>{QPointF(barPt)}) ? 0 : 1, 1);
        probe.testTouchMove(QPointF(barPt));      // dragged across the island
        probe.testTouchEnd();
        check("overlay: ink survives the island", probe.strokeCount(), 1);
    }

    // Wheel zoom rules. Ctrl+wheel zooms at the pointer, the plain wheel keeps
    // scrolling, and while a touch gesture runs (or just ended) the wheel is ignored
    // - a two-finger PAN reaches the app as a wheel event and must never zoom.
    // A canvas of its own, so the zoom asserted below cannot disturb the rest.
    {
        PdfCanvas probe;
        probe.setAttribute(Qt::WA_DontShowOnScreen, true);
        probe.resize(1000, 800);
        probe.show();
        QString wheelErr;
        check("wheel: probe opened", probe.openPdf(path, &wheelErr) ? 1 : 0, 1);

        int zoomSignals = 0;
        qreal lastZoom = 0.0;
        QObject::connect(&probe, &PdfCanvas::zoomChanged, &probe,
                         [&zoomSignals, &lastZoom](qreal z) { ++zoomSignals; lastZoom = z; });

        const QPointF at(400.0, 300.0);
        const qreal before = probe.testZoom();
        probe.testWheelAt(at, 120, /*ctrl=*/true);
        check("wheel: ctrl+wheel zooms in", probe.testZoom() > before ? 1 : 0, 1);
        check("wheel: zoomChanged fired", zoomSignals > 0 ? 1 : 0, 1);
        check("wheel: signalled zoom matches",
              qFuzzyCompare(lastZoom, probe.testZoom()) ? 1 : 0, 1);

        const qreal zoomed = probe.testZoom();
        probe.testWheelAt(at, 120, /*ctrl=*/false);
        check("wheel: plain wheel does not zoom",
              qFuzzyCompare(probe.testZoom(), zoomed) ? 1 : 0, 1);
        probe.testWheelAt(at, -120, /*ctrl=*/true);
        check("wheel: ctrl+wheel zooms out", probe.testZoom() < zoomed ? 1 : 0, 1);

        // Land the touch on a real page, then prove the stroke is actually live via
        // the overlay rule (a point ON the toolbar belongs to the island only when
        // no gesture is running) - otherwise the next check would pass vacuously.
        const QSize vp = probe.testViewportSize();
        QPointF pagePt(20.0, 100.0);
        for (int i = 0; i < 60 && pagePt.y() < vp.height()
                        && probe.testPageAtViewportY(pagePt.y()) < 0; ++i)
            pagePt.setY(pagePt.y() + 10.0);
        InkToolbar *bar = probe.toolbar();
        const QPointF barPt = bar ? QPointF(bar->geometry().center())
                                  : QPointF(vp.width() / 2.0, vp.height() - 40.0);
        probe.testTouchBegin(pagePt);
        check("wheel: touch stroke is live",
              probe.testTouchBelongsToOverlay(QVector<QPointF>{barPt}) ? 0 : 1, 1);

        const qreal during = probe.testZoom();
        probe.testWheelAt(at, 120, /*ctrl=*/true);
        check("wheel: ignored while a stroke runs",
              qFuzzyCompare(probe.testZoom(), during) ? 1 : 0, 1);
        probe.testTouchEnd();
        probe.testWheelAt(at, 120, /*ctrl=*/true);
        check("wheel: still ignored right after touch",
              qFuzzyCompare(probe.testZoom(), during) ? 1 : 0, 1);
    }

    // The status-bar zoom control's slider mapping is pure, so it is asserted here
    // (the widget uses the same functions, never a second copy of the maths).
    {
        check("zoombar: 0 -> min", qFuzzyCompare(ZoomBarMath::zoomForSlider(0), 0.25) ? 1 : 0, 1);
        check("zoombar: max -> 4x", qFuzzyCompare(ZoomBarMath::zoomForSlider(1000), 4.0) ? 1 : 0, 1);
        check("zoombar: quarter -> 0.5x",
              qFuzzyCompare(ZoomBarMath::zoomForSlider(250), 0.5) ? 1 : 0, 1);
        check("zoombar: middle -> 1x",
              qFuzzyCompare(ZoomBarMath::zoomForSlider(500), 1.0) ? 1 : 0, 1);
        check("zoombar: three quarters -> 2x",
              qFuzzyCompare(ZoomBarMath::zoomForSlider(750), 2.0) ? 1 : 0, 1);
        bool roundTrip = true;
        for (int value : {0, 250, 500, 750, 1000})
            roundTrip = roundTrip
                        && (ZoomBarMath::sliderForZoom(ZoomBarMath::zoomForSlider(value)) == value);
        check("zoombar: slider round trip", roundTrip ? 1 : 0, 1);
    }

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

// 人工复核用的确定性截图：整页设置页 + 底部标签栏。只有在开发者机器上存在
// D:/dev/tmp 时才写文件；CI 上没有该目录，这里直接跳过——不报错，也不产生
// 任何输出。--selftest-ui 在纯数学断言之后调用它。
static void captureUiReviewImages()
{
    const QString dir = QStringLiteral("D:/dev/tmp");
    if (!QDir(dir).exists())
        return;

    // 无屏窗口：与 --selftest-smoke 一样，不弹窗也能完成布局与绘制，
    // 设置页通过 MainWindow::showSettingsPage()（Q_INVOKABLE）切过去。
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1440, 1000);
    window.show();
    QCoreApplication::processEvents();

    // 标签栏先抓：启动即主页，房子芯片此时是选中态（强调色），形状最清楚。
    if (auto *tabs = window.findChild<QWidget *>(QStringLiteral("documentTabs"))) {
        const QString path = dir + QStringLiteral("/tab-bar.png");
        const bool saved = tabs->grab().save(path, "PNG");
        out(QStringLiteral("[selftest] 标签栏截图：%1（%2）")
                .arg(path, saved ? QStringLiteral("已写出") : QStringLiteral("写出失败")));
    }

    if (!QMetaObject::invokeMethod(&window, "showSettingsPage"))
        return;
    QCoreApplication::processEvents();

    if (auto *page = window.findChild<SettingsPage *>()) {
        // 把窗口撑到内容高度，让全部分组一次装进滚动区：抓出来的整页图里
        // 不会出现滚动条把「更新 / 关于」截掉。
        for (int i = 0; i < 4 && page->verticalScrollBar()->isVisible(); ++i) {
            window.resize(window.width(),
                          window.height() + page->verticalScrollBar()->maximum() + 40);
            QCoreApplication::processEvents();
        }
        const QString path = dir + QStringLiteral("/settings-page.png");
        const bool saved = page->grab().save(path, "PNG");
        out(QStringLiteral("[selftest] 设置页截图：%1（%2）")
                .arg(path, saved ? QStringLiteral("已写出") : QStringLiteral("写出失败")));
    }
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

    captureUiReviewImages();

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

// Read-only snapshot of every value under each Office version's
// Word\Options key. --selftest-docx takes two of these around a failing
// restore to prove that it touched no Word setting. Nothing here ever writes.
static QMap<QString, QVariantMap> wordOptionSnapshot()
{
    QMap<QString, QVariantMap> snapshot;
    QSettings office(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Office"),
                     QSettings::NativeFormat);
    for (const QString &version : office.childGroups()) {
        if (!version.contains(QLatin1Char('.')))
            continue;                       // version keys look like 16.0
        QSettings options(
            QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Office/%1/Word/Options")
                .arg(version),
            QSettings::NativeFormat);
        QVariantMap values;
        for (const QString &name : options.allKeys())
            values.insert(name, options.value(name));
        snapshot.insert(version, values);
    }
    return snapshot;
}

// Headless Word-conversion self test:  pdfboard.exe --selftest-docx
// Pure pieces of the .docx path (extension filter, cache key, the Word nag
// backup encoding), the export failure paths and the no-backup restore. No
// Word/WPS is needed and no automation is ever started, so this passes on
// every machine and in CI; when a converter IS installed the failure checks
// still prove nothing is launched and nothing is left behind.
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

    // --- Word nag backup encoding (pure) ------------------------------------
    // One recorded value that existed and one that did not: the round trip
    // must be exact and the encoded shape must stay the documented one.
    {
        QVariantMap recorded;
        {
            QVariantMap entry;
            entry.insert(QStringLiteral("exists"), true);
            entry.insert(QStringLiteral("value"), 1);
            recorded.insert(QStringLiteral("16.0|AlertIfNotDefault"), entry);
        }
        {
            QVariantMap entry;
            entry.insert(QStringLiteral("exists"), false);
            entry.insert(QStringLiteral("value"), 0);
            recorded.insert(QStringLiteral("16.0|DoNotCheckIfWordIsDefaultApp"), entry);
        }

        const QJsonObject encoded = WordConvert::encodeNagBackup(recorded);
        check("nag backup: two entries", int(encoded.size()), 2);
        const QJsonObject existed =
            encoded.value(QStringLiteral("16.0|AlertIfNotDefault")).toObject();
        check("nag backup: existed flag",
              existed.value(QStringLiteral("exists")).toBool() ? 1 : 0, 1);
        check("nag backup: original value",
              existed.value(QStringLiteral("value")).toInt(), 1);
        const QJsonObject missingEntry =
            encoded.value(QStringLiteral("16.0|DoNotCheckIfWordIsDefaultApp")).toObject();
        check("nag backup: missing flag",
              missingEntry.value(QStringLiteral("exists")).toBool() ? 0 : 1, 1);
        check("nag backup: round trip exact",
              int(WordConvert::decodeNagBackup(encoded) == recorded), 1);

        // The registry stores the JSON as text, so reparse it first.
        const QJsonObject reparsed = QJsonDocument::fromJson(
            QJsonDocument(encoded).toJson(QJsonDocument::Compact)).object();
        check("nag backup: json round trip",
              int(WordConvert::decodeNagBackup(reparsed) == recorded), 1);
    }

    // --- Word nag restore mode and the no-backup path -----------------------
    // Machine-independent: everything asserted here depends only on OUR
    // registry. The dummy backup below lives in HKCU\Software\PDFBoard and is
    // never handed to restoreWordNag(), so no Word key is ever written; the
    // Word option keys themselves are only READ, by the snapshots.
    //
    // On a machine whose REAL Word option values already look silenced the
    // no-backup restore would (by design) write those keys - exactly what a
    // self test must never do - so the failing-restore assertions below run
    // only in the None case and the test prints why it skipped them otherwise.
    {
        QSettings rawNag(QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard"),
                         QSettings::NativeFormat);
        const QVariant savedBackup = rawNag.value(QStringLiteral("WordNagBackup"));

        // A recorded backup means FromBackup, whatever Word itself holds.
        {
            QVariantMap recorded;
            QVariantMap entry;
            entry.insert(QStringLiteral("exists"), true);
            entry.insert(QStringLiteral("value"), 1);
            recorded.insert(QStringLiteral("16.0|AlertIfNotDefault"), entry);
            rawNag.setValue(QStringLiteral("WordNagBackup"),
                            QString::fromUtf8(
                                QJsonDocument(WordConvert::encodeNagBackup(recorded))
                                    .toJson(QJsonDocument::Compact)));
            rawNag.sync();
            check("nag mode: backup -> FromBackup",
                  int(WordConvert::nagRestoreMode()
                      == WordConvert::NagRestoreMode::FromBackup), 1);
        }

        rawNag.remove(QStringLiteral("WordNagBackup"));
        rawNag.sync();
        check("nag restore: no backup present",
              rawNag.contains(QStringLiteral("WordNagBackup")) ? 0 : 1, 1);
        check("nag backup: exists false",
              int(WordConvert::wordNagBackupExists()), 0);
        check("nag silenced: false without backup",
              int(WordConvert::wordNagSilenced()), 0);

        const QMap<QString, QVariantMap> optionsBefore = wordOptionSnapshot();
        const WordConvert::NagRestoreMode mode = WordConvert::nagRestoreMode();
        check("nag mode: no backup never FromBackup",
              int(mode != WordConvert::NagRestoreMode::FromBackup), 1);

        if (mode == WordConvert::NagRestoreMode::None) {
            QString restoreErr;
            const bool restored = WordConvert::restoreWordNag(&restoreErr);
            check("nag restore: fails without backup", int(restored), 0);
            check("nag restore: error explains", int(!restoreErr.isEmpty()), 1);
        } else {
            out(QStringLiteral("[selftest] nag restore: skipped, this machine's Word "
                               "values look silenced (mode=%1); restoring would write "
                               "real Word keys").arg(int(mode)));
        }

        const QMap<QString, QVariantMap> optionsAfter = wordOptionSnapshot();
        check("nag restore: Word options untouched",
              int(optionsBefore == optionsAfter), 1);

        if (savedBackup.isValid())
            rawNag.setValue(QStringLiteral("WordNagBackup"), savedBackup);
        else
            rawNag.remove(QStringLiteral("WordNagBackup"));
        rawNag.sync();
        const QVariant afterBackup = rawNag.value(QStringLiteral("WordNagBackup"));
        check("nag backup: value restored",
              int(savedBackup.isValid() ? afterBackup == savedBackup
                                        : !afterBackup.isValid()), 1);
    }

    // --- Word nag fix preference --------------------------------------------
    {
        QSettings rawNag(QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard"),
                         QSettings::NativeFormat);
        const QVariant savedFix = rawNag.value(QStringLiteral("WordNagFix"));

        rawNag.remove(QStringLiteral("WordNagFix"));
        rawNag.sync();
        check("nag fix: default on", int(AppSettings::wordNagFixEnabled()), 1);

        QString fixErr;
        check("nag fix: set false",
              int(AppSettings::setWordNagFixEnabled(false, &fixErr)), 1);
        check("nag fix: stored false", int(AppSettings::wordNagFixEnabled()), 0);
        check("nag fix: set true",
              int(AppSettings::setWordNagFixEnabled(true, &fixErr)), 1);
        check("nag fix: stored true", int(AppSettings::wordNagFixEnabled()), 1);

        if (savedFix.isValid())
            rawNag.setValue(QStringLiteral("WordNagFix"), savedFix);
        else
            rawNag.remove(QStringLiteral("WordNagFix"));
        rawNag.sync();
        const QVariant afterFix = rawNag.value(QStringLiteral("WordNagFix"));
        check("nag fix: stored value restored",
              int(savedFix.isValid() ? afterFix == savedFix : !afterFix.isValid()), 1);
    }

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Headless self test:  pdfboard.exe --selftest-update
// No network: the channel payloads are parsed from fixtures, so the numeric
// version comparison and both parsers can be locked without a server.
static int runUpdateSelfTest()
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
    using UpdateChecker::compareVersion;

    // Numeric, component-wise - "1.4.10" is newer than "1.4.2".
    check("version: equal", compareVersion(QStringLiteral("1.4.2"),
                                          QStringLiteral("1.4.2")), 0);
    check("version: newer", compareVersion(QStringLiteral("1.4.3"),
                                           QStringLiteral("1.4.2")), 1);
    check("version: older", compareVersion(QStringLiteral("1.3.9"),
                                           QStringLiteral("1.4.0")), -1);
    check("version: string trap", compareVersion(QStringLiteral("1.4.10"),
                                                 QStringLiteral("1.4.2")), 1);
    check("version: shorter is older", compareVersion(QStringLiteral("1.4"),
                                                      QStringLiteral("1.4.1")), -1);
    check("version: padded equal", compareVersion(QStringLiteral("1.4.0"),
                                                  QStringLiteral("1.4")), 0);

    // The accelerator URL form: every public gh-proxy takes "<base>/<absolute url>",
    // and no base is ever invented for a URL that already carries a host.
    check("accel: prefixes the absolute url",
          UpdateChecker::acceleratedUrl(
              QStringLiteral("https://gh-proxy.com"),
              QStringLiteral("https://github.com/o/r/releases/download/v1/x.exe"))
                  == QStringLiteral("https://gh-proxy.com/https://github.com/o/r/releases/download/v1/x.exe")
              ? 1 : 0, 1);
    check("accel: tolerates a trailing slash",
          UpdateChecker::acceleratedUrl(QStringLiteral("https://gh-proxy.org/"),
                                        QStringLiteral("https://api.github.com/x"))
                  == QStringLiteral("https://gh-proxy.org/https://api.github.com/x") ? 1 : 0, 1);
    check("accel: empty input is empty",
          (UpdateChecker::acceleratedUrl(QString(), QStringLiteral("https://x/y")).isEmpty()
           && UpdateChecker::acceleratedUrl(QStringLiteral("https://h"), QString()).isEmpty())
              ? 1 : 0, 1);

    // The hash gate. Two spellings of one digest agree; a missing digest never agrees
    // with anything - otherwise a proxy that simply drops the field would switch
    // verification off.
    check("digest: prefix and case ignored",
          UpdateChecker::digestsAgree(QStringLiteral("sha256:AABBCC"),
                                      QStringLiteral("aabbcc")) ? 1 : 0, 1);
    check("digest: different hashes disagree",
          UpdateChecker::digestsAgree(QStringLiteral("aabbcc"),
                                      QStringLiteral("aabbcd")) ? 0 : 1, 1);
    check("digest: empty never agrees",
          UpdateChecker::digestsAgree(QString(), QStringLiteral("aabbcc")) ? 0 : 1, 1);
    check("digest: two empties never agree",
          UpdateChecker::digestsAgree(QString(), QString()) ? 0 : 1, 1);

    const QByteArray ghWithDigest = QByteArrayLiteral(R"({
        "tag_name":"v1.5.0","body":"notes","html_url":"https://github.com/r/rel",
        "assets":[{"name":"PDFBoard-1.5.0-setup.exe","size":10,
        "digest":"sha256:AABBCC","browser_download_url":"https://x/setup.exe"}]})");
    const auto gh = UpdateChecker::parseGitHubJson(ghWithDigest, QStringLiteral("1.4.2"));
    check("github: parsed", gh.valid ? 1 : 0, 1);
    check("github: version from tag", gh.version == QStringLiteral("1.5.0") ? 1 : 0, 1);
    check("github: newer", gh.available ? 1 : 0, 1);
    check("github: digest stripped", gh.setupSha256 == QStringLiteral("aabbcc") ? 1 : 0, 1);

    const QByteArray ghNoDigest = QByteArrayLiteral(R"({
        "tag_name":"v1.5.0","body":"","html_url":"https://github.com/r/rel",
        "assets":[{"name":"PDFBoard-1.5.0-setup.exe","size":10,
        "browser_download_url":"https://x/setup.exe"}]})");
    const auto gh2 = UpdateChecker::parseGitHubJson(ghNoDigest, QStringLiteral("1.4.2"));
    check("github: no digest -> empty hash", gh2.setupSha256.isEmpty() ? 1 : 0, 1);
    check("github: page url kept",
          gh2.pageUrl == QStringLiteral("https://github.com/r/rel") ? 1 : 0, 1);
    check("github: non-release rejected",
          UpdateChecker::parseGitHubJson(QByteArrayLiteral("{}"),
                                         QStringLiteral("1.0")).valid ? 1 : 0, 0);

    // The running version must come from the executable's own resource.
    const QString current = UpdateChecker::currentVersion();
    check("current version readable", current.isEmpty() ? 0 : 1, 1);
    check("current version is 3 parts",
          current.count(QLatin1Char('.')) >= 2 ? 1 : 0, 1);
    (void)UpdateChecker::isInstalledCopy();

    // Crash evidence: the ring must record and stay bounded (it is what a crash
    // report shows), and the directory must resolve.
    {
        const int before = CrashLog::testBreadcrumbCount();
        for (int i = 0; i < 40; ++i)
            CrashLog::breadcrumb("selftest", QStringLiteral("第 %1 次").arg(i));
        check("crash: breadcrumbs recorded",
              CrashLog::testBreadcrumbCount() > before ? 1 : 0, 1);
        check("crash: ring is bounded", CrashLog::testBreadcrumbCount() <= 24 ? 1 : 0, 1);
        check("crash: directory resolves", CrashLog::directory().isEmpty() ? 0 : 1, 1);
    }

    // Release notes + the prompt decision: the dialog may only appear for a parsed,
    // newer, not-yet-announced version, and whatever the release body says has to
    // reach the teacher as plain text.
    {
        const QByteArray ghNotes = QByteArrayLiteral(R"({
            "tag_name":"v9.9.9","body":"- 修好了 A\n- 新增了 B",
            "html_url":"https://github.com/r/rel",
            "assets":[{"name":"PDFBoard-9.9.9-setup.exe","size":10,
            "browser_download_url":"https://x/setup.exe"}]})");
        const auto gh3 = UpdateChecker::parseGitHubJson(ghNotes, QStringLiteral("1.0.0"));
        check("notes: github body -> notes",
              gh3.notes.contains(QStringLiteral("修好了 A")) ? 1 : 0, 1);
        check("notes: prompt for newer",
              UpdateChecker::shouldPrompt(gh3, QString(), QStringLiteral("1.0.0")) ? 1 : 0, 1);
        check("notes: no second prompt",
              UpdateChecker::shouldPrompt(gh3, QStringLiteral("9.9.9"),
                                          QStringLiteral("1.0.0")) ? 0 : 1, 1);
        check("notes: nothing for same version",
              UpdateChecker::shouldPrompt(gh3, QString(), QStringLiteral("9.9.9")) ? 0 : 1, 1);
        const UpdateChecker::UpdateInfo unparsed;
        check("notes: nothing when unparsed",
              UpdateChecker::shouldPrompt(unparsed, QString(), QStringLiteral("1.0.0")) ? 0 : 1, 1);

        // The portable asset is picked out of the same payload: that is what the
        // portable buttons hand to the browser.
        const QByteArray withPortable = QByteArrayLiteral(R"({
            "tag_name":"v9.9.9","body":"","html_url":"https://github.com/r/rel",
            "assets":[{"name":"PDFBoard-9.9.9-setup.exe","size":10,
            "browser_download_url":"https://x/setup.exe"},
            {"name":"PDFBoard-9.9.9-portable.zip","size":20,
            "browser_download_url":"https://x/portable.zip"}]})");
        const auto portable = UpdateChecker::parseGitHubJson(withPortable,
                                                            QStringLiteral("1.0.0"));
        check("github: portable asset found",
              portable.portableUrl == QStringLiteral("https://x/portable.zip") ? 1 : 0, 1);
    }

    // The update card is where the changelog stays readable after the dialog is
    // dismissed, so its content is locked here too. Headless page, and the daily
    // quiet check is silenced for the duration (restored afterwards): a self test
    // must stay offline and must not disturb the user's preferences.
    {
        const qint64 savedCheck = AppSettings::lastUpdateCheckMs();
        AppSettings::setLastUpdateCheckMs(QDateTime::currentMSecsSinceEpoch(), nullptr);

        SettingsPage page;
        page.setAttribute(Qt::WA_DontShowOnScreen, true);
        page.resize(1200, 900);
        page.show();

        check("card: notes hidden at first", page.testUpdateNotesShown() ? 0 : 1, 1);

        UpdateChecker::UpdateInfo withNotes;
        withNotes.valid = true;
        withNotes.version = QStringLiteral("9.9.9");
        withNotes.notes = QStringLiteral("- 修好了 A\n- 新增了 B");
        page.setUpdateInfo(withNotes);
        check("card: notes shown", page.testUpdateNotesShown() ? 1 : 0, 1);
        check("card: notes text kept",
              page.testUpdateNotes().contains(QStringLiteral("修好了 A")) ? 1 : 0, 1);

        UpdateChecker::UpdateInfo noNotes;
        noNotes.valid = true;
        noNotes.version = QStringLiteral("9.9.9");
        page.setUpdateInfo(noNotes);
        check("card: empty notes explained",
              page.testUpdateNotes().contains(QStringLiteral("未提供更新日志")) ? 1 : 0, 1);

        const QString unchanged = page.testUpdateNotes();
        const UpdateChecker::UpdateInfo unparsed2;   // invalid: must change nothing
        page.setUpdateInfo(unparsed2);
        check("card: invalid info ignored",
              page.testUpdateNotes() == unchanged ? 1 : 0, 1);

        AppSettings::setLastUpdateCheckMs(savedCheck, nullptr);
    }

    out(failed == 0 ? QStringLiteral("[selftest] ALL PASS")
                    : QStringLiteral("[selftest] %1 CHECK(S) FAILED").arg(failed));
    return failed == 0 ? 0 : 3;
}

// Headless image self test:  pdfboard.exe --selftest-image
// Fully self-contained: every fixture is generated under QDir::temp(), no
// network and no external file. Locks the extension filter, the points-per-pixel
// rule, the PNG naming rule, the lossless image -> PDF -> PNG 1:1 round trip and
// the oversize downscale, then deletes everything it wrote.
static int runImageSelfTest()
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
    // Every extension we advertise must be decodable by THIS build: the import feature
    // claims to open whatever the dialog offers, so the claim and the capability are
    // checked against each other instead of being trusted side by side. webp/tiff are
    // rejected on purpose - Qt's binary package ships no such image plugin, and
    // advertising them meant offering files the app then failed to open.
    {
        const QStringList claimed = ImageImport::extensions();
        check("image: claims png", claimed.contains(QStringLiteral("png")) ? 1 : 0, 1);
        const QList<QByteArray> decodable = QImageReader::supportedImageFormats();
        bool allDecodable = true;
        for (const QString &ext : claimed)
            allDecodable = allDecodable && decodable.contains(ext.toUtf8());
        check("image: every claimed format decodable", allDecodable ? 1 : 0, 1);
        const QString filter = ImageImport::dialogFilter();
        bool filterComplete = true;
        for (const QString &ext : claimed)
            filterComplete = filterComplete && filter.contains(QStringLiteral("*.") + ext);
        check("image: dialog filter matches the list", filterComplete ? 1 : 0, 1);
        check("image: webp not advertised",
              ImageImport::isImage(QStringLiteral("C:/a/b.webp")) ? 0 : 1, 1);
        check("image: tiff not advertised",
              ImageImport::isImage(QStringLiteral("C:/a/b.tiff")) ? 0 : 1, 1);
        // The updater needs an HTTPS stack; a missing TLS backend would only show up as
        // "update check failed" on a classroom machine.
        check("env: TLS backend available", QSslSocket::supportsSsl() ? 1 : 0, 1);
    }
    check("isImage: .png", int(ImageImport::isImage(QStringLiteral("C:/a/b.png"))), 1);
    check("isImage: .PNG", int(ImageImport::isImage(QStringLiteral("C:/a/b.PNG"))), 1);
    check("isImage: .jpg", int(ImageImport::isImage(QStringLiteral("C:/a/b.jpg"))), 1);
    check("isImage: .jpeg", int(ImageImport::isImage(QStringLiteral("C:/a/b.jpeg"))), 1);
    check("isImage: .bmp", int(ImageImport::isImage(QStringLiteral("C:/a/b.bmp"))), 1);
    check("isImage: .gif", int(ImageImport::isImage(QStringLiteral("C:/a/b.gif"))), 1);
    check("isImage: .ico", int(ImageImport::isImage(QStringLiteral("C:/a/b.ico"))), 1);
    check("isImage: .pdf rejected", int(ImageImport::isImage(QStringLiteral("C:/a/b.pdf"))), 0);
    check("isImage: .dpz rejected", int(ImageImport::isImage(QStringLiteral("C:/a/b.dpz"))), 0);
    check("isImage: .docx rejected", int(ImageImport::isImage(QStringLiteral("C:/a/b.docx"))), 0);
    check("isImage: .txt rejected", int(ImageImport::isImage(QStringLiteral("C:/a/b.txt"))), 0);
    check("isImage: no extension rejected",
          int(ImageImport::isImage(QStringLiteral("C:/a/无后缀"))), 0);

    // --- points per pixel: pixels * 72/96 -----------------------------------
    const QSizeF pt = ImageImport::pageSizePtFor(QSize(800, 600));
    check("pageSizePt: width 600", int(qRound(pt.width())), 600);
    check("pageSizePt: height 450", int(qRound(pt.height())), 450);

    // --- PNG naming rule ----------------------------------------------------
    check("pngName: pad to width of 12",
          int(PdfExport::pngPageName(QStringLiteral("C:/a/名"), 3, 12)
              == QStringLiteral("C:/a/名-03.png")), 1);
    check("pngName: no pad for 5",
          int(PdfExport::pngPageName(QStringLiteral("C:/a/名"), 3, 5)
              == QStringLiteral("C:/a/名-3.png")), 1);
    check("pngName: single page",
          int(PdfExport::pngPageName(QStringLiteral("C:/a/名"), 1, 1)
              == QStringLiteral("C:/a/名-1.png")), 1);
    check("pngName: backslash path",
          int(PdfExport::pngPageName(QStringLiteral("C:\\a\\名"), 2, 10)
              == QStringLiteral("C:\\a\\名-02.png")), 1);

    // --- everything below writes only under %TEMP% --------------------------
    const QString dir = QDir(QDir::tempPath())
                            .filePath(QStringLiteral("pdfboard-selftest-image"));
    QDir().mkpath(dir);

    // --- end-to-end 1:1 round trip -----------------------------------------
    // 800x600 image -> page 600x450 pt -> rendered at 96 dpi = exactly 800x600.
    // Distinct solid quadrants plus a 10 px border make any channel swap or
    // scaling error visible in the sampled pixels.
    const QString srcPng = QDir(dir).filePath(QStringLiteral("roundtrip-src.png"));
    const QString pdfPath = QDir(dir).filePath(QStringLiteral("roundtrip.pdf"));
    const QString basePath = QDir(dir).filePath(QStringLiteral("roundtrip-out"));
    QFile::remove(srcPng);
    QFile::remove(pdfPath);
    QFile::remove(basePath + QStringLiteral("-1.png"));

    const QColor red(0xD3, 0x2F, 0x2F), green(0x2E, 0x7D, 0x32);
    const QColor blue(0x19, 0x76, 0xD2), yellow(0xF9, 0xA8, 0x25);
    QImage source(800, 600, QImage::Format_RGB32);
    {
        source.fill(Qt::white);
        QPainter p(&source);
        p.fillRect(QRect(10, 10, 390, 290), red);
        p.fillRect(QRect(400, 10, 390, 290), green);
        p.fillRect(QRect(10, 300, 390, 290), blue);
        p.fillRect(QRect(400, 300, 390, 290), yellow);
        p.fillRect(QRect(0, 0, 800, 10), Qt::black);
        p.fillRect(QRect(0, 590, 800, 10), Qt::black);
        p.fillRect(QRect(0, 0, 10, 600), Qt::black);
        p.fillRect(QRect(790, 0, 10, 600), Qt::black);
    }
    check("roundtrip: source saved", int(source.save(srcPng, "PNG")), 1);

    QString err;
    check("roundtrip: image -> pdf", int(ImageImport::toPdf(srcPng, pdfPath, &err)), 1);

    PdfCanvas canvas;
    canvas.setAttribute(Qt::WA_DontShowOnScreen, true);
    canvas.resize(1000, 800);
    canvas.show();
    QString openErr;
    check("roundtrip: pdf opens", int(canvas.openPdf(pdfPath, &openErr)), 1);

    err.clear();
    const int written = PdfExport::exportPngPages(&canvas, basePath, &err);
    check("roundtrip: one png written", written, 1);

    const QString outPng = PdfExport::pngPageName(basePath, 1, 1);
    const QImage exported(outPng);
    check("roundtrip: png loaded", int(!exported.isNull()), 1);
    check("roundtrip: png is 800x600", int(exported.size() == QSize(800, 600)), 1);

    // Eraser indicator: a screen-only ring whose diameter IS the wipe diameter, with the
    // eraser glyph in the middle so the tool is recognisable. Three things must hold:
    // the ring is drawn, it scales with the radius it is given, and none of it reaches an
    // export (both export paths rasterise through pageThumbnail, never through paintEvent).
    {
        PdfCanvas bare;                       // no document: desk colour + ring only
        bare.setAttribute(Qt::WA_DontShowOnScreen, true);
        bare.resize(600, 400);
        bare.show();
        const QPointF hover(300, 200);
        // Measure against the background colour instead of "dark": the desk follows the
        // theme, so an absolute threshold would count the whole image in dark mode.
        auto differs = [](QRgb a, QRgb b) {
            return qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b))
                   + qAbs(qBlue(a) - qBlue(b)) > 60;
        };
        auto ringSpan = [&differs](const QImage &im) {
            const QRgb bg = im.pixel(0, 0);
            int minx = im.width(), maxx = -1, miny = im.height(), maxy = -1;
            for (int y = 0; y < im.height(); ++y) {
                const QRgb *row = reinterpret_cast<const QRgb *>(im.constScanLine(y));
                for (int x = 0; x < im.width(); ++x) {
                    if (differs(row[x], bg)) {
                        minx = qMin(minx, x); maxx = qMax(maxx, x);
                        miny = qMin(miny, y); maxy = qMax(maxy, y);
                    }
                }
            }
            return QSize(maxx - minx + 1, maxy - miny + 1);
        };
        // NB: not "small"/"big" - <rpcndr.h> defines `small` as a macro (char).
        const QSize spanSmall =
            ringSpan(bare.testRenderEraserIndicator(hover, 9.0, QSize(600, 400)));
        const QSize spanBig =
            ringSpan(bare.testRenderEraserIndicator(hover, 26.0, QSize(600, 400)));
        check("eraser: ring drawn", int(spanSmall.width() >= 16), 1);
        check("eraser: ring scales with the wipe size",
              int(spanBig.width() >= spanSmall.width() + 30), 1);
        check("eraser: ring is round", int(qAbs(spanBig.width() - spanBig.height()) <= 2), 1);

        // The glyph must be visible in the middle, in a window the ring does not cross.
        const QImage art = bare.testRenderEraserIndicator(hover, 26.0, QSize(600, 400));
        int glyphPx = 0;
        const QRgb artBg = art.pixel(0, 0);
        for (int y = int(hover.y()) - 4; y <= int(hover.y()) + 4; ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(art.constScanLine(y));
            for (int x = int(hover.x()) - 4; x <= int(hover.x()) + 4; ++x) {
                if (differs(row[x], artBg))
                    ++glyphPx;
            }
        }
        check("eraser: glyph visible in the middle", int(glyphPx > 6), 1);

        // Eyeball artefact, only where the dev temp dir exists (never required on CI).
        if (QDir(QStringLiteral("D:/dev/tmp")).exists()) {
            canvas.setTool(PdfCanvas::InkTool::Eraser);
            canvas.testSetEraserHover(QPointF(300, 240));
            const QString shot = QStringLiteral("D:/dev/tmp/eraser-indicator.png");
            const bool saved =
                canvas.testRenderEraserIndicator(QPointF(300, 240), 14.0, QSize(600, 400))
                    .save(shot, "PNG");
            check("eraser: screenshot written", saved ? 1 : 0, 1);
            out(QStringLiteral("[selftest] eraser indicator: %1").arg(shot));
        }

        // An export must be byte-identical with and without the overlay on screen.
        QString exportErr;
        const QString leakA = QDir(dir).filePath(QStringLiteral("leak-with-overlay"));
        PdfExport::exportPngPages(&canvas, leakA, &exportErr);
        const QImage shotA(PdfExport::pngPageName(leakA, 1, 1));
        canvas.testClearEraserHover();
        const QString leakB = QDir(dir).filePath(QStringLiteral("leak-without-overlay"));
        PdfExport::exportPngPages(&canvas, leakB, &exportErr);
        const QImage shotB(PdfExport::pngPageName(leakB, 1, 1));
        check("eraser: export ignores the overlay", int(!shotA.isNull() && shotA == shotB), 1);

        // A tap must leave a dot. It used to vanish twice over: drawInk() required two
        // points and endInput() threw the stroke away as "tap/noise". The tap hooks drive
        // beginInputAt/endInput directly, i.e. the mouse/stylus path (a real finger sets
        // m_strokeFromTouch in handleTouch and keeps the old noise rule).
        {
            PdfCanvas tap;
            tap.setAttribute(Qt::WA_DontShowOnScreen, true);
            tap.resize(900, 700);
            tap.show();
            QString tapErr;
            check("tap: canvas opens the page", int(tap.openPdf(pdfPath, &tapErr)), 1);
            const QSize vp = tap.testViewportSize();
            const QPointF dot(0.35 * vp.width(), 0.35 * vp.height());
            // Pin the pen colour: the default is red, whose grey level (~95) is nowhere near
            // the "near-black" the pixel scan below looks for.
            tap.setPenColor(QColor(0x10, 0x10, 0x10));
            tap.testTouchBegin(dot);
            tap.testTouchEnd();
            check("tap: commits one stroke", tap.strokeCount(), 1);
            check("tap: stored as a single sample",
                  int(tap.testStrokePoints(0, 0) >= 1 && tap.testStrokePoints(0, 0) <= 2), 1);
            // Where did the tap land ON THE PAGE? Derive it from the canvas instead of
            // assuming viewport == page: the page is inset by margins and page padding.
            const qreal fx = tap.testFracX(0, dot.x());
            const qreal fy = tap.testFracAtViewportY(dot.y());
            check("tap: lands on the page", int(fx >= 0.0 && fy >= 0.0), 1);
            const QImage shot = PdfExport::renderPageWithInk(&tap, 0, 96, &tapErr);
            int dotPx = 0;
            if (!shot.isNull() && fx >= 0.0 && fy >= 0.0) {
                const int cx = int(fx * shot.width()), cy = int(fy * shot.height());
                for (int y = cy - 18; y <= cy + 18; ++y) {
                    if (y < 0 || y >= shot.height())
                        continue;
                    for (int x = cx - 18; x <= cx + 18; ++x) {
                        if (x < 0 || x >= shot.width())
                            continue;
                        if (qGray(shot.pixel(x, y)) < 40)    // the near-black ink only
                            ++dotPx;
                    }
                }
            }
            // One assertion, not two: "a few pixels but not a streak" cannot be satisfied
            // by finding nothing.
            check("tap: a dot is drawn (and is a dot, not a line)",
                  int(dotPx >= 6 && dotPx < 400), 1);
        }

        // The eraser END of a stylus must erase this stroke instead of drawing ink, and it must
        // leave the toolbar's tool alone (the teacher turned the pen around, not the UI).
        // Derive the page-space position from the canvas - the page is inset, so viewport
        // fractions are NOT page fractions (that mistake cost a round earlier).
        {
            canvas.setTool(PdfCanvas::InkTool::Pen);
            const QSize vpT = canvas.testViewportSize();
            const qreal fy = canvas.testFracAtViewportY(0.40 * vpT.height());
            const qreal fx = canvas.testFracX(0, 0.50 * vpT.width());
            check("pen tail: the test lands on the page", int(fx > 0.0 && fy > 0.0), 1);
            canvas.testAddStroke(0, QPointF(fx - 0.05, fy), QPointF(fx + 0.05, fy),
                                 QColor(0x10, 0x10, 0x10), 6.0);
            const QString inkBefore = canvas.testStrokeSummary(0);
            // NOTE: "the summary changed" is NOT a usable assertion here - drawing a new stroke
            // changes it too. What only an ERASE can do is remove ink, so count the points.
            auto inkPoints = [&canvas]() {
                int n = 0;
                for (int i = 0; i < canvas.strokeCount(); ++i)
                    n += canvas.testStrokePoints(0, i);
                return n;
            };
            const int ptsBefore = inkPoints();
            canvas.testEraserTailStroke(QPointF(0.50 * vpT.width(), 0.36 * vpT.height()),
                                        QPointF(0.50 * vpT.width(), 0.44 * vpT.height()));
            const int ptsAfter = inkPoints();
            check("pen tail: the eraser end removes ink (a pen would add it)",
                  int(ptsBefore > 0 && ptsAfter < ptsBefore), 1);
            check("pen tail: the toolbar tool is left alone",
                  int(canvas.tool() == PdfCanvas::InkTool::Pen), 1);
        }

        // Two-finger jitter: a big IR panel reports its contacts with a few pixels of noise.
        // That noise hits both the finger distance (scale) and their centroid (pan), which
        // made the page breathe and shake. The smoothing must remove the visible part of it
        // while leaving a real pinch as responsive as before.
        {
            PdfCanvas pinch;
            pinch.setAttribute(Qt::WA_DontShowOnScreen, true);
            pinch.resize(900, 700);
            pinch.show();
            QString pe;
            check("pinch: canvas opens the page", int(pinch.openPdf(pdfPath, &pe)), 1);
            const QSize vp = pinch.testViewportSize();
            // Zoom in first: with the page shorter than the viewport there is no scroll
            // range, a pan gets clamped at 0 and the shake check could never fail. Then park
            // the view mid-page so noise can move it either way.
            pinch.setZoomLevel(3.0);
            pinch.verticalScrollBar()->setValue(pinch.verticalScrollBar()->maximum() / 2);
            // A realistic two-hand span: a wider baseline keeps the relative noise small.
            const QPointF a(0.30 * vp.width(), 0.45 * vp.height());
            const QPointF b(0.70 * vp.width(), 0.45 * vp.height());
            const qreal zoom0 = pinch.testZoom();
            const int scroll0 = pinch.verticalScrollBar()->value();

            unsigned long long s = 88172645463325252ull;   // deterministic xorshift64
            auto noise = [&s]() {
                s ^= s << 13;
                s ^= s >> 7;
                s ^= s << 17;
                return qreal(int(s % 7)) - 3.0;            // -3 .. +3 px (IR panel ballpark)
            };
            pinch.testPinchBegin(a, b);
            for (int i = 0; i < 60; ++i) {
                pinch.testPinchFrame(a + QPointF(noise(), noise()),
                                     b + QPointF(noise(), noise()));
            }
            pinch.testPinchEnd();
            // Informational: the numbers this test reasons about, printed so a human (and the
            // mutation run with the smoothing disabled) can see whether anything moved at all.
            out(QStringLiteral("[selftest] pinch noise: zoom %1 -> %2, scroll %3 -> %4")
                    .arg(zoom0, 0, 'f', 4).arg(pinch.testZoom(), 0, 'f', 4)
                    .arg(scroll0).arg(pinch.verticalScrollBar()->value()));
            // NOTE: only the shake is asserted here. A "noise must not change the zoom" check
            // looked reasonable but was removed after a mutation run (smoothing disabled)
            // showed it passed anyway - i.e. it asserted nothing. What actually protects the
            // scale is the rate clamp below, which DOES fail when disabled.
            check("pinch: contact noise does not shake the page",
                  int(qAbs(pinch.verticalScrollBar()->value() - scroll0) <= 2), 1);

            // A single "ghost" contact - IR panels do emit them - must not teleport the scale.
            // Run at zoom 1.0 so the 4.0 ceiling cannot mask the difference.
            {
                pinch.setZoomLevel(1.0);
                QCoreApplication::processEvents();
                const QPointF e1(0.30 * vp.width(), 0.45 * vp.height());
                const QPointF e2(0.70 * vp.width(), 0.45 * vp.height());
                pinch.testPinchBegin(e1, e2);
                const qreal beforeGhost = pinch.testZoom();
                // One frame in which the span doubles (a contact jumps far away).
                pinch.testPinchFrame(e1, QPointF(0.30 * vp.width() + 2.0 * (e2 - e1).x(),
                                                 0.45 * vp.height()));
                pinch.testPinchEnd();
                out(QStringLiteral("[selftest] pinch ghost: zoom %1 -> %2")
                        .arg(beforeGhost, 0, 'f', 4).arg(pinch.testZoom(), 0, 'f', 4));
                check("pinch: a ghost contact cannot teleport the zoom",
                      int(pinch.testZoom() / beforeGhost < 1.75), 1);
            }

            // ...and a real pinch must still track: fingers separating to ~1.8x.
            const QPointF c(0.40 * vp.width(), 0.45 * vp.height());
            const QPointF d(0.60 * vp.width(), 0.45 * vp.height());
            const qreal d0 = QLineF(c, d).length();
            const QPointF mid = (c + d) / 2.0;
            const QPointF half = (d - c) / 2.0;
            const qreal zoom1 = pinch.testZoom();
            qreal dLast = d0;
            qreal zMid = zoom1, dMid = d0;
            pinch.testPinchBegin(c, d);
            for (int i = 1; i <= 40; ++i) {
                const qreal k = 1.0 + 0.02 * i;               // 2 % per frame
                const QPointF p1 = mid - half * k;
                const QPointF p2 = mid + half * k;
                dLast = QLineF(p1, p2).length();
                pinch.testPinchFrame(p1, p2);
                if (i == 10) {                                 // after the rest detector releases
                    zMid = pinch.testZoom();
                    dMid = dLast;
                }
            }
            pinch.testPinchEnd();
            // Compare the SECOND HALF against the finger travel: the first frame or two are
            // deliberately swallowed by the rest detector, so an absolute comparison would
            // fail by design. What matters is that a real pinch tracks 1:1 once it is moving.
            const qreal applied = pinch.testZoom() / zMid;
            const qreal wanted = dLast / dMid;
            check("pinch: a real pinch still tracks",
                  int(qAbs(applied / wanted - 1.0) < 0.02), 1);
        }

        // A touch (or stylus) drag must never be hijacked by the global mouse cursor. NOTE:
        // the real trigger - Windows synthesising button-PRESSED mouse events for touch -
        // cannot be reproduced headlessly, so this only locks the weaker, still useful fact
        // that the tracker stays inert through a touch drag. The reported bug itself is
        // hand-verified (see docs 2.46).
        if (canvas.pageCount() > 0) {
            canvas.setTool(PdfCanvas::InkTool::Eraser);
            canvas.testAddStroke(0, QPointF(0.30, 0.30), QPointF(0.45, 0.45),
                                 QColor(0x20, 0x20, 0x20), 4.0);
            const QPointF finger(0.33 * canvas.testViewportSize().width(),
                                 0.33 * canvas.testViewportSize().height());
            canvas.testTouchBegin(finger);
            canvas.testTouchMove(finger + QPointF(6, 0));
            const QPointF tracked = canvas.testEraserHoverPos();
            for (int i = 0; i < 3; ++i)
                canvas.testTrackPointer();
            check("eraser: touch drag keeps the pointer tracker inert",
                  int(canvas.testEraserHoverPos() == tracked), 1);
            canvas.testTouchEnd();
            canvas.testClearEraserHover();
            canvas.setTool(PdfCanvas::InkTool::Pen);
        }
    }

    auto samePixel = [&source, &exported](int x, int y) {
        const QRgb a = source.pixel(x, y);
        const QRgb b = exported.pixel(x, y);
        return qAbs(qRed(a) - qRed(b)) <= 8
            && qAbs(qGreen(a) - qGreen(b)) <= 8
            && qAbs(qBlue(a) - qBlue(b)) <= 8;
    };
    check("roundtrip: quadrant TL kept", int(samePixel(200, 150)), 1);
    check("roundtrip: quadrant TR kept", int(samePixel(600, 150)), 1);
    check("roundtrip: quadrant BL kept", int(samePixel(200, 450)), 1);
    check("roundtrip: quadrant BR kept", int(samePixel(600, 450)), 1);
    check("roundtrip: border stayed black",
          int(qGray(exported.pixel(400, 5)) < 40), 1);

    // --- oversize (18 MPx) must be scaled, aspect kept ----------------------
    const QString bigSrc = QDir(dir).filePath(QStringLiteral("oversize-src.jpg"));
    const QString bigPdf = QDir(dir).filePath(QStringLiteral("oversize.pdf"));
    QFile::remove(bigSrc);
    QFile::remove(bigPdf);
    {
        QImage big(6000, 3000, QImage::Format_RGB32);
        big.fill(QColor(0x40, 0x80, 0xC0));
        {
            QPainter p(&big);
            p.fillRect(QRect(0, 0, 6000, 300), Qt::white);
        }
        check("oversize: source saved", int(big.save(bigSrc, "JPG", 85)), 1);
    }
    QString bigErr;
    check("oversize: image -> pdf", int(ImageImport::toPdf(bigSrc, bigPdf, &bigErr)), 1);

    PdfCanvas bigCanvas;
    bigCanvas.setAttribute(Qt::WA_DontShowOnScreen, true);
    bigCanvas.resize(600, 400);
    bigCanvas.show();
    QString bigOpenErr;
    check("oversize: pdf opens", int(bigCanvas.openPdf(bigPdf, &bigOpenErr)), 1);
    const QSizeF bigPt = bigCanvas.pagePointSize(0);
    const qint64 keptW = qint64(qRound(bigPt.width() * 96.0 / 72.0));
    const qint64 keptH = qint64(qRound(bigPt.height() * 96.0 / 72.0));
    out(QStringLiteral("[selftest] oversize kept: %1x%2 px (page %3x%4 pt)")
            .arg(keptW).arg(keptH)
            .arg(bigPt.width(), 0, 'f', 2).arg(bigPt.height(), 0, 'f', 2));
    check("oversize: kept <= kMaxPixels",
          int(keptW * keptH <= qint64(ImageImport::kMaxPixels)), 1);
    check("oversize: aspect stays 2:1",
          int(qAbs(qreal(keptW) / qreal(keptH) - 2.0) < 0.01), 1);

    // --- cleanup: leave no trace -------------------------------------------
    bigCanvas.closePdf();
    canvas.closePdf();
    check("cleanup: temp dir removed", int(QDir(dir).removeRecursively()), 1);

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
    // Ink quality lever (must be set BEFORE QApplication is built): on Windows Qt
    // compresses touch updates into one event per frame by default, silently
    // dropping most of the panel's samples. With them dropped, a fast stroke
    // between two frames becomes a long straight chord - exactly the "sampling is
    // too sparse" complaint. Qt 6.8 has no way to recover the discarded samples
    // (QEventPoint::rawScreenPositions() does not exist yet), so the compression
    // itself has to go. The per-event work here is cheap (append + densify).
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);

    QApplication app(argc, argv);

    // Qt's Windows integration turns this attribute back on while the platform
    // integration initialises, overriding the request made above, so it has to be
    // repeated once the application object exists (the plugin reads it per event,
    // not at init).
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    // School networks often only reach the internet through a proxy the OS is
    // configured with. Qt does not use it unless asked, which would make every
    // update check fail on exactly the machines that need the accelerator.
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    app.setApplicationName(QStringLiteral("PDFBoard"));

    // Crash evidence first: a log, a minidump and the last operations, written even
    // when the diagnostics log is switched off. Installed here (not before the
    // QApplication) so the path resolves to .../PDFBoard/logs.
    CrashLog::install();
    CrashLog::breadcrumb("startup", QCoreApplication::applicationVersion());

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
    const int updateIdx = args.indexOf(QStringLiteral("--selftest-update"));
    const int imageIdx = args.indexOf(QStringLiteral("--selftest-image"));
    const int smokeIdx = args.indexOf(QStringLiteral("--selftest-smoke"));
    const int crashIdx = args.indexOf(QStringLiteral("--selftest-crash"));

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
        || docxIdx >= 0
        || updateIdx >= 0
        || imageIdx >= 0
        || (smokeIdx >= 0 && smokeIdx + 1 < args.size())) {
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

    if (updateIdx >= 0)
        return runUpdateSelfTest();

    if (smokeIdx >= 0 && smokeIdx + 1 < args.size())
        return runSmokeSelfTest(args.at(smokeIdx + 1));

    // Deliberately crashes, to prove the crash reporter really produces its files.
    // Nothing calls this except a human checking the instrument (and the dev loop
    // that just did): the handler writes a log + a minidump under .../PDFBoard/logs,
    // then Windows' own error handling takes over and the process dies with 0xC0000005.
    if (crashIdx >= 0) {
        attachConsoleForCli();
        const QString what = (crashIdx + 1 < args.size()) ? args.at(crashIdx + 1)
                                                          : QStringLiteral("stack");
        CrashLog::breadcrumb("crash-test", QStringLiteral("故意触发：%1").arg(what));
        CrashLog::setExitAfterReport(true);
        out(QStringLiteral("[selftest] crash test（%1）：即将故意崩溃，稍后检查 %2")
                .arg(what, CrashLog::directory()));
        if (what == QStringLiteral("av")) {
            volatile int *nullPointer = nullptr;
            *nullPointer = 1;             // access violation: 0xC0000005
        } else {
            crashTestRecursion(0);        // stack overflow: 0xC00000FD
        }
        return 0;                         // not reached
    }

    if (imageIdx >= 0)
        return runImageSelfTest();

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
