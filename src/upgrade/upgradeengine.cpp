#include "upgrade/upgradeengine.h"

#include "serial/serialport.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <cstring>

namespace {

// 协议与流程常量已上移到 UpgradeEngine 类的 public static constexpr（upgradeengine.h），
// 供升级页前置校验共用（修复 Minor 7：此前 kMaxFileSize/kMaxFileNameLen 双处维护）。

// 触发帧（A8/D13：W 值域固定 5 字节且必须含 '.'，"@W12:00000" 不会触发）
const QByteArray kTriggerFrame = "@W12:1.";
// 版本查询帧（@Rxx:y 定长 6 字节）
const QByteArray kVersionQuery = "@R20:0";

// 把字节串转成 "HEX + 可读文本" 一行（B1：原始字节日志是排障命脉）
QString bytesToReadable(const QByteArray& data)
{
    QString hex, ascii;
    for (unsigned char c : data) {
        hex += QString::asprintf("%02X ", c);
        ascii += (c >= 0x20 && c < 0x7F) ? QChar(c) : QChar('.');
    }
    return QStringLiteral("%1 | %2").arg(hex.trimmed(), ascii);
}

// 取消收尾的统一文案（05c §5 风险3：同轮二次 @W12 会卡死设备到只能断电）
const QString kCancelMessage = QStringLiteral(
    "已取消。提示：若设备进入无响应状态（同轮二次 @W12 可能卡死设备，05c §5 风险3），"
    "请断电重启设备后再试。");

} // namespace

UpgradeEngine::UpgradeEngine(IUpgradePort* port, QObject* parent)
    : QObject(parent)
    , m_port(port)
{
    // 工作线程：start() 全程在此线程执行（阻塞式流程），UI 通过队列调用不卡界面。
    // parent 为空：moveToThread 要求对象无父级；UpgradePage 负责析构。
    m_thread = new QThread(this);
    moveToThread(m_thread);
    m_thread->start();
}

UpgradeEngine::~UpgradeEngine()
{
    // 修复 M2：取消 → quit → 无条件 wait()，去掉 terminate()。terminate 的语义是
    // "可能停在任意位置"：若线程此刻正持 SerialPort 的 m_writeMutex（writeSync 的
    // 取消落地等待中）或挂起 OVERLAPPED 写，强制终止后该互斥永久锁死，此后任何
    // write/writeSync 永久死锁，挂起的写也无人回收。
    // 无条件 wait() 是安全的：取消路径的每个等待点都有界（sleepSlices 100ms 分片、
    // waitForC 200ms 分片、readBytes 50ms 分片、writeSync 取消落地等 5s 上限，见
    // serialport.cpp），正常 ≤2~3s 内必然退出，无需超时兜底。
    m_cancelFlag.store(true);
    m_sender.abort();
    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
    }
}

