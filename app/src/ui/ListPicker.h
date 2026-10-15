#pragma once
// 手绘下拉（长列表版）：外观与 DropDown 完全一致（共用 ts::drawField），
// 但弹层不是 QMenu，而是**可滚动列表 + 输入过滤框**的深色卡片。
//
// 为什么需要它：几百项（如系统字体）塞进 QMenu，Qt 会把菜单自动折成多列铺满
// 整个屏幕（实测约 400 个字体），既丑又难找。这里用固定行数的滚动列表 +
// 输入过滤，顺带能按名字搜。
//
// 两个踩过的坑（都别改回去）：
// 1. 弹层**不能用 Qt::Popup**：Qt 的 popup 窗口拿不到输入法的组字消息，
//    中文/日文根本打不进过滤框（用户实测）。改用普通 Qt::Tool 无边框窗口，
//    自己处理"失活就收起" + "点按钮=收起（防抖）"。
// 2. 定位要按**卡片边缘**而不是窗口边缘：卡片四周有 kShadowPad 的透明阴影
//    留白（drawCard 自己内缩），拿窗口左上角去对按钮，卡片看起来会右下偏移。
//
// 纯头文件、无 Q_OBJECT（回调用 std::function，与 MonthView / DropDown 一致）。

#include "UiTheme.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPair>
#include <QScreen>
#include <QVBoxLayout>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <functional>

namespace ts {

// 弹层开合防抖窗口（毫秒）：① 刚弹出时的一过性失活要忽略（否则立刻收起）；
// ② 刚收起后不要马上再弹开（否则"点按钮收起"会变成开关抖动）。两处共用这个值。
constexpr qint64 kPopupDebounceMs = 250;

namespace detail {

// 弹层：卡片(圆角+阴影) 里放过滤框 + 列表
class ListPickerPopup : public QWidget {
public:
    static constexpr int kRows = 10;        // 可见行数
    static constexpr int kRowHMin = 22;     // 行高夹取范围（实测值兜底）
    static constexpr int kRowHMax = 30;
    static constexpr int kCardW = 220;      // 卡片最小宽度
    static constexpr int kPad = 6;          // 卡片内边距（在阴影留白之外）
    static constexpr int kFilterH = 24;
    static constexpr int kGap = 6;          // 过滤框与列表之间

    explicit ListPickerPopup(QWidget* owner)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setStyleSheet(ts::pickerQss());

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(kShadowPad + kPad, kShadowPad + kPad,
                                kShadowPad + kPad, kShadowPad + kPad);
        lay->setSpacing(kGap);

        m_filter = new QLineEdit(this);
        m_filter->setFixedHeight(kFilterH);
        m_filter->setPlaceholderText(QStringLiteral("输入以筛选…"));
        m_filter->setAttribute(Qt::WA_InputMethodEnabled, true);   // 中文/日文输入
        lay->addWidget(m_filter);

        m_list = new QListWidget(this);
        m_list->setFixedHeight(kRows * kRowHMin);   // setItems() 里按真实行高重算
        m_list->setUniformItemSizes(true);
        m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        m_list->setFocusPolicy(Qt::NoFocus);   // 焦点留给过滤框（键盘直接打字）
        lay->addWidget(m_list);

        connect(m_filter, &QLineEdit::textChanged, this,
                [this](const QString& t) { applyFilter(t); });
        connect(m_filter, &QLineEdit::returnPressed, this, [this]() { pickFirst(); });
        connect(m_list, &QListWidget::itemClicked, this,
                [this](QListWidgetItem* it) { pick(it); });
        connect(m_list, &QListWidget::itemActivated, this,
                [this](QListWidgetItem* it) { pick(it); });
    }

    std::function<void(const QVariant&)> onPick;
    std::function<void()> onHidden;   // 收起时通知 owner（它据此做防抖）

    void setItems(const QVector<QPair<QString, QVariant>>& items, const QVariant& current)
    {
        m_list->clear();
        for (const auto& it : items) {
            auto* li = new QListWidgetItem(it.first, m_list);
            li->setData(Qt::UserRole, it.second);
        }
        m_filter->clear();   // 触发 textChanged → applyFilter("")，全部可见
        if (m_list->count() > 0) {
            // 行高随字体变化，按真实值把列表固定成 kRows 行
            const int rh = qBound(kRowHMin, m_list->sizeHintForRow(0), kRowHMax);
            m_list->setFixedHeight(kRows * rh);
        }
        for (int i = 0; i < m_list->count(); ++i) {
            if (m_list->item(i)->data(Qt::UserRole) == current) {
                m_list->setCurrentRow(i);
                m_list->scrollToItem(m_list->item(i),
                                     QAbstractItemView::PositionAtCenter);
                break;
            }
        }
    }

    // 卡片尺寸（不含阴影留白）
    int cardHeight() const
    {
        return kPad * 2 + kFilterH + kGap + m_list->height();
    }

