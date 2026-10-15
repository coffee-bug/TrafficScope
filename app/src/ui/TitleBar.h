#pragma once
// 无边框弹窗的自绘标题栏：图标 + 标题 + 最小化/关闭，按住空白处可拖动窗口。
// 不用原生标题栏的原因：Win10 不支持给原生标题栏自定义颜色（只有 Win11 22000+ 支持），
// 自绘才能让 Win10/Win11 观感一致（与浮框同一套圆角深色卡片）。
//
// 纯头文件（无 Q_OBJECT、无 .cpp），详情弹窗与设置弹窗共用一份；
// 样式规则在 ts::capQss()（UiTheme.h），调用方把它并进自己的 QSS。

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QPushButton>
#include <QWidget>

namespace ts {

// 自绘标题栏高度（窗口总高 = 卡片内容高 + 它 + 四周阴影留白）
constexpr int kTitleH = 36;

class TitleBar : public QWidget {
public:
    // target = 被拖动的弹窗；withMinimize=false 时只显示关闭按钮（设置弹窗用）
    TitleBar(QWidget* target, const QString& title, QWidget* parent = nullptr,
             bool withMinimize = true)
        : QWidget(parent)
        , m_target(target)
    {
        setFixedHeight(kTitleH);
        auto* lay = new QHBoxLayout(this);
        // 标题栏由调用方的 root 布局摆到卡片内，这里回补内边距，
        // 使图标与右侧按钮贴合卡片边缘（与 body 的内容边界对齐）
        lay->setContentsMargins(8, 0, 2, 0);
        lay->setSpacing(8);

        const QIcon ic = target->windowIcon();
        if (!ic.isNull()) {
            auto* logo = new QLabel(this);
            logo->setFixedSize(16, 16);
            logo->setPixmap(ic.pixmap(16, 16));
            lay->addWidget(logo);
        }
        auto* cap = new QLabel(title, this);
        cap->setObjectName(QStringLiteral("capTitle"));
        lay->addWidget(cap);
        lay->addStretch(1);

        m_close = addButton(lay, QStringLiteral("×"),
                            QStringLiteral("capCloseBtn"));
        connect(m_close, &QPushButton::clicked, this, [target]() { target->close(); });

        if (withMinimize) {
            m_min = addButton(lay, QStringLiteral("–"),
                              QStringLiteral("capMinBtn"));
            // 最小化放在关闭左边
            lay->insertWidget(lay->indexOf(m_close), m_min);
            connect(m_min, &QPushButton::clicked, this,
                    [target]() { target->showMinimized(); });
        }
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(e);
            return;
        }
        m_dragging = true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        m_origin = e->globalPosition().toPoint() - m_target->frameGeometry().topLeft();
#else
        m_origin = e->globalPos() - m_target->frameGeometry().topLeft();
#endif
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_dragging)
            return;
        // 拖拽途中左键已抬起（被截图工具等其他窗口抢走释放事件）→ 结束，避免窗口黏住光标
        if (!(e->buttons() & Qt::LeftButton)) {
            m_dragging = false;
            return;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        m_target->move(e->globalPosition().toPoint() - m_origin);
#else
        m_target->move(e->globalPos() - m_origin);
#endif
    }

    void mouseReleaseEvent(QMouseEvent*) override { m_dragging = false; }

private:
    static QPushButton* addButton(QHBoxLayout* lay, const QString& text,
                                  const QString& name)
    {
        auto* b = new QPushButton(text);
        b->setObjectName(name);
        b->setFixedSize(34, 26);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(b);
        return b;
    }

    QWidget* m_target = nullptr;
    QPushButton* m_min = nullptr;
    QPushButton* m_close = nullptr;
    bool m_dragging = false;
    QPoint m_origin;
};

}  // namespace ts
