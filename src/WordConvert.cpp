#include "WordConvert.h"
#include "AppLog.h"
#include "AppSettings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QStringList>

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

// ---------------------------------------------------------------------------
// The "Word is not the default program" option values: where they live, what
// silencing them writes, and where the pre-existing state is recorded so the
// change can be undone from the settings page.
// ---------------------------------------------------------------------------

const QString kOfficeKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Office");
const QString kAppKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\PDFBoard");
const QString kWordNagBackupValue = QStringLiteral("WordNagBackup");
const QString kAlertValue = QStringLiteral("AlertIfNotDefault");
const QString kNoCheckValue = QStringLiteral("DoNotCheckIfWordIsDefaultApp");
constexpr int kAlertSilenced = 0;
constexpr int kNoCheckSilenced = 1;
// Word's documented default for the switch itself: the "Tell me if Microsoft
// Word isn't the default program" checkbox is on, and no
// DoNotCheckIfWordIsDefaultApp value exists at all.
constexpr int kAlertDefault = 1;

QString wordOptionsKey(const QString &version)
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Office/%1/Word/Options")
        .arg(version);
}

// Office version keys look like "16.0"; the same filter the nag fix always
// used keeps "Common", "ClickToRun" and friends out.
QStringList officeVersionKeys()
{
    QSettings office(kOfficeKey, QSettings::NativeFormat);
    QStringList versions;
    for (const QString &group : office.childGroups()) {
        if (group.contains(QLatin1Char('.')))
            versions.append(group);
    }
    return versions;
}

// The raw WordNagBackup value; empty when this app has recorded none.
QString nagBackupJson()
{
    QSettings app(kAppKey, QSettings::NativeFormat);
    return app.value(kWordNagBackupValue).toString();
}

// The two recorded values for one Office version. `exists` tells a restore
// whether to write the original data back or to delete the value.
QVariantMap readWordOptionState(const QString &version)
{
    QSettings options(wordOptionsKey(version), QSettings::NativeFormat);
    QVariantMap state;
    for (const QString &name : { kAlertValue, kNoCheckValue }) {
        QVariantMap entry;
        entry.insert(QStringLiteral("exists"), options.contains(name));
        entry.insert(QStringLiteral("value"), options.value(name).toInt());
        state.insert(version + QLatin1Char('|') + name, entry);
    }
    return state;
}

// "16.0|AlertIfNotDefault" -> version + value name. Returns false for a key
// that does not carry both halves.
bool splitEntryKey(const QString &key, QString *version, QString *name)
{
    const int bar = key.indexOf(QLatin1Char('|'));
    if (bar <= 0 || bar >= key.size() - 1)
        return false;
    *version = key.left(bar);
    *name = key.mid(bar + 1);
    return true;
}

// The silenced value of one of the two option names; false for an unknown
// name (which a hand-edited backup could contain).
bool silencedValueFor(const QString &name, int *value)
{
    if (name == kAlertValue) {
        *value = kAlertSilenced;
        return true;
    }
    if (name == kNoCheckValue) {
        *value = kNoCheckSilenced;
        return true;
    }
    return false;
}

