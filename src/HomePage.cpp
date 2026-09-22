#include "HomePage.h"

#include "AppSettings.h"
#include "Theme.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <iterator>

namespace {

// 一言 endpoint: poems only (c=i). The literature / philosophy genres have
// returned modern web-fiction sources that are not classroom material.
const char *const kQuoteUrl =
    "https://v1.hitokoto.cn/?c=i&encode=json&charset=utf-8";

constexpr qint64 kQuoteThrottleMs = 10 * 60 * 1000;   // at most one per 10 min
constexpr int    kQuoteTimeoutMs  = 4000;
constexpr int    kQuoteMaxChars   = 60;

// Curated offline fallback: classical lines a classroom can always show.
struct FallbackQuote {
    QString text;
    QString source;
};

const FallbackQuote &randomFallback()
{
    static const FallbackQuote kFallbacks[] = {
        { QStringLiteral("书山有路勤为径，学海无涯苦作舟"), QStringLiteral("增广贤文") },
        { QStringLiteral("少壮不努力，老大徒伤悲"),         QStringLiteral("长歌行") },
        { QStringLiteral("千里之行，始于足下"),             QStringLiteral("道德经") },
        { QStringLiteral("学而不思则罔，思而不学则殆"),     QStringLiteral("论语") },
        { QStringLiteral("宝剑锋从磨砺出，梅花香自苦寒来"), QStringLiteral("警世贤文") },
        { QStringLiteral("莫等闲，白了少年头，空悲切"),     QStringLiteral("满江红") },
        { QStringLiteral("会当凌绝顶，一览众山小"),         QStringLiteral("望岳") },
        { QStringLiteral("天生我材必有用，千金散尽还复来"), QStringLiteral("将进酒") },
        { QStringLiteral("长风破浪会有时，直挂云帆济沧海"), QStringLiteral("行路难") },
        { QStringLiteral("问渠那得清如许？为有源头活水来"), QStringLiteral("观书有感") },
    };
    const int count = int(std::size(kFallbacks));
    return kFallbacks[QRandomGenerator::global()->bounded(count)];
}

// `text —— 《source》`, the book title brackets omitted when the source
// already carries them.
QString formatQuote(const QString &text, const QString &source)
{
    if (source.isEmpty())
        return text;
    if (source.contains(QStringLiteral("《")))
        return QStringLiteral("%1 —— %2").arg(text, source);
    return QStringLiteral("%1 —— 《%2》").arg(text, source);
}

// One clickable 最近项目 row: file name over its folder path, a warning note
// and a 移除 button when the file is gone. The row paints its own hover /
// missing state through the page style sheet (see homeSheet).
class RecentRow : public QFrame
{
public:
    RecentRow(const QString &path, bool missing, QWidget *parent)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("homeRecentRow"));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);

        const QFileInfo info(path);
        auto *h = new QHBoxLayout(this);
        h->setContentsMargins(Theme::Space4, Theme::Space2, Theme::Space3, Theme::Space2);
        h->setSpacing(Theme::Space3);

        auto *texts = new QVBoxLayout;
        texts->setContentsMargins(0, 0, 0, 0);
        texts->setSpacing(Theme::Space1);

        auto *name = new QLabel(info.fileName(), this);
        name->setObjectName(missing ? QStringLiteral("homeRecentMissing")
                                    : QStringLiteral("homeRecentName"));
        name->setFont(Theme::chromeFont(font()));
        texts->addWidget(name);

        auto *dir = new QLabel(QDir::toNativeSeparators(info.absolutePath()), this);
        dir->setObjectName(missing ? QStringLiteral("homeRecentMissingDir")
                                   : QStringLiteral("homeRecentDir"));
        dir->setFont(Theme::scaledFont(font(), 0.90, QFont::Normal));
        dir->setWordWrap(true);
        texts->addWidget(dir);
        h->addLayout(texts, 1);

        if (missing) {
            auto *note = new QLabel(QStringLiteral("文件不存在"), this);
            note->setObjectName(QStringLiteral("homeRecentNote"));
            note->setFont(Theme::scaledFont(font(), 0.95, QFont::Medium));
            h->addWidget(note, 0, Qt::AlignVCenter);
        }

        auto *remove = new QPushButton(QStringLiteral("移除"), this);
        remove->setObjectName(QStringLiteral("homeRecentRemove"));
        remove->setCursor(Qt::PointingHandCursor);
        remove->setFocusPolicy(Qt::NoFocus);
        remove->setFont(Theme::scaledFont(font(), 0.95, QFont::Medium));
        remove->setMinimumHeight(int(Theme::metrics(font()).touch * 0.6));
        // The button swallows its own clicks, so the row below never fires.
        connect(remove, &QPushButton::clicked, this, [this] {
            if (onRemove)
                onRemove();
        });
        h->addWidget(remove, 0, Qt::AlignVCenter);
    }

    std::function<void()> onActivate;
    std::function<void()> onRemove;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        m_pressed = (e->button() == Qt::LeftButton);
        QFrame::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        const bool activate = m_pressed && e->button() == Qt::LeftButton
                              && rect().contains(e->position().toPoint());
        m_pressed = false;
        QFrame::mouseReleaseEvent(e);
        if (activate && onActivate)
            onActivate();
    }

