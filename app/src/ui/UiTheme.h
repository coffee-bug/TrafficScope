#pragma once
// TrafficScope 统一视觉主题（纯头文件，仅 inline，UI 层直接引用，无需改 CMakeLists）。
//
// 颜色**只有一份来源**：ts::Theme（当前只有暗色一套 darkTheme）。
// 所有 QSS 文本写成带 %TOKEN% 占位的字符串，交给 ts::themed() 按当前主题替换；
// 手绘部分读 ts::theme() 的成员或 ts::cXxx()。**不要在 UI 文件里写死色值**。
//
// 换主题/字体的唯一广播口是 ts::themeNotifier().notify()：各控件连上它的 changed()
// 后在自己的 restyle() 里重新 setStyleSheet / setPalette / update；换字体走
// ts::setFontFamily()（也是同一个广播）。
//
// 新增颜色时：先加 Theme 成员，再在 themed() 的映射表里加一行同名 token。

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QMenu>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRectF>
#include <QString>
#include <QtGlobal>

namespace ts {

// widget 外框到卡片边缘的留白，用于绘制外阴影
constexpr int kShadowPad = 8;
constexpr qreal kCardRadius = 12.0;

// ---------------------------------------------------------------------------
// 主题
// ---------------------------------------------------------------------------
struct Theme
{
    bool dark = true;

    // 浮框卡片
    QColor card, cardHover, border, borderHover, divider;
    // 弹窗卡片（详情/设置）与表头
    QColor dialogCard, headerBg;
    // 文字：由强到弱
    QColor text, text2, textSub, textFaint, textOff, textDim, textHover, onAccent;
    // 强调
    QColor up, down, accent, accentSoft, accentSoft2, accentSoft3, warn, warnHover, danger;
    // 菜单 / 下拉弹层 / 悬浮提示气泡
    QColor popupBg, popupSel, popupBorder, popupSep;
    // 输入类控件
    QColor fieldBg, fieldBgHover, fieldBgSoft, fieldBorder, fieldBorderHover, fieldBorderOff;
    // 表格 / 滚动条
    QColor rowAlt, scroll, scrollHover;
    // 手绘装饰：单选复选指示器、贴边条底槽、行 hover、日期弹层的未来日期
    QColor indRing, indRingHover, indFill, indFillHover, groove, rowHover, dateOff;

    // 界面字体（系统字体名，如 "Segoe UI" / "Microsoft YaHei"）；QSS 里用 %FONT%
    QString fontFamily;

    // 给"样式自绘装饰"用（下拉弹层容器边框、焦点框、QMessageBox 等）；
    // 调色板不吃半透明色，这里都用实色。
    QPalette palette() const
    {
        QPalette p;
        p.setColor(QPalette::Window, dialogCard);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Base, dark ? popupBg : QColor(0xFF, 0xFF, 0xFF));
        p.setColor(QPalette::AlternateBase, dark ? QColor(0x1F, 0x22, 0x29)
                                                 : QColor(0xF2, 0xF4, 0xF7));
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Button, dark ? QColor(0x23, 0x26, 0x2D)
                                          : QColor(0xEC, 0xEF, 0xF3));
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::Highlight, dark ? popupSel : accent);
        p.setColor(QPalette::HighlightedText, dark ? text : onAccent);
        p.setColor(QPalette::ToolTipBase, popupBg);
        p.setColor(QPalette::ToolTipText, text);
        p.setColor(QPalette::PlaceholderText, textFaint);
        p.setColor(QPalette::Mid, border);
        p.setColor(QPalette::Dark, borderHover);
        return p;
    }
};

