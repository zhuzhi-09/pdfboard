#pragma once

#include "IconPainter.h"
#include "Theme.h"

#include <QColor>
#include <QHash>
#include <QPoint>
#include <QSize>
#include <QVector>
#include <QWidget>

class PdfCanvas;
class QFrame;
class QGraphicsDropShadowEffect;
class QGraphicsOpacityEffect;
class QMouseEvent;
class QPropertyAnimation;
class QToolButton;
class QVariantAnimation;
class PageGrid;
class PenPalette;
class EraserPalette;

// Classroom-whiteboard style floating toolbar that hovers over the page area.
//
// It is a child of PdfCanvas::viewport(), re-centred at the bottom edge on
// every host resize until the user drags it elsewhere (session-only), and
// raised so it always floats above the page. All ink,
// zoom and page commands live here; the pen button opens a colour / width
// palette above the bar. When collapsed, only the chevron button stays visible
// so the toolbar can always be brought back - clicking that chevron toggles
// the collapse directly, there is no menu in between.
//
// Fold / unfold is a DRAWER: the chevron keeps its right edge (the pin) and the
// island's width animates around it, so the content looks like it slides out to
// the right when folding and is revealed from the left when unfolding. The
// content is clipped by the island's own bounds, not squashed - while the
// animation runs the chip sits outside the outer layout at its natural width,
// right edge under the pin, and the body stays visible the whole time. A drag
// re-homes the pin; the default is the CENTRED bar's right edge, never an
// automatic bottom-right dock.
//
// Look: a light rounded chip with a soft shadow, every control showing a hand
// drawn vector icon over its label, and a filled accent pill marking the
// current tool. All geometry comes from Theme::metrics, so it scales with the
// display DPI instead of assuming 96 dpi.
class InkToolbar : public QWidget
{
    Q_OBJECT
public:
    explicit InkToolbar(PdfCanvas *canvas);
    ~InkToolbar() override;

    bool isCollapsed() const { return m_collapsed; }

    // Two looks for the SAME set of buttons (the host switches this from the
    // stored preference; see SettingsPage / MainWindow):
    //   Compact - glyph only, a slim single row (the default)
    //   Text    - glyph over its label (the original look)
    // Only the presentation changes: every button, tooltip, signal and gesture
    // is identical in both, and the labels stay on the buttons for the tooltip
    // and for the self test that locates them by text.
    enum class Style { Compact, Text };

    void  setStyle(Style s);
    Style style() const { return m_style; }

    // Re-centre at the bottom of the host viewport and keep the bar on top.
    void reposition();

    // Moves the island by `delta` from its CURRENT position, clamped to the
    // host. This is the island's own drag path (grabbing its edge / chip
    // padding); the 自由移动 tool is unrelated - it pans the view instead.
    void moveBy(const QPoint &delta);

    // Clamps a drag position so the whole island stays inside the host. A host
    // smaller than the island yields (0, 0) instead of a negative bound.
    static QPoint clampToolbarPos(const QPoint &pos, const QSize &host, const QSize &self);

    // Re-applies the theme-derived pieces (chip sheet, shadow, icons) plus the
    // pen palette and the page-grid overlay this toolbar owns.
    void applyTheme();

