#pragma once

#include <QDateTime>
#include <QWidget>

class QLabel;
class QNetworkAccessManager;
class QPushButton;
class QResizeEvent;
class QVBoxLayout;

// Start page, shown as a page of MainWindow's stack exactly like SettingsPage.
//
// Top to bottom: a time based greeting with the Windows user name, one 一言
// quote (fetched from hitokoto, filtered locally, with a curated offline
// fallback), a large 打开 button and the 最近项目 list. Only PATHS are
// remembered - the files themselves are never copied.
//
// The page owns no file dialog and no canvas, so there is no floating toolbar
// island on it; opening a document is a request to the host instead
// (openRequested / openPathRequested). A failed quote fetch degrades silently
// to the built-in list: no dialog, no console output.
class HomePage : public QWidget
{
    Q_OBJECT
public:
    explicit HomePage(QWidget *parent = nullptr);

    // Re-applies every palette-derived piece after the application theme
    // changed (the host calls this from MainWindow::applyTheme).
    void applyTheme();

    // Re-reads the greeting and the recent list and, at most once per 10
    // minutes, starts a new quote request. Called on every home-page visit.
    void refresh();

    // --- Pure helpers (also covered by --selftest-home) ---------------------
    // 夜深了 / 早上好 / 中午好 / 下午好 / 晚上好 for a 24h hour, plus the
    // user name.
    static QString greetingForHour(int hour, const QString &userName);
    // %USERNAME%, falling back to 老师 when the variable is empty.
    static QString userName();
    // True when a quote is suitable for a classroom: at most 60 characters and
    // free of URLs and a short literal blocklist.
    static bool isQuoteAcceptable(const QString &text);
    // One random line (with its source) from the curated fallback list.
    static QString fallbackQuote();

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
    void fetchQuoteIfDue();                        // throttled network request

    QWidget        *m_body     = nullptr;
    QWidget        *m_column   = nullptr;
    QLabel         *m_greeting = nullptr;
    QLabel         *m_quote    = nullptr;
    QPushButton    *m_openButton  = nullptr;
    QPushButton    *m_clearButton = nullptr;
    QVBoxLayout    *m_recentCol   = nullptr;

    QNetworkAccessManager *m_net = nullptr;
    QDateTime m_lastQuoteAttempt;      // 10 minute throttle between requests
    int m_maxColumn = 0;
};
