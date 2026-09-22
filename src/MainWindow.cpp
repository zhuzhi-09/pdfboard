#include "MainWindow.h"
#include "AnnotationBundle.h"
#include "AppLog.h"
#include "AppSettings.h"
#include "DocumentTabs.h"
#include "PdfCanvas.h"
#include "PdfExport.h"
#include "InkToolbar.h"
#include "MemProbe.h"
#include "SettingsPage.h"
#include "Theme.h"

#include <QAction>
#include <QAbstractButton>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {
// The status bar as a quiet strip of information pills instead of the default
// Qt chrome. Its content is unchanged: 页码 / 渲染 / 笔画 / 内存.
QString statusSheet()
{
    const Theme::Palette &c = Theme::light();
    return QStringLiteral(
               "QStatusBar {"
               " background: %1;"
               " border-top: 1px solid %2;"
               " color: %3; }"
               "QStatusBar::item { border: none; }"
               "QStatusBar QLabel {"
               " color: %3;"
               " background: %4;"
               " border-radius: %5;"
               " padding: %6 %7; }")
        .arg(Theme::rgba(c.surface))
        .arg(Theme::rgba(c.divider))
        .arg(Theme::rgba(c.textMuted))
        .arg(Theme::rgba(c.chipTint))
        .arg(Theme::px(Theme::RadiusPill))
        .arg(Theme::px(Theme::Space1))
        .arg(Theme::px(Theme::Space3 - 2));
}

// True when the mime data carries at least one local PDF or `.dpz` bundle.
bool hasLocalDocument(const QMimeData *mime)
{
    if (!mime || !mime->hasUrls())
        return false;
    const QList<QUrl> urls = mime->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
            || path.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

// Canonical identity of a local file, used to recognise a document that is
// already open when the same path arrives through a drop or the file dialog.
QString fileKey(const QString &path)
{
    const QFileInfo info(path);
    const QString canon = info.canonicalFilePath();
    return canon.isEmpty() ? info.absoluteFilePath() : canon;
}

// Reads a whole file, or returns an empty array when it cannot be opened.
QByteArray readPdfBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
}   // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("大屏 PDF 批注"));
    setAcceptDrops(true);           // a PDF dropped on the window opens a tab

    // The page area is a stack of canvases (one document each) and the tab
    // strip is the docked row right below it - under the floating island, so
    // the island keeps hovering over the pages of the active document.
    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_stack = new QStackedWidget(central);
    layout->addWidget(m_stack, 1);
    m_tabs = new DocumentTabs(central);
    layout->addWidget(m_tabs);
    setCentralWidget(central);

    connect(m_tabs, &DocumentTabs::currentChanged, this, &MainWindow::onTabCurrentChanged);
    connect(m_tabs, &DocumentTabs::closeRequested, this, &MainWindow::onTabCloseRequested);
    connect(m_tabs, &DocumentTabs::addRequested,    this, &MainWindow::onOpen);
    connect(m_tabs, &DocumentTabs::settingsRequested, this, &MainWindow::onSettings);

    // One blank canvas at startup: it shows the empty state, and the first
    // document reuses it instead of adding a tab.
    m_active = createCanvas();
    m_stack->setCurrentWidget(m_active);

    // The settings page is one more page of the stack, next to the canvases.
    // It is not a document: it stays out of m_canvases and closing tabs never
    // touches it.
    m_settingsPage = new SettingsPage(m_stack);
    m_stack->addWidget(m_settingsPage);

    // Ctrl+O / Ctrl+T open a document; Ctrl+W closes the active one.
    auto *openAct = new QAction(this);
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::onOpen);
    addAction(openAct);

    auto *newAct = new QAction(this);
    newAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    connect(newAct, &QAction::triggered, this, &MainWindow::onOpen);
    addAction(newAct);

    auto *closeAct = new QAction(this);
    closeAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    connect(closeAct, &QAction::triggered, this, &MainWindow::onCloseCurrent);
    addAction(closeAct);

    // Ctrl+S saves the current document (bundle by default); Ctrl+Shift+S is
    // always the format chooser (bundle or inject-into-PDF).
    auto *saveAct = new QAction(this);
    saveAct->setShortcut(QKeySequence::Save);
    connect(saveAct, &QAction::triggered, this, &MainWindow::onSave);
    addAction(saveAct);

    auto *saveAsAct = new QAction(this);
    saveAsAct->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAct, &QAction::triggered, this, &MainWindow::onSaveAs);
    addAction(saveAsAct);

    // The status bar follows the ACTIVE canvas (see wireActiveCanvas).
    m_pageLabel   = new QLabel(QStringLiteral("页码 -/-"), this);
    m_renderLabel = new QLabel(QStringLiteral("渲染 -"), this);
    m_inkLabel    = new QLabel(QStringLiteral("笔画 0"), this);
    m_memLabel    = new QLabel(QStringLiteral("内存 -"), this);
    const QFont statusFont = Theme::scaledFont(font(), 0.95, QFont::Medium);
    m_pageLabel->setFont(statusFont);
    m_renderLabel->setFont(statusFont);
    m_inkLabel->setFont(statusFont);
    m_memLabel->setFont(statusFont);
    statusBar()->setStyleSheet(statusSheet());
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(m_pageLabel);
    statusBar()->addWidget(m_renderLabel);
    statusBar()->addWidget(m_inkLabel);
    statusBar()->addPermanentWidget(m_memLabel);

    wireActiveCanvas(m_active);
    updateTitle();

    auto *memTimer = new QTimer(this);
    memTimer->setInterval(1500);
    connect(memTimer, &QTimer::timeout, this, &MainWindow::updateMemLabel);
    memTimer->start();
    updateMemLabel();

    // Size to the work area (excludes the taskbar) so the floating toolbar at
    // the bottom of the viewport is never hidden behind it.
    QScreen *scr = screen();
    if (!scr)
        scr = QGuiApplication::primaryScreen();
    if (scr) {
        const QRect avail = scr->availableGeometry();
        const QSize want(qMin(1400, int(avail.width()  * 0.92)),
                         qMin(900,  int(avail.height() * 0.92)));
        resize(want);
        move(avail.center() - QPoint(want.width() / 2, want.height() / 2));
    } else {
        resize(1400, 900);
    }
}

