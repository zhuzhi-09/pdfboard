#include "MainWindow.h"
#include "AnnotationBundle.h"
#include "AppLog.h"
#include "AppSettings.h"
#include "CrashLog.h"
#include "DocumentTabs.h"
#include "HomePage.h"
#include "ImageImport.h"
#include "PdfCanvas.h"
#include "PdfExport.h"
#include "InkToolbar.h"
#include "MemProbe.h"
#include "SettingsPage.h"
#include "Theme.h"
#include "WordConvert.h"
#include "ZoomBar.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>
#include <QUrl>
#include <QVBoxLayout>

#include <windows.h>
#include <dwmapi.h>

// The immersive dark title bar attribute: 20 on Windows 10 20H1+ / Windows 11.
// Older SDK headers may not define the name, so fall back to the raw value.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace {
// The platform's own palette, captured before the first theme apply, so the
// light modes can restore exactly what Windows gave us at startup.
QPalette &baseAppPalette()
{
    static QPalette pal;
    static bool captured = false;
    if (!captured && qApp) {
        pal = qApp->palette();
        captured = true;
    }
    return pal;
}

// Native dialogs and message boxes must not stay blinding white in dark mode.
QPalette darkAppPalette()
{
    const Theme::Palette &c = Theme::light();
    QPalette pal = baseAppPalette();
    pal.setColor(QPalette::Window, c.desk);
    pal.setColor(QPalette::WindowText, c.text);
    pal.setColor(QPalette::Base, c.surface);
    pal.setColor(QPalette::AlternateBase, c.deskShade);
    pal.setColor(QPalette::Text, c.text);
    pal.setColor(QPalette::Button, c.surface);
    pal.setColor(QPalette::ButtonText, c.text);
    pal.setColor(QPalette::ToolTipBase, c.surface);
    pal.setColor(QPalette::ToolTipText, c.text);
    pal.setColor(QPalette::Highlight, c.accent);
    pal.setColor(QPalette::HighlightedText, c.onAccent);
    pal.setColor(QPalette::PlaceholderText, c.textMuted);
    const QPalette::ColorRole disabledRoles[] = { QPalette::WindowText, QPalette::Text,
                                                  QPalette::ButtonText };
    for (QPalette::ColorRole role : disabledRoles)
        pal.setColor(QPalette::Disabled, role, c.textDisabled);
    return pal;
}

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

// True when the mime data carries at least one local PDF, `.dpz` bundle or
// Word document.
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
            || path.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive)
            || WordConvert::isWordDoc(path)
            || ImageImport::isImage(path))
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

// The 每次询问 chooser for Word documents: exactly TWO touch-sized choices,
// 批注 / 用 Word 打开, and deliberately no "remember my choice" control - the
// preference lives in the settings page only. Dismissing the dialog (Esc / X)
// is a cancel and opens nothing.
enum class WordOpenAnswer { Cancel, Annotate, OpenInWord };

