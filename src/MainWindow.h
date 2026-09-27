#pragma once

#include "UpdateChecker.h"

#include <QMainWindow>
#include <QPixmap>
#include <QVector>

class DocumentTabs;
class HomePage;
class PdfCanvas;
class SettingsPage;
class ZoomBar;
class QCloseEvent;
class QEvent;
class QKeyEvent;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVariantAnimation;
class PageTransitionLayer;   // the frozen-frame overlay (defined in MainWindow.cpp)

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Open a PDF directly (used by the GUI smoke test / CLI arg).
    void openPath(const QString &path);

    // Save routing, for the self tests. A modal file dialog cannot run headlessly, so the
    // save-as flow can be stubbed to "cancelled": the assertions then check what the routing
    // does (or does not) write, and that the document stays unsaved.
    void testStubSaveAsCancel() { m_saveAsCancelled = true; }
    bool testDocumentDirty(int index) const;

    // --- 页面切换动画的自测钩子（见 MainWindow.cpp 的 switchToPage）----------
    // 和 InkToolbar::testDrawerAt 同一套路：把动画钉到 [0, 时长] 内的 ms 毫秒处
    // 并立即应用该帧，不跑事件循环、不 sleep，采样完全确定。
    int   testTransitionDurationMs() const;
    void  testTransitionAt(int ms);
    bool  testTransitionActive() const { return m_transActive; }
    int   testTransitionKind() const;          // 0 = 无 / 1 = 纵向 / 2 = 横向
    int   testTransitionDirection() const;     // 横向 +1 从右入 / -1 从左入；纵向 0
    int   testTransitionStartOffset() const;   // 纵向 = 页面高；横向 = 页面宽
    int   testTransitionOutgoingOffsetY() const { return m_transOutY; }
    int   testTransitionIncomingOffsetY() const { return m_transInY; }
    int   testTransitionOutgoingOffsetX() const { return m_transOutX; }
    int   testTransitionIncomingOffsetX() const { return m_transInX; }
    // 0.55 -> 550（千分数，避免浮点比较）。
    int   testTransitionOutgoingOpacityPermille() const
    {
        return qRound(m_transOutOpacity * 1000.0);
    }
    bool   testTransitionOverlayVisible() const;
    int    testTransitionOverlayChildCount() const;   // 叠加层里绝不该有子件
    qint64 testTransitionFrameBytes() const;          // 两张冻结帧的字节数（4K 代价）
    int    testTransitionFinishedCount() const { return m_transFinishedCount; }
    // 无屏自测里强制走动画路径（真实入口在窗口不可见 / WA_DontShowOnScreen 时直接落位）。
    void   testForceTransitions(bool on) { m_transForceAnim = on; }

    // 当前页 / 文档接线，供动画断言核对"落到哪一页、状态栏跟的是谁"。
    int   testStackPageKind() const;          // 0 主页 / 1 设置 / 2 文档 / -1 无
    int   testDocumentCount() const { return int(m_canvases.size()); }
    int   testActiveDocumentIndex() const;
    int   testStatusDocumentIndex() const;    // m_statusCanvas 在标签序里的下标
    int   testStackWidgetCount() const;
    int   testTabsCurrentIndex() const;

protected:
    // Dropping a PDF onto the window opens it in a new tab.
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    // Esc leaves fullscreen; F11 is handled by a window-level QAction.
    void keyPressEvent(QKeyEvent *event) override;
    // Window state changes are pushed down to every canvas toolbar.
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // Closing the window asks about every dirty document; a single 取消
    // aborts the whole close.
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onOpen();
    void onSave();
    void onSaveAs();
    void onSettings();
    void onFullscreen();
    void onTabCurrentChanged(int index);
    void onTabCloseRequested(int index);
    void onCloseCurrent();
    void onPageChanged(int page, int count);
    void onRenderMeasured(qint64 ms, QSize size);
    void onInkChanged(int strokes);
    void updateMemLabel();
    // Re-applies the current theme to the whole window chrome; called at
    // startup and whenever the setting or the OS colour scheme changes.
    void applyTheme();
    void onSystemColorSchemeChanged();