PdfCanvas *MainWindow::createCanvas()
{
    auto *canvas = new PdfCanvas(m_stack);
    m_stack->addWidget(canvas);

    // Each canvas owns its own floating island; the host supplies the actions
    // the island cannot do itself.
    if (InkToolbar *bar = canvas->toolbar()) {
        // Opening a document is done from the tab strip's 「打开」 chip.
        connect(bar, &InkToolbar::settingsRequested, this, &MainWindow::onSettings);
        connect(bar, &InkToolbar::saveRequested, this, &MainWindow::onSave);
        connect(bar, &InkToolbar::saveAsRequested, this, &MainWindow::onSaveAs);
    }
    return canvas;
}

void MainWindow::onOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开 PDF / 批注包"), QString(),
        QStringLiteral("PDF 与批注包 (*.pdf *.dpz);;PDF 文件 (*.pdf);;批注包 (*.dpz);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    openPath(path);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (hasLocalDocument(e->mimeData()))
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QMimeData *mime = e->mimeData();
    if (!mime)
        return;
    const QList<QUrl> urls = mime->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive)
            && !path.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive))
            continue;
        e->acceptProposedAction();
        openPath(path);      // opens a new tab (or activates the existing one)
        return;
    }
}

void MainWindow::openPath(const QString &path)
{
    QElapsedTimer timer;
    timer.start();
    const bool bundle = AnnotationBundle::isBundle(path);
    const QString key = fileKey(path);

    // Already open somewhere? Bring that tab forward instead of opening twice.
    for (int i = 0; i < m_canvases.size(); ++i) {
        const DocumentInfo &doc = m_docs.at(i);
        const QString open = bundle ? doc.bundlePath : doc.sourcePdf;
        if (!open.isEmpty() && fileKey(open) == key) {
            m_tabs->setCurrentIndex(i);
            onTabCurrentChanged(i);              // idempotent safety net
            return;
        }
    }

    QByteArray pdfBytes;
    QJsonObject annotations;
    QString tempPath;
    QString tabTitle;

    if (bundle) {
        QString err;
        if (!AnnotationBundle::read(path, &pdfBytes, &annotations, &err)) {
            AppLog::write(QStringLiteral("open"),
                          QStringLiteral("批注包读取失败：%1（%2）").arg(path, err));
            statusBar()->showMessage(QStringLiteral("打开失败：%1").arg(err), 8000);
            return;
        }

        // Extract the embedded source once per bundle into the temp cache.
        const QString dir = QDir::temp().filePath(QStringLiteral("pdfboard"));
        QDir().mkpath(dir);
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha1).toHex());
        tempPath = QDir(dir).filePath(hash + QStringLiteral(".pdf"));
        if (!QFileInfo::exists(tempPath)) {
            QFile tf(tempPath);
            if (!tf.open(QIODevice::WriteOnly)
                || tf.write(pdfBytes) != pdfBytes.size()) {
                statusBar()->showMessage(QStringLiteral("打开失败：无法写入临时文件"), 8000);
                return;
            }
            tf.close();
        }
        tabTitle = QFileInfo(path).fileName();
    } else {
        tempPath = path;
        tabTitle = QFileInfo(path).fileName();
    }

    // An untouched empty canvas is reused instead of adding a tab.
    const bool reuse = (m_active && m_active->pdfPath().isEmpty());
    PdfCanvas *canvas = reuse ? m_active : createCanvas();

    QString err;
    if (!canvas->openPdf(tempPath, &err)) {
        AppLog::write(QStringLiteral("open"),
                      QStringLiteral("打开失败：%1（%2）").arg(path, err));
        if (!reuse) {
            m_stack->removeWidget(canvas);
            delete canvas;
        }
        statusBar()->showMessage(QStringLiteral("打开失败：%1").arg(err), 8000);
        return;
    }
    if (bundle)
        canvas->importInk(annotations);

    AppLog::write(QStringLiteral("open"),
                  QStringLiteral("%1：%2 页，%3 ms，批注 %4 条")
                      .arg(tabTitle)
                      .arg(canvas->pageCount())
                      .arg(timer.elapsed())
                      .arg(canvas->strokeCount()));

    DocumentInfo info;
    info.bundlePath = bundle ? path : QString();
    info.sourcePdf = tempPath;
    info.title = tabTitle;
    info.tempPdf = bundle ? tempPath : QString();

    m_canvases.append(canvas);
    m_docs.append(info);
    m_tabs->addTab(tabTitle);
    const int index = int(m_canvases.size()) - 1;
    m_tabs->setCurrentIndex(index);              // emits -> activates the tab
    onTabCurrentChanged(index);                  // idempotent safety net
}

