#include "TouchProbe.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QInputDevice>
#include <QKeyEvent>
#include <QLabel>
#include <QPointingDevice>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <QAbstractNativeEventFilter>
#endif

namespace {

QFile g_log;

void writeStdout(const QByteArray &bytes)
{
#ifdef Q_OS_WIN
    // A GUI-subsystem app started from Explorer has no stdout at all; WriteFile on an
    // invalid handle is a no-op, fwrite on a NULL stream is not.
    const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, bytes.constData(), DWORD(bytes.size()), &written, nullptr);
    }
#else
    fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
    fflush(stdout);
#endif
}

void logLine(const QString &line)
{
    const QString stamped =
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) + QLatin1Char(' ')
        + line;
    if (g_log.isOpen()) {
        g_log.write(stamped.toUtf8());
        g_log.write("\n");
        g_log.flush();
    }
    // The window is the primary readout; stdout is a convenience when launched from
    // a terminal. UTF-8 bytes so Chinese survives any code page.
    writeStdout(stamped.toUtf8() + '\n');
}

QString capabilityText(QInputDevice::Capabilities caps)
{
    QStringList parts;
    if (caps.testFlag(QInputDevice::Capability::Position)) parts << QStringLiteral("Position");
    if (caps.testFlag(QInputDevice::Capability::Area)) parts << QStringLiteral("Area");
    if (caps.testFlag(QInputDevice::Capability::Pressure)) parts << QStringLiteral("Pressure");
    if (caps.testFlag(QInputDevice::Capability::NormalizedPosition))
        parts << QStringLiteral("NormalizedPosition");
    if (caps.testFlag(QInputDevice::Capability::MouseEmulation)) parts << QStringLiteral("MouseEmulation");
    return parts.isEmpty() ? QStringLiteral("-") : parts.join(QLatin1Char('|'));
}

QString systemInfo()
{
    QString s;
#ifdef Q_OS_WIN
    const int digitizers = GetSystemMetrics(SM_DIGITIZER);
    const int maxTouches = GetSystemMetrics(SM_MAXIMUMTOUCHES);
    QStringList flags;
    if (digitizers & NID_INTEGRATED_TOUCH) flags << QStringLiteral("INTEGRATED_TOUCH");
    if (digitizers & NID_EXTERNAL_TOUCH) flags << QStringLiteral("EXTERNAL_TOUCH");
    if (digitizers & NID_INTEGRATED_PEN) flags << QStringLiteral("INTEGRATED_PEN");
    if (digitizers & NID_EXTERNAL_PEN) flags << QStringLiteral("EXTERNAL_PEN");
    if (digitizers & NID_MULTI_INPUT) flags << QStringLiteral("MULTI_INPUT");
    if (digitizers & NID_READY) flags << QStringLiteral("READY");
    s += QStringLiteral("SM_DIGITIZER=0x%1 (%2)\n")
             .arg(digitizers, 0, 16)
             .arg(flags.isEmpty() ? QStringLiteral("-") : flags.join(QLatin1Char('|')));
    s += QStringLiteral("SM_MAXIMUMTOUCHES=%1\n").arg(maxTouches);
#endif
    for (const QInputDevice *dev : QInputDevice::devices()) {
        int maxPoints = -1;
        if (const auto *pd = qobject_cast<const QPointingDevice *>(dev))
            maxPoints = pd->maximumPoints();
        s += QStringLiteral("设备: %1 | type=%2 | caps=%3 | maxPoints=%4\n")
                 .arg(dev->name().isEmpty() ? QStringLiteral("(unnamed)") : dev->name())
                 .arg(int(dev->type()))
                 .arg(capabilityText(dev->capabilities()))
                 .arg(maxPoints);
    }
    return s;
}

#ifdef Q_OS_WIN
QString pointerFlagsText(DWORD flags)
{
    QStringList parts;
    if (flags & POINTER_FLAG_DOWN) parts << QStringLiteral("DOWN");
    if (flags & POINTER_FLAG_UPDATE) parts << QStringLiteral("UPDATE");
    if (flags & POINTER_FLAG_UP) parts << QStringLiteral("UP");
    if (flags & POINTER_FLAG_INCONTACT) parts << QStringLiteral("INCONTACT");
    if (flags & POINTER_FLAG_INRANGE) parts << QStringLiteral("INRANGE");
    if (flags & POINTER_FLAG_PRIMARY) parts << QStringLiteral("PRIMARY");
    return parts.isEmpty() ? QStringLiteral("-") : parts.join(QLatin1Char('|'));
}
#endif

class TouchProbeWindow : public QWidget
{
public:
    TouchProbeWindow()
    {
        setWindowTitle(QStringLiteral("触摸诊断 — 落墨"));
        resize(1100, 720);
        setAttribute(Qt::WA_AcceptTouchEvents, true);
        setStyleSheet(QStringLiteral(
            "QWidget { background:#101418; color:#E8EAED; }"
            "QLabel { color:#E8EAED; }"));

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(28, 24, 28, 24);
        layout->setSpacing(14);

        auto *title = new QLabel(QStringLiteral("触摸诊断（--probe-touch）"), this);
        QFont tf = title->font();
        tf.setPointSize(tf.pointSize() + 10);
        tf.setBold(true);
        title->setFont(tf);
        layout->addWidget(title);

        auto *hint = new QLabel(
            QStringLiteral("用一根手指写一下，再用整个手掌压一下屏幕。\n"
                           "看下面 “contact=” 是不是 0x0；最后按 Esc 退出。"),
            this);
        QFont hf = hint->font();
        hf.setPointSize(hf.pointSize() + 2);
        hint->setFont(hf);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        m_body = new QLabel(this);
        QFont mono(QStringLiteral("Consolas"));
        mono.setStyleHint(QFont::Monospace);
        mono.setPointSize(mono.pointSize() + 3);
        m_body->setFont(mono);
        m_body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        m_body->setWordWrap(true);
        layout->addWidget(m_body, 1);

        m_system = systemInfo();
        logLine(QStringLiteral("=== touch probe start ==="));
        logLine(m_system.trimmed());
        m_body->setText(m_system + QStringLiteral("\n等待触摸…"));

        // Never leave a diagnostic window on the classroom machine forever.
        QTimer::singleShot(180000, this, &QWidget::close);
    }

