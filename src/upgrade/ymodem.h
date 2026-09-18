#pragma once

// YMODEM 发送器（纯 C++，不依赖 Qt：回调方式注入 IO，便于单元测试打桩）。
// 协议约束来自 Finca/docs/05c_OTA审查_上位机.md（逐条对应）：
//   - 数据块固定 128 字节（SOH）—— 接收端不支持 1K/STX（D1/A1）
//   - 块响应超时 ≤1s、每块最多 2 次重试且累计等待 <2.5s（接收端 3s 窗口，D2/D4/D11/A2）
//   - 头包响应超时 2.8s（见 kHeaderTimeoutMs 的 C1 注释），超时不重发头包而是整轮失败（D4：接收端坏头包静默）
//   - EOT×2（首个 EOT 收 NAK）、批次收尾必须发全零空块（D5/A3）
//   - 响应只认白名单 {0x06 ACK, 0x15 NAK, 0x43 'C', 0x18 CAN, 0x04}，其余丢弃并记日志（D14/B3）
//   - CRC-16/XMODEM（poly 0x1021 init 0），线上高字节在前
//   - 序号从 1 起按 256 取模回绕（接收端 complete_check(pack_num+1, ...) 用 uint8_t 加法）
//   - 失败即整轮重来由引擎负责：run() 失败返回 false，引擎重新发 @W12（A5）
//
// 数据来源设计：start() 只收文件名（头包 payload 用，A7：≤31 字符不带路径），
// 文件字节经 Callbacks::readFile 按序注入（引擎提供 QFile 读实现，测试提供内存缓冲），
// 发送器不碰文件系统 —— 保证纯协议逻辑可测、引擎侧文件错误可读。

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace mycom {

class YmodemSender
{
public:
    struct Callbacks {
        std::function<int(const uint8_t*, size_t)> write;   // 返回写入字节数，<0 失败
        std::function<int(uint8_t*, size_t, int)> read;     // (buf, maxLen, timeoutMs) 读到字节数，0=超时，<0=IO错误
        std::function<int(uint8_t*, size_t)> readFile;      // 读文件下一段，返回字节数，0=EOF，<0=错误
        std::function<void(int, int)> progress;             // (已完成块, 总块)
        std::function<void(const std::string&)> log;
    };

    enum class State {
        Idle,       // 未开始
        WaitingC,   // 等待接收端 'C'（超时 30s，OTA 文档 B2；引擎先行等过则 <100ms 即命中）
        SendHeader, // 发送 SOH 00 FF 头包（文件名\0大小\0...）
        SendData,   // 逐块发送 128 字节数据块
        SendEot1,   // 第一个 EOT（预期收 NAK）
        SendEot2,   // 第二个 EOT（预期收 ACK+'C'）
        SendEmpty,  // 全零空块（SOH 00 FF 128×0x00 + CRC）
        Done,       // 成功（空块 ACK 后）
        Error,      // 失败（超时/IO 错误/收到 CAN）
        Aborted     // 用户取消
    };

    /// 注入 IO 回调。必须在 run() 前调用，且至少提供 write/read/readFile。
    void setCallbacks(const Callbacks& cb);

    /// 准备一轮发送：校验文件名（≤31 字符、不含路径分隔符）与大小（>0）。
    /// fileName 是“头包里的文件名”（basename），不含路径（A7/D9）。
    /// fileSize 必须是文件实际字节数 —— 头包 size 字段精确等于它（A6/D6：不一致时
    /// 接收端永远收不到 EOT，表现为 3s 静默超时）。
    /// 开始一轮传输。fileName 仅用于头包记录（不参与写 Flash，05c C3）；
    /// 超 31 字节时内部截断为安全名（保留 .bin 后缀、不切断 UTF-8 字符）——
    /// 接收端 filename[32] 缓冲，超长会溢出（D9），但拒绝升级没有必要（用户反馈）。
    /// fileSize 必须精确等于文件字节数（D6）。返回 false 仅当名字/大小为非法。
    bool start(std::string fileName, uint32_t fileSize);

    /// 头包实际使用的文件名（可能已被截断），供引擎日志与测试核对
    const std::string& fileName() const { return m_fileName; }

    /// 跑完整状态机：WaitingC → 头包 → 逐块 → EOT×2 → 空块 → Done。
    /// 任一处超限/IO 错误/收到 CAN → 返回 false（由引擎整轮重来，不要只重发文件）。
    /// 可在 start() 后多次调用（每次独立一轮）；abort() 后立即返回 false。
    bool run();

    /// 线程安全取消：原子标志，任何等待点 ≤100ms 内退出。
    /// run() 内部所有等待都按 100ms 分片，及时检查本标志。
    void abort();

    State state() const;

    // ---- 组帧（public static，供单元测试逐字节断言；run() 内部也走这些函数） ----

    /// 头包：SOH 00 FF + "name\0size\0" 0x00 填充至 128 + CRC，共 133 字节。
    static std::vector<uint8_t> makeHeaderFrame(const std::string& name, uint32_t fileSize);
    /// 数据块：SOH seq ~seq + 128 字节（不足 0x1A 填充）+ CRC。
    static std::vector<uint8_t> makeDataFrame(uint8_t seq, const uint8_t* data, size_t len);
    /// 批次结束空块：SOH 00 FF + 128×0x00 + CRC（D5：不发空块 = 白传）。
    static std::vector<uint8_t> makeEmptyFrame();

    // 协议常量（测试与引擎也引用）
    static constexpr uint8_t kSOH = 0x01;
    static constexpr uint8_t kEOT = 0x04;
    static constexpr uint8_t kACK = 0x06;
    static constexpr uint8_t kNAK = 0x15;
    static constexpr uint8_t kCAN = 0x18;
    static constexpr uint8_t kCRC_C = 0x43; // 'C'（CRC 模式握手字符）
    static constexpr int kHeaderTimeoutMs = 2800;   // 修复 C1：头包响应超时 2.8s。
    // 时序依据（2026-09-16 审查结论，代码审查报告 C1）：工作树固件 updata.c 的
    // file_info_callback 收到头包后**同步整片擦除 filesize 字节 Flash**（fmc_erase_pages）
    // 才回 ACK+'C'（05c 审查分支已把这段擦除前移，工作树未改，两处擦除并存）。
    // 按 05c §4.2 数据（122K 擦除 1.2~2.4s）折算：≈100K 以上镜像的 ACK 延迟落在
    // 1.2~2.4s，2s 超时正好落在分布中间，"时好时坏"，最坏情况每轮都在头包阶段失败。
    // 放宽到 2.8s 的安全性：① 仍低于接收端 YMD_BEGIN 的 3s 窗口，不会因我们超时
    // 而挤压接收端状态机；② 发送器在头包阶段**不重发头包**（超时即整轮失败），
    // 不存在"重发头包叠加"问题；③ 失败后引擎冷却 4s 再 @W12，接收端（3s 超时）
    // 早已退出 INITIATE，二次 @W12 不会叠加。常量全工程仅此一处定义（ymodem.h
    // 是协议头，引擎与测试均引用此处，避免重复定义漂移）。
    static constexpr int kBlockTimeoutMs = 1000;    // 单次块响应 ≤1s（A2/D11）
    static constexpr int kBlockBudgetMs = 2400;     // 每块累计等待预算 <2.5s（接收端 3s 窗口）
    static constexpr int kMaxBlockRetries = 2;      // 每块最多重发 2 次
    static constexpr int kEotTimeoutMs = 1000;      // EOT 各阶段 1s
    static constexpr int kWaitCTimeoutMs = 30000;    // WaitingC 30s（YMODEM_INITIATE_TIMEOUT）
    static constexpr int kAfterAckCTimeoutMs = 200;  // ACK 后的可选 'C' 等待：固件 ACK/'C' 背靠背发出，
                                                     // 200ms 足够且不挤压接收端 YMD_RECEIVE 的 3s 窗口

private:
    enum class WaitResult { Got, Timeout, Aborted, IoError };

    // 白名单响应等待：在 timeoutMs 内从读回调收集字节，只保留白名单；命中 accepted 中
    // 任意字节返回 Got（*got 记结果），其余白名单字节与噪声丢弃并按 read 批次汇总记日志。
    // 每次读按 100ms 分片，分片间检查 abort（及时退出）；read<0 视为 IoError。
    WaitResult waitFor(const uint8_t* accepted, size_t nAccepted, int timeoutMs,
                       uint8_t* got, std::string& noise);

    bool sendFrame(const std::vector<uint8_t>& frame);
    bool waitAckThenC(int ackTimeoutMs); // 等 ACK（ackTimeoutMs），ACK 后可选等 'C' 200ms
    std::string logHex(const uint8_t* data, size_t len);

    Callbacks m_cb;
    std::string m_fileName;
    uint32_t m_fileSize = 0;
    State m_state = State::Idle;
    std::atomic<bool> m_abort{false};
    std::deque<uint8_t> m_pending; // 已过滤白名单、尚未消费的字节
    uint32_t m_totalBlocks = 0;
    uint8_t m_seq = 0; // 下一块序号（1 起，255 后回绕 0）
    uint32_t m_bytesSent = 0; // 已发送的文件字节数（结束时必须等于 m_fileSize，A6/D6）
};

} // namespace mycom
