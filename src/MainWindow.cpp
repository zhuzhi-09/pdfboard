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
#include <QEasingCurve>
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
#include <QLinearGradient>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>
#include <QVariantAnimation>
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

// The status bar as a quiet line of muted text instead of the default Qt
// chrome. Its content is unchanged: 页码 / 渲染 / 笔画 / 内存.
// The labels carry NO pill any more - no background, no corner radius, no
// padding: the pills were nested chrome that alone kept the strip ~16 px taller
// than its text. What separates the values is a horizontal gap, which
// QStatusBar already provides as its own 6 px item spacing (set in
// QStatusBar::reformat()), so dropping the CSS padding also means the widest
// pinned texts can never be elided inside their fixed width.
QString statusSheet()
{
    const Theme::Palette &c = Theme::light();
    return QStringLiteral(
               "QStatusBar {"
               " background: %1;"
               " border-top: 1px solid %2;"
               " color: %3; }"
               "QStatusBar::item { border: none; }"
               "QStatusBar QLabel { color: %3; }")
        .arg(Theme::rgba(c.surface))
        .arg(Theme::rgba(c.divider))
        .arg(Theme::rgba(c.textMuted));
}

}   // namespace (reopened below: PageTransitionLayer needs external linkage to
    // match the forward declaration in MainWindow.h)

// ---------------------------------------------------------------------------
// Page-switch transitions: ONE helper for every entry point (see switchToPage).
//
// WHY a frozen-frame snapshot overlay, and not the two alternatives the task
// names:
//   * QStackedWidget shows exactly ONE page at a time (StackOne), and a
//     layout-managed child cannot be moved by animating its pos - the layout
//     re-imposes its geometry on the next activation. So the incoming page
//     cannot simply be shown next to / above the outgoing one.
//   * A QGraphicsEffect would recompose a live 4K widget on every animation
//     frame (the expensive path on a classroom panel) and cannot translate a
//     widget at all.
//   * Reparenting the two heavy PdfCanvas widgets into an overlay would fire
//     show/hide + relayout churn on every switch and risks a stale or duplicated
//     page - exactly the failure the task forbids.
// So two one-shot QWidget::grab()s freeze the exact pixels and this layer just
// blits them (outgoing first, incoming ON TOP). At 4K the page area is roughly
// 3840x2000, i.e. ~31 MB per grab, so a switch holds two of them (~63 MB) for
// its ~240 ms; both are released in the finished handler (settleTransition).
// That is the entire transient cost - no per-frame allocations.
//
// The final frame is pixel-identical to the real page: the incoming frame was
// grabbed at the stack's geometry and is drawn at (0,0) fully opaque, while the
// real incoming page is ALREADY the stack's current widget underneath - hiding
// the layer changes nothing on screen (no flash, no jump).
constexpr int   kPageTransitionMs = 240;
constexpr qreal kPageSinkFraction = 0.10;        // outgoing sinks 10% of the height
constexpr qreal kPageOutgoingMinOpacity = 0.55;  // ...and fades 1 -> 0.55