int MainWindow::docIndex(PdfCanvas *canvas) const
{
    return canvas ? int(m_canvases.indexOf(canvas)) : -1;
}

QString MainWindow::docTitle(PdfCanvas *canvas) const
{
    const int index = docIndex(canvas);
    return index >= 0 ? m_docs.at(index).title : QString();
}

// Ctrl+S / the island's 「保存」: overwrite the existing bundle, or fall back to
// 另存为 when the document has no bundle of its own yet.
void MainWindow::onSave()
{
    if (!m_active || m_active->pdfPath().isEmpty())
        return;
    const int index = docIndex(m_active);
    if (index < 0)
        return;

    const DocumentInfo &info = m_docs.at(index);
    if (info.bundlePath.isEmpty()) {
        saveDocumentAs();
        return;
    }

    const QByteArray pdf = readPdfBytes(info.sourcePdf);
    if (pdf.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法读取源 PDF：%1").arg(info.sourcePdf));
        return;
    }

    QString err;
    if (!AnnotationBundle::write(info.bundlePath, pdf, m_active->exportInk(), &err)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("已保存 %1").arg(QFileInfo(info.bundlePath).fileName()), 4000);
}

// Ctrl+Shift+S / the island's 「另存为」: choose between a `.dpz` bundle
// (default) and a flattened, injected PDF that other software can open.
void MainWindow::saveDocumentAs()
{
    if (!m_active || m_active->pdfPath().isEmpty())
        return;
    const int index = docIndex(m_active);
    if (index < 0)
        return;
    const DocumentInfo info = m_docs.at(index);

    // Keep the filter text itself simple, otherwise Qt's suffix handling
    // mangles the pre-filled name when the format is switched.
    const QString bundleFilter = QStringLiteral("打包保存 (*.dpz)");
    const QString injectFilter = QStringLiteral("注入 PDF (*.pdf)");

    // Once a document is bundle-backed its `sourcePdf` is an internal temp copy;
    // never suggest that path or name to the user.
    const QString identity =
        info.bundlePath.isEmpty() ? info.sourcePdf : info.bundlePath;
    const QFileInfo src(identity);
    // Prefer the user's configured save folder; fall back to the document's own
    // directory when it is unset or has gone away.
    QString dir = AppSettings::defaultSavePath();
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        dir = src.absolutePath();
    QString base = src.completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("未命名");

    auto nameFor = [&base](bool inject) {
        return inject ? base + QStringLiteral("-已批注.pdf")
                      : base + QStringLiteral(".dpz");
    };

    QFileDialog dlg(this, QStringLiteral("另存为"), dir);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.setNameFilters({bundleFilter, injectFilter});
    dlg.selectNameFilter(bundleFilter);      // 打包保存 is the default
    dlg.selectFile(nameFor(false));
    // The "for sharing" guidance lives on the file-type label so the filter
    // strings stay clean.
    dlg.setLabelText(QFileDialog::FileType,
                     QStringLiteral("保存类型（注入 PDF 适合分享，其他软件可直接打开）"));
    // Re-fill the name on every format switch: Qt's own suffix juggling is what
    // garbled the suggestion.
    connect(&dlg, &QFileDialog::filterSelected, &dlg,
            [&dlg, &nameFor](const QString &f) {
                dlg.selectFile(nameFor(f.contains(QStringLiteral("*.pdf"))));
            });
    if (dlg.exec() != QDialog::Accepted)
        return;

    QString path = dlg.selectedFiles().value(0);
    if (path.isEmpty())
        return;

    const bool inject = (dlg.selectedNameFilter() == injectFilter);
    if (inject) {
        if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
            path += QStringLiteral(".pdf");
    } else {
        if (!path.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive))
            path += QStringLiteral(".dpz");
    }

    QString err;
    if (inject) {
        // Injecting never changes the open document's own bundle state.
        QElapsedTimer timer;
        timer.start();
        if (!PdfExport::exportFlattened(m_active, path, &err)) {
            AppLog::write(QStringLiteral("save"),
                          QStringLiteral("注入 PDF 失败：%1（%2）").arg(path, err));
            QMessageBox::warning(this, QStringLiteral("保存失败"), err);
            return;
        }
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("注入 PDF %1：%2 页，%3 KB，%4 ms")
                          .arg(QFileInfo(path).fileName())
                          .arg(m_active->pageCount())
                          .arg(double(QFileInfo(path).size()) / 1024.0, 0, 'f', 0)
                          .arg(timer.elapsed()));
        statusBar()->showMessage(
            QStringLiteral("已注入 PDF %1").arg(QFileInfo(path).fileName()), 4000);
        return;
    }

    const QByteArray pdf = readPdfBytes(info.sourcePdf);
    if (pdf.isEmpty()) {
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("读取源 PDF 失败：%1").arg(info.sourcePdf));
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法读取源 PDF：%1").arg(info.sourcePdf));
        return;
    }
    if (!AnnotationBundle::write(path, pdf, m_active->exportInk(), &err)) {
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("保存批注包失败：%1（%2）").arg(path, err));
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        return;
    }
    AppLog::write(QStringLiteral("save"),
                  QStringLiteral("保存批注包 %1：%2 KB，批注 %3 条")
                      .arg(QFileInfo(path).fileName())
                      .arg(double(QFileInfo(path).size()) / 1024.0, 0, 'f', 0)
                      .arg(m_active->strokeCount()));

    m_docs[index].bundlePath = path;
    m_docs[index].title = QFileInfo(path).fileName();
    m_tabs->setTabTitle(index, m_docs.at(index).title);
    updateTitle();
    statusBar()->showMessage(
        QStringLiteral("已打包保存 %1").arg(m_docs.at(index).title), 4000);
}

