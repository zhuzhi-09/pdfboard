#include "AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

#include <shlobj.h>

namespace {

// Current user's auto-start key and the value this application owns there.
const QString kRunKey = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kRunValue = QStringLiteral("PDFBoard");

// Where the "default save folder" preference lives.
const QString kAppKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard");
const QString kSavePathValue = QStringLiteral("DefaultSavePath");
const QString kDebugLogValue = QStringLiteral("DebugLog");
const QString kThemeModeValue = QStringLiteral("ThemeMode");

// The executable path exactly as Windows wants it in the Run key: native
// separators, wrapped in quotes so a path with spaces keeps working.
QString quotedExePath()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return exe.isEmpty() ? QString() : QStringLiteral("\"%1\"").arg(exe);
}

}   // namespace

bool AppSettings::isAutoStartEnabled()
{
    const QString want = quotedExePath();
    if (want.isEmpty())
        return false;

    QSettings run(kRunKey, QSettings::NativeFormat);
    return run.value(kRunValue).toString() == want;
}

bool AppSettings::setAutoStart(bool on, QString *errorOut)
{
    QSettings run(kRunKey, QSettings::NativeFormat);
    if (on) {
        const QString want = quotedExePath();
        if (want.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("无法获取程序路径");
            return false;
        }
        run.setValue(kRunValue, want);
    } else {
        run.remove(kRunValue);
    }
    run.sync();

    if (run.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（错误 %1）").arg(int(run.status()));
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}

bool AppSettings::registerPdfHandler(QString *errorOut)
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    if (exe.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("无法获取程序路径");
        return false;
    }

    const QString progId    = QStringLiteral("PDFBoard.Pdf");
    const QString dpzProgId = QStringLiteral("PDFBoard.Dpz");
    const QString exeQuoted = QStringLiteral("\"%1\"").arg(exe);

    QSettings cls(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
                  QSettings::NativeFormat);
    cls.setValue(progId + QStringLiteral("/."), QStringLiteral("PDF 文档"));
    cls.setValue(progId + QStringLiteral("/DefaultIcon/."), exeQuoted + QStringLiteral(",0"));
    cls.setValue(progId + QStringLiteral("/shell/open/command/."),
                 exeQuoted + QStringLiteral(" \"%1\""));
    cls.setValue(QStringLiteral(".pdf/OpenWithProgids/") + progId, QString());

    // The .dpz annotation bundle is ours alone: register it the same way so a
    // double-clicked bundle opens here too (same command line parsing).
    cls.setValue(dpzProgId + QStringLiteral("/."), QStringLiteral("PDF 批注包"));
    cls.setValue(dpzProgId + QStringLiteral("/DefaultIcon/."), exeQuoted + QStringLiteral(",0"));
    cls.setValue(dpzProgId + QStringLiteral("/shell/open/command/."),
                 exeQuoted + QStringLiteral(" \"%1\""));
    cls.setValue(QStringLiteral(".dpz/OpenWithProgids/") + dpzProgId, QString());
    cls.sync();

    QSettings caps(QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard\\Capabilities"),
                   QSettings::NativeFormat);
    caps.setValue(QStringLiteral("ApplicationName"), QStringLiteral("大屏 PDF 批注"));
    caps.setValue(QStringLiteral("ApplicationDescription"),
                  QStringLiteral("大屏 PDF 查看与批注工具"));
    caps.setValue(QStringLiteral("FileAssociations/.pdf"), progId);
    caps.setValue(QStringLiteral("FileAssociations/.dpz"), dpzProgId);
    caps.sync();

    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\RegisteredApplications"),
                  QSettings::NativeFormat);
    reg.setValue(QStringLiteral("PDFBoard"), QStringLiteral("Software\\PDFBoard\\Capabilities"));
    reg.sync();

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (errorOut)
        errorOut->clear();
    return true;
}

bool AppSettings::debugLogEnabled()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    return s.value(kDebugLogValue, false).toBool();
}

bool AppSettings::setDebugLogEnabled(bool on, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    s.setValue(kDebugLogValue, on);
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（调试日志设置）");
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}

int AppSettings::themeMode()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    const int mode = s.value(kThemeModeValue, 0).toInt();
    return (mode >= 0 && mode <= 2) ? mode : 0;
}

bool AppSettings::setThemeMode(int mode, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    s.setValue(kThemeModeValue, qBound(0, mode, 2));
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（外观设置）");
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}

QString AppSettings::defaultSavePath()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    const QString dir = s.value(kSavePathValue).toString().trimmed();
    if (dir.isEmpty())
        return {};
    return QDir::toNativeSeparators(dir);
}

bool AppSettings::setDefaultSavePath(const QString &dir, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    const QString trimmed = dir.trimmed();
    if (trimmed.isEmpty())
        s.remove(kSavePathValue);              // empty = follow the source file
    else
        s.setValue(kSavePathValue, QDir::toNativeSeparators(trimmed));
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（默认保存路径）");
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}
