#pragma once

// 自绘悬浮提示卡片 + 信息图标（原在 SettingsDialog.cpp 内部，详情弹窗也要用后拎出共享）。
// 为什么不用 QToolTip：在"无边框 + 半透明"的弹窗里实测不出气泡（悬停图标毫无反应），
// 而且原生气泡的配色也不受主题控制。自己画一个最稳。
// 纯头文件、无 Q_OBJECT —— 不用进 CMakeLists 的源列表。

#include "UiTheme.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QRect>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>

namespace ts {

namespace tipcard {
// 悬浮提示卡片文字宽（决定气泡宽度）与内边距（在阴影留白之外）
constexpr int kTipTextW = 300;
constexpr int kTipPad = 10;
}

class TipCard : public QWidget {
public:
    explicit TipCard(QWidget* owner)
        : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        auto* lay = new QVBoxLayout(this);
        const int pad = ts::kShadowPad + tipcard::kTipPad;
        lay->setContentsMargins(pad, pad - 2, pad, pad);
        m_text = new QLabel(this);
        m_text->setObjectName(QStringLiteral("tipText"));
        m_text->setWordWrap(true);
        m_text->setFixedWidth(tipcard::kTipTextW);
        lay->addWidget(m_text);
    }

    // anchor：被悬停控件在屏幕上的矩形（气泡贴在它下方，越界自动翻转/夹取）
    void popup(const QString& text, const QRect& anchor)
    {
        m_text->setText(text);
        // 气泡是独立顶层窗口，样式不会自动跟着弹窗走，这里单独套一遍
        m_text->setStyleSheet(QStringLiteral("#tipText { color: %TEXT%; font-size: 12px; }")
                                  .replace(QStringLiteral("%TEXT%"),
                                           ts::cssColor(ts::theme().text)));
        adjustSize();
        QScreen* s = QGuiApplication::screenAt(anchor.center());
        if (!s)
            s = QGuiApplication::primaryScreen();
        QPoint p(anchor.left(), anchor.bottom() + 6);
        if (s) {
            const QRect avail = s->availableGeometry();
            if (p.x() + width() - ts::kShadowPad > avail.right())
                p.setX(avail.right() - width() + ts::kShadowPad);
            if (p.y() + height() - ts::kShadowPad > avail.bottom())
                p.setY(anchor.top() - 6 - height() + ts::kShadowPad);
            if (p.x() < avail.left() - ts::kShadowPad)
                p.setX(avail.left() - ts::kShadowPad);
            if (p.y() < avail.top() - ts::kShadowPad)
                p.setY(avail.top() - ts::kShadowPad);
        }
        move(p);
        show();
        raise();
    }

    void dismiss() { hide(); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        ts::drawCard(p, rect(), ts::kCardRadius, ts::theme().dialogCard, ts::cBorder());
    }

private:
    QLabel* m_text = nullptr;
};

// 信息图标：悬停时弹出上面的提示卡片（Enter/Leave 用 event() 接，Qt5/Qt6 通吃）。
// 图标颜色由使用方的 QSS 提供（objectName = "infoIcon"）。
class InfoIcon : public QLabel {
public:
    InfoIcon(const QString& tip, TipCard* card, QWidget* parent)
        : QLabel(QStringLiteral("ⓘ"), parent)   // ⓘ
        , m_tip(tip)
        , m_card(card)
    {
        setObjectName(QStringLiteral("infoIcon"));
        setFixedSize(18, 18);
        setAlignment(Qt::AlignCenter);
        setCursor(Qt::WhatsThisCursor);
    }

    // 把旁边的文字也变成热区：图标只有 18px，光标稍偏就悬停不到
    void attachTo(QWidget* w) { w->installEventFilter(this); }

protected:
    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::Enter)
            showTip(this);
        else if (e->type() == QEvent::Leave)
            hideTip();
        return QLabel::event(e);
    }

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::Enter) {
            if (QWidget* w = qobject_cast<QWidget*>(obj))
                showTip(w);
        } else if (e->type() == QEvent::Leave) {
            hideTip();
        }
        return QLabel::eventFilter(obj, e);
    }

private:
    void showTip(QWidget* anchor)
    {
        if (m_card)
            m_card->popup(m_tip, QRect(anchor->mapToGlobal(QPoint(0, 0)), anchor->size()));
    }
    void hideTip()
    {
        if (m_card)
            m_card->dismiss();
    }

    QString m_tip;
    TipCard* m_card = nullptr;
};

}   // namespace ts
