#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// 「检查更新」: asks GitHub which release is the latest, compares it with the
// running version and reports the result. It goes online ONLY when the user
// presses the button - never on its own - and only talks to github.com.
//
// It deliberately does not use api.github.com: the REST API allows 60
// unauthenticated requests per hour per IP, and a school shares one public IP
// between hundreds of machines. The web endpoint /releases/latest answers with
// a plain 302 to /releases/tag/<tag>, which carries the same information and is
// not rate limited. The redirect is read, never followed, so no page is
// downloaded.
//
// Downloading stays with the browser: a newer release is opened as its tag page.
namespace UpdateCheck {

// The repository the app is published from.
QUrl repositoryUrl();
QUrl releasesUrl();       // all releases
QUrl latestUrl();         // /releases/latest (the redirecting endpoint)

// "https://github.com/o/r/releases/tag/v1.14.1" -> "v1.14.1"; empty when the
// location is not a release tag (e.g. the repository has no release yet and
// GitHub redirects to /releases instead).
QString tagFromLocation(const QString &location);

// Compares "v1.14.1" / "1.14.2" style versions numerically (a leading "v" and a
// trailing "-exp" style suffix are ignored): <0 if a < b, 0 if equal, >0 if a > b.
int compareVersions(const QString &a, const QString &b);

struct Result {
    enum class Status {
        UpToDate,     // the running build is the latest release (or newer)
        NewerFound,   // GitHub has a newer release: latestTag / pageUrl
        Failed,       // network error, timeout or an unexpected answer: error
    };
    Status  status = Status::Failed;
    QString latestTag;   // e.g. "v1.14.1"
    QUrl    pageUrl;     // the tag page to open in the browser
    QString error;       // human readable, Chinese
};

class Checker : public QObject
{
    Q_OBJECT
public:
    explicit Checker(QObject *parent = nullptr);

    // Starts one check against `currentVersion` ("1.14.2"). Ignored while a check
    // is already running. finished() is always emitted exactly once per start.
    void start(const QString &currentVersion);
    bool isRunning() const { return m_reply != nullptr; }

    // Pure decision step, exposed for the self test: what a redirect answer
    // (HTTP status + Location header) means for the running version.
    static Result interpret(int httpStatus, const QString &location,
                            const QString &currentVersion);

signals:
    void finished(const UpdateCheck::Result &result);

private:
    void onFinished();

    QNetworkAccessManager *m_net = nullptr;
    QNetworkReply *m_reply = nullptr;
    QString m_current;
};

}   // namespace UpdateCheck