    // --- Test hooks: used only by the `--selftest-ink` mode ----------------
    // Sends one synthetic left-button event to `watched` (a child of the
    // island) through QApplication::sendEvent, so the island's event filter and
    // drag handlers run exactly as they do for a real press / move / release.
    QToolButton *testEraserButton() const { return m_eraserButton; }
    // 橡皮大小面板（自测用）：验证"普通一点开 / 再点收起 / 选中后关闭"这条
    // 交互链路，以及选中档确实写回画布。
    QWidget *testEraserPalette() const;
    void testPressOn(QWidget *watched, const QPoint &localPos);
    void testMoveOn(QWidget *watched, const QPoint &localPos);
    void testReleaseOn(QWidget *watched, const QPoint &localPos);
    // 工具岛风格（自测用）：断言"默认简约、纯图标、切到文字又显示标签、两种风格
    // 按钮数一致、简约更矮且按钮不低于触控下限"。
    Style testStyle() const { return m_style; }
    void  testSetStyle(Style s) { setStyle(s); }
    int   testBarHeight() const;                 // sizeHint().height()
    int   testButtonHeight() const { return m_metrics.buttonHeight; }
    bool  testLabelButtonsIconOnly() const;      // all labelled buttons icon-only?
    int   testLabelButtonCount() const { return int(m_labelButtons.size()); }
    // 折叠按钮（铆钉）右缘的屏幕 x：收起时它必须与展开时一致（工具岛向左收起，
    // 按钮不跳）。
    int   testCollapseButtonRightX() const;
    // 抽屉动画（自测用）：当前这段动画的时长（毫秒），以及把动画钉在 [0, 时长]
    // 内的 ms 毫秒处并立即应用该帧 —— 不跑事件循环、不 sleep，采样完全确定。
    int   testDrawerDurationMs() const;
    void  testDrawerAt(int ms);

    // --- 「工具岛闲置淡出」--------------------------------------------------
    // 5 秒内没有任何用户输入 -> 工具岛淡到「透明度 60%」= 不透明度 0.40；
    // 任何输入立刻回到「透明度 0%」= 不透明度 1.0。两段各约 250ms、缓出。
    // 输入探测与 5 秒计时不在这里：MainWindow 装一个 QApplication 级事件过滤器
    // 统一探测，命中后对所有文档的工具岛调用 setIdleFaded()（见 MainWindow）。
    //
    // 为什么用 QGraphicsOpacityEffect + QPropertyAnimation：工具岛是画布的
    // 子控件，setWindowOpacity() 只对顶层窗口有效（对子控件无效）。效果器挂在
    // 岛自身（不是芯片）上，只改绘制合成，不改几何。
    //
    // 可调参数（单一来源，主机与自测都从这里取）。调参只改这三行。
    static constexpr int   kIdleTimeoutMs = 5000;   // 无输入多久后淡出
    static constexpr int   kIdleFadeMs    = 250;    // 淡出 / 恢复的动画时长
    static constexpr qreal kIdleOpacity   = 0.40;   // 透明度 60% = 不透明度 0.40

    // 淡出目标状态。同一状态重复调用是空操作（每次输入都会调它，必须便宜）。
    void setIdleFaded(bool on);
    // 浮层（笔色调板 / 橡皮大小面板 / 页面缩略图）开着时为 true：那时不淡出，
    // 老师正在选颜色 / 翻页。
    bool hasOpenOverlay() const;