inline Theme darkTheme()
{
    Theme t;
    t.dark = true;
    t.fontFamily = QStringLiteral("Segoe UI");
    t.card        = QColor(23, 25, 30, 246);
    t.cardHover   = QColor(31, 34, 41, 250);
    t.border      = QColor(255, 255, 255, 26);
    t.borderHover = QColor(255, 255, 255, 52);
    t.divider     = QColor(255, 255, 255, 14);
    t.dialogCard  = QColor(28, 30, 34);
    t.headerBg    = QColor(28, 30, 34);
    t.text        = QColor(0xE9, 0xEC, 0xF1);
    t.text2       = QColor(0xCF, 0xD4, 0xDC);
    t.textSub     = QColor(0x8E, 0x94, 0xA0);
    t.textFaint   = QColor(0x6C, 0x72, 0x7C);
    t.textOff     = QColor(0x5C, 0x61, 0x69);
    t.textDim     = QColor(0x4A, 0x4F, 0x58);
    t.textHover   = QColor(0xC6, 0xCC, 0xD6);
    t.onAccent    = QColor(0xFF, 0xFF, 0xFF);
    t.up          = QColor(0xFF, 0xA4, 0x3C);
    t.down        = QColor(0x3E, 0xCF, 0x8E);
    t.accent      = QColor(0x4C, 0x8D, 0xFF);
    t.accentSoft  = QColor(76, 141, 255, 45);
    t.accentSoft2 = QColor(76, 141, 255, 38);
    t.accentSoft3 = QColor(76, 141, 255, 62);
    t.warn        = QColor(0xFF, 0x9F, 0x43);
    t.warnHover   = QColor(0xFF, 0xAE, 0x5E);
    t.danger      = QColor(0xE8, 0x11, 0x23);
    t.popupBg     = QColor(0x23, 0x26, 0x2D);
    t.popupSel    = QColor(0x33, 0x38, 0x45);
    t.popupBorder = QColor(255, 255, 255, 26);
    t.popupSep    = QColor(255, 255, 255, 20);
    t.fieldBg     = QColor(255, 255, 255, 10);
    t.fieldBgHover = QColor(255, 255, 255, 18);
    t.fieldBgSoft = QColor(255, 255, 255, 14);
    t.fieldBorder = QColor(255, 255, 255, 26);
    t.fieldBorderHover = QColor(255, 255, 255, 52);
    t.fieldBorderOff = QColor(255, 255, 255, 14);
    t.rowAlt      = QColor(255, 255, 255, 8);
    t.scroll      = QColor(255, 255, 255, 32);
    t.scrollHover = QColor(255, 255, 255, 58);
    t.indRing     = QColor(255, 255, 255, 75);
    t.indRingHover = QColor(255, 255, 255, 120);
    t.indFill     = QColor(255, 255, 255, 8);
    t.indFillHover = QColor(255, 255, 255, 16);
    t.groove      = QColor(255, 255, 255, 16);
    t.rowHover    = QColor(255, 255, 255, 22);
    t.dateOff     = QColor(0x55, 0x59, 0x5F);
    return t;
}

namespace detail {
inline Theme& current()
{
    static Theme t = darkTheme();
    return t;
}
}  // namespace detail

inline const Theme& theme() { return detail::current(); }

// 主题 / 字体切换广播
class ThemeNotifier : public QObject {
    Q_OBJECT
public:
    static ThemeNotifier& instance()
    {
        static ThemeNotifier n;
        return n;
    }
    void notify() { emit changed(); }

signals:
    void changed();

private:
    ThemeNotifier() = default;
};

inline ThemeNotifier& themeNotifier() { return ThemeNotifier::instance(); }

// ---------------------------------------------------------------------------
// 便捷取色：全部读当前主题，避免各 UI 文件直接访问 Theme 成员
// ---------------------------------------------------------------------------
inline QColor cCard()        { return theme().card; }
inline QColor cCardHover()   { return theme().cardHover; }
inline QColor cBorder()      { return theme().border; }
inline QColor cBorderHover() { return theme().borderHover; }
inline QColor cText()        { return theme().text; }
inline QColor cTextSub()     { return theme().textSub; }
inline QColor cTextFaint()   { return theme().textFaint; }
inline QColor cUp()          { return theme().up; }
inline QColor cDown()        { return theme().down; }
inline QColor cDivider()     { return theme().divider; }

// 当前界面字体（切换字体走 setFontFamily()）
inline const QString& fontFamily() { return theme().fontFamily; }

inline QColor blend(const QColor& a, const QColor& b, double t)
{
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return QColor(int(a.red()   + (b.red()   - a.red())   * t),
                  int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue()  + (b.blue()  - a.blue())  * t),
                  int(a.alpha() + (b.alpha() - a.alpha()) * t));
}