bool UpgradeEngine::busy() const
{
    return m_busy.load();
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

void UpgradeEngine::start(const QString& filePath, const QString& portName)
{
    // 本槽经队列调用跑在工作线程；busy 原子交换防止重复进入。
    if (m_busy.exchange(true)) {
        emit logMessage(QStringLiteral("引擎忙（升级或冷却中），忽略本次启动请求"));
        return;
    }
    emit busyChanged(true);
    // 修复 Minor 5：恢复串口只在"本轮真正碰过设备/串口"时执行。m_w12Sent/m_lastW12Ms
    // 跨 run 保持——取消后立刻重新开始时，冷却仍按上一次 @W12 的时刻计算（防连发卡死，
    // 05c 风险3）。先复位再检查取消，避免取消早退路径读到上一轮残留的 m_portSwitched。
    m_portSwitched = false;
    m_rebootDetected = false;
    // 修复 Suggestion 3：start() 经队列排队期间用户可能已点取消（onCancel 直调 cancel()），
    // 旧逻辑在这里无条件清 0 会把先到的取消请求覆盖掉、升级照常启动。先检查再清 0：
    // 取消标志在本轮结束时（finishRound）会被清掉，因此这里为 true 只可能是"排队期间
    // 新到的取消"，应保留取消意图直接收尾。
    if (m_cancelFlag.load()) {
        finishRound(false, kCancelMessage, QStringLiteral("已取消"));
        return;
    }
    m_cancelFlag.store(false);
    m_image.clear();
    m_imageOff = 0;
    m_rxLine.clear(); // RX 按行聚合缓冲每轮清空（logRxBytes）
    m_versionReply.clear();
    // 原串口状态只快照一次（此后 ensurePortAt115200 反复切换，不能再覆盖快照）
    m_origCfg = m_port->config();
    m_origWasOpen = m_port->isOpen();

    // ---- 前置校验（不碰串口/设备，失败不进轮次，走 failEarly：不恢复串口、不冷却） ----
    emit phaseChanged(QStringLiteral("前置校验"));
    emit progressChanged(-1, -1);
    QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile()) {
        log(QStringLiteral("文件不存在或不可读：%1").arg(filePath));
        failEarly(QStringLiteral("文件不存在或不可读。请重新选择 .bin 固件文件。"));
        return;
    }
    const qint64 size = fi.size();
    if (size <= 0) {
        log(QStringLiteral("文件大小 %1 字节，拒绝（文件为空）").arg(size));
        failEarly(QStringLiteral("文件大小为 0，无法升级。"));
        return;
    }
    if (size > kMaxFileSize) {
        // 用户要求：超过设备硬上限 122K 也不拒绝——只警告，设备侧自行裁决
        log(QStringLiteral("警告：文件 %1 字节 超过设备 APP1_SIZE 硬上限（122K），设备可能拒绝写入。")
                .arg(size));
    }
    const QString baseName = fi.fileName();
    // 文件名超长不再拒绝（用户反馈）：头包文件名不参与写 Flash（05c C3），
    // YmodemSender::start 会按 UTF-8 边界截断为 ≤31 字节安全名，size 字段不受影响（D6）
    if (baseName.isEmpty()) {
        failEarly(QStringLiteral("文件名为空，请重新选择固件文件。"));
        return;
    }
    if (baseName.toUtf8().size() > kMaxFileNameLen) {
        log(QStringLiteral("文件名 UTF-8 共 %1 字节，超 31 上限：头包将使用截断名"
                           "（文件名不参与写 Flash，05c C3，不影响升级）")
                .arg(baseName.toUtf8().size()));
    }
    // 镜像一次读入内存：readFile 回调按序切块，避免传输中文件被替换/句柄失效
    {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) {
            log(QStringLiteral("打开文件失败：%1").arg(f.errorString()));
            failEarly(QStringLiteral("无法打开固件文件，请检查文件是否被占用。"));
            return;
        }
        m_image = f.readAll();
        if (m_image.size() != size) {
            log(QStringLiteral("读取文件不完整：期望 %1 字节，实得 %2 字节")
                    .arg(size)
                    .arg(m_image.size()));
            failEarly(QStringLiteral("固件文件读取不完整，请重试。"));
            return;
        }
    }
    log(QStringLiteral("前置校验通过：%1（%2 字节，%3 块）")
            .arg(baseName)
            .arg(size)
            .arg((size + 127) / 128));

    // ---- 轮次循环：失败/取消 → 冷却 4s → 下一轮从重新发 @W12 开始（A5） ----
    for (int round = 1; round <= kMaxRounds; ++round) {
        if (cancelled()) {
            finishRound(false, kCancelMessage, QStringLiteral("已取消"));
            return;
        }
        log(QStringLiteral("========== 第 %1/%2 轮 ==========").arg(round).arg(kMaxRounds));
        // 冷却检查（05c 风险3）：距离上一次 @W12 至少 4s 才能再发——既覆盖失败后
        // 设备 3s 超时退出窗口，也覆盖"取消后立刻重新开始"的场景。冷却放在这里
        // （发 @W12 前）而不是收尾时：取消/失败后 busy 立即释放、调试面板马上可用
        // （用户反馈），重开升级时由引擎自己在这里等够时间。
        if (m_lastW12Ms > 0) {
            const qint64 wait =
                kCooldownMs - (QDateTime::currentMSecsSinceEpoch() - m_lastW12Ms);
            if (wait > 0) {
                emit phaseChanged(QStringLiteral("冷却中（%1s）").arg((wait + 999) / 1000));
                emit progressChanged(-1, -1);
                if (!sleepSlices(static_cast<int>(wait), false)) {
                    finishRound(false, kCancelMessage, QStringLiteral("已取消"));
                    return;
                }
            }
        }
        if (doRound(round, baseName, size, portName)) {
            // 成功：m_rebootDetected 时跳过版本轮询（用户反馈：检测到重启即可）；
            // 否则以 @R20 版本轮询为成功判据（D10，自动匹配含 App_version 的回复）。
            // 期望版本号功能已按用户要求删除（2026-09-17）
            const QString reason = m_rebootDetected
                ? QStringLiteral("升级完成：已检测到设备重启（BOOT 打印）。\n"
                                 "请自行核对设备当前版本。")
                : QStringLiteral("升级成功：已收到设备 @R20 回复（含 App_version）。\n"
                                 "回显原文：%1")
                      .arg(m_versionReply);
            finishRound(true, reason, QStringLiteral("升级完成"));
            return;
        }
        if (cancelled()) {
            finishRound(false, kCancelMessage, QStringLiteral("已取消"));
            return;
        }
    }
    finishRound(false,
                QStringLiteral("3 轮均失败。建议：1) 断电重启设备后重试；2) 检查 USB-TTL 线材"
                               "（丢一个 ACK 即整轮必死，05c 风险1）；3) 确认固件镜像正确、"
                               "设备调试口 115200 8N1。"),
                QStringLiteral("升级失败"));
}

