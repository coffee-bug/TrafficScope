#pragma once
// 程序图标：托盘、窗口/任务栏、自绘标题栏共用同一枚 —— ↑橙 ↓绿，与浮框里的箭头同源。
//
// 纯头文件（无 Q_OBJECT），不必列进 CMakeLists 的源列表。
// 图标按当前主题色现画，所以切换暗/浅主题时颜色会跟着变（托盘那枚由 FloatWidget
// 在主题广播里重设，窗口那枚由 main.cpp 重设）。
// resources/trafficscope.ico 是这份画法在暗色主题下的静态快照，只用于 exe 文件本身
// （资源管理器 / 任务管理器 / 任务计划程序里显示的那枚）。

#include <QPainter>
#include <QPixmap>
#include <QIcon>

#include "UiTheme.h"

namespace ts {

// 单尺寸图标：透明底 s×s，画法与原来的托盘图标完全一致
inline QPixmap appIconPixmap(int s)
{
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawArrow(p, QPointF(s / 2.0, s * 0.28), s * 0.34, cUp(), true);
    drawArrow(p, QPointF(s / 2.0, s * 0.72), s * 0.34, cDown(), false);
    return pm;
}

// 多尺寸图标：系统按场景挑最合适的一档（托盘 16、任务栏 32、Alt+Tab 大图 …）
inline QIcon appIcon()
{
    QIcon ic;
    const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };
    for (int s : sizes)
        ic.addPixmap(appIconPixmap(s));
    return ic;
}

}  // namespace ts
