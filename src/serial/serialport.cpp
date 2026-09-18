#include "serial/serialport.h"
#include "serial/serialconfig.h"

#include <QDebug>
#include <QMap>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QThread>

#include <chrono>

#include <mmsystem.h> // timeBeginPeriod/timeEndPeriod
#include <setupapi.h> // SetupAPI：枚举“端口”设备类

// ---------------------------------------------------------------------------
// 工具函数
// ---------------------------------------------------------------------------

// 把 Win32 错误码转成可读文本（FormatMessage），拿不到描述时退化为十六进制码
static QString winErrorString(DWORD code)
{
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                       | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    QString text;
    if (buf) {
        text = QString::fromWCharArray(buf, static_cast<int>(n)).trimmed();
        LocalFree(buf);
    }
    if (text.isEmpty())
        text = QStringLiteral("未知错误");
    return QStringLiteral("%1 (错误码 %2)").arg(text).arg(code);
}

// ---------------------------------------------------------------------------
// 接收线程
// ---------------------------------------------------------------------------

// QThread 子类：run() 里跑“发起 Overlapped 读 → 20ms 分片等待 → 处理/中止检查”循环。
// 20ms 分片的意义：WaitForSingleObject 带超时可随时检查中止标志，线程退出延迟 ≤20ms，
// 同时一个事件一个等待者，天然串行，不需要额外锁。
class SerialPort::RxThread : public QThread
{
public:
    explicit RxThread(SerialPort* owner) : m_owner(owner) {}

protected:
    void run() override
    {
        char buf[4096]; // 单块缓冲循环复用：115200 下 20ms 最多 ~300 字节，4KB 足够
        while (!m_owner->m_abort.load(std::memory_order_relaxed)) {
            OVERLAPPED ov{};
            ov.hEvent = m_owner->m_rxEvent;
            DWORD got = 0;
            if (ReadFile(m_owner->m_handle, buf, sizeof(buf), &got, &ov)) {
                // 偶发的同步完成（数据已在驱动缓冲）
                if (got > 0)
                    m_owner->handleRxData(buf, got);
                continue;
            }
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) {
                // close()/PurgeComm 取消：回循环顶看中止标志决定去留
                continue;
            }
            if (err != ERROR_IO_PENDING) {
                // 真实错误（设备拔出、句柄失效等）：报错并让主线程统一收尾。
                // 不能在这里直接调 close()（会 join 自己），用队列调用转到属主线程执行。
                emit m_owner->errorOccurred(
                    QStringLiteral("串口接收失败：%1").arg(winErrorString(err)));
                QMetaObject::invokeMethod(m_owner, [owner = m_owner] { owner->close(); },
                                          Qt::QueuedConnection);
                return;
            }
            // Overlapped 读已挂起：20ms 分片等待，兼顾及时性与可中止性
            bool bail = false;
            while (!bail) {
                const DWORD w = WaitForSingleObject(m_owner->m_rxEvent, 20);
                if (w == WAIT_OBJECT_0) {
                    DWORD got2 = 0;
                    if (GetOverlappedResult(m_owner->m_handle, &ov, &got2, FALSE)) {
                        if (got2 > 0)
                            m_owner->handleRxData(buf, got2);
                    } else {
                        const DWORD e2 = GetLastError();
                        if (e2 != ERROR_OPERATION_ABORTED) {
                            // 非“取消”类失败：按致命错误处理
                            emit m_owner->errorOccurred(
                                QStringLiteral("串口接收失败：%1").arg(winErrorString(e2)));
                            QMetaObject::invokeMethod(
                                m_owner, [owner = m_owner] { owner->close(); },
                                Qt::QueuedConnection);
                        }
                        // ERROR_OPERATION_ABORTED：close/PurgeComm 取消 → 回循环顶看标志
                    }
                    bail = true;
                } else if (w == WAIT_TIMEOUT) {
                    if (m_owner->m_abort.load(std::memory_order_relaxed)) {
                        // 中止：取消挂起的读，让事件立刻完成，本线程尽快退出
                        CancelIoEx(m_owner->m_handle, nullptr);
                        bail = true;
                    }
                } else {
                    // WAIT_FAILED 理论上不会发生，按致命错误处理
                    emit m_owner->errorOccurred(QStringLiteral("串口等待失败：%1")
                                                    .arg(winErrorString(GetLastError())));
                    QMetaObject::invokeMethod(m_owner, [owner = m_owner] { owner->close(); },
                                              Qt::QueuedConnection);
                    bail = true;
                }
            }
        }
    }