// ---------------------------------------------------------------------------
// QSS 主题化：QSS 文本里写 %TOKEN%，由 themed() 替换成当前主题色
// ---------------------------------------------------------------------------
// Qt 的 QSS 只认 #RRGGBB 与 rgba()，不认 #AARRGGBB，所以这里分开输出
inline QString cssColor(const QColor& c)
{
    if (c.alpha() == 255)
        return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

inline QString themed(const QString& qss)
{
    const Theme& t = theme();
    const struct { const char* token; QColor color; } map[] = {
        {"%BORDER%", t.border},
        {"%HEADER_BG%", t.headerBg},
        {"%TEXT%", t.text},
        {"%TEXT2%", t.text2},
        {"%TEXT_SUB%", t.textSub},
        {"%TEXT_FAINT%", t.textFaint},
        {"%TEXT_OFF%", t.textOff},
        {"%TEXT_DIM%", t.textDim},
        {"%TEXT_HOVER%", t.textHover},
        {"%ON_ACCENT%", t.onAccent},
        {"%UP%", t.up},
        {"%DOWN%", t.down},
        {"%ACCENT%", t.accent},
        {"%ACCENT_SOFT%", t.accentSoft},
        {"%ACCENT_SOFT2%", t.accentSoft2},
        {"%ACCENT_SOFT3%", t.accentSoft3},
        {"%WARN%", t.warn},
        {"%WARN_HOVER%", t.warnHover},
        {"%DANGER%", t.danger},
        {"%POPUP_BG%", t.popupBg},
        {"%POPUP_SEL%", t.popupSel},
        {"%POPUP_BORDER%", t.popupBorder},
        {"%POPUP_SEP%", t.popupSep},
        {"%FIELD_BG%", t.fieldBg},
        {"%FIELD_BG_HOVER%", t.fieldBgHover},
        {"%FIELD_BG_SOFT%", t.fieldBgSoft},
        {"%FIELD_BORDER%", t.fieldBorder},
        {"%FIELD_BORDER_HOVER%", t.fieldBorderHover},
        {"%FIELD_BORDER_OFF%", t.fieldBorderOff},
        {"%ROW_ALT%", t.rowAlt},
        {"%SCROLL%", t.scroll},
        {"%SCROLL_HOVER%", t.scrollHover},
        {"%ROW_HOVER%", t.rowHover},
    };
    QString s = qss;
    for (const auto& m : map)
        s.replace(QLatin1String(m.token), cssColor(m.color));
    // 字体不是颜色，单独替换（QSS 里写 font-family: "%FONT%" 保证有空格的字体名可用）
    s.replace(QLatin1String("%FONT%"), theme().fontFamily);
    return s;
}

// ---------------------------------------------------------------------------
// 绘制工具
// ---------------------------------------------------------------------------
inline QPainterPath roundPath(const QRectF& r, qreal radius)
{
    QPainterPath p;
    p.addRoundedRect(r, radius, radius);
    return p;
}

// 圆角卡片 + 柔和外阴影 + 1px 描边
inline void drawCard(QPainter& p, const QRect& outer, qreal radius,
                     const QColor& bg, const QColor& border)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::NoBrush);

    const QRectF card = QRectF(outer).adjusted(kShadowPad, kShadowPad,
                                               -kShadowPad, -kShadowPad);
    for (int i = 5; i >= 1; --i) {
        const qreal g = i * 0.9;
        p.fillPath(roundPath(card.adjusted(-g, -g * 0.45, g, g * 1.05).translated(0, 1),
                             radius + g),
                   QColor(0, 0, 0, 15 - i * 2));
    }

    p.fillPath(roundPath(card, radius), bg);
    p.setPen(QPen(border, 1.0));
    p.drawPath(roundPath(card.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5));
}

// 实心三角形箭头（up=上传）
inline void drawArrow(QPainter& p, const QPointF& c, qreal s, const QColor& col, bool up)
{
    p.setPen(Qt::NoPen);
    p.setBrush(col);
    QPainterPath t;
    if (up) {
        t.moveTo(c.x(), c.y() - s * 0.55);
        t.lineTo(c.x() + s * 0.5, c.y() + s * 0.45);
        t.lineTo(c.x() - s * 0.5, c.y() + s * 0.45);
    } else {
        t.moveTo(c.x(), c.y() + s * 0.55);
        t.lineTo(c.x() + s * 0.5, c.y() - s * 0.45);
        t.lineTo(c.x() - s * 0.5, c.y() - s * 0.45);
    }
    t.closeSubpath();
    p.drawPath(t);
}

