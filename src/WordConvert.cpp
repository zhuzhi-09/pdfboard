#include "WordConvert.h"
#include "AppLog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>

#include <oaidl.h>
#include <objbase.h>
#include <oleauto.h>

#include <initializer_list>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Minimal raw-COM glue for the Word/WPS export.
//
// Word is driven through IDispatch directly from this thread: no PowerShell
// sidecar, no -EncodedCommand, no script text anywhere. AV software that
// prompts on the first launch of a child process never sees one, and no file
// path can ever be interpreted as code. Every interface and every BSTR is
// released on every path.
// ---------------------------------------------------------------------------

// Largest argument list used below (Documents.Open takes four).
constexpr int kMaxComArgs = 8;

// COM for the current thread. Qt has already initialised COM for the GUI
// thread: S_FALSE (already initialised) and RPC_E_CHANGED_MODE (a different
// apartment model is already active) both mean COM is usable here, and only
// our own S_OK may be paired with CoUninitialize.
class ComApartment
{
public:
    ComApartment()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_owns = (hr == S_OK);
        m_usable = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    ~ComApartment()
    {
        if (m_owns)
            CoUninitialize();
    }
    ComApartment(const ComApartment &) = delete;
    ComApartment &operator=(const ComApartment &) = delete;

    bool usable() const { return m_usable; }

private:
    bool m_owns = false;
    bool m_usable = false;
};

// Releases a dispatch and clears the pointer.
void releaseDispatch(IDispatch *&dispatch)
{
    if (dispatch) {
        dispatch->Release();
        dispatch = nullptr;
    }
}

// Owns a dispatch for the length of a scope.
class ComDispatch
{
public:
    ComDispatch() = default;
    ~ComDispatch() { releaseDispatch(m_dispatch); }
    ComDispatch(const ComDispatch &) = delete;
    ComDispatch &operator=(const ComDispatch &) = delete;

    IDispatch *get() const { return m_dispatch; }
    IDispatch *operator->() const { return m_dispatch; }
    explicit operator bool() const { return m_dispatch != nullptr; }
    void reset(IDispatch *dispatch = nullptr)
    {
        releaseDispatch(m_dispatch);
        m_dispatch = dispatch;
    }

private:
    IDispatch *m_dispatch = nullptr;
};

// Owns a VARIANT for the length of a scope (VariantClear on the way out).
class ComVariant
{
public:
    ComVariant() { VariantInit(&m_variant); }
    ~ComVariant() { VariantClear(&m_variant); }
    ComVariant(const ComVariant &) = delete;
    ComVariant &operator=(const ComVariant &) = delete;

    VARIANT *out() { return &m_variant; }

    // True when the call produced a live object. Some servers answer with
    // VT_UNKNOWN instead of VT_DISPATCH; both are accepted.
    bool isDispatch() const
    {
        return (m_variant.vt == VT_DISPATCH && m_variant.pdispVal)
               || (m_variant.vt == VT_UNKNOWN && m_variant.punkVal);
    }

    // Moves the object out: the caller owns the returned reference from here
    // on and the variant no longer releases it.
    IDispatch *takeDispatch()
    {
        IDispatch *dispatch = nullptr;
        if (m_variant.vt == VT_DISPATCH && m_variant.pdispVal) {
            dispatch = m_variant.pdispVal;
        } else if (m_variant.vt == VT_UNKNOWN && m_variant.punkVal) {
            IUnknown *unknown = m_variant.punkVal;
            if (FAILED(unknown->QueryInterface(IID_IDispatch,
                                               reinterpret_cast<void **>(&dispatch))))
                dispatch = nullptr;
            unknown->Release();             // the VT_UNKNOWN reference we own
        }
        m_variant.vt = VT_EMPTY;            // ownership moved out
        return dispatch;
    }

private:
    VARIANT m_variant;
};