private:
    bool m_pressed = false;
};

// The page body sheet. Applied to the scrolling BODY, never to the page root,
// so message boxes spawned from the page keep their native look.
QString homeSheet(const QFont &font)
{
    const Theme::Palette &c = Theme::light();
    const Theme::Metrics m = Theme::metrics(font);

    return QStringLiteral(
               "QWidget#homeBody { background: transparent; }"
               "QLabel { color: %1; background: transparent; }"
               "QLabel#homeQuote { color: %2; }"
               "QLabel#homeSectionHeader { color: %2; }"
               "QLabel#homeRecentDir { color: %2; }"
               "QLabel#homeRecentEmpty { color: %2; }"
               "QLabel#homeRecentNote { color: %3; }"
               "QLabel#homeRecentMissing { color: %4; }"
               "QLabel#homeRecentMissingDir { color: %4; }"
               "QFrame#homeCard {"
               " background: %5;"
               " border: 1px solid %6;"
               " border-radius: %7; }"
               "QFrame#homeRowLine { background: %8; border: none; }"
               "QFrame#homeRecentRow { background: transparent; border: none; }"
               "QFrame#homeRecentRow:hover { background: %9; }"
               "QPushButton {"
               " color: %1;"
               " background: %10;"
               " border: 1px solid %6;"
               " border-radius: %11;"
               " padding: %12 %13; }"
               "QPushButton:hover { background: %9; }"
               "QPushButton:pressed { background: %14; }"
               "QPushButton#homeRecentRemove { color: %2; }"
               "QPushButton#homePrimary {"
               " color: %15;"
               " background: %16;"
               " border: none; }"
               "QPushButton#homePrimary:hover { background: %17; }"
               "QPushButton#homePrimary:pressed { background: %17; }")
        .arg(Theme::rgba(c.text))             // 1
        .arg(Theme::rgba(c.textMuted))        // 2
        .arg(Theme::rgba(c.accent))           // 3
        .arg(Theme::rgba(c.textDisabled))     // 4
        .arg(Theme::rgba(c.surface))          // 5
        .arg(Theme::rgba(c.surfaceEdge))      // 6
        .arg(Theme::px(qMax(Theme::Space2 + 2, m.radiusButton - Theme::Space1)))  // 7
        .arg(Theme::rgba(c.divider))          // 8
        .arg(Theme::rgba(c.surfaceHover))     // 9
        .arg(Theme::rgba(c.chipTint))         // 10
        .arg(Theme::px(m.radiusButton))       // 11
        .arg(Theme::px(m.padY))               // 12
        .arg(Theme::px(m.padX + Theme::Space1))   // 13
        .arg(Theme::rgba(c.surfacePressed))   // 14
        .arg(Theme::rgba(c.onAccent))         // 15
        .arg(Theme::rgba(c.accent))           // 16
        .arg(Theme::rgba(c.accentHover));     // 17
}

// A rounded card with a hairline border (the same building block SettingsPage
// uses), plus the inset hairline separator between rows.
struct Card {
    QFrame      *frame = nullptr;
    QVBoxLayout *col   = nullptr;
};

