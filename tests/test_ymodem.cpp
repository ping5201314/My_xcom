// YMODEM 发送器单元测试（第二阶段）：组帧逐字节断言、CRC 已知向量、序号 256 回绕、
// 假 IO 打桩跑完整状态机（正常路径/NAK 恢复/超时恢复/连续失败/abort/头包超时/噪声容错）。
// 不依赖真实串口与 Qt：假 IO 的 read/write 完全模拟接收端行为。
// 框架约定同 test_main.cpp：CHECK 宏 + 失败计数；入口 run_ymodem_tests() 由 test_main 调用。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "upgrade/crc16.h"
#include "upgrade/ymodem.h"

static int g_failures = 0;
static int g_passes = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            ++g_passes;                                                     \
            printf("PASS: %s\n", msg);                                      \
        } else {                                                            \
            ++g_failures;                                                   \
            printf("FAIL: %s\n", msg);                                      \
        }                                                                   \
    } while (0)

namespace {

using mycom::YmodemSender;

// 协议字节（与固件 ymodem.h 枚举一致）
constexpr uint8_t kSOH = 0x01;
constexpr uint8_t kEOT = 0x04;
constexpr uint8_t kACK = 0x06;
constexpr uint8_t kNAK = 0x15;
constexpr uint8_t kC = 0x43;

// ---------------------------------------------------------------------------
// 假 IO：模拟接收端（GD32 主控 ymodem 状态机）的应答行为
// ---------------------------------------------------------------------------
struct ScriptedIo {
    std::vector<uint8_t> sent;      // 发送端写入的全部字节
    std::deque<uint8_t> pending;    // 待返回给发送端的字节队列
    size_t parsed = 0;              // sent 中已解析偏移（133B 帧 / 1B EOT）
    bool headerAnswered = false;
    int eotCount = 0;
    std::map<int, int> blockSent;   // seq → 已发送次数
    std::vector<int> seqsSeen;      // 数据帧 seq 出现顺序（回绕断言用）

    // 行为注入
    std::string preNoise;           // 握手前注入的噪声文本（一次性）
    bool preNoiseSent = false;
    bool silentHeader = false;      // 头包不应答（头包 2.8s 超时测试，修复 C1）
    int nakOnceSeq = 0;             // 该 seq 首次应答 NAK（之后正常 ACK）
    bool nakOnceIssued = false;
    int timeoutOnceSeq = 0;         // 该 seq 首次发送不回应（模拟块超时，重发后 ACK）
    bool nakAll = false;            // 所有数据块一律 NAK（连续失败测试）
    int silentBlocksFrom = -1;      // seq ≥ 该值的块永不 ACK（abort 测试）
    int nakTwiceSeq = 0;            // 该 seq 前 2 次应答 NAK，第 3 次 ACK（NAK 恰 2 次恢复）
    int nakAlwaysSeq = 0;           // 该 seq 永远 NAK（NAK 3 次 → 失败）
    int nakThenSilentSeq = 0;       // 该 seq 第 1 次 NAK，之后沉默（重试间隙 abort 测试）
    bool noC = false;               // 等待 'C' 阶段不注入 'C'（WaitingC abort 测试）

    // 解析 sent 尾部新增的完整帧/字节，按接收端语义入队应答
    void parseNewFrames()
    {
        for (;;) {
            if (parsed + 133 <= sent.size()) {
                const uint8_t* f = &sent[parsed];
                parsed += 133;
                if (f[0] != kSOH)
                    continue;
                if (silentHeader)
                    continue; // 静默设备（头包超时测试）：任何帧都不应答，
                              // 头包重发 2 次也不会被误答成"数据块 ACK"
                if (f[1] == 0) {
                    if (!headerAnswered) {
                        headerAnswered = true;
                        pending.push_back(kACK);
                        pending.push_back(kC); // 接收端 YMD_BEGIN：ACK 后紧跟 'C'
                    } else if (eotCount > 0) {
                        pending.push_back(kACK); // 空块 ACK（ymodem.c YMD_END result==2）
                    } else {
                        // SOH 00 且未到 EOT 阶段：seq 回绕为 0 的数据块！
                        // 真实接收端按状态机区分（YMD_RECEIVE 里 seq=0 是合法块，
                        // complete_check(pack_num+1)：255 之后期待 0），这里同样按位置判定。
                        seqsSeen.push_back(0);
                        ++blockSent[0];
                        pending.push_back(nakAll ? kNAK : kACK);
                    }
                } else {
                    const int seq = f[1];
                    seqsSeen.push_back(seq);
                    const int cnt = ++blockSent[seq];
                    if (silentBlocksFrom >= 0 && seq >= silentBlocksFrom) {
                        // 不应答：模拟 D2 坏包不回 NAK / D3 重复包不回 ACK 的沉默
                    } else if (timeoutOnceSeq == seq && cnt == 1) {
                        // 模拟块超时：首次发送不回应（发送端需 1s 超时后重发）
                    } else if (nakTwiceSeq == seq) {
                        // 前 2 次 NAK、第 3 次 ACK：发送端“每块最多 2 次重试”内恢复
                        pending.push_back(cnt <= 2 ? kNAK : kACK);
                    } else if (nakAlwaysSeq == seq) {
                        // 永远 NAK：3 次尝试（首发送 + 2 次重试）后必须整轮失败
                        pending.push_back(kNAK);
                    } else if (nakThenSilentSeq == seq) {
                        // 第 1 次 NAK、之后沉默：重试间隙的 abort 测试场景
                        if (cnt == 1)
                            pending.push_back(kNAK);
                    } else if (nakOnceSeq == seq && !nakOnceIssued) {
                        nakOnceIssued = true;
                        pending.push_back(kNAK);
                    } else if (nakAll) {
                        pending.push_back(kNAK);
                    } else {
                        pending.push_back(kACK);
                    }
                }
                continue;
            }
            if (parsed < sent.size() && sent[parsed] == kEOT) {
                ++parsed;
                ++eotCount;
                if (eotCount == 1)
                    pending.push_back(kNAK); // 首个 EOT 回 NAK（ymodem.c YMD_RECEIVE result==2）
                else {
                    pending.push_back(kACK); // 第二个 EOT 回 ACK+'C'（ymodem.c YMD_END）
                    pending.push_back(kC);
                }
                continue;
            }
            break;
        }
    }