// One call argument: the three shapes the export needs (VARIANT_BOOL, LONG,
// BSTR). The text of a BSTR argument must outlive the call.
struct ComArg
{
    VARTYPE type = VT_I4;
    bool boolean = false;
    int integer = 0;
    const wchar_t *text = nullptr;

    static ComArg boolArg(bool value)
    {
        ComArg arg;
        arg.type = VT_BOOL;
        arg.boolean = value;
        return arg;
    }
    static ComArg intArg(int value)
    {
        ComArg arg;
        arg.type = VT_I4;
        arg.integer = value;
        return arg;
    }
    static ComArg bstrArg(const wchar_t *value)
    {
        ComArg arg;
        arg.type = VT_BSTR;
        arg.text = value;
        return arg;
    }
};

// One IDispatch Invoke. The result variant, when wanted, is left for the
// caller to clear (a ComVariant wraps it). Argument BSTRs are allocated here
// and freed by the VariantClear loop - on every path.
HRESULT invoke(const ComDispatch &target, const wchar_t *name, WORD verb,
               const ComArg *args, int argCount, VARIANT *result)
{
    if (!target.get())
        return E_NOINTERFACE;
    if (argCount < 0 || argCount > kMaxComArgs)
        return E_INVALIDARG;

    DISPID id = DISPID_UNKNOWN;
    LPOLESTR names[1] = { const_cast<LPOLESTR>(name) };
    HRESULT hr = target->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr))
        return hr;

    // DISPPARAMS wants the arguments right-to-left, so argument 0 goes into
    // the last slot.
    VARIANT values[kMaxComArgs];
    for (int i = 0; i < kMaxComArgs; ++i)
        VariantInit(&values[i]);

    for (int i = 0; i < argCount; ++i) {
        VARIANT &value = values[argCount - 1 - i];
        if (args[i].type == VT_BSTR) {
            value.vt = VT_BSTR;
            value.bstrVal = SysAllocString(args[i].text ? args[i].text : L"");
            if (!value.bstrVal)
                hr = E_OUTOFMEMORY;
        } else if (args[i].type == VT_BOOL) {
            value.vt = VT_BOOL;
            value.boolVal = args[i].boolean ? VARIANT_TRUE : VARIANT_FALSE;
        } else {
            value.vt = VT_I4;
            value.lVal = args[i].integer;
        }
        if (FAILED(hr)) {
            for (int j = 0; j < kMaxComArgs; ++j)
                VariantClear(&values[j]);
            return hr;
        }
    }

    DISPPARAMS params = {};
    params.rgvarg = (argCount > 0) ? values : nullptr;
    params.cArgs = static_cast<UINT>(argCount);
    DISPID putId = DISPID_PROPERTYPUT;
    if (verb == DISPATCH_PROPERTYPUT) {
        params.rgdispidNamedArgs = &putId;
        params.cNamedArgs = 1;
    }

    hr = target->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, verb, &params,
                        result, nullptr, nullptr);

    for (int i = 0; i < kMaxComArgs; ++i)
        VariantClear(&values[i]);           // frees the BSTRs we allocated
    return hr;
}

// Calls a method; the optional result belongs to the caller.
HRESULT callMethod(const ComDispatch &target, const wchar_t *name,
                   std::initializer_list<ComArg> args = {},
                   VARIANT *result = nullptr)
{
    return invoke(target, name, DISPATCH_METHOD, args.begin(),
                  static_cast<int>(args.size()), result);
}

// Reads a property into `result`.
HRESULT getProperty(const ComDispatch &target, const wchar_t *name, VARIANT *result)
{
    return invoke(target, name, DISPATCH_PROPERTYGET, nullptr, 0, result);
}

// Sets a property: bool, int or BSTR.
HRESULT putProperty(const ComDispatch &target, const wchar_t *name, bool value)
{
    const ComArg arg = ComArg::boolArg(value);
    return invoke(target, name, DISPATCH_PROPERTYPUT, &arg, 1, nullptr);
}