WordOpenAnswer askWordOpen(QWidget *parent, const QString &fileName)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("打开 Word 文档"));
    dlg.setModal(true);

    auto *layout = new QVBoxLayout(&dlg);
    layout->setContentsMargins(Theme::Space5, Theme::Space5, Theme::Space5, Theme::Space5);
    layout->setSpacing(Theme::Space4);

    auto *question = new QLabel(QStringLiteral("「%1」要如何打开？").arg(fileName), &dlg);
    question->setWordWrap(true);
    layout->addWidget(question);

    auto *row = new QHBoxLayout;
    row->setSpacing(Theme::Space3);
    const int touch = Theme::metrics(dlg.font()).touch;
    auto *annotateButton = new QPushButton(QStringLiteral("批注"), &dlg);
    auto *wordButton = new QPushButton(QStringLiteral("用 Word 打开"), &dlg);
    const auto makeTouch = [touch](QPushButton *button) {
        button->setMinimumSize(QSize(touch * 2, touch));
        button->setCursor(Qt::PointingHandCursor);
    };
    makeTouch(annotateButton);
    makeTouch(wordButton);
    annotateButton->setDefault(true);
    row->addWidget(annotateButton, 1);
    row->addWidget(wordButton, 1);
    layout->addLayout(row);

    QObject::connect(annotateButton, &QPushButton::clicked, &dlg, [&dlg] { dlg.done(1); });
    QObject::connect(wordButton, &QPushButton::clicked, &dlg, [&dlg] { dlg.done(2); });

    const int result = dlg.exec();
    if (result == 1)
        return WordOpenAnswer::Annotate;
    if (result == 2)
        return WordOpenAnswer::OpenInWord;
    return WordOpenAnswer::Cancel;
}
}   // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // Capture the untouched platform palette before the first theme apply, so
    // the light modes can put everything back exactly as Windows had it.
    baseAppPalette();

    setWindowTitle(QStringLiteral("落墨·大屏批注"));
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
    connect(m_tabs, &DocumentTabs::homeRequested,     this, &MainWindow::showHomePage);

    // The home page is the startup page. It owns no canvas, so there is no
    // floating toolbar island on it; it requests opens through its signals.
    m_homePage = new HomePage(m_stack);
    m_stack->addWidget(m_homePage);
    connect(m_homePage, &HomePage::openRequested, this, &MainWindow::onOpen);
    connect(m_homePage, &HomePage::openPathRequested, this, &MainWindow::openPath);

    // The settings page is one more page of the stack, next to the canvases.
    // It is not a document: it stays out of m_canvases and closing tabs never
    // touches it.
    m_settingsPage = new SettingsPage(m_stack);
    m_stack->addWidget(m_settingsPage);

    // Picking 系统 / 浅色 / 深色 in the settings page re-applies the theme
    // everywhere. 系统 additionally follows the live OS colour scheme.
    connect(m_settingsPage, &SettingsPage::themeChanged, this, &MainWindow::applyTheme);

    // The settings page reports the Word-settings restore through the window's
    // status bar; the page itself owns no status bar.
    connect(m_settingsPage, &SettingsPage::statusMessage, this,
            [this](const QString &message) {
                if (statusBar())
                    statusBar()->showMessage(message, 6000);
            });

    // Update check: once shortly after launch, and again after each document opens
    // (throttled inside checkForUpdates). A newer release is announced at most once
    // per session, so opening twenty files in a lesson never means twenty dialogs;
    // the changelog stays in the Settings update card for whenever it is wanted.
    m_updateClient = new UpdateChecker::Client(this);
    connect(m_updateClient, &UpdateChecker::Client::checked, this,
            [this](const UpdateChecker::UpdateInfo &info) {
                if (m_settingsPage)
                    m_settingsPage->setUpdateInfo(info);
                if (UpdateChecker::shouldPrompt(info, m_updateAnnouncedVersion,
                                                UpdateChecker::currentVersion())) {
                    m_updateAnnouncedVersion = info.version;
                    promptUpdate(info);
                }
            });
    // A failed check stays silent: an offline classroom must never see a dialog
    // about the network.
    QTimer::singleShot(1500, this, [this] { checkForUpdates(false); });

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, &MainWindow::onSystemColorSchemeChanged);

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

    // F11 toggles fullscreen from anywhere; Esc leaves it (see keyPressEvent).
    // Zoom without a touchscreen, next to the Ctrl+wheel path: Ctrl+= / Ctrl+- /
    // Ctrl+0（适配宽度）. QKeySequence::ZoomIn/Out map to the platform's own keys.
    auto *zoomInAct = new QAction(this);
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAct, &QAction::triggered, this, [this] {
        if (m_active)
            m_active->zoomIn();
    });
    addAction(zoomInAct);

    auto *zoomOutAct = new QAction(this);
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAct, &QAction::triggered, this, [this] {
        if (m_active)
            m_active->zoomOut();
    });
    addAction(zoomOutAct);

    auto *fitWidthAct = new QAction(this);
    fitWidthAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+0")));
    connect(fitWidthAct, &QAction::triggered, this, [this] {
        if (m_active)
            m_active->setFitWidth(true);
    });
    addAction(fitWidthAct);

    auto *fullscreenAct = new QAction(this);
    fullscreenAct->setShortcut(QKeySequence(Qt::Key_F11));
    fullscreenAct->setShortcutContext(Qt::WindowShortcut);
    connect(fullscreenAct, &QAction::triggered, this, &MainWindow::onFullscreen);
    addAction(fullscreenAct);

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
    // 内存信息归到左侧：它右边的滑动条必须钉住不动，而这个标签的文本宽度每秒
    // 都在变（177.7 MB → 106.9 MB）。固定宽度也让左侧那一组不再互相推动。
    m_memLabel->setFixedWidth(QFontMetrics(statusFont)
                                  .horizontalAdvance(QStringLiteral("内存 WS 9999.9 MB / Private 9999.9 MB")));
    statusBar()->addWidget(m_memLabel);

    // Word-like zoom control at the far right of the status bar. It never invents a
    // zoom value: the active canvas reports its own zoom (pinch, Ctrl+wheel,
    // shortcuts) and the control just displays it. Hidden on the home / settings
    // pages by updateZoomBar().
    m_zoomBar = new ZoomBar(statusBar());
    connect(m_zoomBar, &ZoomBar::zoomRequested, this, [this](qreal zoom) {
        if (m_active)
            m_active->setZoomLevel(zoom);
    });
    connect(m_zoomBar, &ZoomBar::fitWidthRequested, this, [this] {
        if (m_active)
            m_active->setFitWidth(true);
    });
    m_zoomBar->setVisible(false);
    statusBar()->addPermanentWidget(m_zoomBar);

    // No document is open at startup: show the home page (neutral status bar,
    // app-name title, no active canvas).
    showHomePage();

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

    // The palette itself was applied before any widget was built (see main).
    // This pushes the theme through every piece that baked colours at
    // construction and syncs the native dialogs' palette.
    applyTheme();
}

PdfCanvas *MainWindow::createCanvas()
{
    auto *canvas = new PdfCanvas(m_stack);
    m_stack->addWidget(canvas);

    // Every canvas tracks its own edits, so a document that was drawn on and
    // then switched away from stays dirty. The receiver context is the canvas
    // itself: the status-canvas rewiring in wireActiveCanvas cannot drop this
    // connection, and the lambda dies with the canvas.
    connect(canvas, &PdfCanvas::inkChanged, canvas,
            [this, canvas](int) { markDocumentDirty(canvas); });

    // Each canvas owns its own floating island; the host supplies the actions
    // the island cannot do itself.
    if (InkToolbar *bar = canvas->toolbar()) {
        // Opening a document is done from the tab strip's 「打开」 chip.
        connect(bar, &InkToolbar::settingsRequested, this, &MainWindow::onSettings);
        connect(bar, &InkToolbar::saveRequested, this, &MainWindow::onSave);
        connect(bar, &InkToolbar::saveAsRequested, this, &MainWindow::onSaveAs);
        connect(bar, &InkToolbar::fullscreenRequested, this, &MainWindow::onFullscreen);
    }
    return canvas;
}

void MainWindow::onOpen()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开 PDF / 批注包 / Word 文档"), QString(),
        QStringLiteral("PDF、批注包与 Word (*.pdf *.dpz *.docx *.doc);;"
                       "PDF 文件 (*.pdf);;批注包 (*.dpz);;"
                       "Word 文档 (*.docx *.doc);;")
        + ImageImport::dialogFilter()
        + QStringLiteral(";;所有文件 (*.*)"));
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
            && !path.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive)
            && !WordConvert::isWordDoc(path)
            && !ImageImport::isImage(path))
            continue;
        e->acceptProposedAction();
        openPath(path);      // opens a new tab (or activates the existing one)
        return;
    }
}

