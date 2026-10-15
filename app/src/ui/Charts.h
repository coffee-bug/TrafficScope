#pragma once
// 自绘图表：实时速率曲线（折线 + 面积）与历史趋势柱状图（上下行堆叠）。
//
// 为什么不用 QChart：会把 QtCharts 模块整个拉进来（要改 CMake、windeployqt 多拷一堆
// DLL），而且 QSS 管不到它内部，配色得额外手写一套。这里只要两条折线和一排柱子，
// 自绘更轻，颜色直接读 ts::theme()，换主题自动跟随。
//
// 纯头文件、类里无 Q_OBJECT（AUTOMOC 不需要处理，因此不用改 CMakeLists）。

#include "UiTheme.h"
#include "core/StatsDb.h"
#include "core/TrafficStore.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <algorithm>

namespace ts {

constexpr int kSpeedGraphH = 76;     // 实时曲线高（含上下留白）
constexpr int kHistoryChartH = 112;  // 历史柱状图高（含 X 轴标签一行）

namespace chart_detail {

// 取一个"好看"的上界：1 / 2 / 5 × 10^n（比原始最大值略大，纵轴标签才不会是 1374B/s 这种数）
inline qint64 niceCeil(qint64 v)
{
    if (v <= 0)
        return 0;
    qint64 p = 1;
    while (p < v)
        p *= 10;
    const qint64 base = p / 10;
    if (base <= 0)
        return p;
    const qint64 cands[] = {base, base * 2, base * 5, p};
    for (qint64 c : cands) {
        if (c >= v)
            return c;
    }
    return p;
}

// 绘图区底色（两块图表共用同一块"深色底 + 圆角"）
inline void drawPlotBg(QPainter& p, const QRectF& plot)
{
    p.setPen(Qt::NoPen);
    p.setBrush(theme().fieldBgSoft);
    p.drawRoundedRect(plot, 8, 8);
}

// 两条水平网格线（三等分，实时曲线与历史柱状图共用）
inline void drawGrid(QPainter& p, const QRectF& plot)
{
    QPen gridPen(theme().divider);
    gridPen.setCosmetic(true);
    p.setPen(gridPen);
    for (int i = 1; i < 3; ++i) {
        const qreal y = plot.top() + plot.height() * i / 3.0;
        p.drawLine(QLineF(plot.left(), y, plot.right(), y));
    }
}

// 空态提示（没有数据时在两块图表里显示同一句话）
// center=true 时居中（历史柱状图）；默认左对齐，保持实时曲线原来的样子
inline void drawEmptyHint(QPainter& p, const QRectF& plot, bool center = false)
{
    p.setPen(theme().textFaint);
    p.drawText(plot.adjusted(6, 0, -6, 0),
               center ? Qt::AlignCenter : (Qt::AlignLeft | Qt::AlignVCenter),
               QStringLiteral("暂无数据"));
}

// 在 plot 区域内画一条折线；valueAt 返回第 i 个样本的值，scale 为纵轴上界
inline void drawPolyline(QPainter& p, const QRectF& plot, int cap, qint64 scale,
                         const QVector<quint64>& data, const QColor& color,
                         bool fillArea)
{
    if (data.isEmpty() || scale <= 0)
        return;

    const int n = data.size();
    QVector<QPointF> pts;
    pts.reserve(n);
    const qreal step = plot.width() / qreal(std::max(cap - 1, 1));
    for (int i = 0; i < n; ++i) {
        const qreal x = plot.left() + i * step;
        const qreal y = plot.bottom()
                      - plot.height() * qMin(1.0, double(data.at(i)) / double(scale));
        pts.append(QPointF(x, y));
    }

    if (fillArea) {
        QPainterPath path;
        path.moveTo(pts.first().x(), plot.bottom());
        for (const QPointF& pt : pts)
            path.lineTo(pt);
        path.lineTo(pts.last().x(), plot.bottom());
        path.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(color.red(), color.green(), color.blue(), 46));
        p.drawPath(path);
    }

