#pragma once

#include <QMainWindow>
#include <QVector>

class DocumentTabs;
class HomePage;
class PdfCanvas;
class SettingsPage;
class QEvent;
class QKeyEvent;
class QLabel;
class QStackedWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    // Open a PDF directly (used by the GUI smoke test / CLI arg).
    void openPath(const QString &path);

protected:
    // Dropping a PDF onto the window opens it in a new tab.
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    // Esc leaves fullscreen; F11 is handled by a window-level QAction.
    void keyPressEvent(QKeyEvent *event) override;
    // Window state changes are pushed down to every canvas toolbar.
    void changeEvent(QEvent *event) override;

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
        QString sourcePdf;    // the PDF the canvas actually opened
        QString title;        // tab strip title
        QString tempPdf;      // temp PDF extracted from a bundle (may be empty)
    };

    PdfCanvas *createCanvas();              // builds + wires one document canvas
    void       wireActiveCanvas(PdfCanvas *canvas);
    void       showHomePage();              // switch the stack to the home page
    void       showSettingsPage();          // switch the stack to the settings page
    void       showCanvasPage(PdfCanvas *canvas);   // switch back to a document page
    void       refreshStatus();
    void       updateTitle();
    int        docIndex(PdfCanvas *canvas) const;
    QString    docTitle(PdfCanvas *canvas) const;
    void       saveDocumentAs();            // shared by 另存为 and the first 保存

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
};