private:
    SerialPort* m_owner;
};

// ---------------------------------------------------------------------------
// SerialPort
// ---------------------------------------------------------------------------

SerialPort::SerialPort(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<SerialConfig>("SerialConfig");
    // 三个事件都用自动复位：一个等待者消耗一次通知，语义最干净
    m_rxEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_writeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_rxNotifyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

SerialPort::~SerialPort()
{
    close();
    if (m_rxEvent)
        CloseHandle(m_rxEvent);
    if (m_writeEvent)
        CloseHandle(m_writeEvent);
    if (m_rxNotifyEvent)
        CloseHandle(m_rxNotifyEvent);
}

QStringList SerialPort::availablePorts()
{
    // 1) 注册表 HARDWARE\DEVICEMAP\SERIALCOMM：系统“登记在册”的串口 → 端口号。
    //    这是最全的清单来源（含蓝牙 SPP 等），SetupAPI 只负责补充友好名。
    QMap<int, QString> portNames; // 端口号 → "COMx"
    {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ,
                          &hk) == ERROR_SUCCESS) {
            for (DWORD idx = 0;; ++idx) {
                wchar_t value[64] = { 0 };
                DWORD valueLen = static_cast<DWORD>(sizeof(value));
                // 键名（\Device\Serial0 等）用不上，直接跳过
                if (RegEnumValueW(hk, idx, nullptr, nullptr, nullptr, nullptr,
                                  reinterpret_cast<LPBYTE>(value), &valueLen) != ERROR_SUCCESS)
                    break;
                const QString port = QString::fromWCharArray(value);
                if (port.startsWith(QStringLiteral("COM"), Qt::CaseInsensitive)) {
                    bool ok = false;
                    const int num = port.mid(3).toInt(&ok);
                    if (ok && num > 0)
                        portNames.insert(num, port);
                }
            }
            RegCloseKey(hk);
        }
    }
    // 2) SetupAPI 按“Ports”设备类枚举友好名（如 "USB-SERIAL CH340 (COM3)"），
    //    用其中的 (COMx) 与注册表清单对应。用类名枚举而非 GUID，避免 MinGW 下
    //    DEFINE_GUID/initguid 的链接麻烦。
    QMap<int, QString> friendlyNames;
    {
        HDEVINFO devs = SetupDiGetClassDevsW(nullptr, L"Ports", nullptr, DIGCF_PRESENT);
        if (devs != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA devData{};
            devData.cbSize = sizeof(devData);
            const QRegularExpression re(QStringLiteral(R"(\(COM(\d+)\))"),
                                        QRegularExpression::CaseInsensitiveOption);
            for (DWORD idx = 0; SetupDiEnumDeviceInfo(devs, idx, &devData); ++idx) {
                wchar_t buf[512] = { 0 };
                DWORD type = 0, needed = 0;
                if (SetupDiGetDeviceRegistryPropertyW(
                        devs, &devData, SPDRP_FRIENDLYNAME, &type,
                        reinterpret_cast<PBYTE>(buf), sizeof(buf) - sizeof(wchar_t), &needed)) {
                    const QString fname = QString::fromWCharArray(buf);
                    const auto m = re.match(fname);
                    if (m.hasMatch()) {
                        const int num = m.captured(1).toInt();
                        // 去掉 "(COMx)" 尾巴后的名字就是纯友好名
                        const QString friendly = fname.left(m.capturedStart()).trimmed();
                        if (!friendly.isEmpty())
                            friendlyNames.insert(num, friendly);
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(devs);
        }
    }
    // 3) 组装 "COMx (友好名)"，按端口号升序
    QStringList result;
    for (auto it = portNames.cbegin(); it != portNames.cend(); ++it) {
        const auto f = friendlyNames.constFind(it.key());
        if (f != friendlyNames.constEnd())
            result << QStringLiteral("%1 (%2)").arg(it.value(), f.value());
        else
            result << it.value();
    }
    if (!result.isEmpty())
        return result;
    // 4) 兜底：注册表/SetupAPI 都不可用时，COM1~COM16 逐个试探（能打开即存在）
    for (int i = 1; i <= 16; ++i) {
        const QString path = QStringLiteral("\\\\.\\COM%1").arg(i);
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            result << QStringLiteral("COM%1").arg(i);
        }
    }
    return result;
}

bool SerialPort::open(const SerialConfig& cfg)
{
    // 修复 C3：句柄与线程生命周期全程持锁（递归锁允许内部 close()），
    // 与升级工作线程的 open/close/writeSync 互斥，杜绝并发换句柄
    std::lock_guard<std::recursive_mutex> lk(m_apiMutex);
    if (isOpen())
        close();
    if (cfg.portName.isEmpty()) {
        emit errorOccurred(QStringLiteral("串口名为空，无法打开"));
        return false;
    }
    // "COM3" → "\\.\COM3"：Win32 设备命名空间；COM10 及以上必须走这种形式
    const QString path = QStringLiteral("\\\\.\\") + cfg.portName;
    m_handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                           GENERIC_READ | GENERIC_WRITE,
                           0, // 独占访问：同一串口同一时刻只允许一个使用者
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        emit errorOccurred(
            QStringLiteral("打开 %1 失败：%2").arg(cfg.portName, winErrorString(GetLastError())));
        return false;
    }
    // 打开失败路径：报错 + 关句柄（后续每个 Set/Get 调用都同样处理）
    auto fail = [this](const QString& what) {
        emit errorOccurred(what);
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return false;
    };
    // DCB 串口参数。流控全部关闭：OTA 文档 A9 明确要求关硬件/软件流控（XON/XOFF 会被当数据）
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_handle, &dcb))
        return fail(QStringLiteral("读取串口参数失败：%1").arg(winErrorString(GetLastError())));
    dcb.BaudRate = static_cast<DWORD>(baudToWin32(cfg.baud));
    dcb.ByteSize = static_cast<BYTE>(dataBitsToWin32(cfg.dataBits));
    dcb.Parity = static_cast<BYTE>(parityToWin32(cfg.parity));
    dcb.StopBits = static_cast<BYTE>(stopBitsToWin32(cfg.stopBits));
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;   // DTR 拉高：多数 USB-TTL 靠它供电/复位
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;  // 不依赖 RTS 握手（无流控）
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(m_handle, &dcb))
        return fail(QStringLiteral("设置串口参数失败：%1").arg(winErrorString(GetLastError())));
    // Overlapped 读不受 COMMTIMEOUTS 约束，这里仅给（防御性的）同步调用兜底
    COMMTIMEOUTS cto{};
    cto.ReadIntervalTimeout = MAXDWORD;
    cto.ReadTotalTimeoutMultiplier = 0;
    cto.ReadTotalTimeoutConstant = 0;
    SetCommTimeouts(m_handle, &cto);
    SetupComm(m_handle, 8192, 8192); // 扩大驱动缓冲，容忍突发
    PurgeComm(m_handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    // 计时器精度提升到 1ms：WaitForSingleObject 默认 ~15.6ms 粒度，
    // 不满足升级协议“同步读超时分辨率 ≤10ms”的要求
    if (timeBeginPeriod(1) == TIMERR_NOERROR)
        m_timerBoosted = true;
    m_cfg = cfg;
    m_abort.store(false);
    m_exclusive.store(false);
    {
        QMutexLocker lk(&m_bufMutex);
        m_rxBuf.clear();
    }
    m_thread = new RxThread(this);
    m_thread->start();
    emit opened(m_cfg);
    return true;
}

