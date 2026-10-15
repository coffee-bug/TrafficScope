#pragma once
// 详情弹窗 / 设置弹窗共用的"自绘确认卡片"。
//
// 为什么不用 QMessageBox：它以调用它的弹窗为父窗口，会**继承** QSS 里的
// "QDialog { background: transparent; }"（QSS 沿 widget 父子级联），而 QLabel 仍用
// 调色板的默认色 → 整个确认框"什么都看不见"（深色下黑字、浅色下白字同理）。
// 本项目所有二次确认 / 提示统一走这里，不要再回到 QMessageBox。
//
// 纯头文件实现（类里没有 Q_OBJECT，AUTOMOC 不需要处理它，因此不用改 CMakeLists）。
#include "TitleBar.h"
#include "UiTheme.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QWidget>

namespace ts {

constexpr int kConfirmPadX = 18;        // 内容左右内边距
constexpr int kConfirmContentW = 384;   // 正文宽度（决定卡片换行位置）
constexpr int kConfirmBtnH = 26;        // 按钮高
// 卡片宽：与设置弹窗同宽（552），确认框与设置弹窗视觉保持一致；
// 窗口 = 卡片 + 2 * kShadowPad。
constexpr int kConfirmCardW = 552;

// 色值一律 %TOKEN%，由 ts::themed() 按当前主题替换（见 UiTheme.h）
constexpr char kConfirmQss[] =
    "QDialog { background: transparent; font-family: \"%FONT%\"; }"
    "#msg { color: %TEXT%; font-size: 12px; }";
// 按钮样式统一由 ts::primaryButtonQss() 提供（见 restyle()），与设置弹窗同一套。

// 深色确认卡片：无边框圆角 + 自绘标题栏 + 正文 + 底部按钮。
// okText：右侧主按钮；altText：可选中间按钮（空串 = 不显示）；withCancel：是否给「取消」。
// 用户点了哪个按钮看 choice()：1=主按钮 2=中间按钮 0=取消/直接关闭。
class ConfirmDialog : public QDialog {
public:
    ConfirmDialog(QWidget* parent, const QString& title, const QString& text,
                  const QString& okText, bool withCancel,
                  const QString& altText = QString())
        : QDialog(parent)
    {
        setAttribute(Qt::WA_QuitOnClose, false);
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        restyle();
        setFixedWidth(kConfirmCardW + kShadowPad * 2);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(kShadowPad, kShadowPad, kShadowPad, kShadowPad);
        root->setSpacing(0);
        root->addWidget(new TitleBar(this, title, this, /*withMinimize=*/false));

        auto* body = new QWidget(this);
        auto* lay = new QVBoxLayout(body);
        lay->setContentsMargins(kConfirmPadX, 14, kConfirmPadX, 14);
        lay->setSpacing(0);
        root->addWidget(body);

        auto* msg = new QLabel(text, body);
        msg->setObjectName(QStringLiteral("msg"));   // 必须显式配色，否则用调色板默认色
        msg->setWordWrap(true);
        msg->setFixedWidth(kConfirmContentW);
        lay->addWidget(msg);
        lay->addSpacing(16);

        auto* bottom = new QWidget(body);
        auto* brow = new QHBoxLayout(bottom);
        brow->setContentsMargins(0, 0, 0, 0);
        brow->setSpacing(10);
        brow->addStretch(1);

        if (withCancel) {
            auto* cancel = new QPushButton(QStringLiteral("取消"), bottom);
            cancel->setMinimumWidth(88);
            cancel->setFixedHeight(kConfirmBtnH);
            cancel->setCursor(Qt::PointingHandCursor);
            cancel->setAutoDefault(false);
            cancel->setDefault(true);   // 回车走"取消"，不会误触
            brow->addWidget(cancel);
            connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        }

        if (!altText.isEmpty()) {
            auto* alt = new QPushButton(altText, bottom);
            alt->setObjectName(QStringLiteral("resetBtn"));   // 与「重置统计」同款强调色
            alt->setMinimumWidth(88);
            alt->setFixedHeight(kConfirmBtnH);
            alt->setCursor(Qt::PointingHandCursor);
            alt->setAutoDefault(false);
            brow->addWidget(alt);
            connect(alt, &QPushButton::clicked, this, [this]() {
                m_choice = 2;
                accept();
            });
        }

        auto* ok = new QPushButton(okText, bottom);
        ok->setObjectName(QStringLiteral("primaryBtn"));
        ok->setMinimumWidth(88);
        ok->setFixedHeight(kConfirmBtnH);
        ok->setCursor(Qt::PointingHandCursor);
        ok->setAutoDefault(false);
        ok->setDefault(!withCancel && altText.isEmpty());
        brow->addWidget(ok);
        connect(ok, &QPushButton::clicked, this, [this]() {
            m_choice = 1;
            accept();
        });
        lay->addWidget(bottom);

        layout()->activate();
        adjustSize();
        setFixedHeight(height());
    }

    int choice() const { return m_choice; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        drawCard(p, rect(), kCardRadius, theme().dialogCard, cBorder());
    }

    void showEvent(QShowEvent* e) override
    {
        QDialog::showEvent(e);
        if (QWidget* pw = parentWidget()) {   // 居中到父窗口
            const QRect g = pw->frameGeometry();
            move(g.center() - QPoint(width() / 2, height() / 2));
        }
    }

private:
    int m_choice = 0;

    void restyle()
    {
        // 按钮样式与设置弹窗共用 primaryButtonQss()
        setStyleSheet(themed(QLatin1String(kConfirmQss)) + capQss()
                      + primaryButtonQss());
        setPalette(theme().palette());
    }
};

// 二次确认：取消（默认，回车落这里）/ 主按钮。返回 true 表示点了主按钮。
inline bool askConfirm(QWidget* parent, const QString& title, const QString& text,
                       const QString& okText)
{
    ConfirmDialog d(parent, title, text, okText, /*withCancel=*/true);
    d.exec();
    return d.choice() == 1;
}

// 单按钮的提示卡片（没有取消，只有「知道了」）
inline void showCardMessage(QWidget* parent, const QString& title, const QString& text)
{
    ConfirmDialog d(parent, title, text, QStringLiteral("知道了"),
                    /*withCancel=*/false);
    d.exec();
}

}  // namespace ts