class PageTransitionLayer : public QWidget
{
public:
    explicit PageTransitionLayer(QWidget *parent) : QWidget(parent)
    {
        setObjectName(QStringLiteral("pageTransitionLayer"));
        // paintEvent covers every pixel (desk + both frozen frames).
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        // The incoming page is already the stack's current widget underneath: a
        // frozen frame must never eat the teacher's next touch, so events pass
        // straight through.
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

    void setFrame(const QPixmap &outgoing, const QPixmap &incoming,
                  int outX, int outY, int inX, int inY, qreal outOpacity)
    {
        m_outgoing = outgoing;
        m_incoming = incoming;
        m_outX = outX;
        m_outY = outY;
        m_inX = inX;
        m_inY = inY;
        m_outOpacity = outOpacity;
    }

    void clearFrame()
    {
        m_outgoing = QPixmap();
        m_incoming = QPixmap();
    }

    // The transient memory a switch holds, for the 4K cost report.
    qint64 frameBytes() const
    {
        const auto bytes = [](const QPixmap &pm) {
            return qint64(pm.width()) * qint64(pm.height()) * (pm.depth() / 8);
        };
        return bytes(m_outgoing) + bytes(m_incoming);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);

        // The desk behind the pages, exactly like PdfCanvas::paintBackdrop: the
        // strip the sinking outgoing sheet uncovers at the top has to read as
        // the same desk, not as a hole.
        const Theme::Palette &pal = Theme::light();
        p.fillRect(rect(), pal.desk);
        const qreal strip = qMax<qreal>(12.0, Theme::metrics(font()).icon * 1.4);
        QLinearGradient shade(0.0, 0.0, 0.0, strip);
        shade.setColorAt(0.0, pal.deskShade);
        shade.setColorAt(1.0, pal.desk);
        p.fillRect(QRectF(0.0, 0.0, qreal(width()), strip), shade);

        // Outgoing first, then the incoming ON TOP: in both kinds the new page
        // covers the old one while it travels in.
        if (!m_outgoing.isNull()) {
            p.setOpacity(m_outOpacity);
            p.drawPixmap(m_outX, m_outY, m_outgoing);
            p.setOpacity(1.0);
        }
        if (!m_incoming.isNull())
            p.drawPixmap(m_inX, m_inY, m_incoming);
    }

private:
    QPixmap m_outgoing;
    QPixmap m_incoming;
    int    m_outX = 0;
    int    m_outY = 0;
    int    m_inX  = 0;
    int    m_inY  = 0;
    qreal  m_outOpacity = 1.0;
};

namespace {
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

    // Page-switch transition plumbing (see switchToPage). The overlay is a child
    // of the stack but NOT added to its layout, so it floats above every page;
    // the driver is a plain QVariantAnimation over an eased 0..1 progress,
    // stopped and re-armed for each leg.
    m_transLayer = new PageTransitionLayer(m_stack);
    m_transAnim = new QVariantAnimation(this);
    m_transAnim->setDuration(kPageTransitionMs);
    m_transAnim->setEasingCurve(QEasingCurve::OutCubic);
    m_transAnim->setStartValue(0.0);
    m_transAnim->setEndValue(1.0);
    connect(m_transAnim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { applyTransitionProgress(value.toReal()); });
    connect(m_transAnim, &QAbstractAnimation::finished, this, &MainWindow::settleTransition);

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
    // 手掌当橡皮的开关：立刻作用于所有已打开的文档（新文档在 createCanvas 里读取）。
    connect(m_settingsPage, &SettingsPage::palmEraserChanged, this, [this] {
        const bool on = AppSettings::palmEraserEnabled();
        for (PdfCanvas *canvas : m_canvases)
            canvas->setPalmEraserEnabled(on);
    });
    // 工具栏风格（简约 / 文字）：同样立刻作用于所有已打开文档的工具岛。
    connect(m_settingsPage, &SettingsPage::toolbarStyleChanged, this, [this] {
        const InkToolbar::Style style = (AppSettings::toolbarStyle() == 0)
                                            ? InkToolbar::Style::Compact
                                            : InkToolbar::Style::Text;
        for (PdfCanvas *canvas : m_canvases) {
            if (InkToolbar *bar = canvas->toolbar())
                bar->setStyle(style);
        }
    });

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
    // Fixed VERTICAL policy: each label is exactly one text line tall. The strip
    // is plain text now, but the policy stays so nothing in the status bar can
    // ever stretch the muted labels into taller slabs (that is what the pills
    // used to do when the zoom bar strutted the strip).
    const QSizePolicy statusChipPolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    for (QLabel *chip : { m_pageLabel, m_renderLabel, m_inkLabel, m_memLabel })
        chip->setSizePolicy(statusChipPolicy);
    statusBar()->setStyleSheet(statusSheet());
    statusBar()->setSizeGripEnabled(false);
    // Every left-hand status element is pinned to its widest possible text. Those
    // values change all the time (页码 3/4, 渲染 71 ms, 笔画 12345, 内存 177.7 MB) and
    // any label growing used to shove everything to its right - including the zoom
    // slider - sideways. Fixed widths also stop the group from jittering internally.
    const auto pinWidth = [&statusFont](QLabel *label, const QString &widest) {
        label->setFixedWidth(QFontMetrics(statusFont).horizontalAdvance(widest));
    };
    pinWidth(m_pageLabel,   QStringLiteral("页码 9999/9999"));
    pinWidth(m_renderLabel, QStringLiteral("渲染 9999 ms (99999×99999)"));
    pinWidth(m_inkLabel,    QStringLiteral("笔画 99999"));
    pinWidth(m_memLabel,    QStringLiteral("内存 WS 9999.9 MB / Private 9999.9 MB"));
    statusBar()->addWidget(m_pageLabel);
    statusBar()->addWidget(m_renderLabel);
    statusBar()->addWidget(m_inkLabel);
    statusBar()->addWidget(m_memLabel);

