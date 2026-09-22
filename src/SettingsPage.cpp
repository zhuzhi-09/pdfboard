#include "SettingsPage.h"

#include "AppSettings.h"
#include "AppLog.h"
#include "IconPainter.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QDesktopServices>
#include <QDir>
#include <QEnterEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

namespace {

// The auto-start control: a WinUI-style toggle switch painted with the Theme
// tokens. Doing it by hand - rather than through a style sheet - keeps the app
// free of image assets, and the metrics follow the font, so the switch grows
// with the display DPI and the touch panel's hit targets.
class ToggleSwitch : public QAbstractButton
{
public:
    explicit ToggleSwitch(QWidget *parent = nullptr)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        const int h = trackHeight();
        return QSize(int(h * 1.9), h);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme::Palette &pal = Theme::light();
        const QSize hint = sizeHint();
        const int h = hint.height();
        const QRectF track((width() - hint.width()) / 2.0, (height() - h) / 2.0,
                           hint.width(), h);
        const qreal margin = h * 0.12;
        const qreal knob = h - 2.0 * margin;
        const qreal radius = h / 2.0;
        const bool on = isChecked();

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        if (hasFocus()) {
            p.setPen(QPen(pal.accentSoft, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(track.adjusted(-2.0, -2.0, 2.0, 2.0),
                              radius + 2.0, radius + 2.0);
        }

        // Off: an inset chip; on: the accent fill. Hover tightens the fill.
        p.setBrush(on ? (underMouse() ? pal.accentHover : pal.accent)
                      : (underMouse() ? pal.surfacePressed : pal.chipTint));
        p.setPen(on ? QPen(Qt::NoPen)
                    : QPen(pal.surfaceEdge, qMax<qreal>(1.0, h * 0.06)));
        p.drawRoundedRect(track, radius, radius);

        const QPointF centre(on ? track.right() - margin - knob / 2.0
                                : track.left() + margin + knob / 2.0,
                             track.center().y());
        p.setPen(Qt::NoPen);
        p.setBrush(pal.onAccent);
        p.drawEllipse(centre, knob / 2.0, knob / 2.0);
    }

    void enterEvent(QEnterEvent *e) override
    {
        QAbstractButton::enterEvent(e);
        update();
    }

    void leaveEvent(QEvent *e) override
    {
        QAbstractButton::leaveEvent(e);
        update();
    }

private:
    int trackHeight() const
    {
        const Theme::Metrics m = Theme::metrics(font());
        return qMax(int(m.touch * 0.52), qMax(20, fontMetrics().height()));
    }
};

// The sheet of the page body. Applied to the scrolling body (never to the page
// root) so the guidance message box spawned from here keeps its native look.
QString pageSheet(const QFont &font)
{
    const Theme::Palette &c = Theme::light();
    const Theme::Metrics m = Theme::metrics(font);

    return QStringLiteral(
               "QWidget#settingsBody { background: transparent; }"
               "QLabel { color: %1; background: transparent; }"
               "QLabel#settingsSectionHeader { color: %2; }"
               "QLabel#settingsRowBody { color: %2; }"
               "QLabel#settingsSectionBody { color: %2; }"
               "QFrame#settingsCard {"
               " background: %3;"
               " border: 1px solid %4;"
               " border-radius: %5; }"
               "QFrame#settingsRowLine { background: %6; border: none; }"
               "QPushButton {"
               " color: %1;"
               " background: %7;"
               " border: 1px solid %4;"
               " border-radius: %8;"
               " padding: %9 %10; }"
               "QPushButton:hover { background: %11; }"
               "QPushButton:pressed { background: %12; }"
               "QPushButton#settingsPrimary {"
               " color: %13;"
               " background: %14;"
               " border: none; }"
               "QPushButton#settingsPrimary:hover { background: %15; }")
        .arg(Theme::rgba(c.text))
        .arg(Theme::rgba(c.textMuted))
        .arg(Theme::rgba(c.paper))
        .arg(Theme::rgba(c.surfaceEdge))
        .arg(Theme::px(qMax(Theme::Space2 + 2, m.radiusButton - Theme::Space1)))
        .arg(Theme::rgba(c.divider))
        .arg(Theme::rgba(c.chipTint))
        .arg(Theme::px(m.radiusButton))
        .arg(Theme::px(m.padY))
        .arg(Theme::px(m.padX + Theme::Space1))
        .arg(Theme::rgba(c.surfaceHover))
        .arg(Theme::rgba(c.surfacePressed))
        .arg(Theme::rgba(c.onAccent))
        .arg(Theme::rgba(c.accent))
        .arg(Theme::rgba(c.accentHover));
}

// A white rounded card with a hairline border. Rows go into `col`; the caller
// separates them with makeRowSeparator().
struct Card {
    QFrame      *frame = nullptr;
    QVBoxLayout *col   = nullptr;
};

Card makeCard(QWidget *parent)
{
    Card card;
    card.frame = new QFrame(parent);
    card.frame->setObjectName(QStringLiteral("settingsCard"));
    card.frame->setAttribute(Qt::WA_StyledBackground, true);

    card.col = new QVBoxLayout(card.frame);
    card.col->setContentsMargins(0, 0, 0, 0);
    card.col->setSpacing(0);
    return card;
}

// A hairline row separator, inset from the card edges like the system sheet.
QWidget *makeRowSeparator(QWidget *parent)
{
    auto *wrap = new QWidget(parent);
    auto *row = new QHBoxLayout(wrap);
    row->setContentsMargins(Theme::Space4, 0, Theme::Space4, 0);
    row->setSpacing(0);

    auto *line = new QFrame(wrap);
    line->setObjectName(QStringLiteral("settingsRowLine"));
    line->setAttribute(Qt::WA_StyledBackground, true);
    line->setFixedHeight(1);
    line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->addWidget(line);
    return wrap;
}

// One card row: title (and optional muted description) on the left, a control
// on the right. All metrics come from the font, so the row scales with DPI.
QWidget *makeTextRow(QWidget *parent, const QString &title, const QString &body,
                     QWidget *trailing, int padTop, int padBottom)
{
    auto *row = new QWidget(parent);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(Theme::Space4, padTop, Theme::Space4, padBottom);
    h->setSpacing(Theme::Space4);

    auto *texts = new QVBoxLayout;
    texts->setContentsMargins(0, 0, 0, 0);
    texts->setSpacing(Theme::Space1);

    auto *titleLabel = new QLabel(title, row);
    titleLabel->setFont(Theme::chromeFont(row->font()));
    titleLabel->setWordWrap(true);
    texts->addWidget(titleLabel);

    if (!body.isEmpty()) {
        auto *bodyLabel = new QLabel(body, row);
        bodyLabel->setObjectName(QStringLiteral("settingsRowBody"));
        bodyLabel->setFont(Theme::scaledFont(row->font(), 0.95, QFont::Normal));
        bodyLabel->setWordWrap(true);
        texts->addWidget(bodyLabel);
    }

    h->addLayout(texts, 1);
    if (trailing)
        h->addWidget(trailing, 0, Qt::AlignVCenter);
    return row;
}

QLabel *makeSectionHeader(QWidget *parent, const QString &text)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("settingsSectionHeader"));
    label->setFont(Theme::scaledFont(parent->font(), 0.95, QFont::DemiBold));
    return label;
}

