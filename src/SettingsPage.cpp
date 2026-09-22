#include "SettingsPage.h"

#include "AppSettings.h"
#include "AppLog.h"
#include "IconPainter.h"
#include "Theme.h"
#include "WordConvert.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QByteArray>
#include <QCoreApplication>
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
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include <windows.h>

namespace {

// The project's licence, shown verbatim in 关于. Keep in sync with LICENSE.
const char *kMitLicenseText =
    "Copyright (c) 2026 zhuzhi-09\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy "
    "of this software and associated documentation files (the \"Software\"), to deal "
    "in the Software without restriction, including without limitation the rights "
    "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
    "copies of the Software, and to permit persons to whom the Software is "
    "furnished to do so, subject to the following conditions:\n\n"
    "The above copyright notice and this permission notice shall be included in all "
    "copies or substantial portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR "
    "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, "
    "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE "
    "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER "
    "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, "
    "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE "
    "SOFTWARE.";

// 关于 shows the version baked into the executable's own version resource
// (assets/app.rc), so the installer, the file properties and this page can
// never disagree about what is running.
QString appVersion()
{
    const QString path = QCoreApplication::applicationFilePath();
    const auto *wide = reinterpret_cast<const wchar_t *>(path.utf16());

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(wide, &ignored);
    if (size == 0)
        return QStringLiteral("-");

    QByteArray buffer(int(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(wide, 0, size, buffer.data()))
        return QStringLiteral("-");

    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(buffer.constData(), L"\\",
                        reinterpret_cast<LPVOID *>(&info), &length)
        || !info) {
        return QStringLiteral("-");
    }
    return QStringLiteral("%1.%2.%3")
        .arg(HIWORD(info->dwFileVersionMS))
        .arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS));
}

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
               // A disabled button (no backup to restore, no log file to open)
               // keeps its surface but greys out, so the state is visible.
               "QPushButton:disabled { color: %16; }"
               "QPushButton#settingsPrimary {"
               " color: %13;"
               " background: %14;"
               " border: none; }"
               "QPushButton#settingsPrimary:hover { background: %15; }"
               // The 外观 segmented control: the checked segment is the accent
               // pill, so the active mode reads at a glance in both themes.
               "QPushButton#settingsSegment:checked {"
               " color: %13;"
               " background: %14;"
               " border: 1px solid %14; }"
               "QPushButton#settingsSegment:checked:hover {"
               " background: %15;"
               " border: 1px solid %15; }"
               "QPushButton#settingsSegment:checked:pressed {"
               " background: %15;"
               " border: 1px solid %15; }")
        .arg(Theme::rgba(c.text))
        .arg(Theme::rgba(c.textMuted))
        // Cards are a surface, not paper: paper stays white in dark mode too
        // (it is the PDF page), and a white card on a dark page looks broken.
        .arg(Theme::rgba(c.surface))
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
        .arg(Theme::rgba(c.accentHover))
        .arg(Theme::rgba(c.textDisabled));
}