    int write(const uint8_t* d, size_t n)
    {
        sent.insert(sent.end(), d, d + n);
        return static_cast<int>(n);
    }

    int read(uint8_t* b, size_t max, int /*timeout*/)
    {
        if (!preNoiseSent) {
            for (unsigned char c : preNoise)
                pending.push_back(static_cast<uint8_t>(c));
            preNoiseSent = true;
        }
        parseNewFrames();
        // 接收端 INITIATE：收不到 SOH 前每 100ms 发一次 'C'，这里每次读请求补一个。
        // 任何场景（含头包超时/abort 测试）都先给 'C'：WaitingC 必须能通过才能测后续阶段。
        // noC=true 时不给 'C'：专门模拟“设备永远不进入握手”（WaitingC abort 测试）。
        if (parsed == 0 && pending.empty() && !noC)
            pending.push_back(kC);
        if (pending.empty())
            return 0; // 0 = 超时
        const size_t take = std::min(max, pending.size());
        for (size_t i = 0; i < take; ++i) {
            b[i] = pending.front();
            pending.pop_front();
        }
        return static_cast<int>(take);
    }
};

// 生成确定性测试文件内容（i%251，覆盖 0x00/0x04/0x06 等协议字节，验证帧解析不串扰）
std::vector<uint8_t> makeFile(size_t size)
{
    std::vector<uint8_t> v(size);
    for (size_t i = 0; i < size; ++i)
        v[i] = static_cast<uint8_t>(i % 251);
    return v;
}

// 组装一次完整传输：返回 run() 结果，进度/日志可选收集
struct RunResult {
    bool ok = false;
    std::vector<std::pair<int, int>> progress;
    std::vector<std::string> logs;
};

RunResult runTransfer(YmodemSender& s, ScriptedIo& io, const std::vector<uint8_t>& file)
{
    RunResult r;
    mycom::YmodemSender::Callbacks cb;
    cb.write = [&io](const uint8_t* d, size_t n) { return io.write(d, n); };
    cb.read = [&io](uint8_t* b, size_t m, int) { return io.read(b, m, 0); };
    size_t off = 0;
    cb.readFile = [&file, &off](uint8_t* b, size_t m) -> int {
        if (off >= file.size())
            return 0;
        const size_t take = std::min(m, file.size() - off);
        std::memcpy(b, file.data() + off, take);
        off += take;
        return static_cast<int>(take);
    };
    cb.progress = [&r](int d, int t) { r.progress.push_back({ d, t }); };
    cb.log = [&r](const std::string& s) { r.logs.push_back(s); };
    s.setCallbacks(cb);
    if (!s.start("test.bin", static_cast<uint32_t>(file.size())))
        return r;
    r.ok = s.run();
    return r;
}

// 解析发送端写入的字节流（帧结构断言用）。
// 按位置区分三类 SOH 00 帧：首个 = 头包；EOT×2 之后 = 批次结束空块；
// 其余（头包之后、EOT 之前）= seq 回绕为 0 的数据块（256 块回绕测试需要）。
struct ParsedTransfer {
    std::vector<uint8_t> headerPayload; // 头包 128 字节 payload
    struct Block {
        int seq;
        uint8_t comp; // 帧内 ~seq 反码字节（回绕后帧结构断言用）
        std::vector<uint8_t> data;
    };
    std::vector<Block> blocks;
    int eots = 0;
    std::vector<uint8_t> emptyPayload; // 空块 payload（全零）
    bool headerOk = false, emptyOk = false;
};

ParsedTransfer parseSent(const std::vector<uint8_t>& sent)
{
    ParsedTransfer p;
    size_t cur = 0;
    while (cur < sent.size()) {
        if (sent[cur] == kEOT) {
            ++p.eots;
            ++cur;
            continue;
        }
        if (cur + 133 > sent.size())
            break; // 不完整帧（理论不可达）
        const uint8_t* f = &sent[cur];
        if (f[0] != kSOH)
            break;
        std::vector<uint8_t> payload(f + 3, f + 3 + 128);
        if (f[1] == 0) {
            if (!p.headerOk) {
                p.headerPayload = payload;
                p.headerOk = true;
            } else if (p.eots > 0) {
                p.emptyPayload = payload;
                p.emptyOk = true;
            } else {
                // seq 回绕为 0 的数据块（第 256 块）
                p.blocks.push_back({ 0, f[2], payload });
            }
        } else {
            p.blocks.push_back({ f[1], f[2], payload });
        }
        cur += 133;
    }
    return p;
}

// 帧内 CRC 校验：线上高字节在前
bool frameCrcOk(const std::vector<uint8_t>& sent, size_t frameOff)
{
    if (frameOff + 133 > sent.size())
        return false;
    const uint16_t crcLine = static_cast<uint16_t>((sent[frameOff + 131] << 8) | sent[frameOff + 132]);
    return crcLine == mycom::Crc16::xmodem(&sent[frameOff + 3], 128);
}

} // namespace

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