HRESULT putProperty(const ComDispatch &target, const wchar_t *name, int value)
{
    const ComArg arg = ComArg::intArg(value);
    return invoke(target, name, DISPATCH_PROPERTYPUT, &arg, 1, nullptr);
}

HRESULT putProperty(const ComDispatch &target, const wchar_t *name,
                    const wchar_t *value)
{
    const ComArg arg = ComArg::bstrArg(value);
    return invoke(target, name, DISPATCH_PROPERTYPUT, &arg, 1, nullptr);
}

// CLSIDFromProgID + CoCreateInstance. Returns null when the ProgID is not
// registered or the server refuses to start; the caller owns the result.
IDispatch *createDispatch(const wchar_t *progId)
{
    CLSID clsid = {};
    if (FAILED(CLSIDFromProgID(progId, &clsid)))
        return nullptr;

    IDispatch *dispatch = nullptr;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
                                reinterpret_cast<void **>(&dispatch))))
        return nullptr;
    return dispatch;
}

// "0x800A1234", for the log and the status bar reason.
QString hrText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

// CLSIDFromProgID only: a probe must never launch the application.
bool probeConverter()
{
    const ComApartment apartment;
    if (!apartment.usable())
        return false;

    // Word first, then the WPS ProgIDs (what most classroom machines actually
    // ship).
    static const wchar_t *const kProgIds[] = { L"Word.Application",
                                               L"KWPS.Application",
                                               L"WPS.Application" };
    for (const wchar_t *progId : kProgIds) {
        CLSID clsid = {};
        if (SUCCEEDED(CLSIDFromProgID(progId, &clsid)))
            return true;
    }
    return false;
}

QString tempDir()
{
    return QDir(QDir::tempPath()).filePath(QStringLiteral("pdfboard"));
}

// The whole automation conversation. Returns an empty string on success or a
// reason for the (status-bar-only) failure. A hidden Word/WPS instance is
// closed and quit before returning, on every path; a running WINWORD.EXE /
// wps.exe the user may own is never terminated.
QString runExport(const QString &src, const QString &part)
{
    const ComApartment apartment;
    if (!apartment.usable())
        return QStringLiteral("无法初始化 COM");

    ComDispatch app;
    const wchar_t *const progIds[] = { L"Word.Application",
                                       L"KWPS.Application",
                                       L"WPS.Application" };
    for (const wchar_t *progId : progIds) {
        app.reset(createDispatch(progId));
        if (app)
            break;
    }
    if (!app)
        return QStringLiteral("无法启动 Word/WPS 自动化");

    QString failure;
    ComDispatch documents;
    ComDispatch document;

    // Hidden and without modal alerts: the GUI thread is blocked while this
    // runs, so any dialog would hang the application.
    if (FAILED(putProperty(app, L"Visible", false))) {
        failure = QStringLiteral("无法隐藏 Word/WPS 窗口");
    } else if (FAILED(putProperty(app, L"DisplayAlerts", 0))) {
        failure = QStringLiteral("无法关闭 Word/WPS 警告");
    }

    // Word pops a modal "Microsoft Word is not the default program for viewing
    // and editing documents" dialog when it starts on a machine where the .docx
    // default was never chosen. While that dialog is up, Open/Export are blocked
    // and Quit() is refused - which is exactly what surfaced as 转换失败.
    // Options.AlertIfNotDefault is the documented object-model switch behind that
    // nag, so clearing it here keeps the export headless. Best effort: if the
    // server does not expose it we carry on and let the caller's retry handle it.
    if (failure.isEmpty()) {
        ComVariant optionsValue;
        if (SUCCEEDED(getProperty(app, L"Options", optionsValue.out()))
            && optionsValue.isDispatch()) {
            ComDispatch options;
            options.reset(optionsValue.takeDispatch());
            putProperty(options, L"AlertIfNotDefault", false);
        }
    }

    if (failure.isEmpty()) {
        ComVariant value;
        if (FAILED(getProperty(app, L"Documents", value.out())) || !value.isDispatch())
            failure = QStringLiteral("无法访问 Word/WPS 的文档集合");
        else
            documents.reset(value.takeDispatch());
    }

    if (failure.isEmpty()) {
        const std::wstring path =
            QDir::toNativeSeparators(src).toStdWString();
        ComVariant value;
        // Open(FileName, ConfirmConversions, ReadOnly, AddToRecentFiles): the
        // original is opened read-only and must not enter Word's own recent
        // list - only this app's 最近项目 remembers the user-visible path.
        const HRESULT hr = callMethod(
            documents, L"Open",
            { ComArg::bstrArg(path.c_str()), ComArg::boolArg(false),
              ComArg::boolArg(true), ComArg::boolArg(false) },
            value.out());
        if (FAILED(hr))
            failure = QStringLiteral("Word/WPS 无法打开该文档（%1）").arg(hrText(hr));
        else if (!value.isDispatch())
            failure = QStringLiteral("Word/WPS 未返回文档对象");
        else
            document.reset(value.takeDispatch());
    }

    if (failure.isEmpty()) {
        const std::wstring target =
            QDir::toNativeSeparators(part).toStdWString();
        // ExportAsFixedFormat(OutputFileName, ExportFormat = 17, i.e.
        // wdExportFormatPDF). The `.part` name keeps the cache file from ever
        // appearing before the export has really succeeded.
        const HRESULT hr = callMethod(
            document, L"ExportAsFixedFormat",
            { ComArg::bstrArg(target.c_str()), ComArg::intArg(17) }, nullptr);
        if (FAILED(hr))
            failure = QStringLiteral("Word/WPS 导出 PDF 失败（%1）").arg(hrText(hr));
    }

    // Close(0) = discard, then Quit: a failed export must not leave a hidden
    // Word/WPS behind.
    if (document)
        callMethod(document, L"Close", { ComArg::intArg(0) });
    if (app)
        callMethod(app, L"Quit");

    return failure;
}

}   // namespace