void SerialPort::close()
{
    // 修复 C3：与 open/writeSync 互斥（见 m_apiMutex 注释）
    std::lock_guard<std::recursive_mutex> lk(m_apiMutex);
    // 1) 通知接收线程退出并取消所有挂起 IO（读写都会被 CancelIoEx 打断）
    if (m_handle != INVALID_HANDLE_VALUE) {
        m_abort.store(true);
        CancelIoEx(m_handle, nullptr);
    }
    // 2) 等线程收尾。线程在 20ms 分片内必然醒来；补一次取消是为了封住
    //    “线程在第一次取消之后又发起新读取”的窗口。
    if (m_thread) {
        const bool selfCall = (QThread::currentThread() == m_thread);
        if (selfCall) {
            // 线程自己触发的收尾（致命错误路径）：不能 join 自己，等 run() 返回后自删
            m_thread->deleteLater();
            m_thread = nullptr;
        } else {
            // 修复 M1：取消后无限期 wait()，绝不"超时放弃线程对象"。放弃的线程仍持
            // m_owner 指针继续访问本对象，SerialPort 析构后即为 use-after-free；
            // 而取消路径本身不需要超时兜底：run() 的等待点全部带 20ms 分片超时
            //（WaitForSingleObject 均带时限），中止标志置位后线程必然在分片内醒来并
            // 退出，不存在会永久挂起的位置。
            if (m_handle != INVALID_HANDLE_VALUE)
                CancelIoEx(m_handle, nullptr);
            m_thread->wait();
            delete m_thread;
            m_thread = nullptr;
        }
    }
    // 3) 句柄与状态收尾
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    if (m_timerBoosted) {
        timeEndPeriod(1);
        m_timerBoosted = false;
    }
    {
        QMutexLocker lk(&m_bufMutex);
        m_rxBuf.clear();
    }
    m_exclusive.store(false);
    m_cfg = SerialConfig();
    emit closed();
}