// True when some Office version's options currently hold the values a
// silencing write leaves behind: AlertIfNotDefault = 0 (the switch itself) or
// DoNotCheckIfWordIsDefaultApp = 1 (its older sibling). Read-only; the
// backup-less path of restoreWordNag() uses it to tell "modified by an older
// build or by hand" apart from "never touched".
bool currentNagIsSilenced()
{
    for (const QString &version : officeVersionKeys()) {
        const QSettings options(wordOptionsKey(version), QSettings::NativeFormat);
        if (options.contains(kAlertValue)
            && options.value(kAlertValue).toInt() == kAlertSilenced) {
            return true;
        }
        if (options.contains(kNoCheckValue)
            && options.value(kNoCheckValue).toInt() == kNoCheckSilenced) {
            return true;
        }
    }
    return false;
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

QJsonObject WordConvert::encodeNagBackup(const QVariantMap &recorded)
{
    QJsonObject backup;
    for (auto it = recorded.constBegin(); it != recorded.constEnd(); ++it) {
        const QVariantMap entry = it.value().toMap();
        QJsonObject value;
        value.insert(QStringLiteral("exists"),
                     entry.value(QStringLiteral("exists")).toBool());
        value.insert(QStringLiteral("value"),
                     entry.value(QStringLiteral("value")).toInt());
        backup.insert(it.key(), value);
    }
    return backup;
}

QVariantMap WordConvert::decodeNagBackup(const QJsonObject &backup)
{
    QVariantMap recorded;
    for (auto it = backup.constBegin(); it != backup.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        QVariantMap value;
        value.insert(QStringLiteral("exists"),
                     entry.value(QStringLiteral("exists")).toBool());
        value.insert(QStringLiteral("value"),
                     entry.value(QStringLiteral("value")).toInt());
        recorded.insert(it.key(), value);
    }
    return recorded;
}

bool WordConvert::silenceWordNag(QString *errorOut)
{
    auto fail = [errorOut](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    const QStringList versions = officeVersionKeys();
    if (versions.isEmpty())
        return fail(QStringLiteral("未找到 Office 版本注册表项，无法关闭 Word 的提醒"));

    // The first call records the pre-existing state exactly once. An existing
    // backup is never overwritten: it has to keep describing the true original
    // state for as long as the settings page can undo the change.
    QSettings app(kAppKey, QSettings::NativeFormat);
    if (!app.contains(kWordNagBackupValue)) {
        QVariantMap recorded;
        for (const QString &version : versions)
            recorded.insert(readWordOptionState(version));
        app.setValue(kWordNagBackupValue,
                     QString::fromUtf8(QJsonDocument(encodeNagBackup(recorded))
                                           .toJson(QJsonDocument::Compact)));
        app.sync();
        if (app.status() != QSettings::NoError)
            return fail(QStringLiteral("无法记录 Word 设置备份（注册表写入失败）"));
    }

    // Word reads these values at startup, so they have to be in place before
    // the automation instance is created. Writing them again is harmless.
    for (const QString &version : versions) {
        QSettings options(wordOptionsKey(version), QSettings::NativeFormat);
        options.setValue(kAlertValue, kAlertSilenced);
        options.setValue(kNoCheckValue, kNoCheckSilenced);
        options.sync();
        if (options.status() != QSettings::NoError)
            return fail(QStringLiteral("无法写入 Word 的提醒设置（注册表写入失败）"));
    }

    AppLog::write(QStringLiteral("open"),
                  QStringLiteral("已关闭 Word 的「不是默认程序」提醒（Office %1）")
                      .arg(versions.join(QStringLiteral(", "))));
    if (errorOut)
        errorOut->clear();
    return true;
}

bool WordConvert::restoreWordNag(QString *errorOut)
{
    auto fail = [errorOut](const QString &message) {
        if (errorOut)
            *errorOut = message;
        return false;
    };

    const QJsonDocument document = QJsonDocument::fromJson(nagBackupJson().toUtf8());
    const bool haveBackup = document.isObject() && !document.object().isEmpty();

    if (!haveBackup) {
        // Nothing recorded to replay. When the current values still look like a
        // silencing write (an older build, or a manual edit), Word's documented
        // defaults are the best restore available: AlertIfNotDefault back to 1
        // and no DoNotCheckIfWordIsDefaultApp value. Only values that actually
        // exist are touched - nothing is invented, no key is created.
        if (!currentNagIsSilenced())
            return fail(QStringLiteral("没有可恢复的 Word 设置备份"));

        for (const QString &version : officeVersionKeys()) {
            QSettings options(wordOptionsKey(version), QSettings::NativeFormat);
            bool changed = false;
            if (options.contains(kAlertValue)
                && options.value(kAlertValue).toInt() == kAlertSilenced) {
                options.setValue(kAlertValue, kAlertDefault);
                changed = true;
            }
            if (options.contains(kNoCheckValue)
                && options.value(kNoCheckValue).toInt() == kNoCheckSilenced) {
                options.remove(kNoCheckValue);
                changed = true;
            }
            if (changed)
                options.sync();
        }

        AppLog::write(QStringLiteral("open"),
                      QStringLiteral("已把 Word 的提醒设置恢复为默认（无备份记录）"));
        // Success with a notice, not an error: the nag WILL come back, and the
        // settings page reports exactly that through the status bar.
        if (errorOut) {
            *errorOut = QStringLiteral(
                "没有备份记录：已恢复为 Word 默认设置（会重新提醒）");
        }
        return true;
    }

    const QVariantMap recorded = decodeNagBackup(document.object());
    for (auto it = recorded.constBegin(); it != recorded.constEnd(); ++it) {
        QString version;
        QString name;
        if (!splitEntryKey(it.key(), &version, &name))
            continue;                       // hand-edited backup: skip the junk

        const QVariantMap entry = it.value().toMap();
        QSettings options(wordOptionsKey(version), QSettings::NativeFormat);
        if (entry.value(QStringLiteral("exists")).toBool())
            options.setValue(name, entry.value(QStringLiteral("value")).toInt());
        else
            options.remove(name);
        options.sync();
    }

    QSettings app(kAppKey, QSettings::NativeFormat);
    app.remove(kWordNagBackupValue);
    app.sync();
    if (app.status() != QSettings::NoError)
        return fail(QStringLiteral("无法清除 Word 设置备份（注册表写入失败）"));

    AppLog::write(QStringLiteral("open"), QStringLiteral("已恢复 Word 的提醒设置"));
    if (errorOut)
        errorOut->clear();
    return true;
}

WordConvert::NagRestoreMode WordConvert::nagRestoreMode()
{
    if (wordNagBackupExists())
        return NagRestoreMode::FromBackup;
    if (currentNagIsSilenced())
        return NagRestoreMode::ToDefaults;
    return NagRestoreMode::None;
}

bool WordConvert::wordNagBackupExists()
{
    const QJsonDocument document = QJsonDocument::fromJson(nagBackupJson().toUtf8());
    return document.isObject() && !document.object().isEmpty();
}

bool WordConvert::wordNagSilenced()
{
    const QJsonDocument document = QJsonDocument::fromJson(nagBackupJson().toUtf8());
    if (!document.isObject() || document.object().isEmpty())
        return false;

    const QVariantMap recorded = decodeNagBackup(document.object());
    if (recorded.isEmpty())
        return false;

    int checked = 0;
    for (auto it = recorded.constBegin(); it != recorded.constEnd(); ++it) {
        QString version;
        QString name;
        if (!splitEntryKey(it.key(), &version, &name))
            return false;

        int expected = 0;
        if (!silencedValueFor(name, &expected))
            continue;                       // not one of the two options

        QSettings options(wordOptionsKey(version), QSettings::NativeFormat);
        if (!options.contains(name) || options.value(name).toInt() != expected)
            return false;
        ++checked;
    }
    return checked > 0;
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
    // COM is too late (the dialog is already up), so the registry write below
    // MUST stay before runExport() creates the automation instance: it clears
    // AlertIfNotDefault and sets DoNotCheckIfWordIsDefaultApp, the same keys
    // Word's own "Tell me if Microsoft Word isn't the default program" checkbox
    // writes. silenceWordNag() records the previous state first, so the user
    // can undo it from the settings page; the toggle there can also turn the
    // whole thing off. Best effort: when the write fails the conversion still
    // runs and reports its own reason.
    if (AppSettings::wordNagFixEnabled()) {
        QString nagError;
        if (!silenceWordNag(&nagError))
            AppLog::write(QStringLiteral("open"),
                          QStringLiteral("Word 提醒开关未写入：%1").arg(nagError));
    }

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