bool WordConvert::wordIsDefaultHandler()
{
    // Windows stores the user's explicit choice under UserChoice; its absence
    // (or a non-Word ProgID) is the state in which Word nags.
    QSettings choice(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion"
                       "\\Explorer\\FileExts\\.docx\\UserChoice"),
        QSettings::NativeFormat);
    const QString progId = choice.value(QStringLiteral("ProgId")).toString();
    return progId.startsWith(QStringLiteral("Word."), Qt::CaseInsensitive);
}

bool WordConvert::isWordDoc(const QString &path)
{
    return path.endsWith(QStringLiteral(".docx"), Qt::CaseInsensitive)
           || path.endsWith(QStringLiteral(".doc"), Qt::CaseInsensitive);
}

QString WordConvert::tempPdfPathFor(const QString &src, qint64 size, qint64 mtimeMs)
{
    // Path, size and mtime together: an edited document re-exports, an
    // untouched one reuses the cached PDF.
    const QByteArray key = src.toUtf8() + '|' + QByteArray::number(size) + '|'
                           + QByteArray::number(mtimeMs);
    const QString hash =
        QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
    return QDir(tempDir()).filePath(QStringLiteral("word-") + hash + QStringLiteral(".pdf"));
}

bool WordConvert::hasConverter()
{
    // Probed once per process: the installed automation cannot change
    // mid-session, and the probe is a registration lookup - it never launches
    // Word/WPS.
    static const bool available = probeConverter();
    return available;
}