Card makeCard(QWidget *parent)
{
    Card card;
    card.frame = new QFrame(parent);
    card.frame->setObjectName(QStringLiteral("homeCard"));
    card.frame->setAttribute(Qt::WA_StyledBackground, true);

    card.col = new QVBoxLayout(card.frame);
    card.col->setContentsMargins(0, 0, 0, 0);
    card.col->setSpacing(0);
    return card;
}

QWidget *makeRowSeparator(QWidget *parent)
{
    auto *wrap = new QWidget(parent);
    auto *row = new QHBoxLayout(wrap);
    row->setContentsMargins(Theme::Space4, 0, Theme::Space4, 0);
    row->setSpacing(0);

    auto *line = new QFrame(wrap);
    line->setObjectName(QStringLiteral("homeRowLine"));
    line->setAttribute(Qt::WA_StyledBackground, true);
    line->setFixedHeight(1);
    line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->addWidget(line);
    return wrap;
}

}   // namespace

HomePage::HomePage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("homePage"));

    // The page background is the same desk the document pages rest on, so the
    // home page reads as one more surface of the app (see SettingsPage).
    const Theme::Palette &pal = Theme::light();
    QPalette pagePal = palette();
    pagePal.setColor(QPalette::Window, pal.desk);
    setPalette(pagePal);
    setAutoFillBackground(true);

    buildUi();

    m_net = new QNetworkAccessManager(this);
    setQuote(fallbackQuote());       // never empty, even with no network

    // The greeting has to age with the clock: a home page left open overnight
    // would otherwise still wish a good night in the morning. Only the greeting
    // is re-rendered here - the quote keeps its own request throttle.
    auto *clock = new QTimer(this);
    clock->setInterval(60 * 1000);
    connect(clock, &QTimer::timeout, this, [this] {
        if (m_greeting)
            m_greeting->setText(greetingForHour(QTime::currentTime().hour(), userName()));
    });
    clock->start();
    refreshGreeting();
    rebuildRecentList();
}

void HomePage::buildUi()
{
    const Theme::Metrics m = Theme::metrics(font());
    m_maxColumn = m.touch * 14;

    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("homeBody"));
    m_body->setStyleSheet(homeSheet(font()));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(m_body);

    auto *row = new QHBoxLayout(m_body);
    row->setContentsMargins(Theme::Space5, Theme::Space6, Theme::Space5, Theme::Space6);
    row->setSpacing(0);
    row->addStretch(1);

    m_column = new QWidget(m_body);
    row->addWidget(m_column, 0);
    row->addStretch(1);
    centreColumn();

    auto *col = new QVBoxLayout(m_column);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(Theme::Space2);

    // The content block sits in the vertical middle of the page: a stretch
    // above (here) balances the one below the 最近项目 card.
    col->addStretch(1);

    // --- greeting -----------------------------------------------------------
    m_greeting = new QLabel(m_column);
    m_greeting->setAlignment(Qt::AlignHCenter);
    // 1.55 * 1.6: the greeting is the page's anchor, so it carries the page.
    m_greeting->setFont(Theme::scaledFont(font(), 2.48, QFont::DemiBold));
    col->addWidget(m_greeting);
    col->addSpacing(Theme::Space2);          // greeting -> quote: a little air

    // --- 一言 ---------------------------------------------------------------
    m_quote = new QLabel(m_column);
    m_quote->setObjectName(QStringLiteral("homeQuote"));
    // A step up from the body size: readable from the back of a classroom.
    m_quote->setFont(Theme::scaledFont(font(), 1.35, QFont::Normal));
    m_quote->setWordWrap(true);
    col->addWidget(m_quote);

    col->addSpacing(Theme::Space6);          // quote -> button: the block breathes

    // --- 打开 ---------------------------------------------------------------
    m_openButton = new QPushButton(QStringLiteral("打开"), m_column);
    m_openButton->setObjectName(QStringLiteral("homePrimary"));
    m_openButton->setCursor(Qt::PointingHandCursor);
    m_openButton->setFont(Theme::scaledFont(font(), 1.25, QFont::DemiBold));
    m_openButton->setMinimumHeight(int(m.touch * 1.15));
    // Narrower than the column and centred: a full-width bar reads as a banner.
    m_openButton->setFixedWidth(qMax(int(m.touch * 3), int(m_maxColumn * 0.55)));
    connect(m_openButton, &QPushButton::clicked, this, &HomePage::openRequested);
    col->addWidget(m_openButton, 0, Qt::AlignHCenter);

    col->addSpacing(Theme::Space4);

    // --- 最近项目 -----------------------------------------------------------
    auto *recentHeader = new QHBoxLayout;
    recentHeader->setSpacing(Theme::Space3);
    auto *recentTitle = new QLabel(QStringLiteral("最近项目"), m_column);
    recentTitle->setObjectName(QStringLiteral("homeSectionHeader"));
    recentTitle->setFont(Theme::scaledFont(font(), 0.95, QFont::DemiBold));
    recentHeader->addWidget(recentTitle);
    recentHeader->addStretch(1);

    m_clearButton = new QPushButton(QStringLiteral("清空列表"), m_column);
    m_clearButton->setCursor(Qt::PointingHandCursor);
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setFont(Theme::scaledFont(font(), 0.95, QFont::Medium));
    m_clearButton->setMinimumHeight(int(m.touch * 0.6));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (AppSettings::recentFiles().isEmpty())
            return;
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, QStringLiteral("清空最近项目"),
            QStringLiteral("确定要清空最近项目列表吗？（不会删除任何文件）"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        QString err;
        if (!AppSettings::clearRecentFiles(&err)) {
            QMessageBox::warning(this, QStringLiteral("操作失败"), err);
            return;
        }
        rebuildRecentList();
    });
    recentHeader->addWidget(m_clearButton, 0, Qt::AlignVCenter);
    col->addLayout(recentHeader);

    const Card card = makeCard(m_column);
    m_recentCol = card.col;
    col->addWidget(card.frame);

    col->addStretch(1);
}

