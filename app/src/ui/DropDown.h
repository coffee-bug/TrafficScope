#pragma once
// 手绘下拉选择控件（纯头文件，无需改 CMakeLists）。
//
// 为什么不用 QComboBox：它的弹层靠"样式自绘"——容器边框、当前项焦点框、下拉箭头
// 都按调色板画，深色卡片上会冒出一圈白框；而且 QSS 一旦接管 ::drop-down 却没给
// image，箭头会直接消失。这里改成完全受控的做法：
//   · 按钮本体：paintEvent 手绘（圆角、底、边、文字、▾ 箭头）—— 颜色全取当前主题；
//   · 弹层：QMenu + ts::menuQss()（与右键菜单同一套样式，纯 QSS，无自绘残留）。
// 于是本控件零样式依赖：换主题改 Theme 的色值即可，不用再和调色板、焦点框较劲。
// 回调用 std::function（与 RangePopup 的 MonthView 一致），避免为一个控件引入 Q_OBJECT。

#include "UiTheme.h"

#include <QAction>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <functional>

namespace ts {

class DropDown : public QWidget {
public:
    explicit DropDown(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);   // :hover 状态自己管
    }

    void clear()
    {
        m_items.clear();
        m_cur = -1;
        update();
    }

    void addItem(const QString& text, const QVariant& data)
    {
        m_items.append(Item{text, data});
        if (m_cur < 0)
            m_cur = 0;
        update();
    }

    // 程序内改选中项：不发回调（只有用户点选才发）
    void setCurrentData(const QVariant& data)
    {
        for (int i = 0; i < m_items.size(); ++i) {
            if (m_items.at(i).data == data) {
                if (i != m_cur) {
                    m_cur = i;
                    update();
                }
                return;
            }
        }
    }

    QVariant currentData() const
    {
        return (m_cur >= 0 && m_cur < m_items.size()) ? m_items.at(m_cur).data : QVariant();
    }

    QString currentText() const
    {
        return (m_cur >= 0 && m_cur < m_items.size()) ? m_items.at(m_cur).text : QString();
    }

    // 用户选择回调（data 为选中项的数据）
    std::function<void(const QVariant&)> onActivated;

protected:
    bool event(QEvent* e) override
    {
        switch (e->type()) {
        case QEvent::Enter:
            m_hover = true;
            update();
            break;
        case QEvent::Leave:
            m_hover = false;
            update();
            break;
        case QEvent::EnabledChange:
            update();
            break;
        default:
            break;
        }
        return QWidget::event(e);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && isEnabled() && !m_items.isEmpty())
            popup();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        // 与 ListPicker 共用同一套手绘外观（圆角 / 底 / 边 / 文字 / ▾）
        drawField(p, rect(), currentText(), m_hover, isEnabled());
    }

private:
    struct Item {
        QString text;
        QVariant data;
    };

    void popup()
    {
        QMenu menu;
        ts::polishMenu(menu);   // 圆角 + 深色样式（无边框半透明，四角才圆得干净）
        for (int i = 0; i < m_items.size(); ++i) {
            QAction* a = menu.addAction(m_items.at(i).text);
            a->setCheckable(true);
            a->setChecked(i == m_cur);
            const QVariant d = m_items.at(i).data;
            connect(a, &QAction::triggered, this, [this, d]() {
                setCurrentData(d);
                if (onActivated)
                    onActivated(d);
            });
        }
        menu.exec(mapToGlobal(QPoint(0, height() + 2)));
    }

    QVector<Item> m_items;
    int m_cur = -1;
    bool m_hover = false;
};

}  // namespace ts
