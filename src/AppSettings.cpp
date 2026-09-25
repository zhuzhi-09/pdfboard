#include "AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
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
const QString kWordOpenModeValue = QStringLiteral("WordOpenMode");
const QString kWordNagFixValue = QStringLiteral("WordNagFix");
const QString kPalmEraserValue = QStringLiteral("PalmEraser");
const QString kRecentFilesValue = QStringLiteral("RecentFiles");

// 最近项目 keeps at most this many paths; the oldest entry is dropped first.
constexpr int kRecentFilesMax = 8;

// The executable path exactly as Windows wants it in the Run key: native
// separators, wrapped in quotes so a path with spaces keeps working.
QString quotedExePath()
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return exe.isEmpty() ? QString() : QStringLiteral("\"%1\"").arg(exe);
}

// True when `path` lives under the system temp directory - which includes
// `<temp>/pdfboard`, where the working copies are kept. Both sides are made
// absolute and separator-normalised; Windows paths compare case-insensitively.
bool isUnderTempDir(const QString &path)
{
    const QString dir =
        QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
    QString temp =
        QDir::fromNativeSeparators(QFileInfo(QDir::tempPath()).absoluteFilePath());
    while (temp.endsWith(QLatin1Char('/')))
        temp.chop(1);
    return dir.compare(temp, Qt::CaseInsensitive) == 0
           || dir.startsWith(temp + QLatin1Char('/'), Qt::CaseInsensitive);
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
    const QString wordProgId = QStringLiteral("PDFBoard.Word");
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
    cls.setValue(dpzProgId + QStringLiteral("/."), QStringLiteral("落墨批注包"));
    cls.setValue(dpzProgId + QStringLiteral("/DefaultIcon/."), exeQuoted + QStringLiteral(",0"));
    cls.setValue(dpzProgId + QStringLiteral("/shell/open/command/."),
                 exeQuoted + QStringLiteral(" \"%1\""));
    cls.setValue(QStringLiteral(".dpz/OpenWithProgids/") + dpzProgId, QString());

    // Word documents are registered as an OpenWith CANDIDATE only: the ProgID
    // lands in OpenWithProgids, which merely offers this app in the "打开方式"
    // list. The user's Word default (the UserChoice hash) is never written, so
    // double-clicking a .docx still starts Word, exactly as before.
    cls.setValue(wordProgId + QStringLiteral("/."), QStringLiteral("Word 文档（批注）"));
    cls.setValue(wordProgId + QStringLiteral("/DefaultIcon/."), exeQuoted + QStringLiteral(",0"));
    cls.setValue(wordProgId + QStringLiteral("/shell/open/command/."),
                 exeQuoted + QStringLiteral(" \"%1\""));
    cls.setValue(QStringLiteral(".docx/OpenWithProgids/") + wordProgId, QString());
    cls.setValue(QStringLiteral(".doc/OpenWithProgids/") + wordProgId, QString());
    cls.sync();

    QSettings caps(QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard\\Capabilities"),
                   QSettings::NativeFormat);
    caps.setValue(QStringLiteral("ApplicationName"), QStringLiteral("落墨·大屏批注"));
    caps.setValue(QStringLiteral("ApplicationDescription"),
                  QStringLiteral("大屏 PDF 查看与批注工具"));
    caps.setValue(QStringLiteral("FileAssociations/.pdf"), progId);
    caps.setValue(QStringLiteral("FileAssociations/.dpz"), dpzProgId);
    caps.setValue(QStringLiteral("FileAssociations/.docx"), wordProgId);
    caps.setValue(QStringLiteral("FileAssociations/.doc"), wordProgId);
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

QStringList AppSettings::recentFiles()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    const QStringList stored = s.value(kRecentFilesValue).toStringList();

    // Read-side hygiene: trim, drop empties and fold duplicates so a manually
    // edited registry value can never show the same document twice.
    QStringList cleaned;
    cleaned.reserve(stored.size());
    for (const QString &path : stored) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed, Qt::CaseInsensitive))
            cleaned.append(trimmed);
    }
    return cleaned;
}

// Shared write path: an empty list removes the value instead of storing an
// empty one, matching how setDefaultSavePath treats "no preference".
static bool writeRecentFiles(const QStringList &list, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    if (list.isEmpty())
        s.remove(kRecentFilesValue);
    else
        s.setValue(kRecentFilesValue, list);
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（最近项目）");
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}

bool AppSettings::addRecentFile(const QString &path, QString *errorOut)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        if (errorOut)
            errorOut->clear();
        return true;                     // nothing to remember: not an error
    }

    // 最近项目 must only ever hold a file the user chose and can reopen. The
    // temp directory is where 保存 keeps its working copies; recording one
    // would list a hash name that the user never picked, so the path is
    // refused outright and the stored list stays untouched.
    if (isUnderTempDir(trimmed)) {
        if (errorOut)
            *errorOut = QStringLiteral("临时文件不会加入最近项目");
        return false;
    }

    QStringList list = recentFiles();
    for (qsizetype i = list.size() - 1; i >= 0; --i) {
        if (list.at(i).compare(trimmed, Qt::CaseInsensitive) == 0)
            list.removeAt(i);            // move to front, keep one entry only
    }
    list.prepend(trimmed);
    while (list.size() > kRecentFilesMax)
        list.removeLast();
    return writeRecentFiles(list, errorOut);
}

bool AppSettings::removeRecentFile(const QString &path, QString *errorOut)
{
    const QString trimmed = path.trimmed();
    QStringList list = recentFiles();
    for (qsizetype i = list.size() - 1; i >= 0; --i) {
        if (list.at(i).compare(trimmed, Qt::CaseInsensitive) == 0)
            list.removeAt(i);
    }
    return writeRecentFiles(list, errorOut);
}

bool AppSettings::clearRecentFiles(QString *errorOut)
{
    return writeRecentFiles(QStringList(), errorOut);
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

qint64 AppSettings::lastUpdateCheckMs()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    return s.value(QStringLiteral("LastUpdateCheckMs"), qint64(0)).toLongLong();
}

bool AppSettings::setLastUpdateCheckMs(qint64 ms, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    s.setValue(QStringLiteral("LastUpdateCheckMs"), ms);
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（更新时间戳）");
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}

int AppSettings::wordOpenMode()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    const int mode = s.value(kWordOpenModeValue, 0).toInt();
    return (mode >= 0 && mode <= 2) ? mode : 0;
}

bool AppSettings::setWordOpenMode(int mode, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    s.setValue(kWordOpenModeValue, qBound(0, mode, 2));
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（Word 文档打开方式）");
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}

bool AppSettings::wordNagFixEnabled()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    return s.value(kWordNagFixValue, true).toBool();
}

bool AppSettings::setWordNagFixEnabled(bool on, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    s.setValue(kWordNagFixValue, on);
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（Word 提醒设置）");
        return false;
    }
    if (errorOut)
        errorOut->clear();
    return true;
}

bool AppSettings::palmEraserEnabled()
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    // 默认关：这是实验性功能，而且多指触摸会先被 Windows 自己的手势识别接走，
    // 我们的分类器收不到完整的多指帧，所以对多数面板不可用。用户显式打开过就保留。
    return s.value(kPalmEraserValue, false).toBool();
}

bool AppSettings::setPalmEraserEnabled(bool on, QString *errorOut)
{
    QSettings s(kAppKey, QSettings::NativeFormat);
    s.setValue(kPalmEraserValue, on);
    s.sync();

    if (s.status() != QSettings::NoError) {
        if (errorOut)
            *errorOut = QStringLiteral("无法写入注册表（手掌擦除设置）");
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