static void testCrcAndFraming()
{
    // CRC-16/XMODEM 标准校验向量（poly 0x1021、初值 0、MSB 先；与固件 crc.c CCITT 表一致）
    {
        const uint8_t vec[] = "123456789";
        CHECK(mycom::Crc16::xmodem(vec, 9) == 0x31C3, "CRC 已知向量 \"123456789\" → 0x31C3");
        CHECK(mycom::Crc16::xmodem(nullptr, 0) == 0, "CRC 空数据 → 0");
    }
    // 头包逐字节结构：SOH 00 FF + "name\0size\0" + 0x00 填充至 128 + CRC（高字节在前）
    {
        const auto h = YmodemSender::makeHeaderFrame("Finca_updata.bin", 73728);
        CHECK(h.size() == 133, "头包 133 字节");
        CHECK(h[0] == kSOH && h[1] == 0x00 && h[2] == 0xFF, "头包 SOH 00 FF");
        CHECK(std::strcmp(reinterpret_cast<const char*>(&h[3]), "Finca_updata.bin") == 0,
              "头包 payload 文件名");
        CHECK(std::strcmp(reinterpret_cast<const char*>(&h[3 + 17]), "73728") == 0,
              "头包 payload 大小十进制（NUL 分隔）");
        bool tailZero = true;
        for (size_t i = 3 + 17 + 6; i < 131; ++i) {
            if (h[i] != 0)
                tailZero = false;
        }
        CHECK(tailZero, "头包 payload 其余 0x00 填充");
        const uint16_t crcLine = static_cast<uint16_t>((h[131] << 8) | h[132]);
        CHECK(crcLine == mycom::Crc16::xmodem(&h[3], 128), "头包 CRC（线上高字节在前）");
    }
    // 数据块：SOH seq ~seq + 128 数据 + CRC；末块 0x1A 填充
    {
        uint8_t full[128];
        for (int i = 0; i < 128; ++i)
            full[i] = static_cast<uint8_t>(i);
        const auto d = YmodemSender::makeDataFrame(1, full, 128);
        CHECK(d.size() == 133 && d[0] == kSOH && d[1] == 1 && d[2] == 0xFE,
              "数据块 SOH 01 FE");
        CHECK(std::memcmp(&d[3], full, 128) == 0, "数据块 payload 原样");
        const uint16_t crcLine = static_cast<uint16_t>((d[131] << 8) | d[132]);
        CHECK(crcLine == mycom::Crc16::xmodem(&d[3], 128), "数据块 CRC");

        uint8_t part[10] = { 0 };
        for (int i = 0; i < 10; ++i)
            part[i] = static_cast<uint8_t>(i + 1);
        const auto p = YmodemSender::makeDataFrame(255, part, 10);
        CHECK(p[1] == 255 && p[2] == 0x00, "seq=255 反码 0x00");
        bool pad = true;
        for (int i = 13; i < 131; ++i) {
            if (p[i] != 0x1A)
                pad = false;
        }
        CHECK(pad, "末块不足 128 字节以 0x1A 填充");
    }
    // 空块：SOH 00 FF + 128×0x00 + CRC
    {
        const auto e = YmodemSender::makeEmptyFrame();
        bool allZero = true;
        for (int i = 3; i < 131; ++i) {
            if (e[i] != 0)
                allZero = false;
        }
        CHECK(e.size() == 133 && e[0] == kSOH && e[1] == 0 && e[2] == 0xFF && allZero,
              "空块 SOH 00 FF + 全零 payload");
        const uint16_t crcLine = static_cast<uint16_t>((e[131] << 8) | e[132]);
        CHECK(crcLine == mycom::Crc16::xmodem(&e[3], 128), "空块 CRC");
    }
}

static void testStartValidation()
{
    YmodemSender s;
    CHECK(!s.start("", 100), "start 拒绝空文件名");
    CHECK(s.start(std::string(31, 'a'), 100), "start 接受 31 字符文件名");
    CHECK(s.state() == YmodemSender::State::WaitingC, "start 成功后 WaitingC");
    CHECK(!s.start("a/b.bin", 100), "start 拒绝路径分隔符 '/'");
    CHECK(!s.start("a\\b.bin", 100), "start 拒绝路径分隔符 '\\'");
    CHECK(!s.start("a.bin", 0), "start 拒绝大小 0");
}

