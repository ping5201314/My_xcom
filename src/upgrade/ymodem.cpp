#include "upgrade/ymodem.h"
#include "upgrade/crc16.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace mycom {

namespace {
// 单帧大小：SOH + seq + ~seq + 128 数据 + CRC16 两字节 = 133。
// 与固件 SOH_PKG_SZ (1+2+128+2) 一致（Ymodem-main/ymodem.c:31）。
constexpr size_t kFramePayload = 128;
constexpr size_t kFrameSize = 133;

// 白名单判定：{0x06 ACK, 0x15 NAK, 0x43 'C', 0x18 CAN, 0x04 EOT}（05c B3/D14）。
// 其余字节（ASCII 文本等）一律丢弃并记日志，绝不做字符串匹配。
bool isWhitelisted(uint8_t b)
{
    return b == YmodemSender::kACK || b == YmodemSender::kNAK || b == YmodemSender::kCRC_C
           || b == YmodemSender::kCAN || b == YmodemSender::kEOT;
}

// 十六进制小串，用于噪声日志（每次少量字节，无需高效实现）
std::string hexOf(const uint8_t* data, size_t len)
{
    static const char* hex = "0123456789ABCDEF";
    std::string s;
    s.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(hex[data[i] >> 4]);
        s.push_back(hex[data[i] & 0x0F]);
        s.push_back(' ');
    }
    if (!s.empty())
        s.pop_back();
    return s;
}
} // namespace

// ---------------------------------------------------------------------------
// 组帧
// ---------------------------------------------------------------------------

std::vector<uint8_t> YmodemSender::makeHeaderFrame(const std::string& name, uint32_t fileSize)
{
    // payload = "文件名\0大小(十进制)\0"，其余 0x00 填充。
    // 接收端 get_size() 取首个 NUL 后的数字（ymodem.c:121），mtime/mode 字段可省（C1）。
    std::vector<uint8_t> frame(kFrameSize, 0);
    frame[0] = kSOH;
    frame[1] = 0x00;
    frame[2] = 0xFF;
    uint8_t* payload = frame.data() + 3;
    size_t off = 0;
    const size_t nameLen = name.size();
    // start() 已保证 ≤31，这里再防御一次：截断会破坏 size 定位，宁可空名也要安全
    if (nameLen > 0 && nameLen < kFramePayload - 2)
        memcpy(payload, name.c_str(), nameLen);
    off = nameLen;
    if (off >= kFramePayload)
        off = kFramePayload - 1;
    payload[off++] = 0;
    // 大小十进制写入，最多留到 payload 结尾
    char num[16] = { 0 };
    std::snprintf(num, sizeof(num), "%u", fileSize);
    const size_t numLen = std::strlen(num);
    if (off + numLen >= kFramePayload)
        return {}; // 理论不可达（文件名 ≤31），防御性返回空
    memcpy(payload + off, num, numLen);
    off += numLen;
    payload[off] = 0; // 第二个 NUL 截断（接收端 atoi 即止）；其后保持 0 填充
    const uint16_t crc = Crc16::xmodem(payload, kFramePayload);
    frame[kFrameSize - 2] = static_cast<uint8_t>(crc >> 8);   // 高字节在前（线上顺序）
    frame[kFrameSize - 1] = static_cast<uint8_t>(crc & 0xFF);
    return frame;
}

std::vector<uint8_t> YmodemSender::makeDataFrame(uint8_t seq, const uint8_t* data, size_t len)
{
    std::vector<uint8_t> frame(kFrameSize, 0x1A); // 预填 0x1A：末块填充语义（C2 也允许 0x00）
    frame[0] = kSOH;
    frame[1] = seq;
    frame[2] = static_cast<uint8_t>(~seq);
    if (len > kFramePayload)
        len = kFramePayload; // 防御：调用方保证 ≤128
    if (data && len > 0)
        memcpy(frame.data() + 3, data, len);
    const uint16_t crc = Crc16::xmodem(frame.data() + 3, kFramePayload);
    frame[kFrameSize - 2] = static_cast<uint8_t>(crc >> 8);
    frame[kFrameSize - 1] = static_cast<uint8_t>(crc & 0xFF);
    return frame;
}

