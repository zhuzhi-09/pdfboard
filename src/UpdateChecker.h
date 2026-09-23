#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

// In-app update, two channels.
//
// GitHub is the authoritative source: its release JSON carries the version and -
// crucially - the sha256 of every asset. In a Chinese classroom github.com is
// usually unreachable (the API host is more often reachable than the download
// host), so the second channel fetches the SAME GitHub JSON and the SAME asset
// through a public gh-proxy: it is a transport accelerator, not a second source of
// truth. The gate is unchanged - a package is only ever started after its sha256
// matched, and without a hash we hand the download link to the browser instead of
// running anything.
namespace UpdateChecker {

enum class Channel {
    GitHub,       // api.github.com + github.com, directly
    Accelerated,  // the same absolute URLs through a public gh-proxy (see kAccelBases)
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

// Pure parser, so both channels are testable without a network. (Both channels
// receive GitHub's payload - the accelerated one just receives it via a proxy.)
UpdateInfo parseGitHubJson(const QByteArray &json, const QString &currentVersion);

// The URL form every public gh-proxy uses: "<base>/<absolute github url>", e.g.
// acceleratedUrl("https://gh-proxy.com", "https://github.com/x/y.zip")
//   -> "https://gh-proxy.com/https://github.com/x/y.zip"
QString acceleratedUrl(const QString &base, const QString &absoluteUrl);

// Do two digests name the same hash? Case and a leading "sha256:" are ignored; an
// empty digest never agrees with anything - that is what keeps the gate honest.
bool digestsAgree(const QString &a, const QString &b);

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

    // Quiet version check. The Accelerated channel walks the proxy list, remembers
    // the first one that answers and rewrites every URL in the result to go through
    // it, so the rest of the app needs no proxy knowledge at all.
    void check(Channel channel);
    // Download the setup for `info` and, once the hash matches, start it and
    // ask the app to quit (the waiting helper relaunches it afterwards). On the
    // accelerated channel a failed or mis-hashed download moves on to the next
    // proxy instead of giving up.
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

    // Accelerated-channel probing: which proxy is being tried, which proxy answered,
    // and which second proxy is asked to confirm the digest it reported.
    int        m_accelProbe = 0;
    int        m_accelCrossCheck = 0;
    QString    m_accelBase;         // the proxy that answered - reused for assets
    QString    m_accelDigest;       // what that proxy said the setup hash is
    UpdateInfo m_accelInfo;         // the manifest waiting for its cross-check

    // Setup download retries: an attempt counter plus the version it belongs to,
    // so a new version starts a fresh walk of the proxy list.
    int     m_setupAttempt = 0;
    QString m_setupVersion;
};

}   // namespace UpdateChecker