QLabel *makeSectionBody(QWidget *parent, const QString &text)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("settingsSectionBody"));
    label->setFont(Theme::scaledFont(parent->font(), 0.95, QFont::Normal));
    label->setWordWrap(true);
    return label;
}

}   // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QScrollArea(parent)
{
    setObjectName(QStringLiteral("settingsPage"));
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::NoFocus);

    // The page background is the same desk the pages rest on: no floating
    // island, the settings tab reads as one more surface of the app.
    const Theme::Palette &pal = Theme::light();
    QPalette pagePal = palette();
    pagePal.setColor(QPalette::Window, pal.desk);
    setPalette(pagePal);
    setAutoFillBackground(true);

    buildUi();
    viewport()->setAutoFillBackground(false);
    viewport()->installEventFilter(this);
    setWidget(m_body);
    centreColumn();
}

void SettingsPage::buildUi()
{
    const Theme::Palette &pal = Theme::light();
    const Theme::Metrics m = Theme::metrics(font());
    m_maxColumn = m.touch * 14;

    m_body = new QWidget;
    m_body->setObjectName(QStringLiteral("settingsBody"));
    m_body->setStyleSheet(pageSheet(font()));

    auto *row = new QHBoxLayout(m_body);
    row->setContentsMargins(Theme::Space5, Theme::Space6, Theme::Space5, Theme::Space6);
    row->setSpacing(0);
    row->addStretch(1);

    m_column = new QWidget(m_body);
    m_column->setMaximumWidth(m_maxColumn);
    row->addWidget(m_column, 0);
    row->addStretch(1);

    auto *col = new QVBoxLayout(m_column);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(Theme::Space2);

    // --- page title ---------------------------------------------------------
    auto *header = new QHBoxLayout;
    header->setSpacing(Theme::Space3);
    const int glyph = qMax(int(Theme::Space6), m.icon + Theme::Space2);
    auto *gear = new QLabel(m_column);
    gear->setPixmap(IconPainter::makeIcon(
                        IconPainter::Glyph::Gear, glyph,
                        IconPainter::States{ pal.accent, pal.accent, pal.textDisabled },
                        devicePixelRatioF())
                        .pixmap(glyph, glyph));
    header->addWidget(gear);

    auto *title = new QLabel(QStringLiteral("设置"), m_column);
    title->setObjectName(QStringLiteral("settingsPageTitle"));
    title->setFont(Theme::scaledFont(font(), 1.55, QFont::DemiBold));
    header->addWidget(title);
    header->addStretch(1);
    col->addLayout(header);

    // --- 通用 ---------------------------------------------------------------
    col->addSpacing(Theme::Space3);
    col->addWidget(makeSectionHeader(m_column, QStringLiteral("通用")));

    const Card general = makeCard(m_column);
    m_autoStart = new ToggleSwitch(general.frame);
    m_autoStart->setToolTip(QStringLiteral("登录 Windows 后自动打开本程序"));
    m_autoStart->setChecked(AppSettings::isAutoStartEnabled());
    connect(m_autoStart, &QAbstractButton::toggled, this, &SettingsPage::onAutoStartToggled);
    general.col->addWidget(makeTextRow(general.frame,
                                       QStringLiteral("开机自动启动"),
                                       QStringLiteral("登录 Windows 后自动打开本程序"),
                                       m_autoStart, Theme::Space3, Theme::Space3));
    col->addWidget(general.frame);

    // --- 保存 ---------------------------------------------------------------
    col->addSpacing(Theme::Space3);
    col->addWidget(makeSectionHeader(m_column, QStringLiteral("保存")));

    col->addWidget(makeSectionBody(m_column, QStringLiteral(
        "批注默认保存到哪个文件夹。选择「跟随源文件」则与源文件保持同目录。")));

    const Card saveCard = makeCard(m_column);
    {
        auto *saveRow = new QWidget(saveCard.frame);
        auto *h = new QHBoxLayout(saveRow);
        h->setContentsMargins(Theme::Space4, Theme::Space3, Theme::Space4, Theme::Space3);
        h->setSpacing(Theme::Space4);

        auto *saveTexts = new QVBoxLayout;
        saveTexts->setContentsMargins(0, 0, 0, 0);
        saveTexts->setSpacing(Theme::Space1);
        auto *saveTitle = new QLabel(QStringLiteral("默认保存位置"), saveRow);
        saveTitle->setFont(Theme::chromeFont(saveRow->font()));
        saveTexts->addWidget(saveTitle);
        m_savePathLabel = new QLabel(saveRow);
        m_savePathLabel->setObjectName(QStringLiteral("settingsRowBody"));
        m_savePathLabel->setFont(Theme::scaledFont(saveRow->font(), 0.95, QFont::Normal));
        m_savePathLabel->setWordWrap(true);
        saveTexts->addWidget(m_savePathLabel);
        h->addLayout(saveTexts, 1);

        auto *saveButtons = new QWidget(saveRow);
        auto *bh = new QHBoxLayout(saveButtons);
        bh->setContentsMargins(0, 0, 0, 0);
        bh->setSpacing(Theme::Space2);
        auto makeBtn = [&](const QString &text) {
            auto *b = new QPushButton(text, saveButtons);
            b->setCursor(Qt::PointingHandCursor);
            b->setFont(Theme::chromeFont(font()));
            b->setMinimumHeight(int(m.touch * 0.72));
            bh->addWidget(b);
            return b;
        };
        m_chooseSaveDir = makeBtn(QStringLiteral("选择文件夹…"));
        m_resetSaveDir  = makeBtn(QStringLiteral("跟随源文件"));
        h->addWidget(saveButtons, 0, Qt::AlignVCenter);

        saveCard.col->addWidget(saveRow);
        col->addWidget(saveCard.frame);
        connect(m_chooseSaveDir, &QPushButton::clicked, this, &SettingsPage::onChooseSaveDir);
        connect(m_resetSaveDir, &QPushButton::clicked, this, &SettingsPage::onResetSaveDir);
        refreshSavePath();
    }

    // --- 文件关联 -----------------------------------------------------------
    col->addSpacing(Theme::Space3);
    col->addWidget(makeSectionHeader(m_column, QStringLiteral("文件关联")));

    col->addWidget(makeSectionBody(m_column, QStringLiteral(
        "把本程序注册为 PDF 的打开方式。Windows 不允许程序直接抢占默认应用，"
        "注册后需要在「默认应用」中选择「大屏 PDF 批注」。")));

    const Card assoc = makeCard(m_column);
    m_register = new QPushButton(QStringLiteral("设为 PDF 默认打开方式"), assoc.frame);
    m_register->setObjectName(QStringLiteral("settingsPrimary"));
    m_register->setCursor(Qt::PointingHandCursor);
    m_register->setFont(Theme::chromeFont(font()));
    m_register->setMinimumHeight(int(m.touch * 0.8));
    m_register->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(m_register, &QPushButton::clicked, this, &SettingsPage::onRegisterPdf);
    assoc.col->addWidget(makeTextRow(assoc.frame,
                                     QStringLiteral("默认打开方式"),
                                     QStringLiteral("注册后可在 Windows「默认应用」中选择本程序"),
                                     m_register, Theme::Space4, Theme::Space4));
    col->addWidget(assoc.frame);

    // --- 调试 ---------------------------------------------------------------
    col->addSpacing(Theme::Space3);
    col->addWidget(makeSectionHeader(m_column, QStringLiteral("调试")));

    col->addWidget(makeSectionBody(m_column, QStringLiteral(
        "开启后会写入诊断日志，用于排查问题。日志只记录运行状态与耗时，"
        "不包含文档内容或批注坐标。默认关闭。")));

    const Card debugCard = makeCard(m_column);
    m_debugLog = new ToggleSwitch(debugCard.frame);
    m_debugLog->setToolTip(QStringLiteral("写入诊断日志（默认关闭）"));
    {
        const QSignalBlocker block(m_debugLog);
        m_debugLog->setChecked(AppSettings::debugLogEnabled() || AppLog::isEnabled());
    }
    debugCard.col->addWidget(makeTextRow(debugCard.frame,
                                         QStringLiteral("写入诊断日志"),
                                         QString(),
                                         m_debugLog, Theme::Space3, Theme::Space3));
    debugCard.col->addWidget(makeRowSeparator(debugCard.frame));
    {
        auto *logRow = new QWidget(debugCard.frame);
        auto *h = new QHBoxLayout(logRow);
        h->setContentsMargins(Theme::Space4, Theme::Space3, Theme::Space4, Theme::Space3);
        h->setSpacing(Theme::Space4);

        auto *logTexts = new QVBoxLayout;
        logTexts->setContentsMargins(0, 0, 0, 0);
        logTexts->setSpacing(Theme::Space1);
        auto *logTitle = new QLabel(QStringLiteral("日志位置"), logRow);
        logTitle->setFont(Theme::chromeFont(logRow->font()));
        logTexts->addWidget(logTitle);
        m_logPathLabel = new QLabel(logRow);
        m_logPathLabel->setObjectName(QStringLiteral("settingsRowBody"));
        m_logPathLabel->setFont(Theme::scaledFont(logRow->font(), 0.95, QFont::Normal));
        m_logPathLabel->setWordWrap(true);
        logTexts->addWidget(m_logPathLabel);

        // Why the switch can start out ON, and why that is not the setting.
        m_logEnvNote = new QLabel(logRow);
        m_logEnvNote->setObjectName(QStringLiteral("settingsRowBody"));
        m_logEnvNote->setFont(Theme::scaledFont(logRow->font(), 0.95, QFont::Normal));
        m_logEnvNote->setWordWrap(true);
        m_logEnvNote->setVisible(false);
        logTexts->addWidget(m_logEnvNote);
        h->addLayout(logTexts, 1);

        m_openLogDir = new QPushButton(QStringLiteral("打开日志文件夹"), logRow);
        m_openLogDir->setCursor(Qt::PointingHandCursor);
        m_openLogDir->setFont(Theme::chromeFont(font()));
        m_openLogDir->setMinimumHeight(int(m.touch * 0.72));
        h->addWidget(m_openLogDir, 0, Qt::AlignVCenter);
        debugCard.col->addWidget(logRow);
    }
    col->addWidget(debugCard.frame);
    connect(m_debugLog, &QAbstractButton::toggled, this, &SettingsPage::onDebugLogToggled);
    connect(m_openLogDir, &QPushButton::clicked, this, &SettingsPage::onOpenLogDir);
    refreshLogPath();

    col->addStretch(1);
}

