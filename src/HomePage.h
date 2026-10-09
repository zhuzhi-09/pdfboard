#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QResizeEvent;
class QVBoxLayout;

// Start page, shown as a page of MainWindow's stack exactly like SettingsPage.
//
// Top to bottom: a time based greeting with the Windows user name, one quote
// (a random line from a curated built-in list - the app never goes online), a
// large 打开 button and the 最近项目 list. Only PATHS are remembered - the files
// themselves are never copied.
//
// The page owns no file dialog and no canvas, so there is no floating toolbar
// island on it; opening a document is a request to the host instead
// (openRequested / openPathRequested).
class HomePage : public QWidget
{
    Q_OBJECT
public:
    explicit HomePage(QWidget *parent = nullptr);

    // Re-applies every palette-derived piece after the application theme
    // changed (the host calls this from MainWindow::applyTheme).
    void applyTheme();

    // Re-reads the greeting and the recent list and picks a new quote. Called
    // on every home-page visit.
    void refresh();

    // --- Pure helpers (also covered by --selftest-home) ---------------------
    // 夜深了 / 早上好 / 中午好 / 下午好 / 晚上好 for a 24h hour, plus the
    // user name.
    static QString greetingForHour(int hour, const QString &userName);
    // %USERNAME%, falling back to 老师 when the variable is empty.
    static QString userName();
    // One random line (with its source) from the curated built-in list.
    static QString randomQuote();

signals:
    void openRequested();                          // the large 打开 button
    void openPathRequested(const QString &path);   // a 最近项目 row was activated

protected:
    // Keeps the centred content column a stable width on every resize.
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi();
    void centreColumn();
    void refreshGreeting();
    void rebuildRecentList();
    void activateRecent(const QString &path);
    void removeRecent(const QString &path);
    void setQuote(const QString &line);

    QWidget        *m_body     = nullptr;
    QWidget        *m_column   = nullptr;
    QLabel         *m_greeting = nullptr;
    QLabel         *m_quote    = nullptr;
    QPushButton    *m_openButton  = nullptr;
    QPushButton    *m_clearButton = nullptr;
    QVBoxLayout    *m_recentCol   = nullptr;

    int m_maxColumn = 0;
};