QString HomePage::greetingForHour(int hour, const QString &userName)
{
    if (hour < 5)
        return QStringLiteral("夜深了，%1，该睡了").arg(userName);
    if (hour < 11)
        return QStringLiteral("早上好，%1，今天看些什么？").arg(userName);
    if (hour <= 12)
        return QStringLiteral("中午好，%1，今天看些什么？").arg(userName);
    if (hour <= 17)
        return QStringLiteral("下午好，%1，今天看些什么？").arg(userName);
    return QStringLiteral("晚上好，%1，今天看些什么？").arg(userName);
}

QString HomePage::userName()
{
    const QString name = qEnvironmentVariable("USERNAME").trimmed();
    return name.isEmpty() ? QStringLiteral("老师") : name;
}

bool HomePage::isQuoteAcceptable(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kQuoteMaxChars)
        return false;
    if (trimmed.contains(QStringLiteral("http"), Qt::CaseInsensitive))
        return false;

    // Deliberately tiny and literal: single characters like 死 occur in
    // perfectly appropriate classical poetry and must NOT be filtered.
    static const QString kBlocked[] = {
        QStringLiteral("色情"), QStringLiteral("裸"), QStringLiteral("赌博"),
        QStringLiteral("毒品"), QStringLiteral("自杀"), QStringLiteral("他妈"),
        QStringLiteral("操你"),
    };
    for (const QString &word : kBlocked) {
        if (trimmed.contains(word))
            return false;
    }
    return true;
}

QString HomePage::fallbackQuote()
{
    const FallbackQuote &pick = randomFallback();
    return formatQuote(pick.text, pick.source);
}

void HomePage::refresh()
{
    refreshGreeting();
    rebuildRecentList();
    fetchQuoteIfDue();
}

void HomePage::refreshGreeting()
{
    if (!m_greeting)
        return;
    m_greeting->setText(greetingForHour(QTime::currentTime().hour(), userName()));
}

