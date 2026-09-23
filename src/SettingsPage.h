#pragma once

#include "UpdateChecker.h"

#include <QScrollArea>

class QAbstractButton;
class QButtonGroup;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
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

    // Show a check result that came from elsewhere (the launch / file-open check
    // in MainWindow) so the update card always reflects the newest news, and bring
    // the card into view when the teacher accepts the "new version" dialog.
    void setUpdateInfo(const UpdateChecker::UpdateInfo &info);
    void focusUpdateSection();

    // --- Test hooks (used by `--selftest-update`) --------------------------
    // The update card's changelog area is the only place the teacher can read the
    // release notes after dismissing the dialog, so its content is asserted.
    QString testUpdateNotes() const;
    bool    testUpdateNotesShown() const;

signals:
    void themeChanged();     // an appearance mode was picked and stored
    // A transient line for the window's status bar (the Word settings restore
    // result; the page itself owns no status bar).
    void statusMessage(const QString &message);

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
    void onWordModePicked(int mode);
    void onWordNagFixToggled(bool on);
    void onRestoreWordNag();

private:
    void buildUi();
    void centreColumn();
    void refreshSavePath();
    void refreshLogPath();
    void refreshThemeNote();
    void refreshWordNag();
    void syncThemeSegment();
    void syncWordSegment();

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

    // 打开 Word 文档时: the same three-segment control, persisted through
    // AppSettings::setWordOpenMode (0 每次询问 / 1 批注 / 2 用 Word 打开).
    QButtonGroup *m_wordGroup = nullptr;
    QPushButton  *m_wordSegments[3] = {};

    // The Word nag fix: the toggle persists AppSettings::wordNagFixEnabled and
    // the button restores according to WordConvert::nagRestoreMode() (exact
    // backup replay / Word defaults / disabled while nothing was modified).
    // m_wordNagNote is the row's state line, kept in sync by refreshWordNag().
    QAbstractButton *m_wordNagFix     = nullptr;
    QPushButton     *m_restoreWordNag = nullptr;
    QLabel          *m_wordNagNote    = nullptr;

    // --- 更新 ---------------------------------------------------------------
    UpdateChecker::Client *m_updateClient = nullptr;
    QProgressBar *m_updateProgress = nullptr;
    QLabel       *m_updateVersion  = nullptr;   // 当前版本 / 线上最新
    QLabel       *m_updateNote     = nullptr;   // state line
    QPlainTextEdit *m_updateNotes  = nullptr;   // release notes (changelog)
    QFrame         *m_updateCard   = nullptr;   // brought into view on demand
    QPushButton  *m_updateGh       = nullptr;
    QPushButton  *m_updateMirror   = nullptr;
    QPushButton  *m_updateSite     = nullptr;
    QPushButton  *m_updatePortableGh     = nullptr;
    QPushButton  *m_updatePortableMirror = nullptr;

    // Last successful check per channel (0 = GitHub, 1 = fallback server): only
    // the fields the panel needs, so no request is repeated.
    QString m_updateSetupUrl[2];
    QString m_updateSetupSha[2];
    QString m_updatePortableUrl[2];
    QString m_updatePageUrl[2];
    bool    m_updateChecked[2] = { false, false };
    bool    m_updateChecking = false;
    bool    m_updateCheckForInstall = false;
    int     m_updateCheckChannel = 0;
    int     m_updateQueuedChannel = -1;
    bool    m_updateDownloading = false;
    bool    m_updateHelperFailed = false;
    QString m_updateOnline[2];      // version each channel last announced

private slots:
    void onUpdateChecked(const UpdateChecker::UpdateInfo &info);
    void onUpdateFailed(const QString &reason);
    void onUpdateProgress(qint64 received, qint64 total);
    void onUpdateStage(const QString &text);
    void onUpdateSetupReady(const QString &path);
    void onUpdateSetupUnverified(const QString &pageUrl);

private:
    void beginUpdateInstall(int channel);
    void startUpdateCheck(int channel, bool forInstall);
    void startUpdateDownload(int channel);
    void finishUpdateBusy();
    void setUpdateState(const QString &text);
    void setUpdateNotes(const QString &notes);
    void refreshUpdateVersionLine();
    void openPortablePage(int channel);

    int           m_maxColumn = 0;
};