void MainWindow::onSaveAs()
{
    saveDocumentAs();
}

void MainWindow::onTabCurrentChanged(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return;

    PdfCanvas *next = m_canvases.at(index);
    const bool wasSettings = m_settingsVisible;

    // Activating a document chip always leaves the settings page (and clears
    // the gear chip's highlight).
    m_settingsVisible = false;
    m_tabs->setSettingsActive(false);

    if (next == m_active && !wasSettings) {
        wireActiveCanvas(next);
        updateTitle();
        return;
    }

    // Memory policy: the document we leave drops its rendered page bitmaps.
    // It keeps its document handle and its ink. Leaving the settings page for
    // the SAME document is not a document switch, so nothing is released.
    if (next != m_active && m_active)
        m_active->releaseCachedPages();

    m_active = next;
    m_stack->setCurrentWidget(next);
    wireActiveCanvas(next);
    updateTitle();
}

void MainWindow::onTabCloseRequested(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return;

    PdfCanvas *canvas = m_canvases.at(index);
    const bool wasActive = (canvas == m_active);
    m_canvases.removeAt(index);
    if (index < m_docs.size())
        m_docs.removeAt(index);

    // The strip selects the neighbour and reports it through currentChanged.
    m_tabs->removeTab(index);

    if (m_canvases.isEmpty()) {
        // Closing the last tab keeps one canvas alive in its empty state. When
        // the settings page is the visible one, it stays exactly where it is.
        canvas->closePdf();
        m_active = canvas;
        if (!m_settingsVisible) {
            m_stack->setCurrentWidget(canvas);
            wireActiveCanvas(canvas);
            updateTitle();
        }
        return;
    }

    if (wasActive) {
        const int next = qMin(index, int(m_canvases.size()) - 1);
        if (m_settingsVisible) {
            // Stay on the settings page: only remember which document is next.
            m_active = m_canvases.at(next);
        } else {
            if (m_tabs->currentIndex() != next)
                m_tabs->setCurrentIndex(next);       // emits -> activates
            if (m_active != m_canvases.at(next))
                onTabCurrentChanged(next);           // safety net
        }
    }

    m_stack->removeWidget(canvas);
    delete canvas;
}

