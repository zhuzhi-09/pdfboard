#pragma once

#include <QScrollArea>

class QAbstractButton;
class QLabel;
class QPushButton;
class QWidget;

// Full-area settings PAGE, shown as one page inside MainWindow's stack - not a
// dialog. It owns the same two machine-level settings the old modal sheet did,
// and nothing else:
//   * 「开机自动启动」  - read from / written straight to the Run registry key
//     through AppSettings, so the control always shows the real state.
//   * 「设为 PDF 默认打开方式」 - registers the ProgID + Capabilities and then
//     walks the user to Windows' 默认应用 page, which is the only way a
//     desktop app can actually become the default handler.
//
// Look: WinUI-style settings - the page sits on the canvas "desk" surface, a
// centred column (at most metrics.touch * 14 wide) carries the title, muted
// section headers and white rounded cards with hairline separators. Every
// geometric value comes from Theme::metrics / the spacing scale, so the layout
// holds up at 150 % / 200 % DPI. The page scrolls when the window is short.
class SettingsPage : public QScrollArea
{
    Q_OBJECT
public:
    explicit SettingsPage(QWidget *parent = nullptr);

protected:
    bool eventFilter(QObject *, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onAutoStartToggled(bool on);
    void onRegisterPdf();
    void onChooseSaveDir();
    void onResetSaveDir();
    void onDebugLogToggled(bool on);
    void onOpenLogDir();

private:
    void buildUi();
    void centreColumn();
    void refreshSavePath();
    void refreshLogPath();

    QWidget         *m_body      = nullptr;   // the scrolling body
    QWidget         *m_column    = nullptr;   // the centred content column
    QAbstractButton *m_autoStart = nullptr;
    QPushButton     *m_register  = nullptr;
    QLabel          *m_savePathLabel = nullptr;
    QPushButton     *m_chooseSaveDir = nullptr;
    QPushButton     *m_resetSaveDir  = nullptr;
    QAbstractButton *m_debugLog      = nullptr;
    QLabel          *m_logPathLabel  = nullptr;
    QLabel          *m_logEnvNote    = nullptr;
    QPushButton     *m_openLogDir    = nullptr;
    int              m_maxColumn = 0;
};