static void testNormalTransfer()
{
    // 394 字节 = 3 个满块 + 1 个 10 字节末块；含噪声注入（文本回显被白名单过滤）
    const auto file = makeFile(394);
    ScriptedIo io;
    io.preNoise = "ymode_updata stra\r\nfactory data reset11111\r\n";
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(r.ok, "正常路径 run() 返回 true");
    CHECK(s.state() == YmodemSender::State::Done, "正常路径状态 Done");

    const ParsedTransfer p = parseSent(io.sent);
    CHECK(p.headerOk, "已发头包");
    CHECK(p.blocks.size() == 4, "4 个数据块");
    CHECK(io.eotCount == 2, "EOT 发送 2 次");
    CHECK(p.emptyOk, "已发批次结束空块");
    if (p.headerOk) {
        CHECK(std::strcmp(reinterpret_cast<const char*>(p.headerPayload.data()),
                          "test.bin") == 0,
              "头包文件名 test.bin");
        CHECK(std::strcmp(reinterpret_cast<const char*>(p.headerPayload.data() + 9),
                          "394") == 0,
              "头包大小精确等于文件字节数 394（A6/D6）");
    }
    if (p.blocks.size() == 4) {
        CHECK(p.blocks[0].seq == 1 && p.blocks[1].seq == 2 && p.blocks[2].seq == 3
                  && p.blocks[3].seq == 4,
              "数据块序号 1/2/3/4");
        CHECK(std::memcmp(p.blocks[3].data.data(), file.data() + 384, 10) == 0,
              "末块前 10 字节为文件内容");
        bool pad = true;
        for (int i = 10; i < 128; ++i) {
            if (p.blocks[3].data[i] != 0x1A)
                pad = false;
        }
        CHECK(pad, "末块其余 0x1A 填充");
    }
    // 每帧 CRC 逐帧复核（用发送端 Crc16 独立重算；注意跳过两个 EOT 字节）
    {
        bool allOk = frameCrcOk(io.sent, 0); // 头包
        size_t off = 133;
        for (size_t i = 0; i < p.blocks.size(); ++i) {
            allOk = allOk && frameCrcOk(io.sent, off);
            off += 133;
        }
        off += static_cast<size_t>(p.eots); // 跳过 EOT×2
        allOk = allOk && frameCrcOk(io.sent, off); // 空块
        CHECK(allOk, "全部帧 CRC 正确");
    }
    CHECK(r.progress.size() == 4 && r.progress.back().first == 4 && r.progress.back().second == 4,
          "进度回调 4 次且以 (4,4) 收尾");
    // 噪声过滤：文本回显必须被丢弃并记日志（白名单解析，05c B3/D14）
    bool noiseLogged = false;
    for (const auto& l : r.logs) {
        if (l.find("丢弃非协议字节") != std::string::npos)
            noiseLogged = true;
    }
    CHECK(noiseLogged, "噪声文本被丢弃并记日志（白名单解析）");
}

static void testNakOnceRecovers()
{
    const auto file = makeFile(394);
    ScriptedIo io;
    io.nakOnceSeq = 2; // 块 2 首次应答 NAK，重发后 ACK
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(r.ok, "块 NAK 一次后重发成功");
    CHECK(s.state() == YmodemSender::State::Done, "NAK 恢复后 Done");
    CHECK(io.blockSent[2] == 2, "块 2 发送 2 次（NAK 重发 1 次）");
    CHECK(io.blockSent[3] == 1, "后续块不受影响只发 1 次");
}

static void testBlockTimeoutOnceRecovers()
{
    // 块 1 首次发送不回应：发送端 1s 超时后重发（真实等待 ~1s，验证 D11 节拍）
    const auto file = makeFile(394);
    ScriptedIo io;
    io.timeoutOnceSeq = 1;
    YmodemSender s;
    const auto t0 = std::chrono::steady_clock::now();
    const RunResult r = runTransfer(s, io, file);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    CHECK(r.ok, "块超时一次后重发成功");
    CHECK(s.state() == YmodemSender::State::Done, "超时恢复后 Done");
    CHECK(io.blockSent[1] == 2, "块 1 发送 2 次（超时重发 1 次）");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "块超时等待 ≈1s（实测 %lld ms，≤1s 单次 + <2.5s 累计，D11）",
                  static_cast<long long>(elapsedMs));
    CHECK(elapsedMs >= 900 && elapsedMs <= 3000, msg);
}

static void testContinuousNakFails()
{
    // 所有块一律 NAK：3 次尝试后放弃（每块最多 2 次重试，A2），快速失败（无真实等待）
    const auto file = makeFile(394);
    ScriptedIo io;
    io.nakAll = true;
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(!r.ok, "连续 NAK → run() false");
    CHECK(s.state() == YmodemSender::State::Error, "连续 NAK → Error");
    CHECK(io.blockSent[1] == 3, "块 1 共发送 3 次（首发送 + 2 次重试上限）");
}

static void testSeqWrap256()
{
    // 258 块（33024 字节）：seq 必须 1..255 后回绕 0（接收端 uint8_t pack_num 同语义，
    // 第 256 块期待 seq=0，若重置为 1 会被判 -2 序号错误）
    const auto file = makeFile(258 * 128);
    ScriptedIo io;
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(r.ok, "258 块传输成功");
    CHECK(s.state() == YmodemSender::State::Done, "258 块 Done");
    CHECK(io.seqsSeen.size() == 258, "seq 序列共 258 个");
    bool wrapOk = io.seqsSeen.size() == 258;
    for (size_t i = 0; wrapOk && i < io.seqsSeen.size(); ++i) {
        if (io.seqsSeen[i] != static_cast<int>((i + 1) & 0xFF))
            wrapOk = false;
    }
    CHECK(wrapOk, "seq 按 256 取模回绕（…255,0,1,2）");
    CHECK(r.progress.size() == 258 && r.progress.back().first == 258,
          "进度覆盖 258 块");
}

