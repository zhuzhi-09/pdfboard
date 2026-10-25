#pragma once

#include <QUrl>
#include <QVector>
#include <QWidget>

class QAbstractButton;
class QButtonGroup;
class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVariantAnimation;

namespace UpdateCheck {
class Checker;
struct Result;
}

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
//   * 「检查更新」 - on demand only, see UpdateCheck.
//
// Look: the Windows 11 Settings structure in the app's MD3 language. A
// navigation rail on the left (app identity block + one pill item per
// category, MD3 navigation-drawer style), and on the right a fixed breadcrumb
// title 「设置 › 外观」 over the category's own scrolling page. Inside a page,
// muted group headers sit over one card per setting: leading glyph, title,
// muted description, trailing control. Below a width threshold the rail folds
// to icons only. Every geometric value comes from Theme::metrics / the spacing
// scale, so the layout holds up at 150 % / 200 % DPI.
class SettingsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPage(QWidget *parent = nullptr);

    // Re-applies the palette-derived pieces (page background, sheet) after the
    // application theme changed; the glyphs paint from the live palette.
    void refreshTheme();

    // Self-test / screenshot hooks.
    int          testCategoryCount() const { return m_categories.size(); }
    QString      testCategoryName(int index) const;
    void         testShowCategory(int index) { showCategory(index); }
    QScrollArea *testCurrentScroll() const;
    bool         testRailCompact() const { return m_railCompact; }
    // Clicks the 颜色模式 dropdown like a user would; returns its popup menu.
    QWidget     *testOpenThemeMenu();
    bool         testTransitionRunning() const;
    void         testSetTransitionProgress(qreal t);   // freeze a frame for screenshots

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
    bool eventFilter(QObject *, QEvent *event) override;   // page viewports resized
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
    void onCheckUpdate();

private:
    // One entry of the navigation rail and its page.
    struct Category {
        QString          name;
        int              glyph = 0;          // IconPainter::Glyph
        QAbstractButton *navItem = nullptr;
        QScrollArea     *scroll  = nullptr;
        QWidget         *column  = nullptr;  // the centred content column
    };

    void buildUi();
    QWidget *addCategory(const QString &name, int glyph);   // returns the column
    void showCategory(int index);
    void layoutForWidth();                   // centre the columns, fold the rail
    void onUpdateResult(const UpdateCheck::Result &result);
    void resetUpdateRow();
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

    QWidget         *m_root      = nullptr;   // carries the page sheet (not `this`:
                                              // message boxes keep their native look)
    QWidget         *m_rail      = nullptr;   // navigation rail
    QWidget         *m_identity  = nullptr;   // app identity block on the rail
    QWidget         *m_identityText = nullptr;
    QStackedWidget  *m_stack     = nullptr;   // one scrolling page per category
    QWidget         *m_content   = nullptr;   // breadcrumb + stack (what a switch animates)
    QWidget         *m_transition = nullptr;  // MD3 fade-through overlay over m_content
    QVariantAnimation *m_transitionAnim = nullptr;
    QWidget         *m_header    = nullptr;   // breadcrumb row, centred like the columns
    QLabel          *m_crumbLeaf = nullptr;   // 「设置 › <category>」
    QVector<Category> m_categories;
    int              m_current = 0;
    bool             m_railCompact = false;

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

    // 检查更新: the row's state line and its button (检查更新 -> 前往下载).
    UpdateCheck::Checker *m_updater = nullptr;
    QLabel      *m_updateNote   = nullptr;
    QPushButton *m_updateButton = nullptr;
    QUrl         m_updatePage;             // set once a newer release was found

    int           m_maxColumn = 0;
};
