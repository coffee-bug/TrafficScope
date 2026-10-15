#pragma once
#include <QDialog>
#include <QString>

class QButtonGroup;
class QCheckBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QPaintEvent;
class QPushButton;
class QRadioButton;
class QShowEvent;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace ts { class DropDown; class ListPicker; }

// 设置弹窗（浮框右键 →「设置…」）：
// 分页顺序：显示 / 统计 / 历史记录 / 网卡 / 关于（左侧导航与右侧页一一对应）
//   - 显示：窗口置顶开关（作用于浮框，通过 topMostToggled 回调）；
//   - 统计：重置浮框与详情里的累计数字（SQLite 历史记录不受影响），带二次确认；
//   - 历史记录：应用 / 域名的保留天数分别设置，改完即时生效；
//   - 网卡：自动跟随默认路由 / 手动指定；切换即时生效（失败弹提示），
//      下方灰字显示当前网卡与抓包计数（原来在右键菜单里的诊断信息）；
//   - 关于：版本 / 作者 / 数据目录；
//      全屏时自动隐藏开关（hideOnFullscreenToggled）；
//      自动贴边隐藏开关（autoEdgeHideToggled，关掉后拖到边缘也不吸附）。
// 无边框圆角卡片 + 自绘标题栏（与详情弹窗同一套视觉），尺寸固定、居中显示；
// 颜色全部来自 ts::Theme，主题切换时 restyle() 重新套一遍 QSS 与调色板。
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    // 显示前由浮框同步真实状态（本弹窗自身不是置顶窗口）
    void setTopMost(bool on);
    void setHideOnFullscreen(bool on);   // 同步「全屏时自动隐藏」开关
    void setAutoEdgeHide(bool on);       // 同步「自动贴边隐藏」开关
    void setAutoStart(bool on);          // 同步「开机自动启动」（读计划任务的真实状态）
    void setRefreshMs(int ms);   // 同步浮框当前的刷新周期
    // 同步"是否统计未知流量"（on = 照常统计；界面上是"不再统计未知流量"）
    void setCountUnattributed(bool on);
    // 重新读取网卡枚举与当前选择（每次显示、以及切换失败回滚时调用）
    void syncDeviceChoice();

signals:
    void topMostToggled(bool on);
    void hideOnFullscreenToggled(bool on);   // 用户在「全屏时自动隐藏」里改了开关
    void autoEdgeHideToggled(bool on);       // 用户在「自动贴边隐藏」里改了开关
    void refreshMsChanged(int ms);   // 用户在「刷新频率」里确认了新的周期

protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    // 设置项的"暂存状态"：界面上的控件只改这个值，**点「应用」才真正写下去**，
    // 点「取消」或直接关窗则丢弃（下一次打开时会按真实状态重新同步）。
    struct State {
        bool topMost = true;
        bool hideFullscreen = true;
        bool autoEdgeHide = true;
        bool autoStart = false;
        int refreshMs = 1000;
        QString font;
        int granularity = 1;
        int appRetain = 90;
        int domainRetain = 90;
        // 界面上的「不再统计未知流量」：默认勾选（= 不统计未知流量，
        // 与 core/Settings.h 的 countUnattributed() 默认值 false 对应）
        bool noUnattr = true;
        bool autoDev = true;
        QString dev;
        int rateUnit = 0;      // 速率单位：0=字节 B/s 1=比特 bps
        bool showPeak = true;  // 面板顶部是否显示峰值速率
        // 是否启用浏览器扩展归属：开 = 运行期注册 native host（扩展能把归属送进来）。
        // 默认开，与 core/Settings.h 的 browserAttribution() 默认值一致。
        bool browserExt = true;

        bool operator==(const State& o) const
        {
            return topMost == o.topMost && hideFullscreen == o.hideFullscreen
                && autoEdgeHide == o.autoEdgeHide
                && autoStart == o.autoStart && refreshMs == o.refreshMs
                && font == o.font && granularity == o.granularity
                && appRetain == o.appRetain && domainRetain == o.domainRetain
                && noUnattr == o.noUnattr && autoDev == o.autoDev && dev == o.dev
                && rateUnit == o.rateUnit && showPeak == o.showPeak
                && browserExt == o.browserExt;
        }
        bool operator!=(const State& o) const { return !(*this == o); }
    };

    QWidget* buildNav(QWidget* parent);          // 左侧导航列（与 m_pages 一一对应）
    // 各页内容：body = 页面容器，lay = 该页的竖向布局（由构造函数建好后传入）。
    // 拆出来只是为了让构造函数从 500 行降到骨架级别，逻辑与原先逐行一致。
    void buildDisplayPage(QWidget* body, QVBoxLayout* lay);
    void buildStatsPage(QWidget* body, QVBoxLayout* lay);
    void buildHistoryPage(QWidget* body, QVBoxLayout* lay);
    void buildNetworkPage(QWidget* body, QVBoxLayout* lay);
    void buildAboutPage(QWidget* body, QVBoxLayout* lay);
    State stateFromUi() const;                   // 从控件读当前（暂存）状态
    void syncFromReal();                         // 按真实状态刷新控件 + m_snap
    void markDirty();                            // 控件变化 → 刷新「应用」按钮可用性
    void applyAll();                             // 点「应用」：把暂存状态写下去
    void refreshDeviceInfo();     // 当前网卡 + 抓包计数（1s 刷新）
    void refreshExtStatus();      // 浏览器扩展归属通道的连接状态（同上，1s）
    void onResetStats();
    void onClearHistory();        // 弹框选「应用 / 域名」→ 清空对应历史记录
    void restyle();               // 主题切换后重新 setStyleSheet / setPalette
    // 按内容重新定高（宽度固定）。**必须在布局建好之后才能调** —— 构造早期那个
    // restyle() 是没布局的，曾经就是在这里解引用空 layout() 直接崩掉。
    void refitHeight();

    QRadioButton* m_autoRadio = nullptr;
    QRadioButton* m_manualRadio = nullptr;
    ts::DropDown* m_devCombo = nullptr;      // 手绘下拉（不用 QComboBox，见 DropDown.h）
    ts::DropDown* m_appRetain = nullptr;     // 应用历史保留天数
    ts::DropDown* m_domainRetain = nullptr;  // 域名历史保留天数
    ts::DropDown* m_granularity = nullptr;   // 历史记录粒度（小时 / 天）
    ts::DropDown* m_unitDrop = nullptr;      // 速率单位（字节 / 比特）
    QCheckBox* m_peakCheck = nullptr;        // 面板顶部显示峰值速率
    QLabel* m_curDev = nullptr;
    QLabel* m_diag = nullptr;
    QLabel* m_extStatus = nullptr;      // 浏览器扩展（可选）的连接状态
    QWidget* m_tip = nullptr;      // 自绘的悬浮提示卡片（QToolTip 在这类弹窗里不出气泡）
    QCheckBox* m_topCheck = nullptr;
    QCheckBox* m_fsHideCheck = nullptr;      // 全屏时自动隐藏浮窗
    QCheckBox* m_edgeHideCheck = nullptr;    // 自动贴边隐藏（拖到屏幕边缘自动收起）
    QCheckBox* m_autoStartCheck = nullptr;   // 开机自动启动（计划任务）
    QPushButton* m_resetBtn = nullptr;
    QPushButton* m_clearHistBtn = nullptr;   // 清空历史记录（应用 / 域名二选一）
    QCheckBox* m_noUnattrCheck = nullptr;    // 不再统计未归因流量
    QCheckBox* m_browserExtCheck = nullptr;  // 启用浏览器扩展归属（native host 注册开关）
    QLineEdit* m_refreshEdit = nullptr;      // 刷新频率（ms，手动输入）
    ts::ListPicker* m_fontDrop = nullptr;    // 界面字体（系统字体列表，可滚动 + 过滤）
    QStackedWidget* m_pages = nullptr;       // 右侧分页（顺序与左侧导航一致）
    QButtonGroup* m_navGroup = nullptr;      // 左侧导航互斥
    QPushButton* m_applyBtn = nullptr;       // 「应用」：有改动才可用
    QPushButton* m_cancelBtn = nullptr;      // 「取消」：丢弃改动并关闭
    State m_snap;                            // 打开时的真实状态（判断改动 / 取消回滚）
    QTimer* m_infoTimer = nullptr;
    bool m_placed = false;        // 是否已居中摆放过（用户拖动过就不再挪）
};
