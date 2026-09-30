#include "UpdateChecker.h"

#include "AppLog.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#include <windows.h>

namespace {

// One authoritative source: GitHub. Both the release JSON and the package come
// from github.com / api.github.com - no third-party accelerator is involved.
const char *kGitHubApi =
    "https://api.github.com/repos/zhuzhi-09/pdfboard/releases/latest";

const char *kUserAgent = "PDFBoard-Updater/1.0";

// The Inno Setup AppId, used to tell an installed copy from a portable one.
const char *kUninstallKey =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall"
    "\\{9C1F4E2A-7B3D-4E58-9A21-6D5C0B8F3A71}_is1";

QString tempDir()
{
    const QString dir = QDir(QDir::tempPath()).filePath(QStringLiteral("pdfboard"));
    QDir().mkpath(dir);
    return dir;
}

QString stripShaPrefix(QString digest)
{
    digest = digest.trimmed().toLower();
    if (digest.startsWith(QStringLiteral("sha256:")))
        digest = digest.mid(7);
    return digest;
}

}   // namespace

int UpdateChecker::compareVersion(const QString &a, const QString &b)
{
    const auto parts = [](const QString &text) {
        QList<int> numbers;
        const QStringList fields = text.split(QLatin1Char('.'));
        for (const QString &field : fields) {
            // "4-nightly" style suffixes still compare by their leading number.
            int value = 0;
            for (const QChar &c : field) {
                if (!c.isDigit())
                    break;
                value = value * 10 + c.digitValue();
            }
            numbers.append(value);
        }
        return numbers;
    };

    QList<int> left = parts(a);
    QList<int> right = parts(b);
    while (left.size() < right.size())
        left.append(0);
    while (right.size() < left.size())
        right.append(0);

    for (int i = 0; i < left.size(); ++i) {
        if (left.at(i) != right.at(i))
            return left.at(i) < right.at(i) ? -1 : 1;
    }
    return 0;
}

bool UpdateChecker::shouldPrompt(const UpdateInfo &info, const QString &announcedVersion,
                                 const QString &runningVersion)
{
    if (!info.valid || info.version.isEmpty())
        return false;
    if (!announcedVersion.isEmpty() && info.version == announcedVersion)
        return false;                  // already announced in this session
    return compareVersion(info.version, runningVersion) > 0;
}

UpdateChecker::UpdateInfo UpdateChecker::parseGitHubJson(const QByteArray &json,
                                                        const QString &currentVersion)
{
    UpdateInfo info;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return info;

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("tag_name")).toString().isEmpty())
        return info;                            // not a release payload

    info.valid = true;
    info.tag = root.value(QStringLiteral("tag_name")).toString();
    info.version = info.tag;
    if (info.version.startsWith(QLatin1Char('v')))
        info.version = info.version.mid(1);
    info.notes = root.value(QStringLiteral("body")).toString();
    info.pageUrl = root.value(QStringLiteral("html_url")).toString();

    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (name.contains(QStringLiteral("portable"), Qt::CaseInsensitive)) {
            if (info.portableUrl.isEmpty())
                info.portableUrl = asset.value(QStringLiteral("browser_download_url")).toString();
            continue;
        }
        if (!name.contains(QStringLiteral("setup"), Qt::CaseInsensitive))
            continue;
        // No `break` here: the portable asset may sit after the setup one, and both
        // are wanted from the same payload.
        if (info.setupUrl.isEmpty()) {
            info.setupUrl = asset.value(QStringLiteral("browser_download_url")).toString();
            // GitHub exposes a per-asset digest of the form "sha256:<hex>"; older
            // payloads have none, and then we simply refuse to run the package.
            info.setupSha256 = stripShaPrefix(asset.value(QStringLiteral("digest")).toString());
            info.setupSize = qint64(asset.value(QStringLiteral("size")).toDouble());
        }
    }
    info.available = !currentVersion.isEmpty()
                     && compareVersion(info.version, currentVersion) > 0;
    return info;
}

QString UpdateChecker::currentVersion()
{
    const QString path = QCoreApplication::applicationFilePath();
    const auto *wide = reinterpret_cast<const wchar_t *>(path.utf16());

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(wide, &ignored);
    if (size == 0)
        return QString();

    QByteArray buffer(int(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(wide, 0, size, buffer.data()))
        return QString();

    VS_FIXEDFILEINFO *fixed = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.constData(), L"\\", reinterpret_cast<LPVOID *>(&fixed),
                        &length)
        || !fixed) {
        return QString();
    }
    return QStringLiteral("%1.%2.%3")
        .arg(HIWORD(fixed->dwFileVersionMS))
        .arg(LOWORD(fixed->dwFileVersionMS))
        .arg(HIWORD(fixed->dwFileVersionLS));
}