void UpgradeEngine::cancel()
{
    // 跨线程安全：UI 线程可直接调用。原子标志 + sender.abort（其内部为原子标志），
    // 阻塞中的 run() 在 ≤100ms 分片内看到并退出。
    m_cancelFlag.store(true);
    m_sender.abort();
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

bool UpgradeEngine::cancelled() const
{
    return m_cancelFlag.load();
}

void UpgradeEngine::log(const QString& msg)
{
    emit logMessage(QStringLiteral("[%1] %2")
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                             msg));
}

QString UpgradeEngine::hexLine(const char* tag, const QByteArray& data)
{
    return QStringLiteral("[%1] %2: %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             QString::fromLatin1(tag), bytesToReadable(data));
}

void UpgradeEngine::logRxBytes(const QByteArray& data)
{
    // 用户反馈：RX 按"完整帧"（一行，以 \n 结尾）聚合上屏——设备回显按片到达
    // （慢打印时一片只有几字节），逐片直接上屏会碎成 "RX 72 | r" 这种断句。
    // 攒到 \n 才发一条完整日志，残尾留在缓冲里等下一片。
    m_rxLine.append(data);
    for (;;) {
        const int nl = m_rxLine.indexOf('\n');
        if (nl < 0)
            break;
        QByteArray line = m_rxLine.left(nl);
        m_rxLine.remove(0, nl + 1);
        while (!line.isEmpty() && (line.endsWith('\r') || line.endsWith('\n')))
            line.chop(1);
        if (!line.isEmpty())
            emit logMessage(hexLine("RX", line));
    }
}

qint64 UpgradeEngine::roundBudgetLeftMs() const
{
    return m_roundDeadlineMs - m_roundTimer.msecsSinceReference();
}

bool UpgradeEngine::sleepSlices(int ms, bool checkBudget)
{
    // 分片静默等待：每 100ms 醒来检查取消标志与轮次预算（120s）。
    // “静默”期间绝不向串口写任何字节（05c B4：设备复位/重枚举，写字节有风险）。
    // checkBudget=false 用于轮间冷却：冷却不属于任何一轮，不受上一轮 120s 预算约束。
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (cancelled())
            return false;
        if (checkBudget && m_roundTimer.isValid() && roundBudgetLeftMs() <= 0)
            return false;
        QThread::msleep(static_cast<unsigned long>(qMin<qint64>(100, ms - t.elapsed())));
    }
    return !cancelled();
}

