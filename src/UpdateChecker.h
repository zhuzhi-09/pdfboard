#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// In-app update, two channels.
//
// GitHub is the primary source (releases/latest); the maintainer's own server is
// a fallback and is labelled as such in the UI, because it is a mirror that may
// lag. Both channels end in the same gate: a setup package is only ever started
// after its sha256 matched, and a package without a hash is never run
// automatically - we open the download page instead.
namespace UpdateChecker {

enum class Channel {
    GitHub,     // api.github.com/repos/zhuzhi-09/pdfboard/releases/latest
    Fallback,   // https://pdz-update-download-latest.zhuzhi.site/
};

struct UpdateInfo {
    bool    valid = false;      // the payload parsed
    bool    available = false;  // newer than the running version
    QString version;            // "1.4.4"
    QString tag;                // "v1.4.4"
    QString commit;
    QString notes;              // markdown from the release, shown as plain text
    QString setupUrl;           // VERSIONED asset url - the hash belongs to it
    QString setupSha256;        // empty when the source has no digest
    QString portableUrl;
    QString pageUrl;            // where a human should go when we refuse to run it
    qint64  setupSize = 0;
};

// Numeric, component-wise: "1.4.10" is NEWER than "1.4.2". Never compare as text.
// Returns <0, 0 or >0 like strcmp.
int compareVersion(const QString &a, const QString &b);

// Pure parsers so both channels are testable without a network.
UpdateInfo parseFallbackJson(const QByteArray &json, const QString &currentVersion);
UpdateInfo parseGitHubJson(const QByteArray &json, const QString &currentVersion);

// Version of the running executable, read from its own version resource.
QString currentVersion();

// True when this copy came from the installer. A portable copy is updated by
// replacing files, not by running a setup, so the UI uses this to decide.
bool isInstalledCopy();

// Everything below needs an event loop.
class Client : public QObject
{
    Q_OBJECT
public:
    explicit Client(QObject *parent = nullptr);

    // Quiet version check, used for the "online latest" line.
    void check(Channel channel);
    // Download the setup for `info` and, once the hash matches, start it and
    // ask the app to quit (the waiting helper relaunches it afterwards).
    void downloadSetup(Channel channel, const UpdateInfo &info);
    // Write the tiny waiting helper: run the installer silently, then bring the
    // app back, so the teacher clicks once.
    void runInstallerAndRestart(const QString &installerPath);
    // Open a page in the browser - used for the portable links and whenever we
    // refuse to run an unverified package.
    static void openInBrowser(const QString &url);

signals:
    void checked(const UpdateChecker::UpdateInfo &info);
    void failed(const QString &reason);
    void progress(qint64 received, qint64 total);
    void stage(const QString &text);            // short Chinese状态 for the UI
    void setupReady(const QString &path);       // verified: the app should quit
    void setupUnverified(const QString &pageUrl);

private:
    QNetworkAccessManager *m_net = nullptr;
};

}   // namespace UpdateChecker