bool SerialPort::isOpen() const
{
    // 修复 C3：句柄读取与 open/close 互斥
    std::lock_guard<std::recursive_mutex> lk(m_apiMutex);
    return m_handle != INVALID_HANDLE_VALUE;
}

SerialConfig SerialPort::config() const
{
    // 修复 C3：持锁快照后按值返回，调用方拿到的不会是并发修改中的半更新状态
    std::lock_guard<std::recursive_mutex> lk(m_apiMutex);
    return m_cfg;
}

qint64 SerialPort::write(const QByteArray& data)
{
    // 修复 Minor 9：空数据语义拆成两条 if——空且开=0（无操作视为成功），
    // 空且关=-1（与 writeSync 一致），非空继续走正常写路径
    if (data.isEmpty())
        return isOpen() ? 0 : -1;
    // 修复（错误 87）：句柄是 FILE_FLAG_OVERLAPPED 打开的，WriteFile 不能传空
    // lpOverlapped——对通讯设备（COM 口）会固定返回 ERROR_INVALID_PARAMETER(87)。
    // 复用 writeSync 的 OVERLAPPED 路径（USB 串口写只入驱动缓冲，实际瞬间完成，
    // 2s 上限仅在驱动异常时兜底；锁由 writeSync 统一持有）
    return writeSync(data, 2000);
}

qint64 SerialPort::writeSync(const QByteArray& data, int timeoutMs)
{
    if (!isOpen())
        return -1;
    if (data.isEmpty())
        return 0;
    // 修复 C3：整个写操作（提交→等待→收尾）持锁，close() 无法在 IO 进行中关闭句柄；
    // 否则旧句柄被 CloseHandle 后系统可能复用同一句柄值，本次写会落进新打开的会话。
    // 代价：close() 最多被本次写阻塞 timeoutMs+5s，可接受（引擎侧按失败整轮重来）。
    std::lock_guard<std::recursive_mutex> lk(m_apiMutex);
    if (!isOpen())
        return -1;
    std::lock_guard<std::mutex> wlk(m_writeMutex);
    // OVERLAPPED 放堆上：取消落地等待有 5s 上限（见下），极端卡死时本函数返回后
    // 驱动完成回调仍可能写该结构——留在堆上安全，栈上则会悬垂（修复 M2 兜底路径）。
    auto* ov = new OVERLAPPED{};
    ov->hEvent = m_writeEvent;
    DWORD written = 0;
    const DWORD total = static_cast<DWORD>(data.size());
    if (!WriteFile(m_handle, data.constData(), total, &written, ov)) {
        const DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            delete ov;
            emit errorOccurred(QStringLiteral("串口写失败：%1").arg(winErrorString(err)));
            return -1;
        }
        const DWORD w = WaitForSingleObject(m_writeEvent,
                                            timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 0);
        if (w == WAIT_TIMEOUT) {
            CancelIoEx(m_handle, ov); // 只取消本次写，不影响接收线程挂起的读
            // 等取消落地：正常驱动立即完成。给 5s 上限而非 INFINITE——否则驱动极端
            // 卡死时工作线程永久挂在这里，引擎析构的 wait() 跟着永久挂起（M2）。
            // 上限到点仍未落地：ov 连同事件留在堆上泄漏（内存安全优先），返回 -1。
            const DWORD landed = WaitForSingleObject(m_writeEvent, 5000);
            if (landed == WAIT_TIMEOUT) {
                qWarning("SerialPort: 写取消未能落地，OVERLAPPED 留堆泄漏保证内存安全");
            } else {
                delete ov; // 取消已落地（IRP 完成），结构可安全回收
            }
            emit errorOccurred(QStringLiteral("串口写超时（%1 ms）").arg(timeoutMs));
            return -1;
        }
        if (w == WAIT_FAILED || !GetOverlappedResult(m_handle, ov, &written, FALSE)) {
            delete ov;
            emit errorOccurred(QStringLiteral("串口写失败：%1").arg(winErrorString(GetLastError())));
            return -1;
        }
    }
    delete ov;
    return static_cast<qint64>(written);
}