static void testAbortPromptExit()
{
    // 块 2 起永不应答 → run() 阻塞在块 2 等待；300ms 后从另一线程 abort()，
    // 等待点 100ms 分片内必须退出（“任何等待点及时退出”）
    const auto file = makeFile(394);
    ScriptedIo io;
    io.silentBlocksFrom = 2;
    YmodemSender s;
    mycom::YmodemSender::Callbacks cb;
    cb.write = [&io](const uint8_t* d, size_t n) { return io.write(d, n); };
    cb.read = [&io](uint8_t* b, size_t m, int) { return io.read(b, m, 0); };
    size_t off = 0;
    cb.readFile = [&file, &off](uint8_t* b, size_t m) -> int {
        if (off >= file.size())
            return 0;
        const size_t take = std::min(m, file.size() - off);
        std::memcpy(b, file.data() + off, take);
        off += take;
        return static_cast<int>(take);
    };
    s.setCallbacks(cb);
    CHECK(s.start("test.bin", 394), "abort 测试 start");

    bool runRet = true;
    std::thread th([&] { runRet = s.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto t0 = std::chrono::steady_clock::now();
    s.abort(); // 跨线程取消（原子标志）
    th.join();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    CHECK(!runRet, "abort 后 run() 返回 false");
    CHECK(s.state() == YmodemSender::State::Aborted, "abort 后状态 Aborted");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "abort 退出延迟 <1s（实测 %lld ms，等待点 100ms 分片）",
                  static_cast<long long>(elapsedMs));
    CHECK(elapsedMs < 1000, msg);
}

static void testHeaderTimeoutFails()
{
    // 头包不应答：3s 窗口内重发 1 次（1600ms + 800ms ≈ 2.4s）后仍无 ACK → false。
    // 用户反馈：像设备轮询 'C' 一样窗口内重发头包；但接收端窗口是一次性 3s，
    // 超时后必须整轮重来（A5）
    const auto file = makeFile(394);
    ScriptedIo io;
    io.silentHeader = true;
    YmodemSender s;
    const auto t0 = std::chrono::steady_clock::now();
    const RunResult r = runTransfer(s, io, file);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    CHECK(!r.ok, "头包窗口内重发打满仍无响应 → run() false");
    CHECK(s.state() == YmodemSender::State::Error, "头包超时 → Error");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "头包重发超时 ≈2.4s（实测 %lld ms，300ms×8 次）",
                  static_cast<long long>(elapsedMs));
    CHECK(elapsedMs >= 2200 && elapsedMs <= 4000, msg);
    // 头包在 2.4s 窗口内以 300ms 节奏重发 8 次（共 1064 字节），未进入数据块/EOT 阶段
    CHECK(io.sent.size() == 8 * 133, "头包重发 8 次（共 1064 字节），未发数据块");
}

// ---------------------------------------------------------------------------
// T4 扩展用例
// ---------------------------------------------------------------------------

// abort 场景通用骨架：配置 io 行为 → 线程跑 run() → delayMs 后 abort → 测退出耗时/状态
struct AbortOutcome {
    bool ok = false;
    YmodemSender::State st = YmodemSender::State::Idle;
    long long exitMs = 0;
};

