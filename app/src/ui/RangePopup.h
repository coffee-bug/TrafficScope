#pragma once
#include <QDate>
#include <QWidget>

class QLabel;
class QPushButton;
class MonthView;

// 深色卡片样式的日期弹层：自绘**双月**日历（左月=选中月的上一个月）+ 顶部翻月 +
// 底部快捷按钮（今天/最近三天/最近七天/最近三十天/全部）+ 橙色确定按钮。
// 点日历某天立即发出 datePicked 并关闭；未来日期灰显且不可点。
// 单月网格 MonthView 定义在 RangePopup.cpp（无 Q_OBJECT，无需 moc）。
class RangePopup : public QWidget {
    Q_OBJECT
public:
    explicit RangePopup(QWidget* parent = nullptr);

    // 在 anchor 控件下方弹出，日历初始选中 sel
    void popupFor(QWidget* anchor, const QDate& selected);

signals:
    void quickRange(int days);           // 0/3/7/30，-2=全部
    void datePicked(const QDate& date);  // 用户点了日历中的某天

private:
    void shiftMonth(int delta);          // delta = ±1，整体翻月
    void syncMonths();                   // 把 m_year/m_month 刷到两个网格与标题
    void restyle();                      // 主题切换：重新套 QSS + 重绘自绘网格

    MonthView* m_left = nullptr;
    MonthView* m_right = nullptr;
    QLabel* m_titleL = nullptr;
    QLabel* m_titleR = nullptr;
    QDate m_selected;
    int m_year = 0;    // 左月年份
    int m_month = 0;   // 左月月份
};