// 手绘"输入框 / 下拉按钮"外观：圆角底 + 描边 + 左对齐文字 + 右侧 ▾。
// DropDown（短列表，弹层用 QMenu）与 ListPicker（长列表，弹层用可滚动列表）共用，
// 保证两者外观完全一致。
inline void drawField(QPainter& p, const QRect& r, const QString& text,
                      bool hovered, bool enabled)
{
    const Theme& t = theme();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(!enabled ? t.fieldBorderOff
                           : (hovered ? t.fieldBorderHover : t.fieldBorder),
                 1.0));
    p.setBrush(enabled && hovered ? t.fieldBgHover : t.fieldBg);
    p.drawRoundedRect(box, 6.0, 6.0);

    QFont f(fontFamily());
    f.setPixelSize(12);
    p.setFont(f);
    p.setPen(!enabled ? t.textOff : t.text2);
    const QRect textR = r.adjusted(8, 0, -20, 0);
    p.drawText(textR, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(f).elidedText(text, Qt::ElideMiddle, textR.width()));

    // ▾（实心三角，不依赖字体里的箭头字形）
    drawArrow(p, QPointF(r.right() - 10.0, r.center().y() + 1.5), 8.0,
              enabled ? t.textSub : t.textOff, /*up=*/false);
}

// "1.23MB/s" -> num="1.23", unit="MB/s"
inline void splitNumberUnit(const QString& text, QString& num, QString& unit)
{
    int i = 0;
    while (i < text.size()
           && (text.at(i).isDigit() || text.at(i) == QLatin1Char('.')))
        ++i;
    num = text.left(i);
    unit = text.mid(i);
}

// ---------------------------------------------------------------------------
// 共用 QSS（都由主题生成）
// ---------------------------------------------------------------------------
// 自绘标题栏（ts::TitleBar）的样式：调用方并进自己的 QSS 即可
inline QString capQss()
{
    static const char* kQss =
        "#capTitle { color: %TEXT2%; font-size: 12px; font-weight: 600; }"
        "QPushButton#capMinBtn, QPushButton#capCloseBtn { border: none;"
        " background: transparent; color: %TEXT_SUB%; font-size: 14px;"
        " border-radius: 6px; padding: 0px; }"
        "QPushButton#capMinBtn:hover { background: %ROW_HOVER%;"
        " color: %TEXT%; }"
        "QPushButton#capCloseBtn:hover { background: %DANGER%; color: %ON_ACCENT%; }";
    return themed(QLatin1String(kQss));
}

// 右键菜单 / 下拉弹层（Fusion 自绘的那圈白边框被 QSS 完全接管）
inline QString menuQss()
{
    static const char* kQss =
        "QMenu { background: %POPUP_BG%; border: 1px solid %POPUP_BORDER%;"
        " border-radius: 10px; padding: 6px; color: %TEXT%; }"
        "QMenu::item { padding: 6px 20px 6px 14px; border-radius: 6px;"
        " font-family: \"%FONT%\"; font-size: 12px; }"
        "QMenu::item:selected { background: %POPUP_SEL%; }"
        "QMenu::item:disabled { color: %TEXT_OFF%; }"
        "QMenu::separator { height: 1px; background: %POPUP_SEP%;"
        " margin: 5px 8px; }"
        "QMenu::indicator { width: 14px; height: 14px; left: 6px; }";
    return themed(QLatin1String(kQss));
}

// QMenu 的四角圆角：菜单窗口默认带系统背景（就是一个矩形），QSS 的 border-radius
// 只圆了"画出来"的那块，四角会露出窗口本身的方角 —— 看着就是"圆弧没做好"。
// 必须把窗口做成无边框 + 半透明，圆角 + 描边才真正生效；顺带关掉系统阴影，
// 免得圆角外面再挂一圈方形阴影。所有 QMenu 都应该走这个函数（样式也一并套上）。
inline void polishMenu(QMenu& menu)
{
    menu.setWindowFlags(menu.windowFlags() | Qt::FramelessWindowHint
                        | Qt::NoDropShadowWindowHint);
    menu.setAttribute(Qt::WA_TranslucentBackground, true);
    menu.setStyleSheet(menuQss());
}

