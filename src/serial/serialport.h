#pragma once

// Win32 串口封装：CreateFile + DCB + COMMTIMEOUTS + Overlapped I/O + 后台接收线程。
// 设计目标（见 Finca/docs/05c_OTA审查_上位机.md）：
//   - 同步读 writeSync/readBytes 必须支撑 1s 级协议超时判定（分辨率 ≤10ms，timeBeginPeriod 提精度）
//   - 独占模式：升级传输期间接收字节进内部缓冲供 readBytes 消费，不打扰终端显示
//   - 接收线程可干净中止（事件 + 原子标志 + CancelIoEx），拔线等错误以可读文本上报
//
// 本头文件会被 Qt 界面代码包含，因此 Qt 头在前、windows.h 在后并关闭宏污染。

#include <QObject>
#include <QByteArray>
#include <QMutex>
#include <QStringList>

#include <atomic>
#include <mutex>

#include "serial/serialconfig.h" // 值成员 m_cfg 需要完整类型（该头不依赖 windows.h，可安全包含）

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

class QThread;

class SerialPort : public QObject
{
    Q_OBJECT
public:
    explicit SerialPort(QObject* parent = nullptr);
    ~SerialPort() override;

    /// 枚举可用串口，返回友好名（如 "COM3 (USB-SERIAL CH340)"）。
    /// SetupAPI/注册表都拿不到时退回 COM1~COM16 逐一试探。
    static QStringList availablePorts();

    /// 按配置打开串口；失败时发出 errorOccurred 并返回 false
    bool open(const SerialConfig& cfg);
    /// 关闭串口、回收接收线程与句柄；可重复调用
    void close();
    bool isOpen() const;
    /// 当前生效配置（未打开时为空配置）。返回副本：内部持锁快照后拷贝，
    /// 升级工作线程与本类调用方并发访问时不会拿到半更新状态（修复 C3）
    SerialConfig config() const;

    /// 非阻塞写：交给驱动后立即返回（失败发出 errorOccurred）
    qint64 write(const QByteArray& data);
    /// 阻塞写：最多等 timeoutMs 毫秒，超时/失败返回 -1（升级协议专用）
    qint64 writeSync(const QByteArray& data, int timeoutMs);
    /// 同步读：阻塞至收到数据或超时，返回已读字节；空返回 = 超时。
    /// 须在 beginExclusive() 之后使用（升级协议专用）。
    QByteArray readBytes(qint64 maxSize, int timeoutMs);

    /// 独占模式开始：此后收到的字节全部进内部缓冲供 readBytes 消费，不再发 dataReceived；
    /// 同时清空驱动与内部积压字节，保证升级握手从干净状态开始（OTA 文档 D12/B2）
    /// purge=true（默认）：清场后进独占——发 @W12 前保证 'C' 握手从干净状态开始（D12/B2）；
    /// purge=false：只进独占不清场——升级完成后的"等待设备重启"阶段用，BOOT 打印
    /// （boot version/updata flag）不会被吞，完整进日志供排障
    void beginExclusive(bool purge = true);
    /// 独占模式结束：缓冲中剩余字节一次性以 dataReceived 发出，恢复正常模式
    void endExclusive();

signals:
    void dataReceived(const QByteArray& data);
    void errorOccurred(const QString& message);
    void opened(const SerialConfig& cfg); // 打开成功
    void closed();                        // 已关闭（含接收线程致命错误后的自动关闭）

private:
    class RxThread; // 后台接收线程（QThread 子类，定义在 cpp）

    /// 接收线程回调：独占模式入缓冲，正常模式直接转 dataReceived
    void handleRxData(const char* data, qint64 len);

    // 修复 C3：句柄/配置/接收线程生命周期互斥。SerialPort 被主线程（终端页开关串口、
    // RxThread 致命错误经队列触发的 close()）与升级工作线程（open/close/writeSync/
    // readBytes）并发调用，m_handle 存在"isOpen 检查与 WriteFile 之间被换句柄"的 TOCTOU，
    // 以及句柄值被系统复用后写进新会话的风险。open/close/isOpen/config/write/writeSync/
    // beginExclusive 共用本锁；递归锁是因为 open() 内部会调 close()（已打开时先关再开）。
    // 锁序约定：m_apiMutex → {m_writeMutex, m_bufMutex}，反向获取不存在，无死锁环。
    mutable std::recursive_mutex m_apiMutex;

    HANDLE m_handle = INVALID_HANDLE_VALUE;
    SerialConfig m_cfg;

    HANDLE m_rxEvent = nullptr;       // 接收 Overlapped 完成事件
    HANDLE m_writeEvent = nullptr;    // 写 Overlapped 完成事件
    HANDLE m_rxNotifyEvent = nullptr; // “缓冲有新数据”通知事件（readBytes 等待用）

    QMutex m_bufMutex;
    QByteArray m_rxBuf;            // 独占模式积压区；正常模式瞬时清空
    std::mutex m_writeMutex;       // 串行化写操作（write 与 writeSync 可能并发）

    std::atomic<bool> m_abort{false};     // 接收线程中止标志
    std::atomic<bool> m_exclusive{false}; // 独占模式标志
    bool m_timerBoosted = false;          // 是否已调用 timeBeginPeriod(1)
    RxThread* m_thread = nullptr;
};
