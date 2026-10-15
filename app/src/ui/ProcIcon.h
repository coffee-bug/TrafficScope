#pragma once
// 进程 exe 图标：浮框榜单（TopPanel）与详情弹窗（DetailDialog）共用同一份实现。
//
// 两处都按"进程名"缓存（同名多进程取流量最大的那个 PID 的 exe），各留一份缓存 ——
// 它们分属不同控件的生命周期，不共享数据，只共享取图标的逻辑。
// 纯头文件（无 Q_OBJECT），不必列进 CMakeLists 的源列表。

#include "core/ProcTraffic.h"

#include <QFileIconProvider>
#include <QFileInfo>
#include <QHash>
#include <QIcon>
#include <QString>

namespace ts {

inline QIcon procIcon(const QString& name, quint64 pid, QHash<QString, QIcon>& cache)
{
    auto it = cache.find(name);
    if (it != cache.end())
        return *it;

    QFileIconProvider provider;
    QIcon ic;
    const QString path = ProcTraffic::instance().pathFor(pid);
    if (!path.isEmpty())
        ic = provider.icon(QFileInfo(path));
    if (ic.isNull())
        ic = provider.icon(QFileIconProvider::File);   // 取不到就退化为通用文件图标

    cache.insert(name, ic);
    return ic;
}

}  // namespace ts
