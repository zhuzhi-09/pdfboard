#include "MainWindow.h"
#include "AppLog.h"
#include "AppSettings.h"
#include "MemProbe.h"
#include "PdfCanvas.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QElapsedTimer>
#include <QImage>
#include <QPdfDocument>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QStringList>
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

    // The shipping build is a GUI executable (no console window when the user
    // double-clicks it). The console-based modes still need their output, so
    // attach to the launching terminal - but only when stdout was NOT already
    // redirected, otherwise we would clobber the caller's capture.
    if ((benchIdx >= 0 && benchIdx + 1 < args.size())
        || (stIdx >= 0 && stIdx + 1 < args.size())
        || logIdx >= 0) {
        attachConsoleForCli();
    }

    if (benchIdx >= 0 && benchIdx + 1 < args.size())
        return runBench(args.at(benchIdx + 1));

    if (stIdx >= 0 && stIdx + 1 < args.size())
        return runInkSelfTest(args.at(stIdx + 1));

    if (logIdx >= 0)
        return runLogSelfTest();

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