bool WordConvert::convertToPdf(const QString &src, QString *pdfOut, QString *errorOut)
{
    auto fail = [errorOut](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };
    if (pdfOut)
        pdfOut->clear();

    if (!isWordDoc(src))
        return fail(QStringLiteral("不是 Word 文档：%1").arg(src));
    if (!hasConverter())
        return fail(QStringLiteral("未检测到 Word/WPS"));

    const QFileInfo info(src);
    const QString out = tempPdfPathFor(src, info.size(), info.lastModified().toMSecsSinceEpoch());
    if (QFileInfo::exists(out)) {
        // Path + size + mtime are all unchanged, so this is still Word's exact
        // layout for the current file: no re-export.
        AppLog::write(QStringLiteral("open"),
                      QStringLiteral("Word 转换使用缓存：%1").arg(QFileInfo(out).fileName()));
        if (pdfOut)
            *pdfOut = out;
        return true;
    }
    if (!info.isFile())
        return fail(QStringLiteral("源文件不存在：%1").arg(src));

    const QString dir = tempDir();
    if (!QDir().mkpath(dir))
        return fail(QStringLiteral("无法创建临时目录：%1").arg(dir));

    // The export writes the `.part` name, so the cache file only ever appears
    // when the export has really succeeded.
    const QString part = out + QStringLiteral(".part");
    QFile::remove(part);                    // a stale partial from a crash

    QElapsedTimer timer;
    timer.start();

    // Word reads its "am I the default program?" options when it STARTS, and on
    // a machine where this app - not Word - owns .docx it will pop a modal
    // dialog that blocks Open/Export and refuses Quit. Setting the option over
    // COM is too late (the dialog is already up), so write the two Word option
    // values first: AlertIfNotDefault is the switch behind that nag and
    // DoNotCheckIfWordIsDefaultApp is its older sibling. Word's own "Tell me if
    // Microsoft Word isn't the default program" checkbox writes the same keys.
    const auto silenceDefaultProgramNag = [] {
        QSettings office(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Office"),
                         QSettings::NativeFormat);
        for (const QString &version : office.childGroups()) {
            if (!version.contains(QLatin1Char('.')))
                continue;                       // version keys look like 16.0
            QSettings options(
                QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Office/%1/Word/Options")
                    .arg(version),
                QSettings::NativeFormat);
            if (options.contains(QStringLiteral("AlertIfNotDefault"))
                && options.value(QStringLiteral("AlertIfNotDefault")).toInt() == 0) {
                continue;                       // already silenced
            }
            options.setValue(QStringLiteral("AlertIfNotDefault"), 0);
            options.setValue(QStringLiteral("DoNotCheckIfWordIsDefaultApp"), 1);
            options.sync();
            AppLog::write(QStringLiteral("open"),
                          QStringLiteral("已关闭 Word 的「不是默认程序」提醒（Office %1）")
                              .arg(version));
        }
    };
    silenceDefaultProgramNag();

    QString failure = runExport(src, part);
    if (!failure.isEmpty()) {
        // One retry: on a machine where Word nagged with its "not the default
        // program" dialog, the attempt above has just cleared that option, so
        // the second run gets through. Cheap (only on failure) and it turns a
        // first-time failure into a working conversion.
        AppLog::write(QStringLiteral("open"),
                      QStringLiteral("Word 转换首次失败，重试一次：%1").arg(failure));
        failure = runExport(src, part);
    }
    if (failure.isEmpty() && !QFileInfo::exists(part))
        failure = QStringLiteral("导出未生成文件");
    if (failure.isEmpty() && !QFile::rename(part, out))
        failure = QStringLiteral("无法写入缓存文件：%1").arg(out);

    if (!failure.isEmpty()) {
        QFile::remove(part);                // never leave a partial behind
        AppLog::write(QStringLiteral("open"),
                      QStringLiteral("Word 转换失败：%1（%2）").arg(src, failure));
        return fail(failure);
    }

    AppLog::write(QStringLiteral("open"),
                  QStringLiteral("Word 转换完成：%1 → %2（%3 ms）")
                      .arg(QFileInfo(src).fileName(), QFileInfo(out).fileName())
                      .arg(timer.elapsed()));
    if (pdfOut)
        *pdfOut = out;
    return true;
}