    QPen pen(color);
    pen.setWidthF(1.6);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    for (int i = 1; i < pts.size(); ++i)
        p.drawLine(pts.at(i - 1), pts.at(i));

    // 最后一点画个小圆点，读数更直观
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    const QPointF& last = pts.last();
    p.drawEllipse(last, 2.2, 2.2);
}

// 鼠标横坐标（Qt5/Qt6 取值 API 不同）
inline qreal mousePosX(QMouseEvent* e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position().x();
#else
    return qreal(e->x());
#endif
}

}  // namespace chart_detail

// 三个绘制小工具：图表类本身不在 chart_detail 里，这里放开名字好让类内直接调用
using chart_detail::drawEmptyHint;
using chart_detail::drawGrid;
using chart_detail::drawPlotBg;

// ---------------------------------------------------------------------------
// 实时速率曲线：环形缓冲存最近 N 拍的 ↑ / ↓ 速率
// ---------------------------------------------------------------------------
class SpeedGraph : public QWidget {
public:
    explicit SpeedGraph(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedHeight(kSpeedGraphH);
        setMinimumWidth(120);
    }

    void push(quint64 up, quint64 down)
    {
        if (m_up.size() >= kCap) {
            m_up.removeFirst();
            m_down.removeFirst();
        }
        m_up.append(up);
        m_down.append(down);
        // 纵轴自适应。关键：**目标值只看最近的几个样本**，不能拿整个缓冲窗口的最大值
        // —— 否则峰值样本在缓冲里待多久，上界就被它钉多久（回落会被 want 顶住），
        // 表现就是"测速一次之后两分钟内两条线都贴地"。
        const int n = m_up.size();
        const int from = std::max(0, n - kScaleRecent);
        quint64 want = 0;
        for (int i = from; i < n; ++i)
            want = std::max(want, std::max(m_up.at(i), m_down.at(i)));
        // 上升立刻跟上（突发要画得出来）；下降每拍压 22%，峰值过后约 10 秒落回当前量级
        if (double(want) >= m_scale)
            m_scale = double(want);
        else
            m_scale = std::max(double(want), m_scale * kScaleDecay);
        // 下限：空闲时不塌缩到 0（否则除 0，且明明有历史曲线却显示"暂无数据"）
        if (m_scale < kMinScale)
            m_scale = kMinScale;
        update();
    }

    QSize sizeHint() const override { return QSize(320, kSpeedGraphH); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal padX = 8.0;
        const QRectF plot(rect().left() + padX, rect().top() + 4.0,
                          rect().width() - 2 * padX, kSpeedGraphH - 12.0);
        if (plot.width() <= 0 || plot.height() <= 0)
            return;

        drawPlotBg(p, plot);
        drawGrid(p, plot);

        // 纵轴上界用**自适应值** m_scale（见 push）：目标只看最近几个样本并持续回落，
        // 突发流量过去之后曲线会自己重新"长"起来。
        if (m_up.isEmpty()) {
            drawEmptyHint(p, plot);
            return;
        }

        const qint64 top = chart_detail::niceCeil(qint64(m_scale * 1.1));
        chart_detail::drawPolyline(p, plot, kCap, top, m_down, cDown(), true);
        chart_detail::drawPolyline(p, plot, kCap, top, m_up, cUp(), false);
    }

private:
    QVector<quint64> m_up;
    QVector<quint64> m_down;
    static constexpr int kCap = 120;   // 缓冲拍数（约 2 分钟的曲线）
    double m_scale = 0.0;   // 纵轴自适应上界（bytes/s）
    // 目标值参考的最近样本数：8 拍 ≈ 8 秒（随刷新周期变化）
    static constexpr int kScaleRecent = 8;
    // 回落系数：每拍把上界往目标值压 22%（10 倍量级约 9 拍落完）
    static constexpr double kScaleDecay = 0.78;
    // 纵轴下限：流量为零时曲线平贴底部但不塌缩
    static constexpr double kMinScale = 1024.0;
};