// Esc is handled here and NOT as a QAction/QShortcut: while the pen palette or
// the page grid is open their app-level event filters consume Esc first, and
// they must keep winning.
void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
        onFullscreen();      // restores the pre-fullscreen window state
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange)
        return;

    // The island's fullscreen button mirrors the real window state. Every
    // canvas is a document now, so m_canvases is the complete list.
    for (PdfCanvas *canvas : m_canvases) {
        if (InkToolbar *bar = canvas->toolbar())
            bar->setFullscreenActive(isFullScreen());
    }
}

// Closing the window walks every dirty document and asks the same four-way
// question as a single tab close; ONE 取消 aborts the whole close.
void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < m_docs.size(); ++i) {
        if (!m_docs.at(i).dirty)
            continue;
        if (!confirmCloseDocument(i)) {
            event->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(event);
}

// The one fan-out for a theme change: the Theme palette first, then every
// piece of chrome that baked colours at construction - including every open
// canvas and its overlays, the settings page, the tab strip and the status
// bar - and finally the native palettes (dialogs, title bar).
void MainWindow::applyTheme()
{
    // Re-sync the runtime mode from the stored preference FIRST: the settings
    // page only writes the value, so without this the palette would be
    // recomputed from whatever mode was set at startup and a click on
    // 浅色 / 深色 would appear to do nothing.
    Theme::applyStoredMode(AppSettings::themeMode());

    // Native dialogs and message boxes follow the theme too; the light modes
    // restore the palette Windows gave us at startup.
    if (qApp) {
        qApp->setPalette(Theme::resolvedMode() == Theme::Mode::Dark
                             ? darkAppPalette()
                             : baseAppPalette());
    }

    if (statusBar())
        statusBar()->setStyleSheet(statusSheet());

    // Every open document canvas, then the non-document pages.
    for (PdfCanvas *canvas : m_canvases)
        canvas->applyTheme();

    if (m_homePage)
        m_homePage->applyTheme();
    if (m_settingsPage)
        m_settingsPage->refreshTheme();
    if (m_zoomBar)
        m_zoomBar->refreshTheme();
    if (m_tabs)
        m_tabs->update();

    // The native title bar (Windows 10 20H1+) follows the chrome. In
    // fullscreen there is no caption, so the call would be a no-op.
    if (!isFullScreen()) {
        const BOOL dark = (Theme::resolvedMode() == Theme::Mode::Dark) ? TRUE : FALSE;
        DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()),
                              static_cast<DWORD>(DWMWA_USE_IMMERSIVE_DARK_MODE),
                              &dark, sizeof(dark));
    }

    update();
}

// 系统 follows the OS; an explicit 浅色 / 深色 stays where the user put it.
void MainWindow::onSystemColorSchemeChanged()
{
    if (AppSettings::themeMode() == 0)
        applyTheme();
}

// Once after launch, and again after a document opens - the second one throttled,
// because a lesson can open a dozen files and GitHub should not see a dozen
// requests. Both are silent unless a newer version shows up.
void MainWindow::checkForUpdates(bool fromFileOpen)
{
    if (!m_updateClient)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kThrottleMs = 10 * 60 * 1000;
    if (fromFileOpen && m_lastUpdateCheckMs > 0 && now - m_lastUpdateCheckMs < kThrottleMs)
        return;
    m_lastUpdateCheckMs = now;
    AppSettings::setLastUpdateCheckMs(now, nullptr);
    m_updateClient->check(UpdateChecker::Channel::GitHub);
}

// "发现新版本" conversation. The changelog is right here, and accepting goes to the
// Settings update card - where the download buttons and their sha256 gate live.
void MainWindow::promptUpdate(const UpdateChecker::UpdateInfo &info)
{
    const QString notes = info.notes.trimmed();
    const int touch = Theme::metrics(font()).touch;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("发现新版本"));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(Theme::Space5, Theme::Space5, Theme::Space5, Theme::Space5);
    layout->setSpacing(Theme::Space4);

    auto *title = new QLabel(QStringLiteral("发现新版本 v%1").arg(info.version), &dialog);
    title->setFont(Theme::titleFont(dialog.font()));
    layout->addWidget(title);

    auto *body = new QLabel(
        QStringLiteral("当前 v%1。确认后打开「设置 → 更新」，在那里下载并安装"
                       "（安装包校验通过后才会运行）。")
            .arg(UpdateChecker::currentVersion()), &dialog);
    body->setWordWrap(true);
    body->setFont(Theme::bodyFont(dialog.font()));
    layout->addWidget(body);

    if (!notes.isEmpty()) {
        auto *caption = new QLabel(QStringLiteral("更新日志"), &dialog);
        caption->setFont(Theme::bodyFont(dialog.font()));
        layout->addWidget(caption);

        auto *view = new QPlainTextEdit(notes, &dialog);
        view->setReadOnly(true);
        view->setFont(Theme::captionFont(dialog.font()));
        view->setMinimumHeight(touch * 5);
        layout->addWidget(view, 1);
    }

    auto *row = new QHBoxLayout;
    row->setSpacing(Theme::Space3);
    auto *later = new QPushButton(QStringLiteral("稍后"), &dialog);
    auto *go = new QPushButton(QStringLiteral("立即更新"), &dialog);
    for (QPushButton *button : {later, go}) {
        button->setMinimumSize(QSize(touch * 2, touch));
        button->setCursor(Qt::PointingHandCursor);
    }
    go->setDefault(true);
    row->addStretch(1);
    row->addWidget(later);
    row->addWidget(go);
    layout->addLayout(row);

    connect(later, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(go, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() == QDialog::Accepted)
        openUpdateSettings();
}

