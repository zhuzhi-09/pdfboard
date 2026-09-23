#include "CrashLog.h"

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QString>

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cstring>
#include <exception>

namespace {

constexpr int kBreadcrumbSlots = 24;
constexpr int kWhatChars = 24;
constexpr int kDetailBytes = 200;

struct Breadcrumb {
    char   what[kWhatChars];
    char   detail[kDetailBytes];
    qint64 ms;
    LONG   used;
};

Breadcrumb g_crumbs[kBreadcrumbSlots];
volatile LONG g_slot = 0;          // monotonically increasing, slot = g_slot % slots
volatile LONG g_used = 0;          // how many were ever written (capped for reporting)
wchar_t g_dir[MAX_PATH] = {0};     // resolved at install() time: the handler may not
bool    g_installed = false;       // touch QStandardPaths

// ---- allocation-free helpers, safe to call from the handler ----------------------

void writeAll(HANDLE file, const char *text)
{
    if (!file || file == INVALID_HANDLE_VALUE || !text || !*text)
        return;
    DWORD written = 0;
    ::WriteFile(file, text, DWORD(::strlen(text)), &written, nullptr);
}

void writeHex(HANDLE file, const char *label, quintptr value)
{
    char line[96];
    ::_snprintf_s(line, sizeof(line), _TRUNCATE, "%s0x%llX\n", label,
                  static_cast<unsigned long long>(value));
    writeAll(file, line);
}

// "module+0x1a2b3" for an address: the module name plus the offset inside it. Without
// symbols this is what a debugger needs to find the exact instruction again.
void writeModuleOffset(HANDLE file, const void *address)
{
    HMODULE module = nullptr;
    if (address
        && ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                    | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(address), &module)
        && module) {
        wchar_t path[MAX_PATH] = {0};
        if (::GetModuleFileNameW(module, path, MAX_PATH)) {
            const wchar_t *name = ::wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            char narrow[MAX_PATH] = {0};
            ::WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1,
                                  nullptr, nullptr);
            const quintptr offset = reinterpret_cast<quintptr>(address)
                                    - reinterpret_cast<quintptr>(module);
            char line[512];
            ::_snprintf_s(line, sizeof(line), _TRUNCATE, "module          %s+0x%llX\n",
                          narrow, static_cast<unsigned long long>(offset));
            writeAll(file, line);
        }
    }
}

void writeReport(DWORD code, const void *address, EXCEPTION_POINTERS *info)
{
    if (!g_dir[0])
        return;

    wchar_t logPath[MAX_PATH] = {0};
    wchar_t dumpPath[MAX_PATH] = {0};
    const unsigned long long stamp =
        static_cast<unsigned long long>(::GetTickCount64());
    ::_snwprintf_s(logPath, MAX_PATH, _TRUNCATE, L"%s\\crash-%lu-%llu.log", g_dir,
                   ::GetCurrentProcessId(), stamp);
    ::_snwprintf_s(dumpPath, MAX_PATH, _TRUNCATE, L"%s\\crash-%lu-%llu.dmp", g_dir,
                   ::GetCurrentProcessId(), stamp);

    HANDLE file = ::CreateFileW(logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    writeAll(file, "PDFBoard crash report\n");
    writeHex(file, "exception code  ", quintptr(code));
    writeHex(file, "fault address   ", reinterpret_cast<quintptr>(address));
    writeModuleOffset(file, address);
    if (info && info->ExceptionRecord) {
        writeHex(file, "flags           ", quintptr(info->ExceptionRecord->ExceptionFlags));
        writeHex(file, "parameters      ", quintptr(info->ExceptionRecord->NumberParameters));
    }

    // Newest breadcrumb first: the last thing the app did is the interesting one.
    writeAll(file, "\nlast operations (newest first)\n");
    const LONG used = g_used;
    const LONG first = used > kBreadcrumbSlots ? used - kBreadcrumbSlots : 0;
    for (LONG i = used - 1; i >= first; --i) {
        const Breadcrumb &crumb = g_crumbs[i % kBreadcrumbSlots];
        if (crumb.used == 0)
            continue;
        char line[512];
        ::_snprintf_s(line, sizeof(line), _TRUNCATE, "  %lld ms  %s  %s\n",
                      static_cast<long long>(crumb.ms), crumb.what, crumb.detail);
        writeAll(file, line);
    }

    ::CloseHandle(file);

    // A minidump next to the log: small, and it carries the whole stack.
    HANDLE dump = ::CreateFileW(dumpPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        // The dump has to be written from the crashing thread, which is this one.
        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo;
        exceptionInfo.ThreadId = ::GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = info;
        exceptionInfo.ClientPointers = FALSE;
        ::MiniDumpWriteDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dump,
                            MiniDumpNormal, info ? &exceptionInfo : nullptr, nullptr,
                            nullptr);
        ::CloseHandle(dump);
    }
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS *info)
{
    const DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    const void *address = (info && info->ExceptionRecord)
                              ? info->ExceptionRecord->ExceptionAddress
                              : nullptr;
    writeReport(code, address, info);
    // Let Windows show its own dialog (and its WER entry) afterwards: we only add
    // evidence, we do not take over.
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminateHandler()
{
    writeReport(0xE0000001 /* our own marker: an uncaught C++ exception */, nullptr, nullptr);
    ::TerminateProcess(::GetCurrentProcess(), 3);
}

QtMessageHandler g_previousHandler = nullptr;

// qFatal() aborts the process, so it gets the same treatment before chaining to the
// handler that was installed before us (keeping the usual stderr output).
void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &text)
{
    if (type == QtFatalMsg)
        writeReport(0xE0000002 /* qFatal */, nullptr, nullptr);
    if (g_previousHandler)
        g_previousHandler(type, context, text);
}

}   // namespace

namespace CrashLog {

void install()
{
    if (g_installed)
        return;
    g_installed = true;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/PDFBoard");
    dir += QStringLiteral("/logs");
    QDir().mkpath(dir);
    const QString native = QDir::toNativeSeparators(dir);
    // Keep it alive for the process: the handler reads only this buffer.
    ::wcsncpy_s(g_dir, MAX_PATH, reinterpret_cast<const wchar_t *>(native.utf16()),
                _TRUNCATE);
    ::SetUnhandledExceptionFilter(unhandledFilter);
    std::set_terminate(terminateHandler);
    g_previousHandler = qInstallMessageHandler(messageHandler);
    // Crash reports must survive a crash even when the diagnostics log is off.
    breadcrumb("crashlog", QStringLiteral("崩溃记录已启用：%1").arg(native));
}

void breadcrumb(const char *what, const QString &detail)
{
    const LONG slot = ::InterlockedIncrement(&g_slot) - 1;
    Breadcrumb &crumb = g_crumbs[slot % kBreadcrumbSlots];
    crumb.used = 0;
    ::strncpy_s(crumb.what, kWhatChars, what ? what : "", _TRUNCATE);
    const QByteArray utf8 = detail.toUtf8();
    ::strncpy_s(crumb.detail, kDetailBytes, utf8.constData(), _TRUNCATE);
    crumb.ms = QDateTime::currentMSecsSinceEpoch();
    crumb.used = 1;
    ::InterlockedIncrement(&g_used);
}

QString directory()
{
    if (g_dir[0])
        return QString::fromWCharArray(g_dir);
    return QString();
}

int testBreadcrumbCount()
{
    const LONG used = g_used;
    return int(used > kBreadcrumbSlots ? kBreadcrumbSlots : used);
}

}   // namespace CrashLog