// ---------------------------------------------------------------------------
// 历史趋势柱状图：每个时间桶一根柱子，下载（绿）在下、上传（橙）在上，堆叠显示
// ---------------------------------------------------------------------------
class HistoryChart : public QWidget {
public:
    explicit HistoryChart(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedHeight(kHistoryChartH);
        setMinimumWidth(160);
        // 悬停信息框：不按下鼠标也要收到 move 事件
        setMouseTracking(true);
    }

    // byDay：true = X 轴按日期标注（MM-DD），false = 按小时标注（HH:mm）
    void setData(QVector<StatsDb::Point> pts, bool byDay)
    {
        m_pts = std::move(pts);
        m_byDay = byDay;
        m_hover = -1;   // 数据换了，旧下标不再对应同一根柱子
        update();
    }

    QSize sizeHint() const override { return QSize(320, kHistoryChartH); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal labelH = 16.0;
        const QRectF plot = plotRect();
        if (plot.width() <= 0 || plot.height() <= 0)
            return;

        drawPlotBg(p, plot);

        if (m_pts.isEmpty()) {
            drawEmptyHint(p, plot, true);   // 柱状图的空态居中，实时曲线仍左对齐
            return;
        }

        quint64 maxV = 0;
        for (const StatsDb::Point& pt : m_pts)
            maxV = std::max(maxV, pt.up + pt.down);
        const qint64 top = chart_detail::niceCeil(qint64(double(maxV) * 1.2));
        if (top <= 0)
            return;

        const int n = m_pts.size();
        const qreal slot = plot.width() / qreal(n);
        const qreal barW = std::max(2.0, slot * 0.62);

        drawGrid(p, plot);

        // 悬停列高亮（画在柱子下面，半透明白，主题跟随）
        if (m_hover >= 0 && m_hover < n) {
            p.setPen(Qt::NoPen);
            p.setBrush(theme().rowHover);
            p.drawRoundedRect(QRectF(plot.left() + m_hover * slot, plot.top(),
                                     slot, plot.height()), 4, 4);
        }

        p.setPen(Qt::NoPen);
        for (int i = 0; i < n; ++i) {
            const StatsDb::Point& pt = m_pts.at(i);
            const qreal x = plot.left() + i * slot + (slot - barW) / 2.0;
            const qreal hD = plot.height() * qMin(1.0, double(pt.down) / double(top));
            const qreal hU = plot.height() * qMin(1.0, double(pt.up) / double(top));
            if (hD > 0) {
                p.setBrush(cDown());
                p.drawRoundedRect(QRectF(x, plot.bottom() - hD, barW, hD), 1.5, 1.5);
            }
            if (hU > 0) {
                p.setBrush(cUp());
                p.drawRoundedRect(QRectF(x, plot.bottom() - hD - hU, barW, hU), 1.5, 1.5);
            }
        }

        // X 轴：首 / 末两个桶的时间标签（柱子多时只标两端，避免糊成一片）
        QFont f = p.font();
        f.setPointSizeF(8.5);
        p.setFont(f);
        p.setPen(theme().textFaint);
        const QString fmt = m_byDay ? QStringLiteral("MM-dd") : QStringLiteral("HH:mm");
        const QDateTime dtFrom = QDateTime::fromSecsSinceEpoch(m_pts.first().ts);
        const QDateTime dtTo = QDateTime::fromSecsSinceEpoch(m_pts.last().ts);
        p.drawText(QRectF(plot.left(), plot.bottom() + 2, 80, labelH),
                   Qt::AlignLeft | Qt::AlignVCenter, dtFrom.toString(fmt));
        p.drawText(QRectF(plot.right() - 80, plot.bottom() + 2, 80, labelH),
                   Qt::AlignRight | Qt::AlignVCenter, dtTo.toString(fmt));

        drawHoverBox(p, plot, slot);
    }