void MainWindow::openUpdateSettings()
{
    showSettingsPage();
    if (m_settingsPage)
        m_settingsPage->focusUpdateSection();
}

void MainWindow::openPath(const QString &path)
{
    CrashLog::breadcrumb("open", QFileInfo(path).fileName());
    QElapsedTimer timer;
    timer.start();
    const bool bundle = AnnotationBundle::isBundle(path);
    const bool word = WordConvert::isWordDoc(path);
    const bool image = ImageImport::isImage(path);
    const QString key = fileKey(path);

    // Already open somewhere? Bring that tab forward instead of opening twice.
    for (int i = 0; i < m_canvases.size(); ++i) {
        const DocumentInfo &doc = m_docs.at(i);
        const QString open = bundle ? doc.bundlePath
                             : !doc.wordPath.isEmpty() ? doc.wordPath
                             : !doc.imagePath.isEmpty() ? doc.imagePath
                             : doc.sourcePdf;
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

    if (word) {
        // The stored preference decides: 每次询问 pops the two-choice modal,
        // 总用 Word 打开 hands the file to the system handler, 总批注 goes
        // straight down the conversion path. Handing over opens NO tab and
        // converts nothing, so the 2.4 s export cost is skipped entirely.
        const int mode = AppSettings::wordOpenMode();
        if (mode == 2) {
            AppLog::write(QStringLiteral("open"),
                          QStringLiteral("Word 文档交给系统默认程序：%1").arg(path));
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            return;
        }
        if (mode == 0) {
            const WordOpenAnswer answer = askWordOpen(this, QFileInfo(path).fileName());
            if (answer == WordOpenAnswer::Cancel)
                return;
            if (answer == WordOpenAnswer::OpenInWord) {
                AppLog::write(QStringLiteral("open"),
                              QStringLiteral("Word 文档交给系统默认程序：%1").arg(path));
                QDesktopServices::openUrl(QUrl::fromLocalFile(path));
                return;
            }
        }

        // The .docx itself is never touched: Word/WPS renders a PDF into the
        // temp cache (reused while path+size+mtime are unchanged) and that PDF
        // is what the normal pipeline sees from here on.
        if (!WordConvert::hasConverter()) {
            AppLog::write(QStringLiteral("open"),
                          QStringLiteral("Word 文档打开失败：未检测到 Word/WPS（%1）").arg(path));
            statusBar()->showMessage(
                QStringLiteral("未检测到 Word/WPS，无法打开该 Word 文档（可先另存为 PDF）"),
                8000);
            return;
        }

        // The export blocks for a few seconds; say so before it starts.
        statusBar()->showMessage(QStringLiteral("正在转换 Word 文档…"));
        QApplication::processEvents();

        QString err;
        if (!WordConvert::convertToPdf(path, &tempPath, &err)) {
            AppLog::write(QStringLiteral("open"),
                          QStringLiteral("Word 文档转换失败：%1（%2）").arg(path, err));
            // Two different causes, two different fixes: Word's "not the default
            // program" nag (fix the association) or a stuck Word instance
            // holding a modal dialog (close Word / reboot). Both are actionable.
            const QString hint =
                WordConvert::wordIsDefaultHandler()
                    ? QStringLiteral("Word 里可能有对话框或卡住的实例："
                                     "请关闭所有 Word 窗口（或重启）后重试")
                    : QStringLiteral("请把 .docx 的默认程序设为 Word"
                                     "（设置 → 默认应用），或先用「用 Word 打开」");
            statusBar()->showMessage(QStringLiteral("转换失败：%1").arg(hint), 12000);
            return;
        }
        statusBar()->clearMessage();
        // Everything from here on annotates a copy, never the .docx: the tab
        // reads <basename>.dpz from the start.
        tabTitle = QFileInfo(path).completeBaseName() + QStringLiteral(".dpz");
    } else if (image) {
        // An image has no pages, so it is wrapped into a one-page PDF at its own
        // pixel size (see ImageImport) and everything then works unchanged: zoom,
        // ink, .dpz. The original image is only ever read.
        statusBar()->showMessage(QStringLiteral("正在导入图片…"));
        QApplication::processEvents();

        QString err;
        if (!ImageImport::toPdf(path, &tempPath, &err)) {
            AppLog::write(QStringLiteral("open"),
                          QStringLiteral("图片导入失败：%1（%2）").arg(path, err));
            statusBar()->showMessage(QStringLiteral("图片导入失败：%1").arg(err), 10000);
            return;
        }
        statusBar()->clearMessage();
        tabTitle = QFileInfo(path).completeBaseName() + QStringLiteral(".dpz");
    } else if (bundle) {
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

    // Every document gets its own canvas and its own tab: the old "reuse the
    // blank startup canvas" special case went away with the blank canvas.
    PdfCanvas *canvas = createCanvas();

    QString err;
    if (!canvas->openPdf(tempPath, &err)) {
        AppLog::write(QStringLiteral("open"),
                      QStringLiteral("打开失败：%1（%2）").arg(path, err));
        m_stack->removeWidget(canvas);
        delete canvas;
        statusBar()->showMessage(QStringLiteral("打开失败：%1").arg(err), 8000);
        return;
    }
    if (bundle)
        canvas->importInk(annotations);

    // A source without a real bundle may still have a working copy from an
    // earlier 保存: restore it so the teacher gets their annotations back. The
    // restore is gated on CONTENT IDENTITY, not the path alone - a swapped USB
    // stick or a replaced file at the same path must not inherit the old ink.
    // The status line says the outcome once, after the tab is in place.
    bool restoredWorkingCopy = false;
    bool workingCopyMismatch = false;
    if (!bundle) {
        const QString working = AnnotationBundle::workingBundlePathFor(path);
        if (QFileInfo::exists(working)) {
            QJsonObject workingInk;
            QString workingErr;
            if (AnnotationBundle::read(working, nullptr, &workingInk, &workingErr)) {
                const QJsonObject recorded =
                    workingInk.value(QStringLiteral("source")).toObject();
                const QJsonObject actual = AnnotationBundle::sourceFingerprintFor(path);
                if (AnnotationBundle::fingerprintMatches(recorded, actual)) {
                    canvas->importInk(workingInk);
                    restoredWorkingCopy = true;
                } else {
                    // The working file is left untouched for inspection: this
                    // is never a modal, just one log line and one status line.
                    workingCopyMismatch = true;
                    AppLog::write(
                        QStringLiteral("open"),
                        QStringLiteral("工作副本与源文件不一致，未恢复批注：%1（源 %2）")
                            .arg(working, path));
                }
            } else {
                AppLog::write(QStringLiteral("open"),
                              QStringLiteral("工作副本读取失败：%1（%2）").arg(working, workingErr));
            }
        }
    }

    // 最近项目 remembers the user-visible path only - the .pdf / .dpz / .docx
    // the user picked, never the generated temp PDF; the file itself is never
    // copied anywhere (bundles are read in place too).
    AppSettings::addRecentFile(path);

    AppLog::write(QStringLiteral("open"),
                  QStringLiteral("%1：%2 页，%3 ms，批注 %4 条")
                      .arg(tabTitle)
                      .arg(canvas->pageCount())
                      .arg(timer.elapsed())
                      .arg(canvas->strokeCount()));

    DocumentInfo info;
    info.bundlePath = bundle ? path : QString();
    info.wordPath = word ? path : QString();
    info.imagePath = image ? path : QString();
    info.sourcePdf = tempPath;
    info.title = tabTitle;
    info.tempPdf = (bundle || word || image) ? tempPath : QString();

    m_canvases.append(canvas);
    m_docs.append(info);
    m_tabs->addTab(tabTitle);
    const int index = int(m_canvases.size()) - 1;
    m_tabs->setCurrentIndex(index);              // emits -> activates the tab
    onTabCurrentChanged(index);                  // idempotent safety net

    if (restoredWorkingCopy)
        statusBar()->showMessage(QStringLiteral("已恢复上次未另存的批注"), 5000);
    else if (workingCopyMismatch)
        statusBar()->showMessage(QStringLiteral("工作副本与源文件不一致，未恢复批注"), 8000);

    // Opening a file is a natural moment to check for a new version (throttled).
    checkForUpdates(true);
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

// Ctrl+S / the island's 「保存」.
void MainWindow::onSave()
{
    const int index = docIndex(m_active);
    if (index < 0)
        return;
    saveDocument(index);
}

// Writes one document's annotations. A document with a real bundle (opened as
// a .dpz, or created by 另存为) saves into that file, exactly as before. Every
// other document - a Word file or a plain PDF - saves into a working copy
// under %TEMP%/pdfboard keyed by its SOURCE path, so a document on a USB stick
// is never joined by a `.dpz` sibling. Returns true only when bytes were
// written.
bool MainWindow::saveDocument(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return false;
    PdfCanvas *canvas = m_canvases.at(index);
    if (!canvas || canvas->pdfPath().isEmpty())
        return false;
    const DocumentInfo info = m_docs.at(index);

    // A real bundle is updated in place.
    if (!info.bundlePath.isEmpty()) {
        const QByteArray pdf = readPdfBytes(info.sourcePdf);
        if (pdf.isEmpty()) {
            AppLog::write(QStringLiteral("save"),
                          QStringLiteral("保存失败：无法读取源 PDF：%1").arg(info.sourcePdf));
            QMessageBox::warning(this, QStringLiteral("保存失败"),
                                 QStringLiteral("无法读取源 PDF：%1").arg(info.sourcePdf));
            return false;
        }

        QString err;
        if (!AnnotationBundle::write(info.bundlePath, pdf, canvas->exportInk(), &err)) {
            AppLog::write(QStringLiteral("save"),
                          QStringLiteral("保存批注包失败：%1（%2）").arg(info.bundlePath, err));
            QMessageBox::warning(this, QStringLiteral("保存失败"), err);
            return false;
        }
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("保存批注包 %1：批注 %2 条")
                          .arg(QFileInfo(info.bundlePath).fileName())
                          .arg(canvas->strokeCount()));
        markDocumentSaved(index);
        statusBar()->showMessage(
            QStringLiteral("已保存 %1").arg(QFileInfo(info.bundlePath).fileName()), 4000);
        return true;
    }

    // No real bundle yet: 保存 keeps a working copy under the temp dir, keyed
    // by the source path, and the tab keeps the SOURCE file name - saving must
    // not turn a .docx into a `work-<hash>.dpz` tab (the 1.3.2 over-correction)
    // and must not create anything next to the source. 另存为 remains the only
    // way to put the annotations where the user chooses.
    const QString source = !info.wordPath.isEmpty() ? info.wordPath : info.sourcePdf;
    const QString target = AnnotationBundle::workingBundlePathFor(source);
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("保存失败：无法创建临时目录：%1")
                          .arg(QFileInfo(target).absolutePath()));
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法创建临时工作目录：%1")
                                 .arg(QFileInfo(target).absolutePath()));
        return false;
    }

    const QByteArray pdf = readPdfBytes(info.sourcePdf);
    if (pdf.isEmpty()) {
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("保存失败：无法读取源 PDF：%1").arg(info.sourcePdf));
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法读取源 PDF：%1").arg(info.sourcePdf));
        return false;
    }

    // The fingerprint pins the working copy to the file it was made from, so a
    // restore can tell "same path" from "same document".
    const QJsonObject fingerprint = AnnotationBundle::sourceFingerprintFor(source);

    QString err;
    if (!AnnotationBundle::write(target, pdf, canvas->exportInk(), &err, fingerprint)) {
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("保存工作副本失败：%1（%2）").arg(target, err));
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        return false;
    }

    // The first 保存 materialises the `<basename>.dpz` title: the source name
    // plus the signal that the annotations live in a copy, not next to it.
    m_docs[index].title = QFileInfo(source).completeBaseName() + QStringLiteral(".dpz");
    AppLog::write(QStringLiteral("save"),
                  QStringLiteral("保存工作副本 %1：批注 %2 条")
                      .arg(QFileInfo(target).fileName())
                      .arg(canvas->strokeCount()));
    markDocumentSaved(index);
    statusBar()->showMessage(QStringLiteral("已保存 %1").arg(m_docs.at(index).title), 4000);
    return true;
}