std::vector<uint8_t> YmodemSender::makeEmptyFrame()
{
    // 全零空块：SOH 00 FF + 128×0x00 + CRC（D5/A3：不发它接收端不写 OTA 标志 = 白传）
    std::vector<uint8_t> frame(kFrameSize, 0);
    frame[0] = kSOH;
    frame[1] = 0x00;
    frame[2] = 0xFF;
    const uint16_t crc = Crc16::xmodem(frame.data() + 3, kFramePayload);
    frame[kFrameSize - 2] = static_cast<uint8_t>(crc >> 8);
    frame[kFrameSize - 1] = static_cast<uint8_t>(crc & 0xFF);
    return frame;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void YmodemSender::setCallbacks(const Callbacks& cb)
{
    m_cb = cb;
}

bool YmodemSender::start(std::string fileName, uint32_t fileSize)
{
    // A7/D9：接收端 filename[32] 目标缓冲，超长会溢出破坏 size 记账（updata.c:175）。
    // 但文件名不参与写 Flash（05c C3），只作记录——超 31 字节时工具侧截断为安全名
    // （保留 .bin 后缀、按 UTF-8 字符边界回退），size 字段精确不受影响（D6）。
    // 不再因文件名过长拒绝升级（用户反馈：打开文件不应被文件名卡住）。
    if (fileName.empty() || fileSize == 0)
        return false;
    if (fileName.size() > 31) {
        const bool hasExt = fileName.size() >= 4
                            && fileName.compare(fileName.size() - 4, 4, ".bin") == 0;
        std::string base = hasExt ? fileName.substr(0, fileName.size() - 4) : fileName;
        const std::string ext = hasExt ? ".bin" : std::string();
        while (base.size() + ext.size() > 31 && !base.empty()) {
            // 逐字节回退并退到 UTF-8 字符边界（不落在续字节 10xxxxxx 上）
            do {
                base.pop_back();
            } while (!base.empty()
                     && (static_cast<unsigned char>(base.back()) & 0xC0) == 0x80);
        }
        fileName = base + ext;
    }
    if (fileName.find('/') != std::string::npos || fileName.find('\\') != std::string::npos)
        return false;
    m_fileName = fileName;
    m_fileSize = fileSize;
    m_totalBlocks = (fileSize + kFramePayload - 1) / kFramePayload;
    m_seq = 1; // 序号从 1 起（接收端期待 pack_num+1，首个数据块 = 1）
    m_bytesSent = 0;
    m_abort.store(false);
    m_pending.clear();
    m_state = State::WaitingC;
    return true;
}

void YmodemSender::abort()
{
    // 跨线程安全：仅原子写。run() 的每个等待分片（≤100ms）都会检查。
    m_abort.store(true);
}

YmodemSender::State YmodemSender::state() const
{
    return m_state;
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

bool YmodemSender::sendFrame(const std::vector<uint8_t>& frame)
{
    if (!m_cb.write)
        return false;
    size_t off = 0;
    while (off < frame.size()) {
        if (m_abort.load())
            return false;
        const int n = m_cb.write(frame.data() + off, frame.size() - off);
        if (n <= 0)
            return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

YmodemSender::WaitResult YmodemSender::waitFor(const uint8_t* accepted, size_t nAccepted,
                                               int timeoutMs, uint8_t* got, std::string& noise)
{
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    noise.clear();

    // 噪声按批次汇总：非白名单字节与“白名单但不被期待”的字节分开计数，避免日志刷屏
    //（INITIATE 阶段设备每 100ms 发一个 'C'，等待头包 ACK 时可能收到十几个）。
    size_t noiseBytes = 0, extraProtocol = 0;
    std::string noiseSample, extraSample;
    auto flushNoise = [&]() {
        if (noiseBytes)
            noise += "丢弃非协议字节 " + std::to_string(noiseBytes) + ": " + noiseSample + "\n";
        if (extraProtocol)
            noise += "忽略多余协议字节 " + extraSample + "\n";
        noiseBytes = extraProtocol = 0;
        noiseSample.clear();
        extraSample.clear();
    };

    for (;;) {
        if (m_abort.load())
            return WaitResult::Aborted;
        // 先消费积压队列（上一轮 read 多收的白名单字节）
        if (!m_pending.empty()) {
            const uint8_t b = m_pending.front();
            m_pending.pop_front();
            for (size_t i = 0; i < nAccepted; ++i) {
                if (b == accepted[i]) {
                    flushNoise();
                    *got = b;
                    return WaitResult::Got;
                }
            }
            if (isWhitelisted(b)) {
                ++extraProtocol;
                if (extraSample.size() < 32)
                    extraSample += hexOf(&b, 1) + " ";
            } else {
                ++noiseBytes;
                if (noiseSample.size() < 64)
                    noiseSample += hexOf(&b, 1) + " ";
            }
            continue;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            flushNoise();
            return WaitResult::Timeout;
        }
        const int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 deadline - now).count());
        // 100ms 分片读：abort 及时生效；read 契约 0=超时（已等满给定时间）
        uint8_t buf[64];
        const int slice = std::min(100, remain);
        const int n = m_cb.read ? m_cb.read(buf, sizeof(buf), slice) : 0;
        if (n < 0) {
            flushNoise();
            return WaitResult::IoError;
        }
        if (n == 0)
            continue; // 本分片超时，继续等下个分片（deadline 控制整体超时）
        for (int i = 0; i < n; ++i) {
            const uint8_t b = buf[i];
            bool hit = false;
            for (size_t k = 0; k < nAccepted; ++k) {
                if (b == accepted[k]) {
                    hit = true;
                    break;
                }
            }
            if (hit) {
                // 命中字节后，其余字节（可能混在同批）继续进积压队列，保持顺序
                for (int j = i + 1; j < n; ++j)
                    m_pending.push_back(buf[j]);
                flushNoise();
                *got = b;
                return WaitResult::Got;
            }
            if (isWhitelisted(b)) {
                ++extraProtocol;
                if (extraSample.size() < 32)
                    extraSample += hexOf(&b, 1) + " ";
            } else {
                ++noiseBytes;
                if (noiseSample.size() < 64)
                    noiseSample += hexOf(&b, 1) + " ";
            }
        }
    }
}

std::string YmodemSender::logHex(const uint8_t* data, size_t len)
{
    return hexOf(data, len);
}

// 头包/EOT#2/空块后的 ACK 共用：等 ACK（ackTimeoutMs），ACK 后可选再等 'C'。
// 'C' 等待只给 200ms：固件把 ACK 与 'C' 背靠背连续发出（ymodem.c YMD_BEGIN/END 的
// data_putc 紧邻），若 ACK 已到而 'C' 未到也继续 —— 接收端收到 ACK 即转下一状态，
// 不依赖我们等到 'C'（C4 明示多余 'C' 无需处理）。200ms 上限同时保证不挤压接收端
// 下一阶段 3s 窗口（头包 ACK 最晚 2.8s 到（修复 C1，见 ymodem.h kHeaderTimeoutMs），
// 块 1 必须落在 3s 窗口内）。
bool YmodemSender::waitAckThenC(int ackTimeoutMs)
{
    std::string noise;
    uint8_t got = 0;
    if (m_state == State::Aborted)
        return false;
    const uint8_t ack = kACK;
    const WaitResult r = waitFor(&ack, 1, ackTimeoutMs, &got, noise);
    if (!noise.empty() && m_cb.log)
        m_cb.log(noise);
    if (r == WaitResult::Aborted) {
        m_state = State::Aborted; // 让调用方拿到精确的终止原因（避免误记“超时”）
        return false;
    }
    if (r != WaitResult::Got)
        return false;
    if (m_cb.log)
        m_cb.log("收到 ACK (0x06)");
    // 可选等 'C'：最多 200ms，超时继续（见上注）
    const uint8_t c = kCRC_C;
    const WaitResult r2 = waitFor(&c, 1, kAfterAckCTimeoutMs, &got, noise);
    if (r2 == WaitResult::Got && m_cb.log)
        m_cb.log("收到 'C' (0x43)");
    return true;
}

// ---------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------

bool YmodemSender::run()
{
    if (m_state != State::WaitingC && m_state != State::SendHeader) {
        // Idle/Done/Error/Aborted：无有效一轮可跑（失败重来由引擎调 start() 重新准备）
        return false;
    }
    if (m_abort.load()) {
        // 进 run 前已被取消（如引擎在 start() 与 run() 之间收到取消请求）
        m_state = State::Aborted;
        return false;
    }
    if (!m_cb.read || !m_cb.readFile) {
        m_state = State::Error;
        return false;
    }
    std::string noise;
    uint8_t got = 0;

    // ---- 阶段 0：WaitingC。引擎在 @W12 后已先行等过 'C'，这里再等一次只是
    //      等接收端下一个 100ms 周期的 'C'（设备在收到 SOH 前一直每 100ms 轮询），
    //      同时让 run() 自洽：直接以 WaitingC 状态进入也能独立完成握手（可测性）。 ----
    if (m_state == State::WaitingC) {
        if (m_cb.log)
            m_cb.log("等待接收端 'C'（最多 30s，期间非协议文本将被丢弃）");
        const uint8_t c = kCRC_C;
        const WaitResult r = waitFor(&c, 1, kWaitCTimeoutMs, &got, noise);
        if (!noise.empty() && m_cb.log)
            m_cb.log(noise);
        if (r == WaitResult::Aborted) {
            m_state = State::Aborted;
            return false;
        }
        if (r != WaitResult::Got) {
            m_state = State::Error;
            if (m_cb.log)
                m_cb.log(r == WaitResult::IoError ? "IO 错误（等待 'C'）"
                                                  : "等待 'C' 超时（30s）");
            return false;
        }
        if (m_cb.log)
            m_cb.log("收到 'C' (0x43)，开始发送头包");
    }

    // ---- 阶段 1：头包。设备头包窗口是"一次性"的 3s（从它第一次见到 SOH 起算，
    //      超时整个退出升级）——所以"一直到收到为止"的无限重发做不到：窗口关闭后
    //      设备不会再回任何东西。折中（用户要求 300ms 节奏）：300ms 等一次，收不到
    //      就重发，2.4s 预算内打满（最多 8 次，留 0.6s 余量）。能救"瞬时噪声打坏
    //      头包/解析器错位"的场景（真机验证：重发后设备重新同步 SOH 即 ACK）；
    //      救不了固件双重让位的确定性缺陷（每次撞同一个 bug，05c §7）。
    //      窗口耗尽仍无 ACK → 整轮重来（A5）。
    //      注意：若固件是"收头包后同步擦 Flash 才回 ACK"的版本（ACK 延迟
    //      1.2~2.4s），300ms 节奏会在 ACK 到达前连发多个头包，多余的帧会污染
    //      后续块解析——真遇到该固件时把首次等待调回 ≥1.6s 即可。 ----
    m_state = State::SendHeader;
    {
        const auto frame = makeHeaderFrame(m_fileName, m_fileSize);
        if (frame.empty()) {
            m_state = State::Error;
            return false;
        }
        if (m_cb.log) {
            m_cb.log("TX 头包 133B: " + logHex(frame.data(), frame.size()));
            m_cb.log("文件名=" + m_fileName + " 大小=" + std::to_string(m_fileSize) + " 共 "
                     + std::to_string(m_totalBlocks) + " 块");
        }
        const int kHeaderRetryMs = 300;    // 重发节奏（用户要求）
        const int kHeaderBudgetMs = 2400;  // 硬上限：接收端一次性 3s 窗口内
        bool acked = false;
        int attempt = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kHeaderBudgetMs);
        while (!acked) {
            const int remainMs = static_cast<int>(std::chrono::duration_cast<
                                                      std::chrono::milliseconds>(
                                                      deadline - std::chrono::steady_clock::now())
                                                      .count());
            if (remainMs <= 0)
                break; // 窗口耗尽：设备已退出升级，再发无意义
            if (attempt > 0 && m_cb.log)
                m_cb.log("头包无应答，重发（第 " + std::to_string(attempt + 1) + " 次）");
            if (!sendFrame(frame)) {
                m_state = m_abort.load() ? State::Aborted : State::Error;
                return false;
            }
            if (waitAckThenC(std::min(kHeaderRetryMs, remainMs))) {
                acked = true;
                break;
            }
            if (m_state == State::Aborted)
                return false;
            ++attempt;
        }
        if (!acked) {
            m_state = State::Error;
            if (m_cb.log)
                m_cb.log("头包重发 " + std::to_string(attempt + 1)
                         + " 次均无 ACK（累计 ~2.4s，受接收端 3s 窗口限制）。"
                         "接收端坏头包静默（D4），由引擎整轮重来");
            return false;
        }
    }

    // ---- 阶段 2：逐块传输（stop-and-wait，A9/D15：收 ACK 再发下一块）。 ----
    m_state = State::SendData;
    for (uint32_t block = 0; block < m_totalBlocks; ++block) {
        if (m_abort.load()) {
            m_state = State::Aborted;
            return false;
        }
        // 读一块（末块可能不足 128 字节）
        uint8_t data[kFramePayload];
        size_t len = 0;
        while (len < kFramePayload) {
            const int n = m_cb.readFile(data + len, kFramePayload - len);
            if (n < 0) {
                m_state = State::Error;
                if (m_cb.log)
                    m_cb.log("读取固件文件失败");
                return false;
            }
            if (n == 0)
                break; // EOF
            len += static_cast<size_t>(n);
        }
        const bool last = (block + 1 == m_totalBlocks);
        if (!last && len < kFramePayload) {
            // 非末块读不满 128：文件实际大小与 start() 声明不符（A6/D6：size 不精确
            // 会让接收端吞掉 EOT，永不响应）。直接失败，避免静默损坏。
            m_state = State::Error;
            if (m_cb.log)
                m_cb.log("文件实际字节数与声明大小不一致（提前 EOF），中止传输");
            return false;
        }
        const auto frame = makeDataFrame(m_seq, data, len);
        m_bytesSent += static_cast<uint32_t>(len);
        if (m_cb.log)
            m_cb.log("TX 块 " + std::to_string(block + 1) + "/" + std::to_string(m_totalBlocks)
                     + " seq=" + std::to_string(m_seq));

        // 每块：首发送 + 最多 2 次重发；累计等待预算 2.4s（< 接收端 3s 窗口，D11）。
        // 注意 D3：丢 ACK 后重发同块，接收端判重复包不回 ACK —— 重试是“最后的希望”，
        // 预算耗尽必须整轮失败，不能无限块级自愈。
        bool acked = false;
        int retries = 0;
        const auto blockDeadline = std::chrono::steady_clock::now()
                                   + std::chrono::milliseconds(kBlockBudgetMs);
        while (!acked) {
            if (m_abort.load()) {
                m_state = State::Aborted;
                return false;
            }
            if (!sendFrame(frame)) {
                m_state = m_abort.load() ? State::Aborted : State::Error;
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= blockDeadline) {
                m_state = State::Error;
                if (m_cb.log)
                    m_cb.log("块 " + std::to_string(block + 1) + " 重试预算耗尽（2.4s）");
                return false;
            }
            const int remain = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(blockDeadline - now).count());
            // CAN 也纳入白名单等待：固件不实现 CAN（D7），但协议完备性上收到即终止
            const uint8_t ackNak[3] = { kACK, kNAK, kCAN };
            const WaitResult r = waitFor(ackNak, 3, std::min(kBlockTimeoutMs, remain), &got,
                                         noise);
            if (!noise.empty() && m_cb.log)
                m_cb.log(noise);
            if (r == WaitResult::Aborted) {
                m_state = State::Aborted;
                return false;
            }
            if (r == WaitResult::IoError) {
                m_state = State::Error;
                return false;
            }
            if (r == WaitResult::Got && got == kCAN) {
                m_state = State::Error;
                if (m_cb.log)
                    m_cb.log("收到 CAN (0x18)，接收端中止会话");
                return false;
            }
            if (r == WaitResult::Got && got == kACK) {
                acked = true;
                if (m_cb.log)
                    m_cb.log("块 " + std::to_string(block + 1) + " ACK");
                break;
            }
            // NAK 或超时 → 重发同块（NAK 理论不会出现：接收端坏块不回 NAK（D2），
            // 此分支为协议完整性保留）
            if (++retries > kMaxBlockRetries) {
                m_state = State::Error;
                if (m_cb.log)
                    m_cb.log("块 " + std::to_string(block + 1)
                             + " 重发 2 次仍未确认，本轮失败（整轮重来）");
                return false;
            }
            if (m_cb.log)
                m_cb.log("块 " + std::to_string(block + 1) + (r == WaitResult::Got ? " NAK" : " 超时")
                         + "，重发（第 " + std::to_string(retries) + "/2 次重试）");
        }
        // 序号 256 取模回绕：255 之后回到 0（接收端 uint8_t pack_num 同语义，
        // 第 256 块期待 seq=0 —— 若重置为 1 会被判序号错误 -2，整块重发也救不回）
        ++m_seq;
        if (m_cb.progress)
            m_cb.progress(static_cast<int>(block + 1), static_cast<int>(m_totalBlocks));
    }

    // 结束一致性检查：实际发送字节数必须等于声明大小（D6）
    if (m_bytesSent != m_fileSize) {
        m_state = State::Error;
        if (m_cb.log)
            m_cb.log("传输字节数 " + std::to_string(m_bytesSent) + " ≠ 声明大小 "
                     + std::to_string(m_fileSize));
        return false;
    }

    // ---- 阶段 3：EOT×2 + 空块（D5/A3：缺空块 = 数据全收但不写 OTA 标志 = 白传）。 ----
    m_state = State::SendEot1;
    {
        if (m_cb.log)
            m_cb.log("TX EOT#1，等待 NAK");
        const uint8_t eot = kEOT;
        if (!sendFrame({ eot })) {
            m_state = m_abort.load() ? State::Aborted : State::Error;
            return false;
        }
        const uint8_t nak = kNAK;
        const WaitResult r = waitFor(&nak, 1, kEotTimeoutMs, &got, noise);
        if (!noise.empty() && m_cb.log)
            m_cb.log(noise);
        if (r == WaitResult::Aborted) {
            m_state = State::Aborted;
            return false;
        }
        if (r != WaitResult::Got) {
            m_state = State::Error;
            if (m_cb.log)
                m_cb.log("EOT#1 未收到 NAK（超时/IO 错误），本轮失败");
            return false;
        }
    }

    m_state = State::SendEot2;
    {
        if (m_cb.log)
            m_cb.log("TX EOT#2，等待 ACK+'C'");
        const uint8_t eot = kEOT;
        if (!sendFrame({ eot })) {
            m_state = m_abort.load() ? State::Aborted : State::Error;
            return false;
        }
        if (!waitAckThenC(kEotTimeoutMs)) {
            if (m_state == State::Aborted)
                return false;
            m_state = State::Error;
            if (m_cb.log)
                m_cb.log("EOT#2 未收到 ACK，本轮失败");
            return false;
        }
    }

    m_state = State::SendEmpty;
    {
        const auto frame = makeEmptyFrame();
        if (m_cb.log)
            m_cb.log("TX 空块 133B（SOH 00 FF + 128×0x00）");
        if (!sendFrame(frame)) {
            m_state = m_abort.load() ? State::Aborted : State::Error;
            return false;
        }
        if (!waitAckThenC(kEotTimeoutMs)) {
            if (m_state == State::Aborted)
                return false;
            m_state = State::Error;
            if (m_cb.log)
                m_cb.log("空块未收到 ACK，本轮失败（接收端将不写 OTA 标志）");
            return false;
        }
        if (m_cb.log)
            m_cb.log("空块 ACK：设备将写 OTA 标志并复位（约 100ms 后）");
    }

    m_state = State::Done;
    return true;
}

} // namespace mycom