void SettingsPage::onChooseSaveDir()
{
    QString start = AppSettings::defaultSavePath();
    if (start.isEmpty() || !QFileInfo(start).isDir())
        start = QDir::homePath();

    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择默认保存位置"), start);
    if (dir.isEmpty())
        return;

    QString err;
    if (!AppSettings::setDefaultSavePath(dir, &err))
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
    refreshSavePath();
}

void SettingsPage::onResetSaveDir()
{
    QString err;
    if (!AppSettings::setDefaultSavePath(QString(), &err))
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
    refreshSavePath();
}

void SettingsPage::refreshSavePath()
{
    if (!m_savePathLabel)
        return;

    const QString dir = AppSettings::defaultSavePath();
    m_savePathLabel->setText(dir.isEmpty()
        ? QStringLiteral("跟随源文件所在目录（默认）")
        : QDir::toNativeSeparators(dir));
    if (m_resetSaveDir)
        m_resetSaveDir->setEnabled(!dir.isEmpty());
}

void SettingsPage::onDebugLogToggled(bool on)
{
    QString err;
    if (!AppLog::setEnabled(on, &err))
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);

    // Reflect the real state: the environment override can force it on.
    {
        const QSignalBlocker block(m_debugLog);
        m_debugLog->setChecked(AppLog::isEnabled());
    }
    refreshLogPath();
}