// A rounded card with a hairline border. Rows go into `col`; the caller
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
    m_gear = new QLabel(m_column);
    m_gear->setPixmap(IconPainter::makeIcon(
                          IconPainter::Glyph::Gear, glyph,
                          IconPainter::States{ pal.accent, pal.accent, pal.textDisabled },
                          devicePixelRatioF())
                          .pixmap(glyph, glyph));
    header->addWidget(m_gear);

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

    // --- Word 文档 ----------------------------------------------------------
    col->addSpacing(Theme::Space3);
    col->addWidget(makeSectionHeader(m_column, QStringLiteral("Word 文档")));

    const Card wordOpen = makeCard(m_column);
    {
        auto *segments = new QWidget(wordOpen.frame);
        auto *sh = new QHBoxLayout(segments);
        sh->setContentsMargins(0, 0, 0, 0);
        sh->setSpacing(0);

        m_wordGroup = new QButtonGroup(this);
        m_wordGroup->setExclusive(true);
        const QString labels[3] = { QStringLiteral("每次询问"), QStringLiteral("批注"),
                                    QStringLiteral("用 Word 打开") };
        for (int i = 0; i < 3; ++i) {
            auto *button = new QPushButton(labels[i], segments);
            button->setObjectName(QStringLiteral("settingsSegment"));
            button->setCheckable(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setCursor(Qt::PointingHandCursor);
            button->setFont(Theme::chromeFont(font()));
            button->setMinimumSize(QSize(m.touch * 2, m.touch));
            m_wordGroup->addButton(button, i);
            m_wordSegments[i] = button;
            sh->addWidget(button, 1);
        }
        connect(m_wordGroup, &QButtonGroup::idClicked,
                this, &SettingsPage::onWordModePicked);
        syncWordSegment();

        wordOpen.col->addWidget(makeTextRow(wordOpen.frame,
                                            QStringLiteral("打开 Word 文档时"),
                                            QStringLiteral("每次询问 / 批注 / 用 Word 打开"),
                                            segments, Theme::Space4, Theme::Space4));
        wordOpen.col->addWidget(makeRowSeparator(wordOpen.frame));

        // The nag fix: a toggle that persists AppSettings::wordNagFixEnabled,
        // with the 恢复 button right below it. The button only becomes usable
        // once a backup from the first silencing write exists.
        auto *nagTrailing = new QWidget(wordOpen.frame);
        auto *nv = new QVBoxLayout(nagTrailing);
        nv->setContentsMargins(0, 0, 0, 0);
        nv->setSpacing(Theme::Space2);

        m_wordNagFix = new ToggleSwitch(nagTrailing);
        m_wordNagFix->setToolTip(
            QStringLiteral("转换前关闭 Word 启动时的「不是默认程序」提醒"));
        {
            const QSignalBlocker block(m_wordNagFix);
            m_wordNagFix->setChecked(AppSettings::wordNagFixEnabled());
        }
        nv->addWidget(m_wordNagFix, 0, Qt::AlignRight);

        m_restoreWordNag = new QPushButton(QStringLiteral("恢复 Word 设置"), nagTrailing);
        m_restoreWordNag->setCursor(Qt::PointingHandCursor);
        m_restoreWordNag->setFont(Theme::chromeFont(font()));
        m_restoreWordNag->setMinimumHeight(int(m.touch * 0.72));
        nv->addWidget(m_restoreWordNag, 0, Qt::AlignRight);

        wordOpen.col->addWidget(makeTextRow(
            wordOpen.frame,
            QStringLiteral("关闭 Word 的「不是默认程序」提醒"),
            QStringLiteral("转换 Word 文档需要它：会写入 Word 自己的两个开关值"
                           "（HKCU\\Software\\Microsoft\\Office\\<版本>\\Word\\Options），"
                           "可用右侧按钮恢复。"),
            nagTrailing, Theme::Space3, Theme::Space3));

        connect(m_wordNagFix, &QAbstractButton::toggled,
                this, &SettingsPage::onWordNagFixToggled);
        connect(m_restoreWordNag, &QPushButton::clicked,
                this, &SettingsPage::onRestoreWordNag);
        refreshWordNag();
    }
    col->addWidget(wordOpen.frame);

    // --- 外观 ---------------------------------------------------------------
    col->addSpacing(Theme::Space3);
    col->addWidget(makeSectionHeader(m_column, QStringLiteral("外观")));

    const Card appearance = makeCard(m_column);
    {
        auto *segments = new QWidget(appearance.frame);
        auto *sh = new QHBoxLayout(segments);
        sh->setContentsMargins(0, 0, 0, 0);
        sh->setSpacing(0);

        m_themeGroup = new QButtonGroup(this);
        m_themeGroup->setExclusive(true);
        const QString labels[3] = { QStringLiteral("系统"), QStringLiteral("浅色"),
                                    QStringLiteral("深色") };
        for (int i = 0; i < 3; ++i) {
            auto *button = new QPushButton(labels[i], segments);
            button->setObjectName(QStringLiteral("settingsSegment"));
            button->setCheckable(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setCursor(Qt::PointingHandCursor);
            button->setFont(Theme::chromeFont(font()));
            button->setMinimumSize(QSize(m.touch * 2, m.touch));
            m_themeGroup->addButton(button, i);
            m_themeSegments[i] = button;
            sh->addWidget(button, 1);
        }
        connect(m_themeGroup, &QButtonGroup::idClicked,
                this, &SettingsPage::onThemePicked);
        syncThemeSegment();

        appearance.col->addWidget(makeTextRow(appearance.frame,
                                              QStringLiteral("外观"),
                                              QStringLiteral("跟随系统 / 浅色 / 深色"),
                                              segments, Theme::Space4, Theme::Space4));
        appearance.col->addWidget(makeRowSeparator(appearance.frame));

        // The hint that explains what 系统 currently resolves to.
        auto *noteWrap = new QWidget(appearance.frame);
        auto *nh = new QHBoxLayout(noteWrap);
        nh->setContentsMargins(Theme::Space4, 0, Theme::Space4, Theme::Space3);
        nh->setSpacing(0);
        m_themeNote = new QLabel(noteWrap);
        m_themeNote->setObjectName(QStringLiteral("settingsRowBody"));
        m_themeNote->setFont(Theme::scaledFont(appearance.frame->font(), 0.95,
                                               QFont::Normal));
        m_themeNote->setWordWrap(true);
        nh->addWidget(m_themeNote, 1);
        appearance.col->addWidget(noteWrap);
    }
    col->addWidget(appearance.frame);

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
    refreshThemeNote();

    // --- 关于 ---------------------------------------------------------------
    col->addSpacing(Theme::Space3);
    col->addWidget(makeSectionHeader(m_column, QStringLiteral("关于")));

    const Card about = makeCard(m_column);
    {
        auto *titleRow = new QWidget(about.frame);
        auto *titleCol = new QVBoxLayout(titleRow);
        titleCol->setContentsMargins(Theme::Space4, Theme::Space3, Theme::Space4, Theme::Space3);
        titleCol->setSpacing(Theme::Space1);

        auto *appLabel = new QLabel(QStringLiteral("大屏 PDF 批注"), titleRow);
        appLabel->setFont(Theme::chromeFont(titleRow->font()));
        titleCol->addWidget(appLabel);

        auto *versionLabel = new QLabel(titleRow);
        versionLabel->setObjectName(QStringLiteral("settingsRowBody"));
        versionLabel->setFont(Theme::scaledFont(titleRow->font(), 0.95, QFont::Normal));
        versionLabel->setWordWrap(true);
        versionLabel->setText(QStringLiteral("版本 %1 · 面向教室大屏一体机的 PDF 查看与手写批注"
                                             "（原生 C++/Qt6，触控与低内存优化）")
                                  .arg(appVersion()));
        titleCol->addWidget(versionLabel);
        about.col->addWidget(titleRow);
    }
    about.col->addWidget(makeRowSeparator(about.frame));
    {
        auto *licenseRow = new QWidget(about.frame);
        auto *licenseCol = new QVBoxLayout(licenseRow);
        licenseCol->setContentsMargins(Theme::Space4, Theme::Space3, Theme::Space4, Theme::Space3);
        licenseCol->setSpacing(Theme::Space1);

        auto *licenseTitle = new QLabel(QStringLiteral("许可证 · MIT License"), licenseRow);
        licenseTitle->setFont(Theme::chromeFont(licenseRow->font()));
        licenseCol->addWidget(licenseTitle);

        auto *licenseText = new QLabel(QString::fromLatin1(kMitLicenseText), licenseRow);
        licenseText->setObjectName(QStringLiteral("settingsRowBody"));
        licenseText->setFont(Theme::scaledFont(licenseRow->font(), 0.92, QFont::Normal));
        licenseText->setWordWrap(true);
        licenseText->setTextInteractionFlags(Qt::TextSelectableByMouse);
        licenseCol->addWidget(licenseText);

        auto *thirdParty = new QLabel(QStringLiteral(
            "第三方组件：Qt 6（LGPL-3.0，动态链接使用）· PDFium（BSD-3-Clause，随 Qt PDF 模块）"),
            licenseRow);
        thirdParty->setObjectName(QStringLiteral("settingsRowBody"));
        thirdParty->setFont(Theme::scaledFont(licenseRow->font(), 0.92, QFont::Normal));
        thirdParty->setWordWrap(true);
        licenseCol->addWidget(thirdParty);
        about.col->addWidget(licenseRow);
    }
    about.col->addWidget(makeRowSeparator(about.frame));
    {
        auto *linksRow = new QWidget(about.frame);
        auto *linksCol = new QHBoxLayout(linksRow);
        linksCol->setContentsMargins(Theme::Space4, Theme::Space3, Theme::Space4, Theme::Space3);

        auto *repoButton = new QPushButton(QStringLiteral("项目主页"), linksRow);
        repoButton->setCursor(Qt::PointingHandCursor);
        repoButton->setFont(Theme::chromeFont(font()));
        repoButton->setMinimumHeight(int(m.touch * 0.72));
        connect(repoButton, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://github.com/zhuzhi-09/pdfboard")));
        });
        linksCol->addWidget(repoButton, 0, Qt::AlignLeft);
        linksCol->addStretch(1);
        about.col->addWidget(linksRow);
    }
    col->addWidget(about.frame);

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

// The page background, the body sheet and the gear pixmap all bake theme
// colours, so a runtime theme switch re-applies every one of them.
void SettingsPage::refreshTheme()
{
    const Theme::Palette &pal = Theme::light();

    QPalette pagePal = palette();
    pagePal.setColor(QPalette::Window, pal.desk);
    setPalette(pagePal);

    if (m_body)
        m_body->setStyleSheet(pageSheet(font()));

    if (m_gear) {
        const Theme::Metrics m = Theme::metrics(font());
        const int glyph = qMax(int(Theme::Space6), m.icon + Theme::Space2);
        m_gear->setPixmap(IconPainter::makeIcon(
                              IconPainter::Glyph::Gear, glyph,
                              IconPainter::States{ pal.accent, pal.accent,
                                                   pal.textDisabled },
                              devicePixelRatioF())
                              .pixmap(glyph, glyph));
    }

    for (QPushButton *segment : m_themeSegments) {
        if (!segment)
            continue;
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    }
    for (QPushButton *segment : m_wordSegments) {
        if (!segment)
            continue;
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
    }

    syncThemeSegment();
    syncWordSegment();
    refreshWordNag();
    refreshThemeNote();
    update();
}

void SettingsPage::refreshThemeNote()
{
    if (!m_themeNote)
        return;

    const int stored = AppSettings::themeMode();
    if (stored == 1) {
        m_themeNote->setText(QStringLiteral("已固定为浅色"));
    } else if (stored == 2) {
        m_themeNote->setText(QStringLiteral("已固定为深色"));
    } else {
        m_themeNote->setText(Theme::systemMode() == Theme::Mode::Dark
            ? QStringLiteral("当前跟随系统：深色")
            : QStringLiteral("当前跟随系统：浅色"));
    }
}

void SettingsPage::syncThemeSegment()
{
    if (!m_themeGroup)
        return;
    QAbstractButton *button = m_themeGroup->button(AppSettings::themeMode());
    if (!button || button->isChecked())
        return;
    const QSignalBlocker block(m_themeGroup);
    button->setChecked(true);
}

void SettingsPage::onThemePicked(int mode)
{
    QString err;
    if (!AppSettings::setThemeMode(mode, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncThemeSegment();      // show the value that is really stored
        return;
    }

    syncThemeSegment();
    emit themeChanged();         // MainWindow re-applies the theme everywhere
    refreshThemeNote();
}

void SettingsPage::syncWordSegment()
{
    if (!m_wordGroup)
        return;
    QAbstractButton *button = m_wordGroup->button(AppSettings::wordOpenMode());
    if (!button || button->isChecked())
        return;
    const QSignalBlocker block(m_wordGroup);
    button->setChecked(true);
}

void SettingsPage::onWordModePicked(int mode)
{
    QString err;
    if (!AppSettings::setWordOpenMode(mode, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        syncWordSegment();      // show the value that is really stored
        return;
    }
    syncWordSegment();
}

void SettingsPage::onWordNagFixToggled(bool on)
{
    QString err;
    if (!AppSettings::setWordNagFixEnabled(on, &err)) {
        QMessageBox::warning(this, QStringLiteral("设置失败"), err);
        const QSignalBlocker block(m_wordNagFix);
        m_wordNagFix->setChecked(AppSettings::wordNagFixEnabled());
        return;
    }
    refreshWordNag();
}

// Restoring only ever acts on OUR backup: WordConvert writes the recorded
// values back itself and refuses when there is none. The dialog is modal on
// purpose - the one action that touches Word's own settings deserves a
// confirmation.
void SettingsPage::onRestoreWordNag()
{
    if (!WordConvert::wordNagBackupExists()) {
        refreshWordNag();
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, QStringLiteral("恢复 Word 设置"),
        QStringLiteral("把 Word 的两个「不是默认程序」开关值恢复到本程序第一次写入"
                       "之前的状态：\n改动过的写回原值，原本不存在的删除。\n\n是否继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    QString err;
    if (!WordConvert::restoreWordNag(&err)) {
        QMessageBox::warning(this, QStringLiteral("恢复失败"), err);
        refreshWordNag();
        return;
    }
    refreshWordNag();
    emit statusMessage(QStringLiteral("已恢复 Word 的「不是默认程序」开关"));
}

void SettingsPage::refreshWordNag()
{
    if (!m_restoreWordNag)
        return;

    const bool canRestore = WordConvert::wordNagBackupExists();
    m_restoreWordNag->setEnabled(canRestore);
    if (!canRestore) {
        m_restoreWordNag->setToolTip(
            QStringLiteral("当前没有可恢复的备份；进行一次 Word 转换后可用"));
    } else if (WordConvert::wordNagSilenced()) {
        m_restoreWordNag->setToolTip(
            QStringLiteral("当前 Word 的提醒已由本程序关闭，点击恢复原值"));
    } else {
        m_restoreWordNag->setToolTip(
            QStringLiteral("把 Word 的两个开关值恢复到本程序第一次写入之前的状态"));
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
    box.setText(QStringLiteral(
        "已把本程序注册为 PDF、批注包 (.dpz) 以及 Word 文档 (.docx/.doc) 的打开方式。"));
    box.setInformativeText(QStringLiteral(
        "Windows 不允许程序直接抢占默认应用，需要你手动确认：\n"
        "接下来打开「默认应用」设置，在 .pdf（以及 .dpz）里选择「大屏 PDF 批注」。\n\n"
        "Word 文档只注册为「打开方式」候选，默认仍由 Word 打开；\n"
        "需要时可右键 .docx / .doc →「打开方式」选择本程序。\n\n"
        "确认后，双击任意 PDF 或 .dpz 批注包都会用本程序打开。"));
    box.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("打开设置"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("稍后"));
    if (box.exec() == QMessageBox::Ok)
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps")));
}
