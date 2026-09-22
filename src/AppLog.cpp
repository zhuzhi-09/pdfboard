#include "AppLog.h"
#include "AppSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QtGlobal>

namespace {

QFile   g_file;
QString g_path;
bool    g_enabled = false;
QMutex  g_mutex;
QtMessageHandler g_previous = nullptr;

// Set once somebody works the switch on the settings page. PDFBOARD_LOG is a
// startup default for machines we cannot click through - it must never make the
// switch dead, so an explicit click outranks it from that moment on.
bool g_envSuppressed = false;

// Rotate rather than grow forever: one previous run is kept as "<file>.1".
constexpr qint64 kMaxBytes = 2 * 1024 * 1024;

// Path asked for by the PDFBOARD_LOG environment variable (empty when unset).
QString envPath()
{
    const QByteArray env = qgetenv("PDFBOARD_LOG");
    return env.isEmpty() ? QString() : QString::fromLocal8Bit(env);
}

QString resolvePath()
{
    // The environment override wins, so a supporter can ask for a log at a
    // known place on a machine whose settings we cannot click through.
    const QString env = envPath();
    if (!env.isEmpty())
        return env;

    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("logs/pdfboard.log"));
}

void openFileLocked()
{
    const QString path = resolvePath();

    // Also handles a path that changed while running (the environment override
    // being set or cleared), so the resolved path always wins.
    if (g_file.isOpen()) {
        if (g_path == path)
            return;
        g_file.close();
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    if (QFileInfo(path).size() > kMaxBytes) {
        const QString previous = path + QStringLiteral(".1");
        QFile::remove(previous);
        QFile::rename(path, previous);
    }

    g_file.setFileName(path);
    if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        g_enabled = false;          // logging must never take the app down
        return;
    }
    g_path = path;
}

void appendLine(const char *level, const QString &category, const QString &message)
{
    if (!g_enabled)
        return;

    QMutexLocker lock(&g_mutex);
    openFileLocked();
    if (!g_file.isOpen())
        return;

    const QString line =
        QStringLiteral("%1 [%2] %3: %4\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                 QString::fromLatin1(level), category, message);
    g_file.write(line.toUtf8());
    g_file.flush();
}

// Qt's own messages (warnings, criticals, fatal) are valuable when a bug is
// reported, so they land in the same file and still reach the old handler.
void messageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    const char *level = "info";
    switch (type) {
    case QtDebugMsg:    level = "debug"; break;
    case QtInfoMsg:     level = "info";  break;
    case QtWarningMsg:  level = "warn";  break;
    case QtCriticalMsg: level = "error"; break;
    case QtFatalMsg:    level = "fatal"; break;
    }
    const QString category = ctx.category ? QString::fromLatin1(ctx.category)
                                          : QStringLiteral("qt");
    appendLine(level, category, msg);

    if (g_previous)
        g_previous(type, ctx, msg);
}

void installHandlerOnce()
{
    if (!g_previous)
        g_previous = qInstallMessageHandler(messageHandler);
}

}   // namespace

void AppLog::applySettings()
{
    // ON when the user asked for it, or when PDFBOARD_LOG asked for it and no
    // click has overridden that yet.
    const bool envForces = !envPath().isEmpty() && !g_envSuppressed;
    if (!AppSettings::debugLogEnabled() && !envForces) {
        QMutexLocker lock(&g_mutex);
        g_enabled = false;
        if (g_file.isOpen())
            g_file.close();      // "off" must not keep the file locked
        return;
    }

    g_enabled = true;
    {
        QMutexLocker lock(&g_mutex);
        openFileLocked();
    }
    installHandlerOnce();
}

bool AppLog::setEnabled(bool on, QString *errorOut)
{
    if (!AppSettings::setDebugLogEnabled(on, errorOut))
        return false;

    // A click here outranks PDFBOARD_LOG for the rest of the session. Without
    // this the switch snapped straight back on and could never be turned off.
    g_envSuppressed = !on;
    applySettings();
    if (on)
        write(QStringLiteral("log"),
              QStringLiteral("诊断日志已开启（%1）").arg(g_path));
    return true;
}

bool AppLog::isEnabled()
{
    return g_enabled;
}

QString AppLog::logFilePath()
{
    return g_enabled ? g_path : QString();
}

QString AppLog::logDirectory()
{
    return QFileInfo(resolvePath()).absolutePath();
}

QString AppLog::envOverridePath()
{
    return envPath();
}

void AppLog::write(const QString &category, const QString &message)
{
    appendLine("info", category, message);
}
