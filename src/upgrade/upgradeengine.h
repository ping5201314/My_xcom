#pragma once

// 固件升级引擎：完整流程见 Finca/docs/05c_OTA审查_上位机.md 与 docs/需求分析.md 附录A。
// 目标流程：
//   前置校验（≤120K、文件名 ≤31）→ 强制串口 115200 8N1 → beginExclusive
//   → 发 @W12:0000. → 等 'C' ≤30s → YmodemSender::run()（128B 块 / EOT×2 / 空块）
//   → endExclusive → 静默 10s（设备复位、BOOT 搬运 3~6s，可能重枚举 USB 串口）
//   → beginExclusive → 轮询 @R20:0（每 500ms ≤30s）比对期望版本号
// 失败处理：任何阶段失败 → 本轮终止 → 冷却 4s → 下一轮从重新发 @W12 开始（A5：
//   绝不只重发文件——①重复包不会 ACK（D3）②不重新擦 Flash 会写失败被吞（D10））；最多 3 轮。
// 取消：原子标志 + 每步检查 + sender.abort()（线程安全）。修复 Minor 5：冷却只在
//   "本轮确实向设备发过 @W12"时执行——前置校验失败（未碰设备/串口）直接收尾不冷却，
//   冷却中取消不再补第二个 4s（与循环内冷却去重）。
// 析构（修复 M2）：取消 → quit → 无条件 wait()，不再 terminate()（强制终止可能停在
//   持锁/挂起 IO 中途，留下永久锁死的互斥与无人回收的 OVERLAPPED 写）；工作线程的
//   所有等待点均有界（writeSync 取消落地等 5s 上限），正常 ≤2~3s 内必然退出。
// 运行模型：本对象 moveToThread 到自建 QThread，start() 经队列调用在工作线程执行；
//   cancel() 必须可被 UI 线程直接调用（只碰原子标志与 sender.abort，跨线程安全）。

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include <atomic>

#include "serial/serialconfig.h" // 值成员 m_origCfg 需要完整类型
#include "upgrade/ymodem.h"

class SerialPort;
class QThread;

class UpgradeEngine : public QObject
{
    Q_OBJECT
public:
    explicit UpgradeEngine(SerialPort* port, QObject* parent = nullptr);
    ~UpgradeEngine() override;

    /// 是否忙（含冷却期）。UI 线程读安全（原子）。
    bool busy() const;

