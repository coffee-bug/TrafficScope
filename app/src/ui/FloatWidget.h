#pragma once
#include <QPoint>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>
#include "core/Settings.h"
#include "core/TrafficStore.h"

class QMenu;
class QPropertyAnimation;
class QSystemTrayIcon;
class DetailDialog;
class SettingsDialog;
class TopPanel;

// 无边框置顶悬浮窗：实时 ↑↓ 速率，**单击**展开/收起进程榜（不再是悬停展开，2026-10-11 改），
// 可拖动，右键菜单只有「设置… / 退出」（网卡选择、重置统计、置顶都在设置弹窗里）。
// 面板关闭时机：再次单击浮框 / 单击浮框与面板以外的地方 / 拖动浮框。
// 拖动时自动收起进程榜，松手后光标仍在浮框上则恢复。
// 位置记忆：默认停在屏幕右下角，拖动后写入 QSettings，下次启动回到最后位置。
// 全屏自动隐藏：检测到有程序全屏（游戏 / 放映）就去掉置顶并隐藏，退出全屏后
//               按用户原本的置顶意愿恢复（开关在设置弹窗「显示」区，默认开）。
// 托盘图标：右键菜单与浮框右键**同一份**（设置 / 退出），左键单击打开设置；
//           图标按当前主题色现画（没有 .ico 资源），主题切换时重画。
// 边缘吸附：拖到屏幕边缘松手会贴边并只露出一条窄条；窄条上下分成两段——
//           上段=上传（橙）、下段=下载（绿），各占一半，段长按固定档位
//           （0/100KB/s、1MB/s、10MB/s）取条长，不再按占比互相挤压；
//           鼠标移入滑出完整卡片，移开滑回。
class FloatWidget : public QWidget {
    Q_OBJECT
public:
    explicit FloatWidget(QWidget* parent = nullptr);
    ~FloatWidget() override;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;
    bool event(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent*) override;
#else
    void enterEvent(QEvent*) override;
#endif

private slots:
    void onTick();
    void hidePanel();
    void checkFullscreen();            // 定时探测是否有程序处于全屏

private:
    void showPanel();
    void positionPanel();
    void animateHover(bool in);
    void restoreEdgePeek();            // 吸附态：滑回贴边只留占比条（鼠标已离开浮框时）
    void checkPanelFocus();            // 焦点离开浮框/面板（点到别的程序）时关掉面板
    void showDetail();
    void showSettings();               // 懒创建设置弹窗并同步置顶/刷新频率
    void fillMenu(QMenu& menu);        // 浮框右键与托盘右键共用同一份菜单项
    void setupTray();                  // 托盘图标（系统不支持时静默跳过）
    QIcon trayIcon() const;            // 与窗口图标同源，见 ui/AppIcon.h
    void setTopMost(bool on);          // 由设置弹窗的开关驱动
    void setHideOnFullscreen(bool on); // 同上：全屏时自动隐藏
    void setAutoEdgeHide(bool on);     // 同上：拖到边缘自动贴边隐藏
    // 置顶标志与显隐的唯一出口：置顶 = 用户意愿 && 未因全屏隐藏
    void applyWindowState();

    // 刷新周期（ms）：统计/面板的刷新节拍，由设置弹窗驱动并落盘
    int refreshMs() const { return m_refreshMs; }
    void setRefreshMs(int ms);

    void endDrag();                            // 拖拽收尾（松手/丢键/失焦统一入口）
    void updateUsage();                        // 今日用量摘要（10s 一次，喂给悬停面板）

    // ---- 位置记忆与边缘吸附 ----
    void loadPosition();                       // 启动时恢复（默认右下角）
    void savePosition() const;                 // 拖动/吸附后落盘
    QRect availGeometry() const;               // 鼠标所在屏幕的可用区域
    QRect desktopAvail() const;                // 所有屏幕可用区的并集（拖动夹取用）
    QRect cardRect() const;                    // 可见卡片矩形（不含透明阴影留白）
    bool cursorOnCard(int slack = 0) const;    // 光标是否真的停在可见卡片上（只认真实坐标）
    QPoint edgeRestPos(const QRect& avail, int edge, const QPoint& hint) const;
    QPoint edgeHidePos(const QRect& avail, int edge, const QPoint& rest) const;
    QPoint clampToAvail(const QRect& avail, const QPoint& p) const;
    int nearestEdge(const QRect& avail) const; // 0=不吸附，1=左 2=右 3=上 4=下
    QRect peekRect(const QRect& card) const;   // 贴边隐藏时露出的那一条
    void slideTo(const QPoint& target);        // 贴边/滑出的位移动画
    void drawEdgeBar(QPainter& p, const QRect& card);

    TrafficStore& m_store;
    int m_cardW = 0;   // 卡片宽：按最宽可能读数实测字体宽度算出（见 measureCardW）
    TopPanel* m_panel = nullptr;
    DetailDialog* m_detail = nullptr;   // 懒创建，随本浮框销毁
    SettingsDialog* m_settings = nullptr;   // 同上
    QSystemTrayIcon* m_tray = nullptr;  // 托盘（不支持时为 nullptr，所有用法都要判空）
    QTimer m_timer;                  // 统计刷新（周期由用户设置，默认 1s）
    int m_refreshMs = ts::kRefreshMsDefault;
    QTimer m_procTimer;              // 300ms 连接表快照（进程归因，独立于 UI 节拍）
    QTimer m_hideTimer;              // 榜单延迟隐藏
    QPropertyAnimation* m_fade = nullptr;  // 榜单淡入淡出
    QVariantAnimation m_hoverAnim;   // 悬停高亮过渡
    double m_hover = 0.0;            // 0=常态 1=悬停
    quint64 m_upRate = 0;
    quint64 m_downRate = 0;
    bool m_panelShown = false;       // 榜单显示意图（动画中亦为 true）
    bool m_dragging = false;
    QPoint m_dragPos;
    QPoint m_pressWinPos;            // 按下时的窗口位置（松开时没变过 = 单击，不是拖动）
    bool m_panelWasShown = false;    // 按下时面板是否开着：只有"拖之前就开着"的才在松手后恢复
    bool m_menuOpen = false;         // 右键菜单是否开着（开着时吸附态不收回窄条）

    QPropertyAnimation* m_slide = nullptr;  // 贴边 <-> 滑出 的位移动画
    QTimer m_fsTimer;                // 全屏程序探测（250ms = kFsPollMs，只判状态不取数据）
    bool m_topMostWanted = true;     // 用户在设置里要的置顶状态（与实际标志区分开）
    bool m_hideOnFullscreen = true;  // 同上：检测到全屏程序时自动隐藏
    bool m_autoEdgeHide = true;      // 同上：拖到屏幕边缘时自动贴边隐藏
    bool m_fsHidden = false;         // 当前是否因全屏而隐藏
    int m_edge = 0;                  // 0=未吸附，1=左 2=右 3=上 4=下
    bool m_snapped = false;          // 是否处于吸附态
    bool m_hovered = false;          // 鼠标是否在浮框/面板上（吸附态据此滑出）
    QPoint m_restPos;                // 完整显示时的位置（吸附态即"贴边位"）
    qint64 m_lastUsageMs = 0;        // 上次查「今日用量」的时刻（节流用）
};

