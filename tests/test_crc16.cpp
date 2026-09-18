// CRC-16/XMODEM 单元测试（T4 扩展）：多组已知向量、长度 1~4 边界、空数据/单字节、
// 大块（10KB 全 0 / 全 FF / i%251 模式）校验稳定性。
// 已知向量由独立的查表参考实现生成（poly 0x1021、初值 0、MSB 先，非反射），
// 并锚定标准校验向量 "123456789" → 0x31C3（与 Finca 接收端 crc.c 的 CCITT 表实现一致）。
// 框架约定同 test_main.cpp：CHECK 宏 + 失败计数；入口 run_crc16_tests() 由 test_main 调用。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "upgrade/crc16.h"

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

using mycom::Crc16;

} // namespace

static void testKnownVectors()
{
    // 标准校验向量（CRC-16/XMODEM 权威值：poly 0x1021、初值 0、MSB 先）
    {
        const uint8_t v[] = "123456789";
        CHECK(Crc16::xmodem(v, 9) == 0x31C3, "CRC 标准向量 \"123456789\" → 0x31C3");
    }
    // 参考实现交叉验证向量（查表法独立生成，与被测位级实现算法不同）
    {
        const uint8_t ab[] = { 'A', 'B' };
        CHECK(Crc16::xmodem(ab, 2) == 0x567B, "CRC 向量 \"AB\" → 0x567B");
        const uint8_t abc[] = { 'A', 'B', 'C' };
        CHECK(Crc16::xmodem(abc, 3) == 0x3994, "CRC 向量 \"ABC\" → 0x3994");
        const uint8_t abcd[] = { 'A', 'B', 'C', 'D' };
        CHECK(Crc16::xmodem(abcd, 4) == 0x3B3A, "CRC 向量 \"ABCD\" → 0x3B3A");
        const uint8_t t[] = "Test";
        CHECK(Crc16::xmodem(t, 4) == 0xAC48, "CRC 向量 \"Test\" → 0xAC48");
        const uint8_t dead[] = { 0xDE, 0xAD, 0xBE, 0xEF };
        CHECK(Crc16::xmodem(dead, 4) == 0xC457, "CRC 向量 DE AD BE EF → 0xC457");
    }
    // 单字节与空数据（含防御性空指针 + 长度 0）
    {
        const uint8_t z = 0x00;
        CHECK(Crc16::xmodem(&z, 1) == 0x0000, "CRC 单字节 0x00 → 0x0000");
        const uint8_t one = 0x01;
        CHECK(Crc16::xmodem(&one, 1) == 0x1021, "CRC 单字节 0x01 → 0x1021");
        const uint8_t a = 'A';
        CHECK(Crc16::xmodem(&a, 1) == 0x58E5, "CRC 单字节 'A' → 0x58E5");
        CHECK(Crc16::xmodem(nullptr, 0) == 0x0000, "CRC 空数据（nullptr,0）→ 0x0000");
        const uint8_t dummy = 0;
        CHECK(Crc16::xmodem(&dummy, 0) == 0x0000, "CRC 长度 0（有效指针）→ 0x0000");
    }
}

static void testBigBlocksStable()
{
    // 大块稳定性：10KB 全 0（恒为 0，CRC 线性性质）/ 全 FF / i%251 模式。
    // 期望值由独立查表参考实现给出；重复计算两次验证纯函数确定性。
    std::vector<uint8_t> zeros(10240, 0x00);
    std::vector<uint8_t> ffs(10240, 0xFF);
    std::vector<uint8_t> pat(10240);
    for (size_t i = 0; i < pat.size(); ++i)
        pat[i] = static_cast<uint8_t>(i % 251);

    CHECK(Crc16::xmodem(zeros.data(), zeros.size()) == 0x0000, "CRC 10KB 全 0 → 0x0000");
    CHECK(Crc16::xmodem(ffs.data(), ffs.size()) == 0xD94C, "CRC 10KB 全 FF → 0xD94C");
    CHECK(Crc16::xmodem(pat.data(), pat.size()) == 0xBC2C, "CRC 10KB i%251 模式 → 0xBC2C");
    const uint16_t p1 = Crc16::xmodem(pat.data(), pat.size());
    const uint16_t p2 = Crc16::xmodem(pat.data(), pat.size());
    CHECK(p1 == p2 && p1 == 0xBC2C, "CRC 大块重复计算结果一致（纯函数稳定性）");
}

static void testProtocolFramePayloads()
{
    // 协议帧常用 payload 的 CRC 基准（供 YMODEM 组帧测试交叉引用）：
    // 128×0x00（空块）/ 128×0x00..0x7F（满块）/ 128×0x1A（末块全填充）
    {
        std::vector<uint8_t> z(128, 0x00);
        CHECK(Crc16::xmodem(z.data(), z.size()) == 0x0000, "CRC 128×0x00（空块 payload）→ 0x0000");
        std::vector<uint8_t> inc(128);
        for (size_t i = 0; i < inc.size(); ++i)
            inc[i] = static_cast<uint8_t>(i);
        CHECK(Crc16::xmodem(inc.data(), inc.size()) == 0xE80A, "CRC 128×0..127（满块 payload）→ 0xE80A");
        std::vector<uint8_t> ia(128, 0x1A);
        CHECK(Crc16::xmodem(ia.data(), ia.size()) == 0xF8B0, "CRC 128×0x1A（末块全填充）→ 0xF8B0");
    }
}

int run_crc16_tests()
{
    testKnownVectors();
    testBigBlocksStable();
    testProtocolFramePayloads();
    printf("SECTION crc16: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
