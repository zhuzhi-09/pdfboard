#include "UpdateCheck.h"

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>

namespace UpdateCheck {

namespace {

constexpr int kTimeoutMs = 10000;

QVersionNumber parseVersion(const QString &text)
{
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        s.remove(0, 1);
    // fromString stops at the first non-numeric part, so "1.14.2-exp" -> 1.14.2.
    return QVersionNumber::fromString(s);
}

}   // namespace

QUrl repositoryUrl()
{
    return QUrl(QStringLiteral("https://github.com/zhuzhi-09/pdfboard"));
}

QUrl releasesUrl()
{
    return QUrl(QStringLiteral("https://github.com/zhuzhi-09/pdfboard/releases"));
}

QUrl latestUrl()
{
    return QUrl(QStringLiteral("https://github.com/zhuzhi-09/pdfboard/releases/latest"));
}

QString tagFromLocation(const QString &location)
{
    static const QRegularExpression re(QStringLiteral("/releases/tag/([^/?#]+)"));
    const QRegularExpressionMatch m = re.match(location);
    if (!m.hasMatch())
        return {};
    return QUrl::fromPercentEncoding(m.captured(1).toUtf8());
}

int compareVersions(const QString &a, const QString &b)
{
    return QVersionNumber::compare(parseVersion(a), parseVersion(b));
}

Checker::Checker(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
    // Always a direct connection to github.com, never through a proxy: school
    // networks flag proxy-like traffic and block the machine's IP for it.
    m_net->setProxy(QNetworkProxy::NoProxy);
}

void Checker::start(const QString &currentVersion)
{
    if (m_reply)
        return;
    m_current = currentVersion;

    QNetworkRequest req(latestUrl());
    // Read the redirect, do not follow it: the Location header is the answer.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(kTimeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("pdfboard/%1").arg(currentVersion));
    m_reply = m_net->head(req);
    connect(m_reply, &QNetworkReply::finished, this, &Checker::onFinished);
}

void Checker::onFinished()
{
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    if (!reply)
        return;
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        // No HTTP answer at all: offline, DNS, TLS, proxy or timeout.
        Result r;
        r.status = Result::Status::Failed;
        r.error = reply->error() == QNetworkReply::OperationCanceledError
                      || reply->error() == QNetworkReply::TimeoutError
                      ? QStringLiteral("连接 GitHub 超时")
                      : QStringLiteral("无法连接 GitHub（%1）").arg(reply->errorString());
        emit finished(r);
        return;
    }
    const QString location = QString::fromUtf8(reply->rawHeader("Location"));
    emit finished(interpret(status, location, m_current));
}

Result Checker::interpret(int httpStatus, const QString &location,
                          const QString &currentVersion)
{
    Result r;
    if (httpStatus < 300 || httpStatus >= 400) {
        r.status = Result::Status::Failed;
        r.error = QStringLiteral("GitHub 返回了意外的结果（HTTP %1）").arg(httpStatus);
        return r;
    }
    const QString tag = tagFromLocation(location);
    if (tag.isEmpty() || parseVersion(tag).isNull()) {
        r.status = Result::Status::Failed;
        r.error = QStringLiteral("没有找到已发布的版本");
        return r;
    }
    r.latestTag = tag;
    r.pageUrl = QUrl(location).isRelative() ? latestUrl().resolved(QUrl(location))
                                            : QUrl(location);
    r.status = compareVersions(tag, currentVersion) > 0 ? Result::Status::NewerFound
                                                        : Result::Status::UpToDate;
    return r;
}

}   // namespace UpdateCheck
