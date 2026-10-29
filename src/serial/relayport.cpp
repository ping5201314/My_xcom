#include "serial/relayport.h"

#include <QDir>
#include <QFile>
#include <QThread>

#include "serial/serialconfig.h"

namespace {
// 中转写轮询切片（ms）：CLI 等待 GUI 消费/等待 RX 数据的粒度
constexpr int kPollSliceMs = 5;
// RX 镜像文件上限：会话内设备回包远小于此值，超限截断防失控膨胀
constexpr qint64 kRxFileCap = 1 * 1024 * 1024;
}

RelayPort::~RelayPort()
{
    close();
}

QString RelayPort::txPartPath() { return QDir::tempPath() + QStringLiteral("/my_com_ai_tx.part"); }
QString RelayPort::txRdyPath()  { return QDir::tempPath() + QStringLiteral("/my_com_ai_tx.rdy"); }
QString RelayPort::rxPath()     { return QDir::tempPath() + QStringLiteral("/my_com_ai_rx.txt"); }
QString RelayPort::flagPath()   { return QDir::tempPath() + QStringLiteral("/my_com_ai_relay.flag"); }

bool RelayPort::open(const SerialConfig& cfg)
{
    close(); // 清理上一个会话的残留（含过期标记与半包）
    m_cfg = cfg;
    // 截断 RX 镜像，保证本次会话从干净位置开始读
    {
        QFile f(rxPath());
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.close();
    }
    m_rxOff = 0;
    // 建立会话标记：GUI 见到标记即开始把设备回包镜像到 RX 文件
    QFile flag(flagPath());
    if (!flag.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    flag.close();
    m_open = true;
    return true;
}

void RelayPort::close()
{
    if (!m_open)
        return;
    QFile::remove(flagPath());
    QFile::remove(txPartPath());
    QFile::remove(txRdyPath());
    m_open = false;
}

bool RelayPort::isOpen() const
{
    return m_open;
}

SerialConfig RelayPort::config() const
{
    return m_cfg;
}

qint64 RelayPort::write(const QByteArray& data)
{
    return writeSync(data, 2000);
}

qint64 RelayPort::writeSync(const QByteArray& data, int timeoutMs)
{
    if (!m_open || data.isEmpty())
        return -1;

    // 1) 等上一笔被 GUI 消费完（.rdy 消失），避免两笔数据互踩
    {
        int waited = 0;
        while (QFile::exists(txRdyPath()) && waited < timeoutMs / 2) {
            QThread::msleep(kPollSliceMs);
            waited += kPollSliceMs;
        }
        if (QFile::exists(txRdyPath())) {
            QFile::remove(txPartPath());
            QFile::remove(txRdyPath());
            return -1;
        }
    }

    // 2) 先写 .part 全量、关文件，再落 .rdy 就绪标记——GUI 只认 .rdy 存在后才读，
    //    从根本上避免读到半包
    {
        QFile part(txPartPath());
        if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return -1;
        const qint64 n = part.write(data);
        part.close();
        if (n != data.size()) {
            QFile::remove(txPartPath());
            return -1;
        }
    }
    {
        QFile rdy(txRdyPath());
        if (!rdy.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QFile::remove(txPartPath());
            return -1;
        }
        rdy.close();
    }

    // 3) 等 GUI 消费（读 .part 代发后删除两文件）
    {
        int waited = 0;
        while (QFile::exists(txRdyPath()) && waited < timeoutMs) {
            QThread::msleep(kPollSliceMs);
            waited += kPollSliceMs;
        }
        if (QFile::exists(txRdyPath())) {
            QFile::remove(txPartPath());
            QFile::remove(txRdyPath());
            return -1; // 超时：GUI 未运行/串口未开
        }
    }
    return data.size();
}

QByteArray RelayPort::readBytes(qint64 maxSize, int timeoutMs)
{
    if (!m_open)
        return QByteArray();

    int waited = 0;
    for (;;) {
        QFile f(rxPath());
        if (f.open(QIODevice::ReadOnly)) {
            const qint64 size = f.size();
            if (size < m_rxOff) {
                m_rxOff = 0; // 文件被新会话截断，从头读
            } else if (size > m_rxOff) {
                f.seek(m_rxOff);
                const qint64 avail = qMin<qint64>(maxSize, size - m_rxOff);
                const QByteArray data = f.read(avail);
                m_rxOff += data.size();
                f.close();
                return data;
            }
            f.close();
        }
        waited += kPollSliceMs;
        if (waited >= timeoutMs)
            return QByteArray(); // 超时
        QThread::msleep(kPollSliceMs);
    }
}

void RelayPort::beginExclusive(bool purge)
{
    Q_UNUSED(purge);
    // 中转模式下串口归 GUI 管，独占/清场语义由 GUI 正常接收路径承担，无需动作
}

void RelayPort::endExclusive()
{
    // 空操作
}