    // Word-like zoom control, docked in the tab row's fixed slot immediately
    // left of 设置 (DocumentTabs::setTrailingWidget) so the bottom strip stays a
    // single line of text. It never invents a zoom value: the active canvas
    // reports its own zoom (pinch, Ctrl+wheel, shortcuts) and the control just
    // displays it. Hidden on the home / settings pages by updateZoomBar(); the
    // slot stays reserved either way, so 设置 / 打开 never move.
    m_zoomBar = new ZoomBar(m_tabs);
    m_tabs->setTrailingWidget(m_zoomBar);

    // Fullscreen needs a visible way out on EVERY page: the island carries the fullscreen
    // button, but it is hidden on the home and settings pages, so a teacher who went
    // fullscreen and then tapped the house saw no way back at all (user report). This pill
    // belongs to the window, floats above the page stack, and exists only in fullscreen.
    {
        const Theme::Palette &pal = Theme::light();
        m_fullscreenExit = new QPushButton(QStringLiteral("退出全屏（Esc）"), this);
        m_fullscreenExit->setObjectName(QStringLiteral("fullscreenExit"));
        m_fullscreenExit->setCursor(Qt::PointingHandCursor);
        m_fullscreenExit->setFocusPolicy(Qt::NoFocus);
        m_fullscreenExit->setStyleSheet(
            QStringLiteral("QPushButton#fullscreenExit {"
                           " background: %1; color: %2;"
                           " border: 1px solid %3; border-radius: %4;"
                           " padding: %5 %6; font-weight: 600; }"
                           "QPushButton#fullscreenExit:hover { background: %7; }")
                .arg(Theme::rgba(pal.surface))
                .arg(Theme::rgba(pal.text))
                .arg(Theme::rgba(pal.surfaceEdge))
                .arg(Theme::px(Theme::Space4))
                .arg(Theme::px(Theme::Space2))
                .arg(Theme::px(Theme::Space4))
                .arg(Theme::rgba(pal.surfaceHover)));
        m_fullscreenExit->hide();
        connect(m_fullscreenExit, &QPushButton::clicked, this, &MainWindow::onFullscreen);
    }
    connect(m_zoomBar, &ZoomBar::zoomRequested, this, [this](qreal zoom) {
        // A feedback loop through this connection is what overflowed the stack
        // (0xC00000FD) when the slider was dragged quickly. ZoomBar no longer writes
        // back while the handle is held, and this cap makes any remaining loop a
        // logged no-op instead of a crash.
        if (!m_active)
            return;
        if (m_zoomWiring >= 4) {
            CrashLog::breadcrumb("zoom-reentry",
                                 QStringLiteral("缩放信号已嵌套 %1 层，本次忽略").arg(m_zoomWiring));
            return;
        }
        ++m_zoomWiring;
        m_active->setZoomLevel(zoom);
        --m_zoomWiring;
    });
    connect(m_zoomBar, &ZoomBar::fitWidthRequested, this, [this] {
        if (m_active)
            m_active->setFitWidth(true);
    });
    m_zoomBar->setVisible(false);

    // No document is open at startup: show the home page (neutral status bar,
    // app-name title, no active canvas).
    showHomePage();

    auto *memTimer = new QTimer(this);
    memTimer->setInterval(1500);
    connect(memTimer, &QTimer::timeout, this, &MainWindow::updateMemLabel);
    memTimer->start();
    updateMemLabel();

