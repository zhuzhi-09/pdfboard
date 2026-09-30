#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// In-app update, one authoritative source only: GitHub.
//
// The release JSON from api.github.com carries the version and - crucially - the
// sha256 of every asset, and the package is fetched from github.com itself. No
// third-party accelerator is involved any more (compliance): update traffic is
// never handed to an unknown domain.
//
// KNOWN COST of the single-source rule: on a classroom network that cannot reach
// GitHub directly AND has no system proxy configured, checking for updates fails
// silently - no dialog, exactly as if there were no update at all. That is the
// deliberate price of not routing updates through a third party;
// QNetworkProxyFactory::setUseSystemConfiguration(true) in main.cpp (see 2.75 in
// docs/03) is the only fallback left.
//
// The gate is unchanged: a package is only ever started after its sha256 matched,
// and without a digest we hand the download link to the browser instead of running
// anything.
namespace UpdateChecker {

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

// Pure parser, so it is testable without a network.
UpdateInfo parseGitHubJson(const QByteArray &json, const QString &currentVersion);

// Should a "new version" dialog be shown? Kept pure so the self test can lock the
// decision: only for a parsed release that is really newer than the running
// version AND has not been announced yet in this session. A teacher opens dozens
// of files in a lesson - being interrupted by the same dialog each time would be
// worse than not being told at all.
bool shouldPrompt(const UpdateInfo &info, const QString &announcedVersion,
                  const QString &runningVersion);

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

    // Quiet version check against api.github.com. A failure is reported through
    // failed() and the caller stays silent - an offline classroom must never see
    // a dialog about the network.
    void check();
    // Download the setup for `info` and, once the hash matches, start it and ask
    // the app to quit (the waiting helper relaunches it afterwards). Without a
    // digest nothing is downloaded or run: the download page goes to the browser.
    void downloadSetup(const UpdateInfo &info);
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