    void noteRaw(const QString &line)
    {
        m_lastRaw = line;
        refresh();
    }

protected:
    bool event(QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            handleTouch(static_cast<QTouchEvent *>(e));
            return true;
        case QEvent::KeyPress:
            if (static_cast<QKeyEvent *>(e)->key() == Qt::Key_Escape)
                close();
            break;
        default:
            break;
        }
        return QWidget::event(e);
    }

private:
    void handleTouch(QTouchEvent *te)
    {
        QString s = QStringLiteral("Qt touch type=%1 points=%2\n")
                        .arg(int(te->type()))
                        .arg(te->points().size());
        for (const QEventPoint &p : te->points()) {
            const QSizeF d = p.ellipseDiameters();
            s += QStringLiteral("  id=%1 state=%2 pos=%3,%4 contact=%5x%6 pressure=%7\n")
                     .arg(p.id())
                     .arg(int(p.state()))
                     .arg(p.position().x(), 0, 'f', 1)
                     .arg(p.position().y(), 0, 'f', 1)
                     .arg(d.width(), 0, 'f', 1)
                     .arg(d.height(), 0, 'f', 1)
                     .arg(p.pressure(), 0, 'f', 2);
        }
        // Panel events arrive at ~100 Hz; the window can follow them, the log should not.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastQtLog >= 250) {
            m_lastQtLog = now;
            logLine(s.trimmed());
        }
        m_lastQt = s;
        refresh();
    }

    void refresh()
    {
        m_body->setText(m_system + QLatin1Char('\n') + m_lastQt + QLatin1Char('\n')
                        + QStringLiteral("Windows WM_POINTER:\n") + m_lastRaw);
    }

    QLabel *m_body = nullptr;
    QString m_system;
    QString m_lastQt = QStringLiteral("(还没有 QT 触摸事件)");
    QString m_lastRaw = QStringLiteral("(还没有 WM_POINTER 触摸消息)");
    qint64 m_lastQtLog = 0;
};

#ifdef Q_OS_WIN
class TouchProbeFilter : public QAbstractNativeEventFilter
{
public:
    explicit TouchProbeFilter(TouchProbeWindow *window) : m_window(window) {}

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
    {
        if (type != "windows_generic_MSG" && type != "windows_dispatcher_MSG")
            return false;
        MSG *msg = static_cast<MSG *>(message);
        if (!msg || !m_window)
            return false;
        const HWND hwnd = reinterpret_cast<HWND>(m_window->internalWinId());
        if (!hwnd || msg->hwnd != hwnd)
            return false;
        switch (msg->message) {
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
        case WM_POINTERCAPTURECHANGED:
            break;
        default:
            return false;
        }

        const UINT32 pointerId = GET_POINTERID_WPARAM(msg->wParam);
        POINTER_INPUT_TYPE ptype = PT_POINTER;
        if (!GetPointerType(pointerId, &ptype) || ptype != PT_TOUCH)
            return false;
        POINTER_TOUCH_INFO info{};
        if (!GetPointerTouchInfo(pointerId, &info))
            return false;

        const bool hasArea = (info.touchMask & TOUCH_MASK_CONTACTAREA) != 0;
        const long w = info.rcContact.right - info.rcContact.left;
        const long h = info.rcContact.bottom - info.rcContact.top;
        const bool hasPressure = (info.touchMask & TOUCH_MASK_PRESSURE) != 0;
        const QString line =
            QStringLiteral("msg=0x%1 id=%2 flags=%3 maskArea=%4 rcContact=%5x%6 "
                           "pressure=%7 (maskPressure=%8)")
                .arg(msg->message, 0, 16)
                .arg(pointerId)
                .arg(pointerFlagsText(info.pointerInfo.pointerFlags))
                .arg(hasArea ? QStringLiteral("YES") : QStringLiteral("no"))
                .arg(w)
                .arg(h)
                .arg(info.pressure)
                .arg(hasPressure ? QStringLiteral("YES") : QStringLiteral("no"));

        // A touch panel repeats a sample several times; 20 lines/second is plenty and
        // keeps the log readable.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastLog >= 50) {
            m_lastLog = now;
            logLine(line);
        }
        m_window->noteRaw(line);
        return false;   // observe only: Qt still handles the message
    }

private:
    TouchProbeWindow *m_window = nullptr;
    qint64 m_lastLog = 0;
};
#endif

}   // namespace

int runTouchProbe()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/logs");
    QDir().mkpath(dir);
    g_log.setFileName(dir + QStringLiteral("/touch-probe.txt"));
    g_log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);

    TouchProbeWindow window;
    window.show();

#ifdef Q_OS_WIN
    TouchProbeFilter filter(&window);
    qApp->installNativeEventFilter(&filter);
#endif
    const int rc = qApp->exec();
#ifdef Q_OS_WIN
    qApp->removeNativeEventFilter(&filter);
#endif
    logLine(QStringLiteral("=== touch probe end ==="));
    g_log.close();
    return rc;
}