void SettingsPage::onOpenLogDir()
{
    const QString dir = AppLog::logDirectory();
    QDir().mkpath(dir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void SettingsPage::refreshLogPath()
{
    if (!m_logPathLabel)
        return;

    const bool on = AppLog::isEnabled();
    m_logPathLabel->setText(on
        ? QDir::toNativeSeparators(AppLog::logFilePath())
        : QStringLiteral("已关闭（不写入任何日志）"));
    if (m_openLogDir)
        m_openLogDir->setEnabled(on);

    if (m_logEnvNote) {
        const QString env = AppLog::envOverridePath();
        m_logEnvNote->setVisible(!env.isEmpty());
        if (!env.isEmpty())
            m_logEnvNote->setText(QStringLiteral(
                "本次由环境变量 PDFBOARD_LOG 指定路径：%1（只决定启动时默认开启，"
                "上面的开关随时可以关闭）").arg(QDir::toNativeSeparators(env)));
    }
}

bool SettingsPage::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::Resize)
        centreColumn();
    return false;
}

void SettingsPage::resizeEvent(QResizeEvent *event)
{
    QScrollArea::resizeEvent(event);
    centreColumn();
}

void SettingsPage::centreColumn()
{
    if (!m_column)
        return;
    const int avail = qMax(0, viewport()->width() - 2 * Theme::Space5);
    m_column->setMinimumWidth(qMin(avail, m_maxColumn));
}

void SettingsPage::onAutoStartToggled(bool on)
{
    QString err;
    if (AppSettings::setAutoStart(on, &err))
        return;

    QMessageBox::warning(this, QStringLiteral("设置失败"), err);
    // Put the switch back to the registry state that actually exists.
    const QSignalBlocker block(m_autoStart);
    m_autoStart->setChecked(AppSettings::isAutoStartEnabled());
}

void SettingsPage::onRegisterPdf()
{
    QString err;
    if (!AppSettings::registerPdfHandler(&err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("已注册为打开方式"));
    box.setText(QStringLiteral("已把本程序注册为 PDF 与批注包 (.dpz) 的打开方式。"));
    box.setInformativeText(QStringLiteral(
        "Windows 不允许程序直接抢占默认应用，需要你手动确认：\n"
        "接下来打开「默认应用」设置，在 .pdf（以及 .dpz）里选择「大屏 PDF 批注」。\n\n"
        "确认后，双击任意 PDF 或 .dpz 批注包都会用本程序打开。"));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("打开设置"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("稍后"));
    if (box.exec() == QMessageBox::Ok)
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")));
}