void MainWindow::onCloseCurrent()
{
    const int index = m_canvases.indexOf(m_active);
    if (index >= 0)
        onTabCloseRequested(index);
}

void MainWindow::wireActiveCanvas(PdfCanvas *canvas)
{
    if (m_statusCanvas != canvas) {
        if (m_statusCanvas)
            disconnect(m_statusCanvas, nullptr, this, nullptr);
        m_statusCanvas = canvas;
        if (canvas) {
            connect(canvas, &PdfCanvas::pageChanged,    this, &MainWindow::onPageChanged);
            connect(canvas, &PdfCanvas::renderMeasured, this, &MainWindow::onRenderMeasured);
            connect(canvas, &PdfCanvas::inkChanged,     this, &MainWindow::onInkChanged);
        }
    }
    refreshStatus();     // the labels must show the newly active document
}

void MainWindow::refreshStatus()
{
    PdfCanvas *c = m_statusCanvas;
    onPageChanged(c ? c->currentPage() : 0, c ? c->pageCount() : 0);
    onInkChanged(c ? c->strokeCount() : 0);
    if (c && c->lastRenderMs() > 0)
        onRenderMeasured(c->lastRenderMs(), c->lastRenderSize());
    else
        m_renderLabel->setText(QStringLiteral("渲染 -"));
}

void MainWindow::updateTitle()
{
    if (m_settingsVisible) {
        setWindowTitle(QStringLiteral("设置 — 大屏 PDF 批注"));
        return;
    }

    const QString name = docTitle(m_active);
    setWindowTitle(name.isEmpty()
        ? QStringLiteral("大屏 PDF 批注")
        : QStringLiteral("%1 — 大屏 PDF 批注").arg(name));
}

// The island's 「设置」 button and the tab strip's Settings chip both land here.
// The chip doubles as a toggle: pressing it while the page is already up
// returns to the page the user came from - including the blank canvas.
void MainWindow::onSettings()
{
    if (m_settingsVisible) {
        showCanvasPage(m_active);
        return;
    }
    showSettingsPage();
}

void MainWindow::showSettingsPage()
{
    m_settingsVisible = true;
    m_tabs->setSettingsActive(true);
    m_stack->setCurrentWidget(m_settingsPage);
    // No document is active while the settings page is up: the status bar shows
    // the neutral values and the hidden canvas keeps its rendered pages.
    wireActiveCanvas(nullptr);
    updateTitle();
}

void MainWindow::showCanvasPage(PdfCanvas *canvas)
{
    if (!canvas)
        return;
    m_settingsVisible = false;
    m_tabs->setSettingsActive(false);
    m_active = canvas;
    m_stack->setCurrentWidget(canvas);
    wireActiveCanvas(canvas);
    updateTitle();
}

void MainWindow::onPageChanged(int page, int count)
{
    m_pageLabel->setText(count > 0
        ? QStringLiteral("页码 %1/%2").arg(page + 1).arg(count)
        : QStringLiteral("页码 -/-"));
}

void MainWindow::onRenderMeasured(qint64 ms, QSize size)
{
    m_renderLabel->setText(QStringLiteral("渲染 %1 ms (%2×%3)")
                               .arg(ms).arg(size.width()).arg(size.height()));
}

void MainWindow::onInkChanged(int strokes)
{
    m_inkLabel->setText(QStringLiteral("笔画 %1").arg(strokes));
}

void MainWindow::updateMemLabel()
{
    const MemInfo m = currentMemInfo();
    m_memLabel->setText(QStringLiteral("内存 WS %1 MB / Private %2 MB")
                            .arg(m.workingSetMB, 0, 'f', 1)
                            .arg(m.privateMB, 0, 'f', 1));
}