    // 悬停时在柱子附近弹一个小信息卡片（时间 + ↓下载 + ↑上传 + 合计）。
    // 不用 QToolTip：无边框 + 半透明窗口里 QToolTip 出不来气泡，得自己画卡片。
    void drawHoverBox(QPainter& p, const QRectF& plot, qreal slot)
    {
        if (m_hover < 0 || m_hover >= m_pts.size())
            return;
        const StatsDb::Point& pt = m_pts.at(m_hover);

        const QString title = QDateTime::fromSecsSinceEpoch(pt.ts).toString(
            m_byDay ? QStringLiteral("MM-dd") : QStringLiteral("MM-dd HH:mm"));
        const QStringList rows = {
            title,
            QStringLiteral("↓ %1").arg(formatBytes(pt.down)),
            QStringLiteral("↑ %1").arg(formatBytes(pt.up)),
            QStringLiteral("共 %1").arg(formatBytes(pt.up + pt.down)),
        };

        QFont f = p.font();
        f.setPointSizeF(9.0);
        p.setFont(f);
        const QFontMetrics fm(f);
        int textW = 0;
        for (const QString& s : rows)
            textW = std::max(textW, fm.horizontalAdvance(s));
        const int padX = 10;
        const int padY = 7;
        const int lineH = fm.height() + 3;
        const qreal boxW = textW + padX * 2.0;
        const qreal boxH = padY * 2.0 + lineH * rows.size();

        // 盒子水平居中于悬停柱，夹在绘图区内；垂直贴绘图区顶部（柱子从底往上长，顶部基本是空的）
        qreal bx = plot.left() + m_hover * slot + slot / 2.0 - boxW / 2.0;
        bx = qBound(plot.left(), bx, plot.right() - boxW);
        const qreal by = plot.top() + 4.0;

        p.setPen(QPen(theme().popupBorder, 1));
        p.setBrush(theme().popupBg);
        p.drawRoundedRect(QRectF(bx, by, boxW, boxH), 6, 6);

        qreal ty = by + padY;
        for (int i = 0; i < rows.size(); ++i) {
            // 颜色：时间弱化，↓绿 ↑橙，合计用正文色
            p.setPen(i == 0   ? theme().textSub
                     : i == 1 ? cDown()
                     : i == 2 ? cUp()
                              : theme().text);
            p.drawText(QRectF(bx + padX, ty, textW, lineH),
                       Qt::AlignLeft | Qt::AlignVCenter, rows.at(i));
            ty += lineH;
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int idx = barAt(chart_detail::mousePosX(e));
        if (idx != m_hover) {
            m_hover = idx;
            update();
        }
    }

    void leaveEvent(QEvent* e) override
    {
        if (m_hover != -1) {
            m_hover = -1;
            update();
        }
        QWidget::leaveEvent(e);
    }

private:
    // 绘图区几何：paintEvent 与悬停命中测试必须用同一份，否则手指的位置和柱子对不上
    QRectF plotRect() const
    {
        const qreal padX = 8.0;
        const qreal labelH = 16.0;
        return QRectF(rect().left() + padX, rect().top() + 4.0,
                      rect().width() - 2 * padX, kHistoryChartH - 8.0 - labelH);
    }

    // 横坐标落在哪根柱子的槽位上；不在绘图区内返回 -1
    int barAt(qreal x) const
    {
        if (m_pts.isEmpty())
            return -1;
        const QRectF plot = plotRect();
        if (x < plot.left() || x > plot.right())
            return -1;
        const qreal slot = plot.width() / qreal(m_pts.size());
        return qBound(0, int((x - plot.left()) / slot), m_pts.size() - 1);
    }

    QVector<StatsDb::Point> m_pts;
    bool m_byDay = true;
    int m_hover = -1;
};

}  // namespace ts
