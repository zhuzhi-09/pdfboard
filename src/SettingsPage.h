#pragma once

#include <QScrollArea>

class QAbstractButton;
class QButtonGroup;
class QFrame;
class QLabel;
class QPushButton;
class QWidget;

// 设置行的下拉选择控件。和 InkToolbar.h 里的 PenPalette / EraserPalette 同一
// 做法：完整定义在 .cpp 里（文件局部），头文件只持有指针。
class SettingsSelect;

// Full-area settings PAGE, shown as one page inside MainWindow's stack - not a
// dialog. It owns the machine-level settings and nothing else:
//   * 「开机自动启动」  - read from / written straight to the Run registry key
//     through AppSettings, so the control always shows the real state.
//   * 「设为 PDF 默认打开方式」 - registers the ProgID + Capabilities and then
//     walks the user to Windows' 默认应用 page, which is the only way a
//     desktop app can actually become the default handler.
//   * 「外观」 - 系统 / 浅色 / 深色, stored through AppSettings and applied by
//     the host through the themeChanged() signal.
//   * 「打开 Word 文档时」 - 每次询问 / 批注 / 用 Word 打开, stored through
//     AppSettings; MainWindow reads the mode whenever a Word file is opened.
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
    // 手掌当橡皮的开关变了：主窗口据此更新所有已打开的文档（新文档在创建时读取）。
    void palmEraserChanged();
    // 工具栏风格变了（简约 / 文字）：主窗口据此更新所有已打开文档的工具岛。
    void toolbarStyleChanged();
    // A transient line for the window's status bar (the Word settings restore
    // result; the page itself owns no status bar).
    void statusMessage(const QString &message);

protected:
    bool eventFilter(QObject *, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onAutoStartToggled(bool on);
    void onPalmEraserToggled(bool on);
    void onReduceMotionToggled(bool on);
    void onRegisterPdf();
    void onChooseSaveDir();
    void onResetSaveDir();
    void onDebugLogToggled(bool on);
    void onOpenLogDir();
    void onThemePicked(int mode);
    void onWordModePicked(int mode);
    void onWordEnginePicked(int engine);
    void onChooseSoffice();
    void onClearSoffice();
    void onOpenSofficePage();
    void onToolbarStylePicked(int style);
    void onThemePresetPicked(int preset);
    void onPaletteStylePicked(int style);
    void onWordNagFixToggled(bool on);
    void onRestoreWordNag();

private:
    void buildUi();
    void centreColumn();
    void refreshSavePath();
    void refreshLogPath();
    void refreshThemeNote();
    void refreshWordNag();
    void syncThemeSelect();
    void syncWordSelect();
    void syncWordEngineSelect();
    void refreshLibreOfficeStatus();
    void syncToolbarStyleSelect();
    void syncThemePreset();
    void syncPaletteStyleSelect();

    QWidget         *m_body      = nullptr;   // the scrolling body
    QWidget         *m_column    = nullptr;   // the centred content column
    QLabel          *m_gear      = nullptr;   // page title gear (repixmapped)
    QAbstractButton *m_autoStart = nullptr;
    QAbstractButton *m_palmEraser = nullptr;   // 手掌当橡皮（实验性，默认关）
    QAbstractButton *m_reduceMotion = nullptr; // 减少动效（默认关）
    QPushButton     *m_register  = nullptr;
    QLabel          *m_savePathLabel = nullptr;
    QPushButton     *m_chooseSaveDir = nullptr;
    QPushButton     *m_resetSaveDir  = nullptr;
    QAbstractButton *m_debugLog      = nullptr;
    QLabel          *m_logPathLabel  = nullptr;
    QLabel          *m_logEnvNote    = nullptr;
    QPushButton     *m_openLogDir    = nullptr;

    // 外观: the 外观 dropdown and the hint line under it.
    SettingsSelect *m_themeSelect = nullptr;
    QLabel         *m_themeNote = nullptr;

    // 工具栏风格: 简约（纯图标，默认）/ 文字（图标+名称）。
    SettingsSelect *m_toolbarStyleSelect = nullptr;

    // 主题色: 每个种子色一枚圆形色板（互斥）。控件本体 SeedSwatch 是 .cpp 里的
    // 文件局部类，所以这里只保存基类指针。
    QButtonGroup    *m_themePresetGroup = nullptr;
    QAbstractButton *m_themePresetSwatches[8] = {};

    // 调色板风格: 保真 / 均衡 / 中性 / 单色，由种子色推导配色的算法。
    SettingsSelect *m_paletteStyleSelect = nullptr;

    // 打开 Word 文档时: 每次询问 / 批注 / 用 Word 打开，persisted through
    // AppSettings::setWordOpenMode (0 每次询问 / 1 批注 / 2 用 Word 打开).
    SettingsSelect *m_wordSelect = nullptr;
    SettingsSelect *m_wordEngineSelect = nullptr;
    QLabel *m_libreOfficeStatus = nullptr;   // LibreOffice 附加包的检测状态
    QPushButton *m_chooseSoffice = nullptr;
    QPushButton *m_clearSoffice = nullptr;
    QPushButton *m_openSofficePage = nullptr;

    // The Word nag fix: the toggle persists AppSettings::wordNagFixEnabled and
    // the button restores according to WordConvert::nagRestoreMode() (exact
    // backup replay / Word defaults / disabled while nothing was modified).
    // m_wordNagNote is the row's state line, kept in sync by refreshWordNag().
    QAbstractButton *m_wordNagFix     = nullptr;
    QPushButton     *m_restoreWordNag = nullptr;
    QLabel          *m_wordNagNote    = nullptr;

    int           m_maxColumn = 0;
};