static AbortOutcome runAbortCase(const std::function<void(ScriptedIo&)>& cfg, int delayMs)
{
    const auto file = makeFile(394);
    ScriptedIo io;
    cfg(io);
    YmodemSender s;
    YmodemSender::Callbacks cb;
    cb.write = [&io](const uint8_t* d, size_t n) { return io.write(d, n); };
    cb.read = [&io](uint8_t* b, size_t m, int) { return io.read(b, m, 0); };
    size_t off = 0;
    cb.readFile = [&file, &off](uint8_t* b, size_t m) -> int {
        if (off >= file.size())
            return 0;
        const size_t take = std::min(m, file.size() - off);
        std::memcpy(b, file.data() + off, take);
        off += take;
        return static_cast<int>(take);
    };
    s.setCallbacks(cb);
    AbortOutcome o;
    if (!s.start("test.bin", 394))
        return o;
    std::thread th([&] { o.ok = s.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    const auto t0 = std::chrono::steady_clock::now();
    s.abort();
    th.join();
    o.exitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
    o.st = s.state();
    return o;
}

static void testNakTwiceRecovers()
{
    // 块 3 前 2 次应答 NAK、第 3 次 ACK：发送端在“每块最多 2 次重试”内恢复，
    // 块 3 共发送 3 次（首发送 + 2 次重试），后续块不受影响。
    const auto file = makeFile(394);
    ScriptedIo io;
    io.nakTwiceSeq = 3;
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(r.ok, "NAK 恰好 2 次后第 3 次发送 ACK → run() true");
    CHECK(s.state() == YmodemSender::State::Done, "NAK×2 恢复后 Done");
    CHECK(io.blockSent[3] == 3, "块 3 共发送 3 次（首发送 + 2 次重试上限内）");
    CHECK(io.blockSent[4] == 1, "块 4 不受影响只发 1 次");
}

static void testNakThriceFails()
{
    // 块 1 永远 NAK：3 次尝试（首发送 + 2 次重试）后整轮失败（A2：每块最多 2 次重试）。
    const auto file = makeFile(394);
    ScriptedIo io;
    io.nakAlwaysSeq = 1;
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(!r.ok, "连续 NAK 3 次 → run() false");
    CHECK(s.state() == YmodemSender::State::Error, "NAK×3 → Error");
    CHECK(io.blockSent[1] == 3, "块 1 共发送 3 次后放弃");
    // 失败后 run() 不重入：引擎必须重新 start() 整轮重来（A5），不能原地重试
    CHECK(!s.run(), "Error 后 run() 拒绝重入（须重新 start() 整轮重来）");
    CHECK(io.blockSent[1] == 3, "重入被拒，未多发任何块");
}

static void testBlockBudgetExhausted()
{
    // 块 1 永不回应：首发送超时 1s → 重发 1 超时 1s → 重发 2 等待至 2.4s 预算耗尽。
    // 累计等待 >2.4s（接收端 3s 窗口约束，D11）→ 整轮失败，块 1 共发送 3 次。
    const auto file = makeFile(394);
    ScriptedIo io;
    io.silentBlocksFrom = 1;
    YmodemSender s;
    const auto t0 = std::chrono::steady_clock::now();
    const RunResult r = runTransfer(s, io, file);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    CHECK(!r.ok, "块持续无响应 → run() false");
    CHECK(s.state() == YmodemSender::State::Error, "预算耗尽 → Error");
    CHECK(io.blockSent[1] == 3, "块 1 共发送 3 次（首发送 + 2 次重试上限）");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "累计重试预算 ≈2.4s（实测 %lld ms，<2.5s 规范 D11）",
                  static_cast<long long>(elapsedMs));
    CHECK(elapsedMs >= 2300 && elapsedMs <= 4000, msg);
}

static void testSeqWrapFrameStructure()
{
    // 258 块：除 seq 序列 1..255,0,1,2 外，逐帧断言 SOH/seq/~seq 三字节结构、
    // 每帧 CRC（高字节在前）、EOT×2 与批次结束空块的发送次序。
    const auto file = makeFile(258 * 128);
    ScriptedIo io;
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(r.ok, "258 块传输成功");
    CHECK(s.state() == YmodemSender::State::Done, "258 块 Done");

    const ParsedTransfer p = parseSent(io.sent);
    CHECK(p.headerOk && p.blocks.size() == 258, "帧结构：头包 + 258 个数据块");
    CHECK(p.eots == 2, "帧结构：EOT 发送 2 次");
    CHECK(p.emptyOk, "帧结构：批次结束空块存在（EOT×2 之后）");

    bool seqOk = p.blocks.size() == 258;
    bool compOk = p.blocks.size() == 258;
    bool crcOk = p.headerOk && p.blocks.size() == 258 && p.emptyOk;
    for (size_t i = 0; i < p.blocks.size(); ++i) {
        const int expectSeq = static_cast<int>((i + 1) & 0xFF);
        if (p.blocks[i].seq != expectSeq)
            seqOk = false;
        if (p.blocks[i].comp != static_cast<uint8_t>(~expectSeq))
            compOk = false;
    }
    CHECK(seqOk, "接收序：seq 严格按 1..255,0,1,2 取模回绕");
    CHECK(compOk, "帧结构：每个数据块 ~seq 反码正确（含回绕块 0xFF）");

    // 逐帧 CRC（帧内字节序：CRC 高字节在前）
    if (crcOk) {
        size_t off = 0;
        crcOk = frameCrcOk(io.sent, off); // 头包
        off += 133;
        for (size_t i = 0; i < p.blocks.size(); ++i) {
            crcOk = crcOk && frameCrcOk(io.sent, off);
            off += 133;
        }
        off += static_cast<size_t>(p.eots); // EOT×2
        crcOk = crcOk && frameCrcOk(io.sent, off); // 空块
    }
    CHECK(crcOk, "258 块传输全部帧（含回绕块与空块）CRC 正确");

    // 空块出现在 EOT×2 之后（发送次序：头包 → 数据块×258 → EOT → EOT → 空块）
    const size_t emptyOff = 133 + 258 * 133 + 2;
    CHECK(emptyOff + 133 <= io.sent.size() && io.sent[emptyOff] == kSOH
              && io.sent[emptyOff + 1] == 0x00 && io.sent[emptyOff + 2] == 0xFF,
          "空块位于 EOT×2 之后的固定偏移（发送次序正确）");
    CHECK(r.progress.size() == 258 && r.progress.back().first == 258,
          "进度覆盖 258 块");
}

static void testHeaderFileNameUtf8Boundary()
{
    // 头包文件名 ≤31 字节边界（字节数，非字符数；中文按 UTF-8 计 3 字节/字）。
    // 用字节转义构造，避免源文件编码歧义：E5 8D 87 = "升"。
    const std::string ch = "\xE5\x8D\x87";
    const std::string name30 = ch + ch + ch + ch + ch + ch + ch + ch + ch + ch; // 10×3 = 30 字节
    const std::string name31 = ch + ch + ch + ch + ch + ch + ch + ch + ch + "abcd"; // 27+4 = 31
    const std::string name32 = name31 + "e"; // 32 字节

    {
        YmodemSender s;
        CHECK(s.start(name30, 100), "30 字节 UTF-8 文件名接受");
        const auto h = YmodemSender::makeHeaderFrame(name30, 100);
        CHECK(h.size() == 133, "30 字节名头包 133 字节");
        CHECK(std::memcmp(&h[3], name30.data(), 30) == 0, "30 字节名逐字节回传");
        CHECK(h[3 + 30] == 0x00, "30 字节名后紧跟 NUL");
        CHECK(std::strcmp(reinterpret_cast<const char*>(&h[3 + 31]), "100") == 0,
              "30 字节名后大小字段正确");
    }
    {
        YmodemSender s;
        CHECK(s.start(name31, 1234567), "31 字节 UTF-8 文件名接受（边界值）");
        const auto h = YmodemSender::makeHeaderFrame(name31, 1234567);
        CHECK(h.size() == 133, "31 字节名头包 133 字节");
        CHECK(std::memcmp(&h[3], name31.data(), 31) == 0, "31 字节名逐字节回传（中文无截断）");
        CHECK(h[3 + 31] == 0x00, "31 字节名后紧跟 NUL");
        CHECK(std::strcmp(reinterpret_cast<const char*>(&h[3 + 32]), "1234567") == 0,
              "31 字节名后大小字段正确");
    }
    {
        YmodemSender s;
        CHECK(s.start(name32, 100), "32 字节 UTF-8 文件名接受（自动截断，不再拒绝）");
        CHECK(s.fileName() == name31, "截断到 31 字节且不切断多字节字符（32=31+'e' → 去尾 'e'）");
    }
    {
        YmodemSender s;
        const std::string longName = std::string(30, 'a') + ".bin"; // 34 字节，带 .bin 后缀
        CHECK(s.start(longName, 100), "34 字节带 .bin 文件名接受");
        CHECK(s.fileName() == std::string(27, 'a') + ".bin",
              "截断保留 .bin 后缀（27 字节基名 + 后缀 = 31 字节）");
    }
    // 纯 ASCII 31 字节边界（与 UTF-8 混排口径一致：一律按字节计）
    {
        YmodemSender s;
        CHECK(s.start(std::string(31, 'a'), 100), "31 字节 ASCII 文件名接受");
        CHECK(s.start(std::string(32, 'a'), 100), "32 字节 ASCII 文件名接受（截断）");
        CHECK(s.fileName() == std::string(31, 'a'), "32 字节 ASCII 截断为 31 字节");
    }
}

static void testLastBlockPaddingPrecision()
{
    // 末块 0x1A 填充精确性：只填充“数据之后”的字节，不侵入数据区。
    // 场景 A：文件 129 字节 = 1 满块 + 1 字节末块 → 末块后 127 字节必须全 0x1A。
    {
        const auto file = makeFile(129);
        ScriptedIo io;
        YmodemSender s;
        const RunResult r = runTransfer(s, io, file);
        CHECK(r.ok, "129 字节（末块 1 字节）传输成功");
        const ParsedTransfer p = parseSent(io.sent);
        CHECK(p.blocks.size() == 2, "129 字节 → 2 个数据块");
        if (p.blocks.size() == 2) {
            CHECK(p.blocks[1].data[0] == file[128], "末块第 1 字节为文件内容");
            bool pad = true;
            for (int i = 1; i < 128; ++i) {
                if (p.blocks[1].data[i] != 0x1A)
                    pad = false;
            }
            CHECK(pad, "末块其余 127 字节精确 0x1A 填充");
        }
    }
    // 场景 B：文件 256 字节 = 恰好 2 满块 → 末块不填充，逐字节等于文件内容
    //（文件模式本身含 0x1A 字节，可证“填充不侵入数据”）。
    {
        const auto file = makeFile(256);
        ScriptedIo io;
        YmodemSender s;
        const RunResult r = runTransfer(s, io, file);
        CHECK(r.ok, "256 字节（128 整倍数）传输成功");
        const ParsedTransfer p = parseSent(io.sent);
        CHECK(p.blocks.size() == 2, "256 字节 → 2 个数据块");
        if (p.blocks.size() == 2) {
            CHECK(std::memcmp(p.blocks[1].data.data(), file.data() + 128, 128) == 0,
                  "末块（128 整倍数）无填充、逐字节等于文件内容");
        }
    }
    // 组帧函数直测：len=0 时整块 0x1A（防御语义）；len=128 时无填充
    {
        const auto f0 = YmodemSender::makeDataFrame(1, nullptr, 0);
        bool all = true;
        for (int i = 3; i < 131; ++i) {
            if (f0[i] != 0x1A)
                all = false;
        }
        CHECK(all, "makeDataFrame len=0 → payload 全 0x1A");
    }
}

static void testEmptyBlockByteByByte()
{
    // 批次结束空块逐字节：SOH 00 FF + 128×0x00 + CRC（D5/A3：不发空块 = 白传）。
    const auto file = makeFile(10);
    ScriptedIo io;
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(r.ok, "10 字节文件传输成功");
    const ParsedTransfer p = parseSent(io.sent);
    CHECK(p.emptyOk, "空块存在");
    if (p.emptyOk) {
        bool allZero = true;
        for (int i = 0; i < 128; ++i) {
            if (p.emptyPayload[i] != 0x00) {
                allZero = false;
                char msg[96];
                std::snprintf(msg, sizeof(msg), "空块 payload[%d] == 0x00", i);
                CHECK(false, msg);
                break; // 报首个违例即可，避免刷屏
            }
        }
        if (allZero)
            CHECK(true, "空块 payload 128 字节逐字节全 0x00");
    }
    // 空块帧头与 CRC（高字节在前）
    {
        const size_t off = 133 + 133 + 2; // 头包 + 1 数据块 + EOT×2
        CHECK(off + 133 <= io.sent.size() && io.sent[off] == kSOH
                  && io.sent[off + 1] == 0x00 && io.sent[off + 2] == 0xFF,
              "空块帧头 SOH 00 FF");
        if (off + 133 <= io.sent.size()) {
            const uint16_t crcLine = static_cast<uint16_t>(
                (io.sent[off + 131] << 8) | io.sent[off + 132]);
            CHECK(crcLine == mycom::Crc16::xmodem(&io.sent[off + 3], 128), "空块 CRC 正确");
        }
    }
}

static void testNoiseWhitelistFilter()
{
    // 噪声注入：ASCII 文本回显（非白名单）+ 白名单控制字节 0x06/0x15/0x04/0x18 +
    // 白名单 'C' 0x43。预期：文本被“丢弃非协议字节”日志丢弃；0x06/0x15/0x04/0x18
    // 被“忽略多余协议字节”日志丢弃（等待 'C' 阶段只认 0x43）；0x43 直接完成握手。
    const auto file = makeFile(394);
    ScriptedIo io;
    io.preNoise = std::string("ymode_updata stra\r\n")
                  + static_cast<char>(0x06) + static_cast<char>(0x15)
                  + static_cast<char>(0x04) + static_cast<char>(0x18)
                  + "factory data reset11111\r\n" + static_cast<char>(0x43);
    YmodemSender s;
    const RunResult r = runTransfer(s, io, file);
    CHECK(r.ok, "含白名单字节噪声注入后传输成功");
    CHECK(s.state() == YmodemSender::State::Done, "噪声注入后 Done");
    bool noiseLogged = false, extraLogged = false;
    for (const auto& l : r.logs) {
        if (l.find("丢弃非协议字节") != std::string::npos)
            noiseLogged = true;
        if (l.find("忽略多余协议字节") != std::string::npos)
            extraLogged = true;
    }
    CHECK(noiseLogged, "非白名单文本被丢弃并记日志");
    CHECK(extraLogged, "白名单但非当前期待字节（06/15/04/18）被忽略并记日志");
    const ParsedTransfer p = parseSent(io.sent);
    CHECK(p.headerOk && p.blocks.size() == 4 && p.emptyOk,
          "噪声注入未破坏帧流（头包 + 4 块 + 空块）");
}

static void testAbortDuringWaitingC()
{
    // abort 时点 ①：等待 'C' 握手期间（设备永不发 'C'，30s 等待中取消）。
    const AbortOutcome o = runAbortCase([](ScriptedIo& io) { io.noC = true; }, 300);
    CHECK(!o.ok, "WaitingC 期间 abort → run() false");
    CHECK(o.st == YmodemSender::State::Aborted, "WaitingC 期间 abort → Aborted");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "WaitingC 时点 abort 退出耗时 %lld ms（<1.5s 规范）",
                  static_cast<long long>(o.exitMs));
    CHECK(o.exitMs < 1500, msg);
}