bool UpgradeEngine::ensurePortAt115200(const QString& fallbackPortName)
{
    // 按 OTA 要求切换：115200 8N1 无流控（A9）。open() 内部已打开时会先 close 再开。
    SerialConfig ota = m_port->config();
    if (ota.portName.isEmpty())
        ota.portName = fallbackPortName; // 未打开时用页面传入的最后使用端口
    ota.baud = B115200;
    ota.dataBits = Data8;
    ota.parity = ParityNone;
    ota.stopBits = StopOne;
    ota.flowControl = FlowNone;
    if (ota.portName.isEmpty()) {
        log(QStringLiteral("无可用串口名：串口未打开且无候选端口。请先在“串口助手”页打开串口。"));
        return false;
    }
    const SerialConfig cur = m_port->config();
    if (m_port->isOpen() && cur.portName == ota.portName && cur.baud == B115200
        && cur.dataBits == Data8 && cur.parity == ParityNone && cur.stopBits == StopOne
        && cur.flowControl == FlowNone) {
        log(QStringLiteral("串口已是 %1 115200 8N1，无需切换").arg(ota.portName));
        return true;
    }
    log(QStringLiteral("切换串口为 %1 115200 8N1 无流控（升级前配置已保存，结束后恢复）")
            .arg(ota.portName));
    m_portSwitched = true; // 修复 Minor 5：只有真正切过参数，收尾才需要恢复原会话
    if (!m_port->open(ota)) {
        log(QStringLiteral("打开串口 %1 失败（115200 8N1）").arg(ota.portName));
        return false;
    }
    return true;
}

void UpgradeEngine::restorePort()
{
    // 恢复升级前配置：原本打开 → 原参数重开；原本关闭 → 关闭。给终端页继续用。
    // 修复 Minor 5：引擎从未切过参数（如前置校验失败，或串口本来就是 115200 8N1）
    // 时直接返回——不该为"恢复"而把用户会话关掉再重开一次。
    if (!m_portSwitched)
        return;
    if (m_origWasOpen && !m_origCfg.portName.isEmpty()) {
        log(QStringLiteral("恢复串口原配置：%1").arg(m_origCfg.toString()));
        m_port->open(m_origCfg);
    } else if (m_port->isOpen()) {
        log(QStringLiteral("升级前串口未打开，现在关闭串口"));
        m_port->close();
    }
}

bool UpgradeEngine::waitForC(int timeoutMs)
{
    // 等第一个 0x43('C')（B2：以 'C' 为握手起点，不是 @W12 回显）。
    // 期间设备先打印 "ymode_updata stra"/"factory data reset11111" 文本，随后擦除
    // 122K Flash 静默 1.2~2.4s，之后每 100ms 发 'C'。非 'C' 字节记日志忽略（B3）。
    // 200ms 分片：及时响应取消与轮次预算。
    // 修复 Minor 8：噪声原始字节按 ~1s 聚合成一条日志——设备处于异常持续输出
    //（05c 泛洪态可占满 115200）时，逐片记日志会以 ~5 行/s 刷屏（30s 等待期 ~150 行）；
    // 聚合后原始字节不丢（B1 排障命脉），只是合批输出。
    QElapsedTimer t;
    t.start();
    QElapsedTimer lastNoiseEmit; // 上次噪声聚合输出时刻
    QElapsedTimer lastReminder;  // “继续等待”提醒节奏（10s 一次）
    lastNoiseEmit.start();
    lastReminder.start();
    bool noiseEmitted = false;
    QByteArray noiseBatch; // 聚合中的噪声字节
    while (t.elapsed() < timeoutMs) {
        if (cancelled())
            return false;
        if (roundBudgetLeftMs() <= 0) {
            log(QStringLiteral("单轮 120s 预算耗尽（等待 'C' 阶段）"));
            return false;
        }
        if (!m_port->isOpen()) {
            log(QStringLiteral("等待 'C' 期间串口被关闭"));
            return false;
        }
        const QByteArray r = m_port->readBytes(64, 200);
        if (!r.isEmpty()) {
            for (unsigned char c : r) {
                if (c == mycom::YmodemSender::kCRC_C) {
                    if (!noiseBatch.isEmpty())
                        logRxBytes(noiseBatch); // 命中前攒下的噪声一并落日志（按行聚合）
                    log(QStringLiteral("收到设备 'C' (0x43)，握手就绪"));
                    return true;
                }
            }
            noiseBatch.append(r);
            if (!noiseEmitted || lastNoiseEmit.elapsed() >= 1000) {
                logRxBytes(noiseBatch);
                noiseBatch.clear();
                lastNoiseEmit.restart();
                noiseEmitted = true;
            }
            if (lastReminder.elapsed() >= 10000) {
                log(QStringLiteral("等待 'C' 期间持续收到非协议字节，继续等待…（设备可能仍在擦 Flash）"));
                lastReminder.restart();
            }
        }
    }
    if (!noiseBatch.isEmpty())
        logRxBytes(noiseBatch); // 收尾清空（B1：不丢原始字节）
    log(QStringLiteral("等待 'C' 超时（%1s）").arg(timeoutMs / 1000));
    return false;
}