    // 协议与流程常量（依据：Finca/docs/05c_OTA审查_上位机.md，逐条注释出处）。
    // 修复 Minor 7：升级页前置校验与引擎共用同一份定义（此前 upgradepage.cpp
    // 与 upgradeengine.cpp 双处维护，注释"与引擎一致"恰恰说明有漂移风险）。
    static constexpr int kMaxFileSize = 120 * 1024;   // 接受上限 120K（A6：122K 是设备硬上限，留余量）
    static constexpr int kMaxFileNameLen = 31;        // A7/D9：接收端 filename[32] 缓冲
    static constexpr int kMaxRounds = 3;              // 最多 3 轮（B5：2~3 轮后报现场故障）
    static constexpr int kRoundBudgetMs = 120000;     // 单轮总超时 120s（B5）
    static constexpr int kWaitCTimeoutMs = 30000;     // 等 'C' 30s（接收端 YMD_INITIATE_TIMEOUT）
    static constexpr int kDeviceResetMs = 10000;      // 复位后静默 10s（B4：BOOT 搬运 3~6s + 余量）
    static constexpr int kVerifyPollMs = 500;         // @R20 轮询间隔 500ms
    static constexpr int kVerifyTimeoutMs = 30000;    // 版本轮询总时长 30s
    static constexpr int kCooldownMs = 4000;          // 失败/取消后冷却 4s（任务规格：冷却期计入 busy）
    static constexpr int kWriteSyncTimeoutMs = 2000;  // writeSync 超时（触发帧/轮询帧）

signals:
    void phaseChanged(const QString& phase);       // 当前阶段（显示用）
    void progressChanged(int current, int total);  // 块进度；total<0 表示进入忙碌型阶段（UI 用 range(0,0)）
    void logMessage(const QString& message);       // 过程日志（含 TX/RX 原始字节十六进制，05c B1）
    void finished(bool ok, const QString& reason); // 整体结束（升级成功判据：@R20 版本号，B4/D10）
    void busyChanged(bool busy);                   // busy 含冷却期，冷却结束才置 false

public slots:
    /// 启动升级。filePath=固件路径；portName=串口未打开时的候选端口（页面从
    /// AppConfig 读最后使用的端口传入）。期望版本号功能已按用户要求删除：
    /// 成功判据 = 检测到设备重启（BOOT 打印）；10s 未检测到才回退 @R20 轮询
    /// （自动匹配含 App_version 的回复）。
    void start(const QString& filePath, const QString& portName);
    /// 取消请求。可被 UI 线程直接调用（线程安全：仅原子标志 + sender.abort + 信号）。
    void cancel();

private:
    // ---- 分阶段实现 ----
    bool doRound(int round, const QString& baseName, qint64 fileSize, const QString& portName);
    bool ensurePortAt115200(const QString& fallbackPortName); // 切到 115200 8N1（原配置已在 start() 快照）
    void restorePort();                               // 恢复升级前配置（含关闭；仅引擎切过参数时才动作）
    bool waitForC(int timeoutMs);                     // 等 'C'，200ms 分片可取消，非 'C' 字节按 ~1s 聚合记日志
    bool sleepSlices(int ms, bool checkBudget = true); // 分片静默等待（可取消/可查轮次预算）
    // 统一收尾：恢复串口 →（失败/取消且本轮发过 @W12 且未冷却过）冷却 4s → busy=false
    // → 清取消标志（供下一轮 start() 区分"排队前的取消"与"上轮残留标志"）→ finished。
    // cooled=true 表示冷却已发生（冷却中取消），不再补第二个 4s（Minor 5 去重）。
    void finishRound(bool ok, const QString& reason, const QString& finalPhase);
    // 前置校验失败收尾：未触碰串口与设备，不恢复串口（会话未被引擎改动）、不冷却（Minor 5①）
    void failEarly(const QString& reason);
    qint64 roundBudgetLeftMs() const;                 // 本轮剩余预算（120s，B5）
    bool cancelled() const;                           // 取消标志检查
    void log(const QString& msg);                     // 统一加时间戳
    QString hexLine(const char* tag, const QByteArray& data); // "[ts] TX/RX HEX+可读文本"
    // RX 按"完整帧"（以 \n 结尾的行）聚合上屏：设备回显按片到达，攒满一行才发一条
    // 日志，残尾留在缓冲（用户反馈：逐片上屏会碎成断句）
    void logRxBytes(const QByteArray& data);

    SerialPort* m_port;
    QThread* m_thread = nullptr;
    mycom::YmodemSender m_sender;

    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_cancelFlag{false};

    // 以下成员仅工作线程访问（start() 全程在工作线程执行），无并发问题
    QByteArray m_image;                 // 镜像内容（≤120K，一次读入内存，readFile 回调按序切块）
    qint64 m_imageOff = 0;              // 镜像读取游标
    SerialConfig m_origCfg;             // 升级前配置快照
    bool m_origWasOpen = false;
    bool m_portSwitched = false;        // 本轮是否由引擎切换过串口参数（restorePort 前提，Minor 5）
    bool m_w12Sent = false;             // 引擎生命周期内是否发过 @W12（冷却前提，跨 run 保持）
    qint64 m_lastW12Ms = 0;             // 最近一次 @W12 的发送时刻（ms 时间戳，冷却计算用）
    bool m_rebootDetected = false;      // 本轮是否检测到设备重启（BOOT 打印），跳过版本轮询
    QElapsedTimer m_roundTimer;         // 本轮计时（120s 预算，B5）
    qint64 m_roundDeadlineMs = 0;       // 本轮截止（timer.msecsSinceReference 基准）
    QString m_versionReply;             // 版本确认阶段命中的 @R20 回显原文（成功横幅用）
    QByteArray m_rxLine;                // RX 按行聚合的残尾缓冲（logRxBytes 用）
};