    void focusFilter()
    {
        // 输入法需要窗口"真的被激活"才会把组字消息送进来
        activateWindow();
        m_filter->setFocus(Qt::OtherFocusReason);
    }

protected:
    bool event(QEvent* e) override
    {
        // 点别处 / 切到别的窗口 → 收起。刚弹出时的一过性失活忽略掉。
        if (e->type() == QEvent::WindowDeactivate && m_shownMs > 0
            && QDateTime::currentMSecsSinceEpoch() - m_shownMs > kPopupDebounceMs)
            hide();
        return QWidget::event(e);
    }

    void showEvent(QShowEvent* e) override
    {
        setStyleSheet(ts::pickerQss());   // 主题/字体可能刚变过
        m_shownMs = QDateTime::currentMSecsSinceEpoch();
        QWidget::showEvent(e);
    }

    void hideEvent(QHideEvent* e) override
    {
        QWidget::hideEvent(e);
        if (onHidden)
            onHidden();
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        ts::drawCard(p, rect(), ts::kCardRadius, ts::theme().popupBg, ts::cBorder());
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape) {
            hide();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    void applyFilter(const QString& text)
    {
        const QString key = text.trimmed();
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem* it = m_list->item(i);
            const bool show = key.isEmpty()
                              || it->text().contains(key, Qt::CaseInsensitive);
            it->setHidden(!show);
        }
    }

    void pick(QListWidgetItem* it)
    {
        if (!it || it->isHidden())
            return;
        const QVariant d = it->data(Qt::UserRole);
        hide();
        if (onPick)
            onPick(d);
    }

    void pickFirst()
    {
        for (int i = 0; i < m_list->count(); ++i) {
            if (!m_list->item(i)->isHidden()) {
                pick(m_list->item(i));
                return;
            }
        }
    }

    QLineEdit* m_filter = nullptr;
    QListWidget* m_list = nullptr;
    qint64 m_shownMs = 0;
};

}  // namespace detail

class ListPicker : public QWidget {
public:
    struct Item {
        QString text;
        QVariant data;
    };

    explicit ListPicker(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
    }

    // 弹层以本控件为父，随本控件一起销毁（无需手工 delete）

    void addItem(const QString& text, const QVariant& data)
    {
        m_items.append(Item{text, data});
        if (m_cur < 0)
            m_cur = 0;
    }

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
        return (m_cur >= 0 && m_cur < m_items.size()) ? m_items.at(m_cur).data
                                                      : QVariant();
    }

    QString currentText() const
    {
        return (m_cur >= 0 && m_cur < m_items.size()) ? m_items.at(m_cur).text
                                                      : QString();
    }

    void closePopup()
    {
        if (m_popup)
            m_popup->hide();
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
        drawField(p, rect(), currentText(), m_hover, isEnabled());
    }

private:
    void popup()
    {
        // 弹层刚收起（点按钮本身 / 点到外面）时不要马上又弹开，否则是开关抖动：
        // 点按钮关闭时，窗口激活先触发 deactivate→hide，紧接着才是按钮的 mousePress
        if (m_popup && !m_popup->isVisible()
            && QDateTime::currentMSecsSinceEpoch() - m_closedMs < kPopupDebounceMs)
            return;

        if (!m_popup) {
            m_popup = new detail::ListPickerPopup(this);
            m_popup->onPick = [this](const QVariant& d) {
                setCurrentData(d);
                if (onActivated)
                    onActivated(d);
            };
            m_popup->onHidden = [this]() {
                m_closedMs = QDateTime::currentMSecsSinceEpoch();
            };
        }

        QVector<QPair<QString, QVariant>> list;
        list.reserve(m_items.size());
        for (const Item& it : m_items)
            list.append(qMakePair(it.text, it.data));
        m_popup->setItems(list, currentData());

        const int cardW = qMax(width(), detail::ListPickerPopup::kCardW);
        const int cardH = m_popup->cardHeight();
        // 让**卡片**左边缘对齐按钮、上沿落在按钮下方 4px（窗口还要多出阴影留白）
        QPoint p = mapToGlobal(QPoint(-ts::kShadowPad, height() + 4 - ts::kShadowPad));
        QScreen* s = QGuiApplication::screenAt(mapToGlobal(rect().center()));
        if (!s)
            s = QGuiApplication::primaryScreen();
        if (s) {
            const QRect a = s->availableGeometry();
            int cardX = p.x() + ts::kShadowPad;
            if (cardX + cardW > a.right() - 4)
                cardX = a.right() - 4 - cardW;
            if (cardX < a.left() + 4)
                cardX = a.left() + 4;
            p.setX(cardX - ts::kShadowPad);

            int cardY = p.y() + ts::kShadowPad;
            if (cardY + cardH > a.bottom() - 4)     // 下面放不下 → 翻到按钮上方
                cardY = mapToGlobal(QPoint(0, 0)).y() - 4 - cardH;
            if (cardY < a.top() + 4)
                cardY = a.top() + 4;
            p.setY(cardY - ts::kShadowPad);
        }
        m_popup->resize(cardW + ts::kShadowPad * 2, cardH + ts::kShadowPad * 2);
        m_popup->move(p);
        m_popup->show();
        m_popup->focusFilter();
    }

    QVector<Item> m_items;
    int m_cur = -1;
    bool m_hover = false;
    qint64 m_closedMs = 0;
    detail::ListPickerPopup* m_popup = nullptr;
};

}  // namespace ts