// Ctrl+Shift+S / the island's 「另存为」: choose between a `.dpz` bundle
// (default) and a flattened, injected PDF that other software can open.
// Returns true only when a file was actually written.
bool MainWindow::saveDocumentAs(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return false;
    PdfCanvas *canvas = m_canvases.at(index);
    if (!canvas || canvas->pdfPath().isEmpty())
        return false;
    const DocumentInfo info = m_docs.at(index);

    // Keep the filter text itself simple, otherwise Qt's suffix handling
    // mangles the pre-filled name when the format is switched.
    const QString bundleFilter = QStringLiteral("打包保存 (*.dpz)");
    const QString injectFilter = QStringLiteral("注入 PDF (*.pdf)");
    const QString pngFilter = QStringLiteral("导出 PNG 图片 (*.png)");

    // Once a document is bundle-backed its `sourcePdf` is an internal temp copy;
    // never suggest that path or name to the user. A Word document is instead
    // identified by its .docx, so the suggestion lands next to the original -
    // the same basename, with a .dpz / -已批注.pdf suffix.
    const QString identity = !info.bundlePath.isEmpty() ? info.bundlePath
                             : !info.wordPath.isEmpty() ? info.wordPath
                             : !info.imagePath.isEmpty() ? info.imagePath
                             : info.sourcePdf;
    const QFileInfo src(identity);
    // Prefer the user's configured save folder; fall back to the document's own
    // directory when it is unset or has gone away.
    QString dir = AppSettings::defaultSavePath();
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        dir = src.absolutePath();
    QString base = src.completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("未命名");

    auto nameFor = [&base](const QString &filter) {
        if (filter.contains(QStringLiteral("*.png")))
            return base + QStringLiteral("-1.png");
        if (filter.contains(QStringLiteral("*.pdf")))
            return base + QStringLiteral("-已批注.pdf");
        return base + QStringLiteral(".dpz");
    };

    QFileDialog dlg(this, QStringLiteral("另存为"), dir);
    dlg.setAcceptMode(QFileDialog::AcceptSave);
    dlg.setFileMode(QFileDialog::AnyFile);
    dlg.setNameFilters({bundleFilter, injectFilter, pngFilter});
    dlg.selectNameFilter(bundleFilter);      // 打包保存 is the default
    dlg.selectFile(nameFor(bundleFilter));
    // The "for sharing" guidance lives on the file-type label so the filter
    // strings stay clean.
    dlg.setLabelText(QFileDialog::FileType,
                     QStringLiteral("保存类型（注入 PDF 适合分享；PNG 每页导出一张）"));
    // Re-fill the name on every format switch: Qt's own suffix juggling is what
    // garbled the suggestion.
    connect(&dlg, &QFileDialog::filterSelected, &dlg,
            [&dlg, &nameFor](const QString &f) {
                dlg.selectFile(nameFor(f));
            });
    if (dlg.exec() != QDialog::Accepted)
        return false;

    QString path = dlg.selectedFiles().value(0);
    if (path.isEmpty())
        return false;

    const bool inject = (dlg.selectedNameFilter() == injectFilter);
    const bool png = (dlg.selectedNameFilter() == pngFilter);
    if (png) {
        if (!path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
            path += QStringLiteral(".png");
    } else if (inject) {
        if (!path.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
            path += QStringLiteral(".pdf");
    } else {
        if (!path.endsWith(QStringLiteral(".dpz"), Qt::CaseInsensitive))
            path += QStringLiteral(".dpz");
    }

    QString err;

    if (png) {
        // One PNG per page: <base>-<n>.png, the index padded to the page count
        // (PdfExport::pngPageName owns that rule). Exporting is not saving: the
        // document stays dirty until its own .dpz is written.
        QString basePath = path;
        if (basePath.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
            basePath.chop(4);

        // Never write over the source file: with a base such as "img" a one-page
        // export would happily overwrite the teacher's own "img-1.png".
        const auto samePath = [](const QString &a, const QString &b) {
            if (a.isEmpty() || b.isEmpty())
                return false;
            return QFileInfo(a).absoluteFilePath().compare(QFileInfo(b).absoluteFilePath(),
                                                          Qt::CaseInsensitive) == 0;
        };
        const int pages = qMax(1, canvas->pageCount());
        for (int i = 1; i <= pages; ++i) {
            const QString out = PdfExport::pngPageName(basePath, i, pages);
            if (samePath(out, info.imagePath) || samePath(out, info.sourcePdf)
                || samePath(out, info.bundlePath)) {
                AppLog::write(QStringLiteral("save"),
                              QStringLiteral("拒绝把源文件当作导出目标：%1").arg(out));
                statusBar()->showMessage(
                    QStringLiteral("不能用源文件本身作为导出目标，请换个文件名"), 8000);
                return false;
            }
        }

        QElapsedTimer timer;
        timer.start();
        const int written = PdfExport::exportPngPages(canvas, basePath, &err);
        if (written < 0) {
            AppLog::write(QStringLiteral("save"),
                          QStringLiteral("导出 PNG 失败：%1（%2）").arg(path, err));
            QMessageBox::warning(this, QStringLiteral("导出失败"), err);
            return false;
        }
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("导出 PNG %1：%2 张，%3 ms")
                          .arg(QFileInfo(path).fileName()).arg(written).arg(timer.elapsed()));
        statusBar()->showMessage(
            written == 1
                ? QStringLiteral("已导出 %1").arg(QFileInfo(
                      PdfExport::pngPageName(basePath, 1, 1)).fileName())
                : QStringLiteral("已导出 %1 张 PNG：%2-1.png …")
                      .arg(written).arg(QFileInfo(basePath).completeBaseName()),
            6000);
        return true;
    }

    if (inject) {
        // Never write over the document being annotated: the export target must
        // be a different file (the dialog suggests <basename>-已批注.pdf).
        const auto sameFile = [](const QString &a, const QString &b) {
            if (a.isEmpty() || b.isEmpty())
                return false;
            return QFileInfo(a).absoluteFilePath().compare(QFileInfo(b).absoluteFilePath(),
                                                          Qt::CaseInsensitive) == 0;
        };
        if (sameFile(path, info.wordPath) || sameFile(path, info.imagePath)
            || sameFile(path, info.sourcePdf)) {
            AppLog::write(QStringLiteral("save"),
                          QStringLiteral("拒绝把源文件当作导出目标：%1").arg(path));
            statusBar()->showMessage(
                QStringLiteral("不能用源文件本身作为导出目标，请换一个文件名"), 8000);
            return false;
        }

        // Injecting never changes the open document's own bundle state.
        QElapsedTimer timer;
        timer.start();
        if (!PdfExport::exportFlattened(canvas, path, &err)) {
            AppLog::write(QStringLiteral("save"),
                          QStringLiteral("注入 PDF 失败：%1（%2）").arg(path, err));
            QMessageBox::warning(this, QStringLiteral("保存失败"), err);
            return false;
        }
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("注入 PDF %1：%2 页，%3 KB，%4 ms")
                          .arg(QFileInfo(path).fileName())
                          .arg(canvas->pageCount())
                          .arg(double(QFileInfo(path).size()) / 1024.0, 0, 'f', 0)
                          .arg(timer.elapsed()));
        markDocumentSaved(index);
        statusBar()->showMessage(
            QStringLiteral("已注入 PDF %1").arg(QFileInfo(path).fileName()), 4000);
        return true;
    }

    const QByteArray pdf = readPdfBytes(info.sourcePdf);
    if (pdf.isEmpty()) {
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("读取源 PDF 失败：%1").arg(info.sourcePdf));
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法读取源 PDF：%1").arg(info.sourcePdf));
        return false;
    }
    if (!AnnotationBundle::write(path, pdf, canvas->exportInk(), &err)) {
        AppLog::write(QStringLiteral("save"),
                      QStringLiteral("保存批注包失败：%1（%2）").arg(path, err));
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
        return false;
    }
    AppLog::write(QStringLiteral("save"),
                  QStringLiteral("保存批注包 %1：%2 KB，批注 %3 条")
                      .arg(QFileInfo(path).fileName())
                      .arg(double(QFileInfo(path).size()) / 1024.0, 0, 'f', 0)
                      .arg(canvas->strokeCount()));

    // 另存为 promotes the document to a real bundle: later 保存 go there and
    // the unsaved marker clears because the bytes are really on disk.
    m_docs[index].bundlePath = path;
    m_docs[index].title = QFileInfo(path).fileName();
    markDocumentSaved(index);
    updateTitle();
    statusBar()->showMessage(
        QStringLiteral("已打包保存 %1").arg(m_docs.at(index).title), 4000);
    return true;
}

