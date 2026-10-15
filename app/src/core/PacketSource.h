#pragma once

#include "IpKey.h"
#include "EtwPacketSource.h"

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <cstdint>

// 基于系统原生 ETW (Microsoft-Windows-NDIS-PacketCapture) 的抓包引擎：
// 无需任何内核驱动或 Npcap 依赖。对外保持完全兼容的网卡选择与监控接口。
class PacketSource : public QObject {
    Q_OBJECT
public:
    static PacketSource& instance();
    ~PacketSource() override;

    static QStringList availableDevices();

    // deviceName 为空时自动选择（跟随默认路由）
    bool startCapture(QString& err, const QString& deviceName = QString());
    void stopCapture();
    bool restart(const QString& deviceName, QString& err);

    bool isRunning() const { return m_etwSource.isRunning(); }

    // ---- 网卡选择 ----
    bool autoSelect() const { return m_auto; }
    void setAutoSelect(bool on);
    // 手工指定网卡（自动关闭自动模式）；空串表示恢复自动
    bool selectDevice(const QString& deviceName, QString& err);

    QString currentDevice() const { return m_deviceName; }
    QString deviceDescription() const { return m_deviceDesc; }

    // 诊断：抓到的包总数 / 成功计入统计的包数。
    quint64 packetTotal() const { return m_pktTotal.load(std::memory_order_relaxed); }
    quint64 packetCounted() const { return m_pktCounted.load(std::memory_order_relaxed); }

signals:
    void captureError(const QString& msg);
    void deviceChanged(const QString& device);

private:
    PacketSource();

    void handlePacket(const uint8_t* bytes, uint32_t len, uint32_t ifIndex);
    void updateLocalAddresses();

private slots:
    void onRecheck(); // 周期检查默认路由是否变化，变化则自动重挂

private:
    ts::EtwPacketSource m_etwSource;
    QString m_deviceName;
    QString m_deviceDesc;
    uint32_t m_currentIfIndex = 0;
    QSet<IpKey> m_localSet; // 本机所有网卡的地址（IPv4 以 mapped 形式存放）
    QTimer m_recheck;
    bool m_auto = true;
    std::atomic<quint64> m_pktTotal{0};
    std::atomic<quint64> m_pktCounted{0};
};