// 主/重置按钮：确认卡片（ConfirmCard.h）与设置弹窗（SettingsDialog）共用同一套外观
inline QString primaryButtonQss()
{
    static const char* kQss =
        "QPushButton { background: %FIELD_BG%; color: %TEXT2%;"
        " border: 1px solid %FIELD_BORDER%; border-radius: 6px;"
        " padding: 3px 14px; font-size: 12px; }"
        "QPushButton:hover { background: %FIELD_BG_HOVER%; color: %TEXT%; }"
        "QPushButton#primaryBtn { background: %ACCENT_SOFT2%; color: %TEXT%;"
        " border-color: %ACCENT%; }"
        "QPushButton#primaryBtn:hover { background: %ACCENT_SOFT3%; }"
        "QPushButton#resetBtn { background: %ACCENT_SOFT2%; color: %UP%;"
        " border-color: %UP%; }"
        "QPushButton#resetBtn:hover { background: %ACCENT_SOFT3%; }";
    return themed(QLatin1String(kQss));
}

// 文本输入框：ListPicker 的过滤框与设置弹窗的数字框共用（属性顺序统一，渲染结果一致）
inline QString lineEditQss()
{
    static const char* kQss =
        "QLineEdit { background: %FIELD_BG%; color: %TEXT2%;"
        " border: 1px solid %FIELD_BORDER%; border-radius: 6px;"
        " padding: 2px 8px; font-family: \"%FONT%\"; font-size: 12px;"
        " selection-background-color: %ACCENT_SOFT%; }"
        "QLineEdit:hover { border-color: %FIELD_BORDER_HOVER%; }"
        "QLineEdit:focus { border-color: %ACCENT%; color: %TEXT%; }";
    return themed(QLatin1String(kQss));
}

// 主题 / 字体切换后统一重绘：控件自身 + 全部子控件（QSS 换过之后必须重画）
inline void repaintAll(QWidget* w)
{
    for (QWidget* c : w->findChildren<QWidget*>())
        c->update();
    w->update();
}

// 可滚动列表弹层（ListPicker 用）：过滤框 + 列表 + 细滚动条。
// 为什么单独一套：几百项（系统字体）塞进 QMenu 会被 Qt 自动折成多列铺满屏幕，
// 又丑又难用；这里用带滚动条的列表 + 输入过滤。
// 过滤框样式与设置弹窗的输入框共用 lineEditQss()。
inline QString pickerQss()
{
    static const char* kQss =
        "QListWidget { background: transparent; border: none; color: %TEXT2%;"
        " font-family: \"%FONT%\"; font-size: 12px; outline: none; }"
        "QListWidget::item { padding: 4px 8px; border-radius: 5px; }"
        "QListWidget::item:hover { background: %FIELD_BG_HOVER%; }"
        "QListWidget::item:selected { background: %POPUP_SEL%; color: %TEXT%; }"
        "QScrollBar:vertical { background: transparent; width: 8px;"
        " margin: 2px 0px; }"
        "QScrollBar::handle:vertical { background: %SCROLL%; border-radius: 4px;"
        " min-height: 30px; }"
        "QScrollBar::handle:vertical:hover { background: %SCROLL_HOVER%; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical"
        " { height: 0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical"
        " { background: transparent; }";
    return themed(QLatin1String(kQss)) + lineEditQss();
}

// ---------------------------------------------------------------------------
// 切换界面字体（放在最后：要用到上面的东西）
// ---------------------------------------------------------------------------
// 改主题里的字体 → 刷 QApplication 字体（QSS 里的控件跟着变）
// → 广播，各控件 restyle() 重套 QSS（%FONT% 被替换成新字体），手绘部分在 paint 时
// 读 ts::fontFamily()，也是自动跟随。
// 持久化由调用方负责（settings.ini 的 [UI] fontFamily，见 core/Settings.h）。
inline void setFontFamily(const QString& family)
{
    const QString f = family.trimmed();
    if (f.isEmpty())
        return;
    Theme& t = detail::current();
    if (t.fontFamily == f)
        return;
    t.fontFamily = f;
    if (QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        QFont font = app->font();
        font.setFamily(f);   // 只换字体，字号保留（默认 9pt）
        app->setFont(font);
    }
    themeNotifier().notify();
}

}  // namespace ts
