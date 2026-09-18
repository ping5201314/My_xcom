// 第一阶段占位测试：验证协议库（Crc16 / YmodemSender 骨架）可编译、可链接、基础行为正确。
// 第二阶段测试工程师可在此框架上扩展（框架约定：CHECK 宏 + 失败计数，返回非 0 即测试失败）。
// 注：本测试链接 mycom_proto（纯 C++ 协议库），不依赖 Qt。

#include <cstdio>
#include <cstdint>
#include <cstring>

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

// 第二阶段扩展：YMODEM 发送器完整测试（tests/test_ymodem.cpp），返回失败计数
int run_ymodem_tests();
// T4 扩展：CRC-16/XMODEM 多向量与大块稳定性测试（tests/test_crc16.cpp）
int run_crc16_tests();

int main()
{
    // CRC-16/XMODEM 标准校验向量："123456789" -> 0x31C3
    // （poly 0x1021、初值 0、MSB 先；与 Finca 接收端 crc.c 的 CCITT 表实现一致）
    {
        const uint8_t vec[] = "123456789";
        CHECK(mycom::Crc16::xmodem(vec, 9) == 0x31C3, "Crc16::xmodem 标准校验向量");
        CHECK(mycom::Crc16::xmodem(nullptr, 0) == 0, "Crc16::xmodem 空数据");
    }
    // YmodemSender 基础行为（完整状态机测试见 run_ymodem_tests → tests/test_ymodem.cpp）
    {
        mycom::YmodemSender s;
        CHECK(s.state() == mycom::YmodemSender::State::Idle, "YmodemSender 初始 Idle");
        CHECK(s.start("", 0) == false, "YmodemSender 空文件名拒绝");
        CHECK(s.start("Finca_updata.bin", 73728), "YmodemSender start 成功");
        CHECK(s.state() == mycom::YmodemSender::State::WaitingC, "YmodemSender start 后 WaitingC");
        s.abort();
        // abort 只置原子标志（跨线程安全）；状态在 run() 观察到标志后才转 Aborted
        CHECK(s.run() == false, "YmodemSender abort 后 run 返回 false");
        CHECK(s.state() == mycom::YmodemSender::State::Aborted, "YmodemSender abort 后 Aborted");
    }
    // 第二阶段：组帧/回绕/状态机（假 IO 打桩，不依赖串口）
    g_failures += run_ymodem_tests();
    // T4：CRC-16/XMODEM 已知向量与大块稳定性
    g_failures += run_crc16_tests();
    if (g_failures)
        printf("RESULT: %d PASSED, %d FAILED\n", g_passes, g_failures);
    else
        printf("RESULT: ALL PASSED (%d)\n", g_passes);
    return g_failures ? 1 : 0;
}