// ---------------------------------------------------------------------------
// 单轮流程
// ---------------------------------------------------------------------------

bool UpgradeEngine::doRound(int round, const QString& baseName, qint64 fileSize,
                            const QString& portName)
{
    Q_UNUSED(round);
    // 轮次预算 120s（B5）：从本轮开始计时，覆盖 @W12 到版本确认的所有阶段
    m_roundTimer.start();
    m_roundDeadlineMs = m_roundTimer.msecsSinceReference() + kRoundBudgetMs;

    // ---- 1. 串口参数 ----
    emit phaseChanged(QStringLiteral("切换串口（115200 8N1）"));
    emit progressChanged(-1, -1);
    if (!ensurePortAt115200(portName)) {
        log(QStringLiteral("串口不可用，本轮失败"));
        return false;
    }
    if (cancelled())
        return false;

    // ---- 2. 独占 + 触发（A9：传输期间不穿插任何其它命令；一轮只发一次 @W12，风险3） ----
    emit phaseChanged(QStringLiteral("触发 OTA（@W12:0000.）"));
    m_port->beginExclusive(); // 清场：驱动+用户层积压字节都丢掉，保证 'C' 握手干净（D12/B2）
    emit logMessage(hexLine("TX", kTriggerFrame));
    const qint64 w = m_port->writeSync(kTriggerFrame, kWriteSyncTimeoutMs);
    if (w != kTriggerFrame.size()) {
        m_port->endExclusive();
        log(QStringLiteral("发送 @W12 失败（写 %1/%2 字节）").arg(w).arg(kTriggerFrame.size()));
        return false;
    }
    m_w12Sent = true; // 修复 Minor 5：冷却的前提是"本轮确实向设备发过 @W12"
    m_lastW12Ms = QDateTime::currentMSecsSinceEpoch(); // 冷却在发 @W12 前计算，跨 run 有效

    // ---- 3. 等 'C' ----
    emit phaseChanged(QStringLiteral("等待设备就绪（'C'，≤30s）"));
    if (!waitForC(kWaitCTimeoutMs)) {
        m_port->endExclusive();
        log(QStringLiteral("未收到 'C'，本轮失败（05c B2：擦除静默 1.2~2.4s 后每 100ms 发 'C'）"));
        return false;
    }

    // ---- 4. YMODEM 传输（发送器自含 WaitingC 再握手：设备收到 SOH 前持续轮询 'C'，
    //         ≤100ms 即续上；引擎的等 'C' 与发送器的等 'C' 各司其职：前者管阶段/取消，后者保自洽） ----
    emit phaseChanged(QStringLiteral("传输固件（YMODEM 128B 块）"));
    m_imageOff = 0;
    {
        mycom::YmodemSender::Callbacks cb;
        cb.write = [this](const uint8_t* data, size_t len) -> int {
            if (cancelled())
                return -1;
            const QByteArray bytes(reinterpret_cast<const char*>(data),
                                   static_cast<int>(len));
            const qint64 n = m_port->writeSync(bytes, kWriteSyncTimeoutMs);
            if (n < 0) {
                log(QStringLiteral("串口写失败"));
                return -1;
            }
            emit logMessage(hexLine("TX", bytes));
            return static_cast<int>(n);
        };
        cb.read = [this](uint8_t* buf, size_t maxLen, int timeoutMs) -> int {
            // 取消/预算耗尽 → 返回 -1 让发送器立即失败退出（IoError 路径）。
            // 注意：readBytes 依赖独占模式（非独占时接收线程直接转终端显示，
            // readBytes 永远等满超时）——本回调只在 beginExclusive 覆盖的 run() 内使用。
            if (cancelled() || roundBudgetLeftMs() <= 0)
                return -1;
            const QByteArray r = m_port->readBytes(static_cast<qint64>(maxLen), timeoutMs);
            if (r.isEmpty()) {
                if (!m_port->isOpen())
                    return -1; // 串口被意外关闭（拔线等）→ 致命错误
                return 0;      // 0 = 超时
            }
            std::memcpy(buf, r.constData(), static_cast<size_t>(r.size()));
            return static_cast<int>(r.size());
        };
        cb.readFile = [this](uint8_t* buf, size_t len) -> int {
            const qint64 left = m_image.size() - m_imageOff;
            if (left <= 0)
                return 0; // EOF
            const int take = static_cast<int>(qMin<qint64>(len, left));
            std::memcpy(buf, m_image.constData() + m_imageOff,
                        static_cast<size_t>(take));
            m_imageOff += take;
            return take;
        };
        cb.progress = [this](int done, int total) { emit progressChanged(done, total); };
        cb.log = [this](const std::string& s) {
            emit logMessage(QStringLiteral("[%1] %2")
                                .arg(QDateTime::currentDateTime().toString(
                                         QStringLiteral("HH:mm:ss.zzz")),
                                     QString::fromUtf8(s.c_str())));
        };
        m_sender.setCallbacks(cb);
    }
    if (!m_sender.start(baseName.toStdString(), static_cast<uint32_t>(fileSize))) {
        m_port->endExclusive();
        log(QStringLiteral("YMODEM 发送器启动校验失败（文件名/大小）"));
        return false;
    }
    emit progressChanged(0, static_cast<int>((fileSize + 127) / 128));
    const bool ok = m_sender.run();
    m_port->endExclusive();
    if (cancelled()) {
        log(QStringLiteral("传输被取消"));
        return false;
    }
    if (!ok) {
        log(QStringLiteral("YMODEM 传输失败（%1）。按 05c A5 整轮重来：重新发 @W12，绝不只重发文件")
                .arg(m_sender.state() == mycom::YmodemSender::State::Aborted
                         ? QStringLiteral("已中止")
                         : QStringLiteral("协议错误/超时")));
        return false;
    }
    log(QStringLiteral("传输完成：%1 字节 / %2 块全部 ACK，空块已确认（设备将写 OTA 标志并复位）")
            .arg(fileSize)
            .arg((fileSize + 127) / 128));

    // ---- 5. 等待设备重启：独占读取 BOOT 打印检测重启（用户反馈：检测到重启就
    //        结束升级、立即放开调试面板，不再等版本确认轮询）。BOOT 会打印
    //        "boot version"/"write percent"（B4），10s 内未检测到则回退到
    //        原有的 @R20 版本轮询。期间不发送任何字节（USB 可能重枚举）。 ----
    emit phaseChanged(QStringLiteral("等待设备重启（检测 BOOT 打印，≤10s）"));
    emit progressChanged(-1, -1);
    {
        // 不清场（purge=false）：BOOT 的 boot version/updata flag 打印发生在
        // 升级收尾与本阶段之间的空档，清场会吞掉它们（真机日志证实过）
        m_port->beginExclusive(false);
        QByteArray bootEcho;
        QElapsedTimer rb;
        rb.start();
        while (rb.elapsed() < kDeviceResetMs && !cancelled()
               && roundBudgetLeftMs() > 0 && !m_rebootDetected) {
            const QByteArray r = m_port->readBytes(512, 200);
            if (!r.isEmpty()) {
                bootEcho.append(r);
                logRxBytes(r);
                const QString t = QString::fromLatin1(bootEcho);
                if (t.contains(QStringLiteral("boot version"))
                    || t.contains(QStringLiteral("write percent"))) {
                    m_rebootDetected = true;
                }
            }
        }
        m_port->endExclusive();
        if (m_rebootDetected) {
            log(QStringLiteral("检测到设备重启（BOOT 打印），跳过版本确认轮询"));
            return true;
        }
        if (cancelled()) {
            log(QStringLiteral("等待重启期间被取消"));
            return false;
        }
        if (roundBudgetLeftMs() <= 0) {
            log(QStringLiteral("单轮 120s 预算耗尽（等待重启阶段）"));
            return false;
        }
        log(QStringLiteral("10s 内未见 BOOT 打印，回退到版本确认轮询"));
    }

    // ---- 6. 版本确认（D10：以 @R20 轮询为唯一成功判据，不以传输 100% 为判据） ----
    emit phaseChanged(QStringLiteral("确认版本（轮询 @R20:0）"));
    {
        // readBytes 依赖独占模式，轮询前必须重新 beginExclusive
        m_port->beginExclusive();
        QByteArray collected; // 跨轮询累积回显（@R20 回显多行，可能分包到达）
        QElapsedTimer t;
        t.start();
        bool matched = false;
        while (t.elapsed() < kVerifyTimeoutMs) {
            if (cancelled()) {
                m_port->endExclusive();
                return false;
            }
            if (roundBudgetLeftMs() <= 0) {
                m_port->endExclusive();
                log(QStringLiteral("单轮 120s 预算耗尽（版本确认阶段）"));
                return false;
            }
            emit logMessage(hexLine("TX", kVersionQuery));
            const qint64 w2 = m_port->writeSync(kVersionQuery, kWriteSyncTimeoutMs);
            if (w2 != kVersionQuery.size()) {
                m_port->endExclusive();
                log(QStringLiteral("发送 @R20:0 失败（串口可能已重枚举，需要重新打开）"));
                return false;
            }
            // 200ms 收集窗口（50ms 分片读，及时响应取消）
            QElapsedTimer collect;
            collect.start();
            while (collect.elapsed() < 200 && !cancelled()) {
                const QByteArray r = m_port->readBytes(512, 50);
                if (r.isEmpty())
                    continue;
                logRxBytes(r);
                collected.append(r);
                const QString text = QString::fromLatin1(collected);
                if (text.contains(QStringLiteral("App_version"))) {
                    // 期望版本号功能已删除：匹配任意含 App_version 的回复（用户反馈）
                    m_versionReply = text.trimmed();
                    matched = true;
                    break;
                }
            }
            if (matched)
                break;
            // 等到下一个 500ms 周期边界（100ms 分片睡眠，可取消，不提前重发 @R20）。
            // 修复 Minor 4：分片里一并检查 120s 轮次预算——预算耗尽交给外层循环的
            // 预算分支统一收尾，消除"注释声称全阶段覆盖、这里最多超时 500ms"的缺口。
            const qint64 wait = kVerifyPollMs - (t.elapsed() % kVerifyPollMs);
            QElapsedTimer rest;
            rest.start();
            while (rest.elapsed() < wait && !cancelled() && roundBudgetLeftMs() > 0)
                QThread::msleep(static_cast<unsigned long>(qMin<qint64>(100, wait - rest.elapsed())));
        }
        m_port->endExclusive();
        if (!matched) {
            // 附上实际收到的字节（截断）便于排障：设备若停留在卡死态（05c 风险3），
            // 回显会打到一半断流，这里能看到"收到了什么/卡在哪"
            const QString dump =
                QString::fromLatin1(collected.left(300).toHex(' '));
            log(QStringLiteral("版本确认超时（30s）：@R20 回显未见 App_version"
                               "（累计收到 %1 字节，前 300 字节 HEX：%2）。"
                               "传输 100% 不代表成功（D10）。"
                               "若设备此前升级失败过，请断电重启设备后重试。")
                    .arg(collected.size())
                    .arg(dump));
            return false;
        }
        log(QStringLiteral("版本确认成功：@R20 回显含 App_version"));
        return true;
    }
}