QByteArray SerialPort::readBytes(qint64 maxSize, int timeoutMs)
{
    // 本接口配合 beginExclusive() 使用：独占模式下接收线程把字节留在 m_rxBuf，
    // 这里消费；数据到达即返回（升级协议是 stop-and-wait，等 1 个应答字节）。
    if (!isOpen() || maxSize <= 0)
        return {};
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (!isOpen())
            return {}; // 等待期间被关闭：立即返回空
        {
            QMutexLocker lk(&m_bufMutex);
            if (!m_rxBuf.isEmpty()) {
                const qint64 take = qMin<qint64>(maxSize, m_rxBuf.size());
                QByteArray out = m_rxBuf.left(static_cast<int>(take));
                m_rxBuf.remove(0, static_cast<int>(take));
                return out;
            }
        }
        if (timeoutMs <= 0)
            return {};
        const qint64 remainMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - Clock::now())
                                    .count();
        if (remainMs <= 0)
            return {}; // 空返回 = 超时
        // 50ms 分片等待：deadline 计算保证整体精度（timeBeginPeriod(1) 下 ~1ms 粒度），
        // 分片让 close()/中止能及时生效
        const DWORD slice = static_cast<DWORD>(qMin<qint64>(50, remainMs));
        WaitForSingleObject(m_rxNotifyEvent, slice);
    }
}

void SerialPort::beginExclusive(bool purge)
{
    // purge=true：清场再进独占——驱动 + 用户层积压字节都不要，保证 'C' 握手从
    // 干净状态开始（D12/B2）。PurgeComm(RXCLEAR) 会取消接收线程挂起的读
    // （线程把它当正常取消处理，循环继续）。
    // purge=false：不清场——"等待设备重启"阶段用：BOOT 的 boot version/updata flag
    // 打印发生在升级收尾与检测阶段之间的空档，清场会把它们吞掉（真机日志证实）。
    if (purge) {
        std::lock_guard<std::recursive_mutex> lk(m_apiMutex); // 修复 C3：句柄访问互斥
        if (m_handle != INVALID_HANDLE_VALUE)
            PurgeComm(m_handle, PURGE_RXCLEAR);
    }
    // 修复 Minor 3：m_exclusive 的变更与 m_rxBuf 的清空放在同一把锁内，
    // 与 handleRxData/endExclusive 的"锁内读标志 + 缓冲交换"构成全序，
    // 消除"读到 stale exclusive=true 后字节被 append 进无人消费的缓冲"的竞态
    QMutexLocker lk(&m_bufMutex);
    if (purge)
        m_rxBuf.clear();
    m_exclusive.store(true, std::memory_order_release);
}

void SerialPort::endExclusive()
{
    QByteArray rest;
    {
        QMutexLocker lk(&m_bufMutex);
        m_exclusive.store(false, std::memory_order_release); // 修复 Minor 3：与缓冲交换同锁
        rest.swap(m_rxBuf);
    }
    // 升级期间积压的剩余字节一次性交给终端显示，不丢数据
    if (!rest.isEmpty())
        emit dataReceived(rest);
}

void SerialPort::handleRxData(const char* data, qint64 len)
{
    QByteArray chunk;
    bool exclusive = false;
    {
        // 修复 Minor 3：独占标志在缓冲锁内读取。若读到的 exclusive=true 而
        // endExclusive 恰在之后置 false 并换走缓冲，本批字节会 append 进无人
        // 消费的 m_rxBuf 直到下次 beginExclusive 清空（升级收尾字节丢显示）。
        // 锁内读标志后：endExclusive 要么先执行（标志已 false，本批走正常显示
        // 路径），要么后执行（本批进缓冲，被 endExclusive 一起换走），两条路径都
        // 不丢字节。
        QMutexLocker lk(&m_bufMutex);
        exclusive = m_exclusive.load(std::memory_order_acquire);
        m_rxBuf.append(data, static_cast<int>(len));
        if (!exclusive)
            chunk.swap(m_rxBuf); // 正常模式：不积压，直接取出交给界面
    }
    SetEvent(m_rxNotifyEvent); // 唤醒 readBytes 的等待者（两种模式都通知，开销可忽略）
    if (!exclusive && !chunk.isEmpty())
        emit dataReceived(chunk);
}