private:
    // Per-document bookkeeping that lives alongside each canvas, in tab order.
    struct DocumentInfo {
        QString bundlePath;   // `.dpz` path (empty for a plain-PDF document)
        QString wordPath;     // source `.docx`/`.doc` (empty for PDF / `.dpz`)
        // Source image (empty unless this document was imported from one). Like a
        // Word file it is only ever read: the canvas annotates the converted temp
        // PDF, and the .dpz records the image as its source.
        QString imagePath;
        QString sourcePdf;    // the PDF the canvas actually opened
        QString title;        // tab strip title
        QString tempPdf;      // temp PDF backing the document (may be empty)
        bool    dirty = false;  // ink edited since open / last save
    };

    PdfCanvas *createCanvas();              // builds + wires one document canvas
    void       wireActiveCanvas(PdfCanvas *canvas);
    // Q_INVOKABLE: the deterministic self tests switch pages through
    // QMetaObject::invokeMethod so they need no access change, and they assert the
    // invocation actually succeeded - a mistyped name used to fail silently and leave
    // the following assertion vacuously true (showHomePage() was not a slot).
    Q_INVOKABLE void       showHomePage();              // switch the stack to the home page
    Q_INVOKABLE void       showSettingsPage();          // switch the stack to the settings page
    void       showCanvasPage(PdfCanvas *canvas);   // switch back to a document page
    void       refreshStatus();
    // Shows/hides the status-bar zoom control for the current page: it only makes
    // sense on a document (not on the home or settings page), and it always shows
    // the active canvas' own zoom.
    void       updateZoomBar();
    void       updateTitle();
    int        docIndex(PdfCanvas *canvas) const;
    QString    docTitle(PdfCanvas *canvas) const;
    // Saves one document: its own bundle when it has one, otherwise the
    // per-source working copy in %TEMP%. Returns true only when bytes were
    // written. Shared by 保存, 另存为's close prompt and the close prompts.
    bool       saveDocument(int index);
    // The 另存为 dialog for one document. Returns true when a file was written.
    bool       saveDocumentAs(int index);
    // The 保存 / 另存为 / 舍弃 / 取消 close conversation. Returns true when the
    // caller may proceed to close the document.
    bool       confirmCloseDocument(int index);
    void       closeTabAt(int index);       // the removal, after confirmations
    void       markDocumentDirty(PdfCanvas *canvas);
    void       markDocumentSaved(int index);
    void       refreshTabTitle(int index);  // source name + the dirty `*`

    // --- 页面切换动画（单一来源）--------------------------------------------
    // 所有换页都走 switchToPage()：
    //   * 纵向（任一页是主页 / 设置）：旧页下沉 ~10% 并淡化 1 -> 0.55，新页从
    //     下方一整页高处升起盖住旧页；
    //   * 横向（文档 <-> 文档）：旧页不动，新页沿标签顺序从左右滑入盖住旧页。
    // 新页永远画在最上层（冻结帧叠加层），底部标签 / 状态栏 / ZoomBar 不参与动画。
    // releaseOnLeave 是"离开时释放位图"的内存策略目标（原逻辑只在标签切换时释放
    // 上一个活动文档，这里原样保留）。
    // dirHint: 0 = 按标签下标推导；+1 = 从右缘进入；-1 = 从左缘进入（关标签时被关
    // 的画布已不在 m_canvases 里，下标会读成 -1，所以关标签显式传方向）。
    enum class PageTransition { None, Vertical, Horizontal };
    void switchToPage(QWidget *incoming, PdfCanvas *releaseOnLeave = nullptr, int dirHint = 0);
    void startTransition(PageTransition kind, int direction,
                         const QPixmap &outgoing, const QPixmap &incoming,
                         const QSize &pageSize);
    void applyTransitionProgress(qreal eased);
    // 收尾（自然结束与自测钉到末帧共用同一条路径）：停表、隐藏叠加层、释放两张
    // 冻结帧（finished 处理）。
    void settleTransition();
    bool transitionCanAnimate() const;

    // Update check: once shortly after launch, and again after every document
    // opens (throttled). A newer release is announced at most once per session -
    // a teacher opening twenty files must not be interrupted twenty times, and
    // the Settings page keeps the changelog for whenever they look.
    void       checkForUpdates(bool fromFileOpen);
    void       promptUpdate(const UpdateChecker::UpdateInfo &info);
    void       openUpdateSettings();        // settings page, update card in view

    QStackedWidget *m_stack = nullptr;      // one PdfCanvas per open document
    DocumentTabs   *m_tabs  = nullptr;      // the docked strip under the pages

    QVector<PdfCanvas *>    m_canvases;     // open documents, in tab order
    QVector<DocumentInfo>   m_docs;         // parallel to m_canvases
    bool                    m_fullscreenWasMaximized = false;
    // The document canvas the user last worked on. NULL while the home page is
    // shown with no document open - every use must be null-safe.
    PdfCanvas *m_active       = nullptr;
    PdfCanvas *m_statusCanvas = nullptr;    // the canvas the status bar follows

    // The home page and the settings page are pages of the stack, NOT
    // documents: they never enter m_canvases, and closing tabs never touches
    // them. The app starts on the home page and returns to it when the last
    // document is closed.
    HomePage     *m_homePage     = nullptr;
    bool          m_homeVisible  = false;
    SettingsPage *m_settingsPage   = nullptr;
    bool          m_settingsVisible = false;

    QLabel *m_pageLabel   = nullptr;
    QLabel *m_renderLabel = nullptr;
    QLabel *m_inkLabel    = nullptr;
    QLabel *m_memLabel    = nullptr;
    ZoomBar *m_zoomBar    = nullptr;   // Word-like zoom control, document pages only
    // Fullscreen exit pill. The island's fullscreen button hides with the island (home and
    // settings pages), which used to leave fullscreen with no on-screen way out.
    QPushButton *m_fullscreenExit = nullptr;
    void updateFullscreenExit();
    void repositionFullscreenExit();
    // One-shot: self tests cannot drive a modal file dialog, so the save-as flow is stubbed to
    // "cancelled" - see the public testStubSaveAsCancel()/testDocumentDirty() above.
    bool m_saveAsCancelled = false;
    // Depth of the zoom signal chain (bar -> canvas -> bar). A value above one means
    // something is feeding back; the cap turns that into a logged no-op instead of a
    // stack overflow, and writes a breadcrumb so a crash report names the culprit.
    int      m_zoomWiring = 0;

    UpdateChecker::Client *m_updateClient = nullptr;
    QString                m_updateAnnouncedVersion;  // per session
    qint64                 m_lastUpdateCheckMs = 0;   // throttles file-open checks

    // The page-switch transition. ONE driver for every switch; the two frozen
    // frames are the only transient memory (see the comment on
    // PageTransitionLayer in MainWindow.cpp and docs 2.73).
    QVariantAnimation  *m_transAnim  = nullptr;   // the eased 0..1 progress driver
    PageTransitionLayer *m_transLayer = nullptr;  // opaque overlay child of m_stack
    PageTransition      m_transKind  = PageTransition::None;
    bool                m_transActive = false;
    bool                m_transForceAnim = false; // test hook: animate while headless
    int                 m_transDir  = 0;          // horizontal: +1 right->left, -1 left->right
    int                 m_pendingDirHint = 0;     // one-shot close direction (see closeTabAt)
    int                 m_transSpan = 0;          // page height (V) / width (H)
    int                 m_transSink = 0;          // V: how far the outgoing sinks
    int                 m_transOutX = 0;
    int                 m_transOutY = 0;
    int                 m_transInX  = 0;
    int                 m_transInY  = 0;
    qreal               m_transOutOpacity = 1.0;
    QPixmap             m_transOutPix;            // frozen outgoing frame
    QPixmap             m_transInPix;             // frozen incoming frame (on top)
    int                 m_transFinishedCount = 0;
};