void MainWindow::onSaveAs()
{
    const int index = docIndex(m_active);
    if (index >= 0)
        saveDocumentAs(index);
}

// True when the caller may close the document: it carries no unsaved ink, or
// the user chose 保存 / 另存为 and something was really written, or 舍弃.
// 取消 (and dismissing the box) keeps the document open.
bool MainWindow::confirmCloseDocument(int index)
{
    if (index < 0 || index >= m_docs.size())
        return true;
    if (!m_docs.at(index).dirty)
        return true;

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("未保存的批注"));
    box.setText(QStringLiteral("「%1」有未保存的批注。").arg(m_docs.at(index).title));
    box.setInformativeText(QStringLiteral("关闭前要保存吗？"));
    QPushButton *saveButton = box.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
    QPushButton *saveAsButton = box.addButton(QStringLiteral("另存为"), QMessageBox::ActionRole);
    QPushButton *discardButton =
        box.addButton(QStringLiteral("舍弃"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(saveButton);
    const int touch = Theme::metrics(font()).touch;
    const QList<QAbstractButton *> buttons = box.buttons();
    for (QAbstractButton *button : buttons)
        button->setMinimumHeight(int(touch * 0.72));
    box.exec();

    if (box.clickedButton() == saveButton)
        return saveDocument(index);
    if (box.clickedButton() == saveAsButton)
        return saveDocumentAs(index);
    if (box.clickedButton() == discardButton)
        return true;
    return false;
}

void MainWindow::markDocumentDirty(PdfCanvas *canvas)
{
    const int index = docIndex(canvas);
    if (index < 0 || m_docs.at(index).dirty)
        return;
    m_docs[index].dirty = true;
    refreshTabTitle(index);
}

void MainWindow::markDocumentSaved(int index)
{
    if (index < 0 || index >= m_docs.size())
        return;
    m_docs[index].dirty = false;
    refreshTabTitle(index);
}

// The tab strip shows the source name plus a leading `*` while the ink has
// unsaved changes; the window title keeps the clean name.
void MainWindow::refreshTabTitle(int index)
{
    if (index < 0 || index >= m_docs.size())
        return;
    const DocumentInfo &doc = m_docs.at(index);
    m_tabs->setTabTitle(index, doc.dirty ? QStringLiteral("*") + doc.title : doc.title);
}

// F11 / the island's 「全屏」: a plain toggle that returns to the window state
// the user came from (maximized or normal).
void MainWindow::onFullscreen()
{
    if (isFullScreen()) {
        if (m_fullscreenWasMaximized)
            showMaximized();
        else
            showNormal();
    } else {
        m_fullscreenWasMaximized = isMaximized();
        showFullScreen();
    }
}

void MainWindow::onTabCurrentChanged(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return;

    PdfCanvas *next = m_canvases.at(index);
    const bool wasOverlay = m_settingsVisible || m_homeVisible;

    // Activating a document chip always leaves the settings / home page (and
    // clears their chip highlights).
    m_settingsVisible = false;
    m_homeVisible = false;
    m_tabs->setSettingsActive(false);
    m_tabs->setHomeActive(false);

    if (next == m_active && !wasOverlay) {
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

// The ✕ on a tab / Ctrl+W: ask about unsaved ink first, remove only when the
// user did not cancel (and any chosen save really wrote).
void MainWindow::onTabCloseRequested(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return;
    if (!confirmCloseDocument(index))
        return;
    closeTabAt(index);
}

// The removal itself, after every confirmation has passed.
void MainWindow::closeTabAt(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return;

    PdfCanvas *canvas = m_canvases.at(index);
    const bool wasActive = (canvas == m_active);
    m_canvases.removeAt(index);
    if (index < m_docs.size())
        m_docs.removeAt(index);

    // The strip selects the neighbour and reports it through currentChanged
    // (silent while the settings / home page is up).
    m_tabs->removeTab(index);

    if (m_canvases.isEmpty()) {
        // The last document went away: the home page takes its place. When the
        // settings page is the visible one, it stays exactly where it is.
        if (canvas == m_active)
            m_active = nullptr;
        canvas->closePdf();
        m_stack->removeWidget(canvas);
        delete canvas;
        if (m_settingsVisible)
            wireActiveCanvas(nullptr);       // status bar was already neutral
        else
            showHomePage();
        return;
    }

    if (wasActive) {
        const int next = qMin(index, int(m_canvases.size()) - 1);
        if (m_settingsVisible || m_homeVisible) {
            // Stay on the overlay page: only remember which document is next.
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
            // The zoom control follows whatever the document does (pinch, Ctrl+wheel,
            // shortcuts). Connected per active canvas - the disconnect above already
            // cleared the previous one, so there is never a double update.
            connect(canvas, &PdfCanvas::zoomChanged, this,
                    [this](qreal zoom) {
                        if (m_zoomBar)
                            m_zoomBar->setZoom(zoom);
                    });
        }
    }
    refreshStatus();     // the labels must show the newly active document
    updateZoomBar();     // ...and the zoom control must match it (or hide)
}

void MainWindow::updateZoomBar()
{
    if (!m_zoomBar)
        return;
    // Only a document has a zoom; the home and settings pages are not zoomable.
    const bool onDocument = m_statusCanvas && !m_homeVisible && !m_settingsVisible;
    m_zoomBar->setVisible(onDocument);
    if (onDocument)
        m_zoomBar->setZoom(m_statusCanvas->testZoom());
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
        setWindowTitle(QStringLiteral("设置 — 落墨·大屏批注"));
        return;
    }
    if (m_homeVisible) {
        setWindowTitle(QStringLiteral("落墨·大屏批注"));
        return;
    }

    const QString name = docTitle(m_active);
    setWindowTitle(name.isEmpty()
        ? QStringLiteral("落墨·大屏批注")
        : QStringLiteral("%1 — 落墨·大屏批注").arg(name));
}

// The island's 「设置」 button and the tab strip's Settings chip both land here.
// The chip doubles as a toggle: pressing it while the page is already up
// returns to the page the user came from - a document, or the home page when
// no document is open.
void MainWindow::onSettings()
{
    if (m_settingsVisible) {
        showCanvasPage(m_active);        // falls back to the home page on null
        return;
    }
    showSettingsPage();
}

void MainWindow::showHomePage()
{
    m_homeVisible = true;
    m_settingsVisible = false;
    m_tabs->setSettingsActive(false);
    m_tabs->setHomeActive(true);
    m_stack->setCurrentWidget(m_homePage);
    // No document is active while the home page is up: the status bar shows the
    // neutral values and the hidden canvases keep their rendered pages.
    wireActiveCanvas(nullptr);
    updateTitle();
    m_homePage->refresh();
}

void MainWindow::showSettingsPage()
{
    m_settingsVisible = true;
    m_homeVisible = false;
    m_tabs->setSettingsActive(true);
    m_tabs->setHomeActive(false);
    m_stack->setCurrentWidget(m_settingsPage);
    // No document is active while the settings page is up: the status bar shows
    // the neutral values and the hidden canvas keeps its rendered pages.
    wireActiveCanvas(nullptr);
    updateTitle();
}

void MainWindow::showCanvasPage(PdfCanvas *canvas)
{
    if (!canvas) {
        showHomePage();                  // no document to return to
        return;
    }
    m_settingsVisible = false;
    m_homeVisible = false;
    m_tabs->setSettingsActive(false);
    m_tabs->setHomeActive(false);
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