// ---------------------------------------------------------------------------
// 收尾
// ---------------------------------------------------------------------------

void UpgradeEngine::finishRound(bool ok, const QString& reason, const QString& finalPhase)
{
    // 统一收尾：恢复串口 → busy=false → 清取消标志 → finished。
    // 冷却不再阻塞在这里（用户反馈：取消/失败后调试面板应立即可用）——
    // 4s 防连发 @W12 的冷却由下一轮/下一次 start() 在发 @W12 前等待
    //（见 start() 轮次循环开头的冷却检查），安全语义不变。
    restorePort();
    emit phaseChanged(finalPhase);
    emit busyChanged(false);
    m_busy.store(false);
    // 清取消标志（修复 Suggestion 3）：让下一轮 start() 的"排队前取消"检查不会
    // 被本轮残留标志误触发——此处之后为 true 的取消只可能是 UI 新一轮的请求。
    m_cancelFlag.store(false);
    emit finished(ok, reason);
}

void UpgradeEngine::failEarly(const QString& reason)
{
    // 修复 Minor 5①：前置校验失败（文件不存在/超限/名字过长/读取失败）时设备与串口
    // 都没被碰过——不恢复串口（用户会话未被引擎改动，恢复反而会关掉重开一次）、
    // 不冷却（纯白等 4s），直接以失败收尾。
    emit phaseChanged(QStringLiteral("升级失败"));
    emit busyChanged(false);
    m_busy.store(false);
    m_cancelFlag.store(false);
    emit finished(false, reason);
}