    // 「工具岛闲置淡出」：一个 5 秒单次定时器 + 一个 QApplication 级事件过滤器。
    // 过滤器挂在应用对象上，所以画布 / 工具岛 / 标签栏 / 状态栏任意位置的输入都会
    // 经过这里（见 eventFilter / noteUserActivity）。间隔与目标不透明度是工具岛的
    // 命名常量（单一来源）。
    m_idleTimer = new QTimer(this);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(InkToolbar::kIdleTimeoutMs);
    connect(m_idleTimer, &QTimer::timeout, this, &MainWindow::onIdleTimeout);
    if (qApp)
        qApp->installEventFilter(this);

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

    // 手掌当橡皮是全局设置：新文档按当前值初始化（已打开的文档由设置页的信号更新）。
    canvas->setPalmEraserEnabled(AppSettings::palmEraserEnabled());

    // 工具栏风格同样是全局设置：新文档按当前值初始化（已打开的文档由设置页的信号更新）。
    if (InkToolbar *bar = canvas->toolbar())
        bar->setStyle(AppSettings::toolbarStyle() == 0 ? InkToolbar::Style::Compact
                                                       : InkToolbar::Style::Text);

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

// --- 「工具岛闲置淡出」----------------------------------------------------
//
// 探测很便宜：一个 QApplication 级事件过滤器，只按事件类型判断"这是不是用户
// 输入"，命中就恢复所有岛并重启 5 秒单次定时器。每个事件里不做任何重活。
//
// 纯 hover（没有按键的鼠标移动）刻意不算动作：教室大屏是触控，鼠标随手一碰 /
// 微动就会不断刷新计时器，那样工具岛将永远不淡出。按住键的拖动算输入。
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::Wheel:
    case QEvent::KeyPress:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        noteUserActivity();
        break;
    case QEvent::MouseMove:
        // 只有按住键的移动才算"拖动"；空手移动是 hover，按上面的口径排除。
        if (static_cast<QMouseEvent *>(event)->buttons() != Qt::NoButton)
            noteUserActivity();
        break;
    default:
        break;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::noteUserActivity()
{
    // 任何输入立刻恢复：setIdleFaded(false) 对已恢复的岛是空操作，所以循环调用
    // 很便宜（文档数很小）。然后重新计满 5 秒。
    for (PdfCanvas *canvas : m_canvases) {
        if (InkToolbar *bar = canvas->toolbar())
            bar->setIdleFaded(false);
    }
    armIdleTimer();
}

void MainWindow::armIdleTimer()
{
    if (!m_idleTimer)
        return;
    if (m_canvases.isEmpty()) {
        m_idleTimer->stop();   // 没有文档就没有工具岛，不必空转
        return;
    }
    m_idleTimer->start(InkToolbar::kIdleTimeoutMs);   // 单次：每次都重新计时
    ++m_idleArms;
}

void MainWindow::onIdleTimeout()
{
    bool anyOverlayOpen = false;
    for (PdfCanvas *canvas : m_canvases) {
        InkToolbar *bar = canvas->toolbar();
        if (!bar)
            continue;
        if (bar->hasOpenOverlay()) {
            // 老师正在选颜色 / 翻页：这一轮不淡出，浮层关了之后下一轮再说。
            anyOverlayOpen = true;
            continue;
        }
        bar->setIdleFaded(true);
    }
    if (anyOverlayOpen)
        m_idleTimer->start(InkToolbar::kIdleTimeoutMs);   // 冻结：稍后再看
}

void MainWindow::testFireIdleTimeout()
{
    onIdleTimeout();          // 与真实 timeout 完全同一条处理路径
}

bool MainWindow::testIdleTimerActive() const
{
    return m_idleTimer && m_idleTimer->isActive();
}

int MainWindow::testIdleTimerIntervalMs() const
{
    return InkToolbar::kIdleTimeoutMs;
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
    // A DPI change re-lays out every page (and can resize the stack): settle any
    // in-flight switch first, so a frozen frame can never be stretched across a
    // scale change. Fullscreen / maximise resizes the stack too.
    if (event->type() == QEvent::DevicePixelRatioChange) {
        settleTransition();
        return;
    }
    if (event->type() != QEvent::WindowStateChange)
        return;

    settleTransition();

    // The island's fullscreen button mirrors the real window state. Every
    // canvas is a document now, so m_canvases is the complete list.
    for (PdfCanvas *canvas : m_canvases) {
        if (InkToolbar *bar = canvas->toolbar())
            bar->setFullscreenActive(isFullScreen());
    }
    // ...and so does the exit pill, which must be reachable on the home / settings pages
    // where no island is shown at all.
    updateFullscreenExit();
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
        const HWND handle = reinterpret_cast<HWND>(winId());
        // The attribute number changed across Win10 builds (19 on 1809-1909, 20 on
        // 20H1+) and an unknown attribute just fails: try the modern one first and
        // fall back, so the title bar follows the theme on every supported Windows.
        if (FAILED(DwmSetWindowAttribute(handle,
                                         static_cast<DWORD>(DWMWA_USE_IMMERSIVE_DARK_MODE),
                                         &dark, sizeof(dark))))
            DwmSetWindowAttribute(handle, 19, &dark, sizeof(dark));
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
    // Settle-first on a document load: the switch that follows (and the layout it
    // triggers) must not start from a half-travelled frozen frame.
    settleTransition();
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

    // 有工具岛可淡出了：立刻开始计 5 秒（此后每次输入都会重新计时）。
    armIdleTimer();

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

    // No real bundle yet: a plain PDF, a Word file or an imported image. Saving into a temp
    // working copy was wrong - the teacher would never find the annotations there, and the
    // 14-day cache sweep would eventually delete them. Like Word with a document that has no
    // file yet, 保存 asks where the .dpz should live; 取消 writes nothing, and the caller (the
    // close prompt) treats that as "keep the document open".
    return saveDocumentAs(index);
}

// Ctrl+Shift+S / the island's 「另存为」: choose between a `.dpz` bundle
// (default) and a flattened, injected PDF that other software can open.
// Returns true only when a file was actually written.
bool MainWindow::saveDocumentAs(int index)
{
    // Self tests cannot drive a modal file dialog: this one-shot flag makes the call report
    // "the user cancelled" instead, which is what the routing assertions check.
    if (m_saveAsCancelled) {
        m_saveAsCancelled = false;
        return false;
    }
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

bool MainWindow::testDocumentDirty(int index) const
{
    if (index < 0 || index >= m_docs.size())
        return false;
    return m_docs.at(index).dirty;
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
// Fullscreen must never be a trap: this pill is shown while - and only while - the window is
// fullscreen, on every page, so leaving fullscreen never depends on the island being visible.
void MainWindow::repositionFullscreenExit()
{
    if (!m_fullscreenExit || !m_fullscreenExit->isVisible())
        return;
    m_fullscreenExit->adjustSize();
    const int margin = Theme::Space4;
    m_fullscreenExit->move(width() - m_fullscreenExit->width() - margin, margin);
    m_fullscreenExit->raise();
}

void MainWindow::updateFullscreenExit()
{
    if (!m_fullscreenExit)
        return;
    m_fullscreenExit->setVisible(isFullScreen());
    repositionFullscreenExit();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    // Settle-first on a window resize: a frozen frame is bound to the old stack
    // geometry, so it must not be blitted across the new one.
    settleTransition();
    QMainWindow::resizeEvent(event);
    repositionFullscreenExit();
}

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
    // switchToPage() performs the release AFTER freezing the outgoing frame, so
    // the animation never plays on a blanked canvas; the target document is
    // captured here because m_active is about to move on.
    PdfCanvas *const previousActive = (next != m_active) ? m_active : nullptr;
    m_active = next;
    switchToPage(next, previousActive);
    wireActiveCanvas(next);
    updateTitle();
}

// The ✕ on a tab / Ctrl+W: ask about unsaved ink first, remove only when the
// user did not cancel (and any chosen save really wrote).
void MainWindow::onTabCloseRequested(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return;
    // Settle-first: the confirmation modal (and the removal after it) must not
    // run while a frozen frame is still travelling.
    settleTransition();
    if (!confirmCloseDocument(index))
        return;
    closeTabAt(index);
}

// The removal itself, after every confirmation has passed.
void MainWindow::closeTabAt(int index)
{
    if (index < 0 || index >= m_canvases.size())
        return;
    settleTransition();

    PdfCanvas *canvas = m_canvases.at(index);
    const bool wasActive = (canvas == m_active);
    // Closing the ACTIVE tab lands on a neighbour; the closed canvas is already
    // out of m_canvases by the time that switch runs, so its index would read as
    // -1 and the slide direction would be guessed. Compute it here instead:
    // closing the last chip falls LEFT (-1), otherwise the chip that slides left
    // into the freed slot enters from the RIGHT (+1). switchToPage consumes and
    // clears this hint, so it can never leak into a later, unrelated switch.
    const bool willSwitch = wasActive && !(m_settingsVisible || m_homeVisible);
    m_pendingDirHint = willSwitch
                           ? ((index < int(m_canvases.size()) - 1) ? +1 : -1)
                           : 0;
    m_canvases.removeAt(index);
    if (index < m_docs.size())
        m_docs.removeAt(index);

    // The strip selects the neighbour and reports it through currentChanged
    // (silent while the settings / home page is up).
    m_tabs->removeTab(index);
    m_pendingDirHint = 0;   // the switch (if any) already consumed it

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
    // Refresh BEFORE the transition freezes the page: the frozen incoming frame
    // must be what the real page shows, or hiding the overlay would jump.
    if (m_homePage)
        m_homePage->refresh();
    switchToPage(m_homePage);
    // No document is active while the home page is up: the status bar shows the
    // neutral values and the hidden canvases keep their rendered pages.
    wireActiveCanvas(nullptr);
    updateTitle();
}

void MainWindow::showSettingsPage()
{
    m_settingsVisible = true;
    m_homeVisible = false;
    m_tabs->setSettingsActive(true);
    m_tabs->setHomeActive(false);
    switchToPage(m_settingsPage);
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
    // The document the settings / home page was entered FROM: leaving it for
    // another document must still drop its bitmaps (the helper owns that policy;
    // capture it before m_active is reassigned).
    PdfCanvas *const previousActive = m_active;
    m_settingsVisible = false;
    m_homeVisible = false;
    m_tabs->setSettingsActive(false);
    m_tabs->setHomeActive(false);
    m_active = canvas;
    switchToPage(canvas, previousActive);
    wireActiveCanvas(canvas);
    updateTitle();
}

// ---------------------------------------------------------------------------
// Page-switch transitions: ONE helper, every entry point.
//
// 纵向 (either page is 主页 / 设置): the outgoing sinks ~10% of the page height
// and fades 1 -> 0.55; the incoming starts one full page height BELOW and rises
// to 0, drawn on top so it covers the outgoing as it rises.
// 横向 (document <-> document): the outgoing does not move; the incoming slides
// in from the side that matches the tab order (a larger index enters from the
// right edge and moves left) and covers it. In both the incoming is on top.
//
// dirHint: 0 = derive from the tab indices (from < to => +1); +1 = enter from the
// right; -1 = enter from the left. A tab close passes it explicitly because by
// then the closed canvas is already out of m_canvases (its index reads as -1).
// ---------------------------------------------------------------------------

bool MainWindow::transitionCanAnimate() const
{
    if (!m_transAnim || !m_transLayer || !m_stack)
        return false;
    if (m_stack->width() <= 0 || m_stack->height() <= 0)
        return false;
    // A window that is not on screen - or a headless WA_DontShowOnScreen window -
    // must not animate: the switch settles at once. The self test forces the
    // animation back on with testForceTransitions() to sample deterministic frames.
    if (!m_transForceAnim
        && (!isVisible() || testAttribute(Qt::WA_DontShowOnScreen)))
        return false;
    return true;
}

void MainWindow::switchToPage(QWidget *incoming, PdfCanvas *releaseOnLeave, int dirHint)
{
    if (!m_stack || !incoming)
        return;

    // One-shot close direction: consumed and cleared here so it can never leak
    // into a later, unrelated switch.
    if (dirHint == 0)
        dirHint = m_pendingDirHint;
    m_pendingDirHint = 0;

    // A second switch mid-flight settles the previous leg first: exactly one
    // overlay / driver can ever be alive, so a stale or duplicated page is
    // impossible and the new leg starts from a fully landed page.
    settleTransition();

    QWidget *const outgoing = m_stack->currentWidget();
    const bool samePage = (outgoing == incoming);

    PageTransition kind = PageTransition::None;
    int direction = 0;
    if (!samePage) {
        const bool overlayInvolved = (incoming == m_homePage) || (incoming == m_settingsPage)
                                     || (outgoing == m_homePage) || (outgoing == m_settingsPage);
        kind = overlayInvolved ? PageTransition::Vertical : PageTransition::Horizontal;
        if (kind == PageTransition::Horizontal) {
            if (dirHint != 0) {
                direction = dirHint;
            } else {
                const int from = docIndex(qobject_cast<PdfCanvas *>(outgoing));
                const int to   = docIndex(qobject_cast<PdfCanvas *>(incoming));
                // To a tab on the RIGHT (larger index) enters from the right edge
                // and moves left; to the LEFT enters from the left edge.
                direction = (from >= 0 && to >= 0 && to < from) ? -1 : +1;
            }
        }
    }

    // Freeze the outgoing BEFORE the memory policy drops its rendered bitmaps:
    // the animation must never play on a blanked canvas.
    const bool wantAnim = !samePage && transitionCanAnimate();
    QPixmap outPix;
    if (wantAnim)
        outPix = outgoing->grab();

    // The logical switch happens now: the incoming page is the current widget
    // for the whole animation, so input (the overlay is mouse-transparent) and
    // any interrupt already target the right page.
    m_stack->setCurrentWidget(incoming);

    QPixmap inPix;
    if (wantAnim) {
        // StackOne gives geometry only to the CURRENT widget; activate the
        // layout so the incoming really has the stack's rect before the grab
        // (otherwise the frozen frame would be taken at a stale size).
        if (m_stack->layout())
            m_stack->layout()->activate();
        inPix = incoming->grab();
    }

    // Memory policy, unchanged: the document we leave for a DIFFERENT document
    // drops its rendered page bitmaps (it keeps its handle and its ink).
    if (releaseOnLeave && releaseOnLeave != incoming)
        releaseOnLeave->releaseCachedPages();

    if (samePage)
        return;

    if (wantAnim && !outPix.isNull() && !inPix.isNull()
        && outgoing->size() == incoming->size()) {
        startTransition(kind, direction, outPix, inPix, outgoing->size());
    } else {
        // Defensive path (no driver / hidden window / zero-size page / failed
        // grab): the logical switch already happened; just stay settled.
        settleTransition();
    }
}

void MainWindow::startTransition(PageTransition kind, int direction,
                                 const QPixmap &outgoing, const QPixmap &incoming,
                                 const QSize &pageSize)
{
    if (!m_transAnim || !m_transLayer)
        return;

    m_transKind = kind;
    m_transDir = direction;
    m_transSpan = (kind == PageTransition::Vertical) ? pageSize.height() : pageSize.width();
    // Sink distance: 10 % of the page height (inside the requested 8-15 %).
    m_transSink = qRound(pageSize.height() * kPageSinkFraction);
    m_transOutPix = outgoing;
    m_transInPix  = incoming;

    m_transLayer->setGeometry(m_stack->rect());
    m_transLayer->show();
    m_transLayer->raise();

    // The driver is shared: stop it before re-arming so the two legs can never
    // run together (switchToPage already settled, but be explicit).
    m_transAnim->stop();
    m_transActive = true;
    applyTransitionProgress(0.0);        // explicit first frame: no flash
    m_transAnim->start();
}

void MainWindow::applyTransitionProgress(qreal eased)
{
    if (!m_transActive || !m_transLayer)
        return;

    const qreal p = qBound<qreal>(0.0, eased, 1.0);
    if (m_transKind == PageTransition::Vertical) {
        // Old sheet sinks down and fades a little; new sheet starts one full
        // page height BELOW its final position and rises to 0.
        m_transOutX = 0;
        m_transOutY = qRound(m_transSink * p);
        m_transInX  = 0;
        m_transInY  = qRound(m_transSpan * (1.0 - p));
        m_transOutOpacity = 1.0 - (1.0 - kPageOutgoingMinOpacity) * p;
    } else {
        // The outgoing does not move; the incoming slides in along the tab order
        // (from the right when moving to a larger index, from the left when
        // moving to a smaller one).
        m_transOutX = 0;
        m_transOutY = 0;
        m_transInX  = qRound(m_transDir * m_transSpan * (1.0 - p));
        m_transInY  = 0;
        m_transOutOpacity = 1.0;
    }

    m_transLayer->setFrame(m_transOutPix, m_transInPix,
                           m_transOutX, m_transOutY, m_transInX, m_transInY,
                           m_transOutOpacity);
    // The stack raised the incoming page on setCurrentWidget; keep the overlay
    // above it so the travelling sheet really is on top.
    m_transLayer->raise();
    m_transLayer->update();
}

void MainWindow::settleTransition()
{
    const bool wasActive = m_transActive;
    // Re-entrancy guard FIRST: stop() below may re-enter through finished().
    m_transActive = false;
    if (m_transAnim && m_transAnim->state() != QAbstractAnimation::Stopped)
        m_transAnim->stop();

    m_transKind = PageTransition::None;
    m_transDir = 0;
    m_transOutX = m_transOutY = m_transInX = m_transInY = 0;
    m_transOutOpacity = 1.0;
    // The two frozen frames are released here - the finished handler - which is
    // the only transient memory a switch holds.
    m_transOutPix = QPixmap();
    m_transInPix  = QPixmap();
    if (m_transLayer) {
        m_transLayer->clearFrame();
        m_transLayer->hide();
    }
    if (wasActive)
        ++m_transFinishedCount;
}

// --- deterministic self-test hooks (no event loop, no sleep) ----------------

int MainWindow::testTransitionDurationMs() const
{
    return m_transAnim ? m_transAnim->duration() : 0;
}

void MainWindow::testTransitionAt(int ms)
{
    if (!m_transAnim || !m_transActive)
        return;   // nothing in flight: the hook has nothing to sample

    const int duration = qMax(1, m_transAnim->duration());
    const int at = qBound(0, ms, duration);

    // Stop the clock: the animation's own timer must not advance while the
    // assertion samples.
    if (m_transAnim->state() != QAbstractAnimation::Stopped)
        m_transAnim->pause();
    m_transAnim->setCurrentTime(at);

    // Apply this frame explicitly: same curve, same endpoints, so a paused
    // sample and a running animation agree exactly.
    const QEasingCurve easing(QEasingCurve::OutCubic);
    applyTransitionProgress(easing.valueForProgress(qreal(at) / qreal(duration)));

    if (at >= duration)
        settleTransition();   // the same finish path as a natural end
}

bool MainWindow::testTransitionOverlayVisible() const
{
    return m_transLayer && m_transLayer->isVisible();
}

int MainWindow::testTransitionOverlayChildCount() const
{
    // The overlay must never adopt a real page (that is the point of the frozen
    // frames): zero children, always.
    return m_transLayer ? int(m_transLayer->children().size()) : 0;
}

qint64 MainWindow::testTransitionFrameBytes() const
{
    return m_transLayer ? m_transLayer->frameBytes() : 0;
}

int MainWindow::testTransitionKind() const
{
    switch (m_transKind) {
    case PageTransition::Vertical:
        return 1;
    case PageTransition::Horizontal:
        return 2;
    case PageTransition::None:
        break;
    }
    return 0;
}

int MainWindow::testTransitionDirection() const
{
    return m_transDir;
}

int MainWindow::testTransitionStartOffset() const
{
    return m_transSpan;
}

int MainWindow::testStackPageKind() const
{
    if (!m_stack)
        return -1;
    QWidget *const page = m_stack->currentWidget();
    if (page == m_homePage)
        return 0;
    if (page == m_settingsPage)
        return 1;
    if (qobject_cast<PdfCanvas *>(page))
        return 2;
    return -1;
}

int MainWindow::testActiveDocumentIndex() const
{
    return m_active ? docIndex(m_active) : -1;
}

int MainWindow::testStatusDocumentIndex() const
{
    return m_statusCanvas ? docIndex(m_statusCanvas) : -1;
}

int MainWindow::testStackWidgetCount() const
{
    return m_stack ? m_stack->count() : 0;
}

int MainWindow::testTabsCurrentIndex() const
{
    return m_tabs ? m_tabs->currentIndex() : -1;
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