// True when this copy came from the installer, which is what decides whether the
// in-app buttons may run a setup: a portable copy is updated by replacing files,
// not by installing.
bool UpdateChecker::isInstalledCopy()
{
    QSettings installed(QString::fromLatin1(kUninstallKey), QSettings::NativeFormat);
    return !installed.value(QStringLiteral("DisplayName")).toString().isEmpty();
}

UpdateChecker::Client::Client(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
{
}

void UpdateChecker::Client::openInBrowser(const QString &url)
{
    if (!url.isEmpty())
        QDesktopServices::openUrl(QUrl(url));
}

void UpdateChecker::Client::check()
{
    QNetworkRequest request{QUrl(QString::fromLatin1(kGitHubApi))};
    request.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    request.setTransferTimeout(8000);

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // Offline, blocked, or GitHub simply not reachable from a classroom
            // network without a system proxy: the caller treats this as "no update"
            // and stays quiet (see the known cost in UpdateChecker.h).
            emit failed(reply->errorString());
            return;
        }
        const UpdateInfo info = parseGitHubJson(reply->readAll(), currentVersion());
        if (!info.valid) {
            emit failed(QStringLiteral("响应无法解析"));
            return;
        }
        emit checked(info);
    });
}

void UpdateChecker::Client::downloadSetup(const UpdateInfo &info)
{
    if (!info.setupUrl.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        emit failed(QStringLiteral("下载地址不是 HTTPS"));
        return;
    }
    if (info.setupSha256.isEmpty()) {
        // No hash means we cannot prove what we downloaded is what GitHub
        // published: hand the link to the browser instead of running anything.
        emit stage(QStringLiteral("该来源未提供校验值，已改为打开下载链接"));
        emit setupUnverified(info.pageUrl);
        return;
    }

    const QString path = QDir(tempDir()).filePath(
        QStringLiteral("PDFBoard-update-%1-setup.exe").arg(info.version));

    auto *file = new QFile(path, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file->deleteLater();
        emit failed(QStringLiteral("无法写入临时文件"));
        return;
    }

    emit stage(QStringLiteral("正在下载 %1 …").arg(info.version));
    QNetworkRequest request{QUrl(info.setupUrl)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kUserAgent));
    request.setTransferTimeout(120000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) { emit progress(received, total); });
    connect(reply, &QNetworkReply::readyRead, this,
            [reply, file] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, file, path, info] {
                reply->deleteLater();
                file->close();
                if (reply->error() != QNetworkReply::NoError) {
                    file->remove();
                    file->deleteLater();
                    emit failed(reply->errorString());
                    return;
                }

                // Streaming hash: the package is verified before anything runs.
                QCryptographicHash hash(QCryptographicHash::Sha256);
                bool hashed = false;
                {
                    QFile reader(path);
                    if (reader.open(QIODevice::ReadOnly))
                        hashed = hash.addData(&reader);
                }
                const QString actual =
                    QString::fromLatin1(hash.result().toHex()).toLower();
                file->deleteLater();

                if (!hashed || actual != info.setupSha256) {
                    QFile::remove(path);
                    AppLog::write(QStringLiteral("update"),
                                  QStringLiteral("更新包校验失败：期望 %1，实际 %2")
                                      .arg(info.setupSha256, actual));
                    emit failed(QStringLiteral("下载文件校验失败，已丢弃"));
                    return;
                }

                AppLog::write(QStringLiteral("update"),
                              QStringLiteral("更新包校验通过：%1").arg(QFileInfo(path).fileName()));
                emit progress(1, 1);
                emit setupReady(path);
            });
}

// Writes the helper that runs the installer and brings the app back, so the
// teacher only clicks once. The installer is silent (see installer/).
void UpdateChecker::Client::runInstallerAndRestart(const QString &installerPath)
{
    const QString helper =
        QDir(tempDir()).filePath(QStringLiteral("apply-update.cmd"));
    const QString app = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString setup = QDir::toNativeSeparators(installerPath);

    const QString script =
        QStringLiteral("@echo off\r\n"
                       "chcp 65001 >nul\r\n"
                       "\"%1\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART\r\n"
                       "start \"\" \"%2\"\r\n"
                       "del \"%%~f0\"\r\n")
            .arg(setup, app);

    QFile file(helper);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(QStringLiteral("无法准备安装脚本"));
        return;
    }
    file.write(script.toUtf8());
    file.close();

    QProcess::startDetached(QStringLiteral("cmd.exe"),
                            { QStringLiteral("/c"), QDir::toNativeSeparators(helper) });
}