static void testAbortDuringHeaderWait()
{
    // abort 时点 ②：头包 ACK 2.8s 等待期间（头包不应答场景，修复 C1 后窗口变长）。
    const AbortOutcome o = runAbortCase([](ScriptedIo& io) { io.silentHeader = true; }, 300);
    CHECK(!o.ok, "头包等待期间 abort → run() false");
    CHECK(o.st == YmodemSender::State::Aborted, "头包等待期间 abort → Aborted");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "头包等待时点 abort 退出耗时 %lld ms（<1.5s 规范）",
                  static_cast<long long>(o.exitMs));
    CHECK(o.exitMs < 1500, msg);
}

static void testAbortDuringRetryGap()
{
    // abort 时点 ③：重试间隙 —— 块 1 首发送收 NAK，重发后接收端沉默，
    // 发送端处于“NAK 后重发块的 1s 响应等待”中，此时取消必须及时退出。
    const AbortOutcome o = runAbortCase([](ScriptedIo& io) { io.nakThenSilentSeq = 1; }, 500);
    CHECK(!o.ok, "重试间隙 abort → run() false");
    CHECK(o.st == YmodemSender::State::Aborted, "重试间隙 abort → Aborted");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "重试间隙时点 abort 退出耗时 %lld ms（<1.5s 规范）",
                  static_cast<long long>(o.exitMs));
    CHECK(o.exitMs < 1500, msg);
}

int run_ymodem_tests()
{
    testCrcAndFraming();
    testStartValidation();
    testNormalTransfer();
    testNakOnceRecovers();
    testBlockTimeoutOnceRecovers();
    testContinuousNakFails();
    testSeqWrap256();
    testAbortPromptExit();
    testHeaderTimeoutFails();
    // T4 扩展
    testNakTwiceRecovers();
    testNakThriceFails();
    testBlockBudgetExhausted();
    testSeqWrapFrameStructure();
    testHeaderFileNameUtf8Boundary();
    testLastBlockPaddingPrecision();
    testEmptyBlockByteByByte();
    testNoiseWhitelistFilter();
    testAbortDuringWaitingC();
    testAbortDuringHeaderWait();
    testAbortDuringRetryGap();
    printf("SECTION ymodem: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
