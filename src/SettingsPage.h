#pragma once

#include <QScrollArea>

class QAbstractButton;
class QButtonGroup;
class QLabel;
class QPushButton;
class QWidget;

// Full-area settings PAGE, shown as one page inside MainWindow's stack - not a
// dialog. It owns the machine-level settings and nothing else:
//   * 「开机自动启动」  - read from / written straight to the Run registry key
//     through AppSettings, so the control always shows the real state.
//   * 「设为 PDF 默认打开方式」 - registers the ProgID + Capabilities and then
//     walks the user to Windows' 默认应用 page, which is the only way a
//     desktop app can actually become the default handler.
//   * 「外观」 - 系统 / 浅色 / 深色, stored through AppSettings and applied by
//     the host through the themeChanged() signal.
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

    // Re-applies the palette-derived pieces (page background, sheet, gear
    // pixmap) after the application theme changed.
    void refreshTheme();

signals:
    void themeChanged();     // an appearance mode was picked and stored

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
    void onThemePicked(int mode);

private:
    void buildUi();
    void centreColumn();
    void refreshSavePath();
    void refreshLogPath();
    void refreshThemeNote();
    void syncThemeSegment();

    QWidget         *m_body      = nullptr;   // the scrolling body
    QWidget         *m_column    = nullptr;   // the centred content column
    QLabel          *m_gear      = nullptr;   // page title gear (repixmapped)
    QAbstractButton *m_autoStart = nullptr;
    QPushButton     *m_register  = nullptr;
    QLabel          *m_savePathLabel = nullptr;
    QPushButton     *m_chooseSaveDir = nullptr;
    QPushButton     *m_resetSaveDir  = nullptr;
    QAbstractButton *m_debugLog      = nullptr;
    QLabel          *m_logPathLabel  = nullptr;
    QLabel          *m_logEnvNote    = nullptr;
    QPushButton     *m_openLogDir    = nullptr;

    // 外观: three mutually exclusive segments and the hint line under them.
    QButtonGroup *m_themeGroup = nullptr;
    QPushButton  *m_themeSegments[3] = {};
    QLabel       *m_themeNote = nullptr;
    int           m_maxColumn = 0;
};