    int   testIdleTimeoutMs() const { return kIdleTimeoutMs; }
    int   testIdleFadeDurationMs() const;
    qreal testIdleOpacity() const;
    bool  testIdleEffectEnabled() const;
    bool  testIdleFaded() const { return m_idleFaded; }
    // 把淡出动画钉到 [0, 时长] 内的 ms 毫秒处并立即应用该帧 —— 不跑事件循环、
    // 不 sleep，采样完全确定（与 testDrawerAt 同一套路）。
    void  testIdleAt(int ms);

public slots:
    void setCollapsed(bool on);
    void toggleCollapsed();
    void showPenPalette();
    void showEraserPalette();
    void setFullscreenActive(bool on);

signals:
    void hiddenChanged(bool hidden);
    void settingsRequested();      // 「设置」: the host switches to the settings page
    void saveRequested();          // 「保存」: bundle save (.dpz)
    void saveAsRequested();        // 「另存为」: bundle or inject-PDF export
    void fullscreenRequested();    // 「全屏」: the host toggles window fullscreen

protected:
    void resizeEvent(QResizeEvent *e) override;
    void changeEvent(QEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    void buildUi();
    // Sets every button's presentation for the current m_style: Compact = glyph
    // only (a slim row at the touch floor), Text = glyph over its label. Also
    // re-sizes the page chip and the chevron, which are not in m_labelButtons.
    void applyButtonStyle();
    // One shared drag path: the island's own mouse handlers and the child
    // event filter both funnel through these, so the gesture behaves the same
    // no matter where on the island it was started.
    void beginDrag(const QPoint &posInIsland);
    void dragTo(const QPoint &posInIsland);
    void endDrag();
    void resetPress();             // drops the drag candidate / pressed child
    void refreshIcons();           // (re)build the button glyphs, incl. the pen badge
    void syncFromCanvas();
    // 三层嵌套布局（岛 / 芯片 / 内容体）的 invalidate + activate：任何一条链没
    // 刷新，sizeHint 都会拿旧的宽度（历史 bug：收起后条形仍按展开宽摆放）。
    void refreshLayoutChain();
    // 量"收起态"的自然宽：临时藏起 body 与分隔线问同一条布局链，量完原样还原
    //（同步完成，中间不画帧）。收起动画的终点就是这条宽度。
    int  measureCollapsedWidth();
    // 量"展开态"的自然宽：确保 body/分隔线可见后问布局链。动画途中反向时，
    // 芯片挂在布局外，两条测量都会改用芯片自己的 sizeHint + 投影余量。
    int  measureExpandedWidth();
    // 抽屉动画的一帧：只动宽度与 x（x 由 m_pinRight - width 导出，所以铆钉右缘
    // 一动不动）；y 归 reposition()。
    void applyDrawerWidth(int wide);
    int  drawerWidthAtProgress(qreal progress) const;
    // 动画期间把芯片从外层布局里取出来/放回去：布局会钳制超宽子件，取出来才能
    // 保持自然宽、让左溢部分被岛裁剪（"抽屉"的观感就来自这里）。
    void takeChipForDrawer();
    void returnChipFromDrawer();
    // 抽屉动画收尾（自然结束与自测定位到末帧共用）：落位到目标宽、按状态显隐
    // body/分隔线、刷新宽度缓存。
    void settleDrawer();
    // 闲置淡出：把动画钉到某一帧时显式应用的不透明度，以及收尾（淡出态保持
    // 效果器启用、恢复态关掉效果器让静止态零开销）。
    void applyIdleFrame(qreal progress);
    void settleIdle();
    // 面板定位：锚在 `anchorButton` 上方并钳进页面区域；笔和橡皮共用同一条路径。
    void positionPaletteAbove(QWidget *anchorButton, QWidget *card);
    void hidePalettes();           // 收起两块面板（笔调色板 / 橡皮大小）
    void onPenColorPicked(const QColor &color);
    void onPenWidthPicked(qreal width);
    void onEraserSizePicked(qreal radiusPx);
    void togglePageGrid();         // the "n / N" chip: page-thumbnail picker
    void dismissPageGrid();        // any command hides the picker it covers

    Theme::Metrics m_metrics;
    // The widget's own font BEFORE the island scale was applied. Metrics are
    // derived from it (Theme::metrics(chromeFont(m_baseFont), IslandScale, ...)),
    // so the style switch can recompute them without double-scaling font().
    QFont m_baseFont;

    PdfCanvas   *m_canvas      = nullptr;
    QFrame      *m_chip        = nullptr;
    QGraphicsDropShadowEffect *m_glow = nullptr;
    QWidget     *m_body        = nullptr;
    QFrame      *m_moreSep     = nullptr;
    QToolButton *m_penButton   = nullptr;
    QToolButton *m_eraserButton = nullptr;
    QToolButton *m_moveButton  = nullptr;
    QToolButton *m_undoButton  = nullptr;
    QToolButton *m_redoButton  = nullptr;
    QToolButton *m_clearButton = nullptr;
    QToolButton *m_fitButton    = nullptr;
    QToolButton *m_pageButton  = nullptr;
    QToolButton *m_saveButton  = nullptr;
    QToolButton *m_saveAsButton = nullptr;
    QToolButton *m_settingsButton = nullptr;
    QToolButton *m_fullscreenButton = nullptr;
    QToolButton *m_moreButton  = nullptr;   // the collapse / expand chevron
    PenPalette  *m_palette     = nullptr;
    EraserPalette *m_eraserPalette = nullptr;   // 橡皮按钮上的大小面板
    PageGrid    *m_pageGrid    = nullptr;

    // Glyph of every icon button, so the icons can be rebuilt (DPI change or a
    // new pen colour) without tracking each button separately.
    QHash<QToolButton *, IconPainter::Glyph> m_glyphs;
    QVector<QToolButton *> m_labelButtons;   // the icon(+label) buttons, not the arrows
    QColor m_iconPenColor;         // pen colour baked into the current icons
    qreal  m_iconDpr = 1.0;        // device pixel ratio the icons were drawn for

    bool m_collapsed = false;
    bool m_fullscreenActive = false;
    Style m_style = Style::Compact;   // factory default: the slim glyph-only row

    // The fold / unfold drawer. Only the WIDTH is animated; x follows from
    // m_pinRight - width (so the chevron's right edge cannot move) and y stays
    // owned by reposition(). While it runs, the body stays visible and the chip
    // (out of the layout, see m_drawerChipWidth) protrudes to the left, where
    // the island's own bounds clip it - that reads as "slides out to the right
    // / in from the left".
    QVariantAnimation *m_drawerAnim = nullptr;
    bool m_drawerActive = false;      // owns width + x while the drawer runs
    int  m_drawerFrom = 0;            // this leg's starting width
    int  m_drawerTo = 0;              // this leg's target width
    // While the drawer runs the chip is OUT of the outer layout (a QBoxLayout
    // clamps an over-wide child back inside the island, squashing it - see
    // QWidgetItem::setGeometry) and is placed by hand at its natural width with
    // its right edge under the pin. 0 = chip is owned by the layout.
    int  m_drawerChipWidth = 0;
    // Natural widths of the two rest states. The default pin is derived from
    // the EXPANDED one (the centred bar's right edge), so it must be cached
    // while folded - the live width() is the small one there.
    int  m_expandedWidth = -1;
    int  m_collapsedWidth = -1;

    // Drag state. m_userPos and m_dragged are session-only: the island returns
    // to its default spot on the next launch.
    bool   m_dragging = false;
    bool   m_dragged  = false;
    QPoint m_dragOffset;
    QPoint m_userPos;
    // The pin: the island is anchored by its RIGHT edge (where the collapse
    // chevron sits). BOTH the folded and the expanded bar are placed from this
    // one value, so folding retracts leftwards and unfolding grows back to the
    // very same spot - neither re-centres, so the chevron never jumps. A drag
    // moves the pin (see moveBy) and reposition() then never touches it again;
    // otherwise it is the CENTRED expanded bar's right edge (user: 不要改为怎样
    // 都自动靠右下). -1 = not set yet.
    int    m_pinRight = -1;

    // A press anywhere on the island arms a drag, but it only turns into one
    // after the pointer travels startDragDistance(): a tap must keep clicking
    // the button under it. m_pressPos is in island coordinates.
    bool     m_pressArmed = false;
    QPoint   m_pressPos;
    QWidget *m_pressChild = nullptr;

    // 闲置淡出。效果器挂在岛自身（只改绘制，不改 hit-test）；恢复态时把它
    // 关掉，静止不花钱。m_idleFaded 是目标状态。
    QGraphicsOpacityEffect *m_idleEffect = nullptr;
    QPropertyAnimation     *m_idleAnim   = nullptr;
    bool                    m_idleFaded  = false;
};