void HomePage::rebuildRecentList()
{
    if (!m_recentCol)
        return;

    // Deferred calls (see removeRecent) run on the next event-loop turn, so a
    // rebuild can never delete the button whose signal is currently running.
    while (QLayoutItem *item = m_recentCol->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QStringList recent = AppSettings::recentFiles();
    if (m_clearButton)
        m_clearButton->setVisible(!recent.isEmpty());

    QWidget *card = m_recentCol->parentWidget();
    if (recent.isEmpty()) {
        auto *empty = new QLabel(
            QStringLiteral("暂无最近项目。打开一份 PDF 或批注包后会出现在这里。"), card);
        empty->setObjectName(QStringLiteral("homeRecentEmpty"));
        empty->setFont(Theme::scaledFont(font(), 0.95, QFont::Normal));
        empty->setWordWrap(true);
        empty->setContentsMargins(Theme::Space4, Theme::Space3, Theme::Space4, Theme::Space3);
        m_recentCol->addWidget(empty);
        return;
    }

    for (int i = 0; i < recent.size(); ++i) {
        const QString &path = recent.at(i);
        if (i > 0)
            m_recentCol->addWidget(makeRowSeparator(card));

        auto *recentRow = new RecentRow(path, !QFileInfo::exists(path), card);
        recentRow->onActivate = [this, path] { activateRecent(path); };
        recentRow->onRemove = [this, path] { removeRecent(path); };
        m_recentCol->addWidget(recentRow);
    }
}

void HomePage::activateRecent(const QString &path)
{
    if (QFileInfo::exists(path)) {
        emit openPathRequested(path);
        return;
    }

    // The file is gone (moved, renamed, or its drive was unplugged): say so
    // and offer to drop it from the list instead of failing silently.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("文件不存在"));
    box.setText(QStringLiteral("找不到文件：\n%1").arg(QDir::toNativeSeparators(path)));
    box.setInformativeText(QStringLiteral(
        "它可能已被移动、重命名，或所在磁盘 / U 盘已拔出。\n是否从最近项目中移除？"));
    QPushButton *removeButton =
        box.addButton(QStringLiteral("移除"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("保留"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() == removeButton)
        removeRecent(path);
}

void HomePage::removeRecent(const QString &path)
{
    QString err;
    if (!AppSettings::removeRecentFile(path, &err)) {
        QMessageBox::warning(this, QStringLiteral("操作失败"), err);
        return;
    }

    // The click came from inside a row that this rebuild will delete, so do it
    // on the next event-loop turn instead of inside the running signal handler.
    QTimer::singleShot(0, this, [this] { rebuildRecentList(); });
}

void HomePage::setQuote(const QString &line)
{
    if (m_quote && !line.isEmpty())
        m_quote->setText(line);
}

void HomePage::fetchQuoteIfDue()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // One request per window, whatever the previous outcome was: a machine
    // with no network must not be hammered on every visit.
    if (m_lastQuoteAttempt.isValid()
        && m_lastQuoteAttempt.msecsTo(now) < kQuoteThrottleMs) {
        return;
    }
    m_lastQuoteAttempt = now;

    QNetworkRequest request{QUrl(QString::fromUtf8(kQuoteUrl))};
    request.setTransferTimeout(kQuoteTimeoutMs);

    QNetworkReply *reply = m_net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;                       // keep the fallback line: silent

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        const QString text = obj.value(QStringLiteral("hitokoto")).toString().trimmed();
        if (!isQuoteAcceptable(text))
            return;                       // keep the fallback line: silent

        setQuote(formatQuote(text,
                             obj.value(QStringLiteral("from")).toString().trimmed()));
    });
}

// The content column keeps a stable width and stays centred: mirror of
// SettingsPage::centreColumn(), but on the page widget itself (HomePage is not
// a QScrollArea, so there is no viewport to measure).
void HomePage::centreColumn()
{
    if (!m_column)
        return;
    const int avail = qMax(Theme::Space6 * 6, width() - 2 * Theme::Space5);
    m_column->setFixedWidth(qMin(m_maxColumn, avail));
}

void HomePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    centreColumn();
}

void HomePage::applyTheme()
{
    const Theme::Palette &pal = Theme::light();

    QPalette pagePal = palette();
    pagePal.setColor(QPalette::Window, pal.desk);
    setPalette(pagePal);

    if (m_body)
        m_body->setStyleSheet(homeSheet(font()));

    update();
}
