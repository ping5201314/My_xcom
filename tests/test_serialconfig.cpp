// SerialConfig 单元测试（T4）：
// ① 枚举 → Win32 DCB 常量映射全量断言 —— 直接包含 windows.h 与真实 CBR_*/NOPARITY/
//    ODDPARITY/EVENPARITY/ONESTOPBIT/TWOSTOPBITS 宏逐一比对（而非比对猜测的数值）；
// ② 未知值回退行为；③ standardBaudRates 完整性与升序；④ toString 状态栏格式
//    （按 serialconfig.h 头注释约定的 "COM3 115200,8N1" 语义）。
// 框架约定同 my_com_tests：CHECK 宏 + 失败计数；入口 run_serialconfig_tests() 由 qt_test_main 调用。

#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // 真实 Win32 常量（ONESTOPBIT/TWOSTOPBITS/CBR_*）—— 只在本测试使用

#include <QString>
#include <QVector>

#include "serial/serialconfig.h"

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

static void testBaudMappingAll()
{
    // Win32 定义了 CBR_ 宏的档位：与宏逐一比对
    CHECK(baudToWin32(B1200) == CBR_1200, "baudToWin32(1200) == CBR_1200");
    CHECK(baudToWin32(B2400) == CBR_2400, "baudToWin32(2400) == CBR_2400");
    CHECK(baudToWin32(B4800) == CBR_4800, "baudToWin32(4800) == CBR_4800");
    CHECK(baudToWin32(B9600) == CBR_9600, "baudToWin32(9600) == CBR_9600");
    CHECK(baudToWin32(B14400) == CBR_14400, "baudToWin32(14400) == CBR_14400");
    CHECK(baudToWin32(B19200) == CBR_19200, "baudToWin32(19200) == CBR_19200");
    CHECK(baudToWin32(B38400) == CBR_38400, "baudToWin32(38400) == CBR_38400");
    CHECK(baudToWin32(B57600) == CBR_57600, "baudToWin32(57600) == CBR_57600");
    CHECK(baudToWin32(B115200) == CBR_115200, "baudToWin32(115200) == CBR_115200");
    CHECK(baudToWin32(B128000) == CBR_128000, "baudToWin32(128000) == CBR_128000");
    CHECK(baudToWin32(B256000) == CBR_256000, "baudToWin32(256000) == CBR_256000");
    // Win32 未定义 CBR_ 宏的档位：DCB.BaudRate 接受原始数值，映射必须直传
    CHECK(baudToWin32(B230400) == 230400, "baudToWin32(230400) == 230400（无 CBR_ 宏，直传数值）");
    CHECK(baudToWin32(B460800) == 460800, "baudToWin32(460800) == 460800");
    CHECK(baudToWin32(B500000) == 500000, "baudToWin32(500000) == 500000");
    CHECK(baudToWin32(B576000) == 576000, "baudToWin32(576000) == 576000");
    CHECK(baudToWin32(B921600) == 921600, "baudToWin32(921600) == 921600");
    // 未知值：实现为直传（头注释“未知值退回最保守默认”与实现不一致，已登记测试报告，
    // 供 T5 审查裁决；UI 下拉只提供档位，运行期不可达）
    CHECK(baudToWin32(12345) == 12345, "未知波特率直传语义（头注释不一致，见测试报告登记）");
}

static void testDataBitsMapping()
{
    CHECK(dataBitsToWin32(Data5) == 5, "dataBitsToWin32(5) == 5");
    CHECK(dataBitsToWin32(Data6) == 6, "dataBitsToWin32(6) == 6");
    CHECK(dataBitsToWin32(Data7) == 7, "dataBitsToWin32(7) == 7");
    CHECK(dataBitsToWin32(Data8) == 8, "dataBitsToWin32(8) == 8");
    // 未知值退回最保守默认 8
    CHECK(dataBitsToWin32(4) == 8, "dataBitsToWin32(4) → 8（回退）");
    CHECK(dataBitsToWin32(0) == 8, "dataBitsToWin32(0) → 8（回退）");
    CHECK(dataBitsToWin32(9) == 8, "dataBitsToWin32(9) → 8（回退）");
    CHECK(dataBitsToWin32(-1) == 8, "dataBitsToWin32(-1) → 8（回退）");
}

static void testParityMapping()
{
    CHECK(parityToWin32(ParityNone) == NOPARITY, "parityToWin32(None) == NOPARITY(0)");
    CHECK(parityToWin32(ParityOdd) == ODDPARITY, "parityToWin32(Odd) == ODDPARITY(1)");
    CHECK(parityToWin32(ParityEven) == EVENPARITY, "parityToWin32(Even) == EVENPARITY(2)");
    CHECK(parityToWin32(-1) == NOPARITY, "parityToWin32(-1) → NOPARITY（回退）");
    CHECK(parityToWin32(3) == NOPARITY, "parityToWin32(3) → NOPARITY（回退）");
}

static void testStopBitsMapping()
{
    // 关键映射：StopTwo(1) 必须映射到 TWOSTOPBITS=2（枚举值与 Win32 常量不一致，注释明示）
    CHECK(stopBitsToWin32(StopOne) == ONESTOPBIT, "stopBitsToWin32(StopOne) == ONESTOPBIT(0)");
    CHECK(stopBitsToWin32(StopTwo) == TWOSTOPBITS, "stopBitsToWin32(StopTwo) == TWOSTOPBITS(2)");
    CHECK(stopBitsToWin32(2) == ONESTOPBIT, "stopBitsToWin32(2) → ONESTOPBIT（未知回退）");
    CHECK(stopBitsToWin32(-1) == ONESTOPBIT, "stopBitsToWin32(-1) → ONESTOPBIT（回退）");
    CHECK(TWOSTOPBITS == 2, "TWOSTOPBITS 宏值确为 2（防 Windows SDK 版本漂移）");
}

static void testStandardBaudRates()
{
    const QVector<int> rates = standardBaudRates();
    CHECK(rates.size() == 16, "standardBaudRates 共 16 档");
    bool ascending = true;
    for (int i = 1; i < rates.size(); ++i) {
        if (rates[i - 1] >= rates[i])
            ascending = false;
    }
    CHECK(ascending, "standardBaudRates 严格升序");
    CHECK(!rates.isEmpty() && rates.first() == B1200, "首档 1200");
    CHECK(!rates.isEmpty() && rates.last() == B921600, "末档 921600");
    CHECK(rates.contains(B115200), "包含 115200（项目默认 115200 8N1）");
}

static void testToStringFormat()
{
    // 按 serialconfig.h 头注释约定："COM3 115200,8N1"（状态栏摘要）。
    // 注：当前实现打印 stopBits 枚举值（StopOne=0 → "8N0"、StopTwo=1 → "8N1"），
    // 与“显示停止位个数”的约定不符 —— 以下断言按头注释约定书写，如实反映缺陷。
    {
        SerialConfig c;
        c.portName = QStringLiteral("COM3");
        c.baud = B115200;
        c.dataBits = Data8;
        c.parity = ParityNone;
        c.stopBits = StopOne; // 默认配置：1 停止位
        CHECK(c.toString() == QStringLiteral("COM3 115200,8N1"),
              "toString 默认配置 \"COM3 115200,8N1\"（头注释约定）");
    }
    {
        SerialConfig c;
        c.portName = QStringLiteral("COM5");
        c.baud = B9600;
        c.dataBits = Data7;
        c.parity = ParityEven;
        c.stopBits = StopTwo; // 2 停止位 → 应显示 "2"
        CHECK(c.toString() == QStringLiteral("COM5 9600,7E2"),
              "toString 7E2（StopTwo 应显示停止位个数 2）");
    }
    // 其余字段组合正确性（波特率/数据位/校验字符与端口名拼接）
    {
        SerialConfig c;
        c.portName = QStringLiteral("COM4");
        c.baud = B57600;
        c.dataBits = Data8;
        c.parity = ParityOdd;
        c.stopBits = StopOne;
        CHECK(c.toString().startsWith(QStringLiteral("COM4 57600,8O")),
              "toString 端口/波特率/数据位/校验字符拼接正确");
    }
    // 非法校验值 → '?' 防御
    {
        SerialConfig c;
        c.portName = QStringLiteral("COM3");
        c.baud = B9600;
        c.dataBits = Data8;
        c.parity = 7;
        c.stopBits = StopOne;
        CHECK(c.toString().contains(QLatin1Char('?')), "非法校验值 → '?'（防御分支）");
    }
}

int run_serialconfig_tests()
{
    testBaudMappingAll();
    testDataBitsMapping();
    testParityMapping();
    testStopBitsMapping();
    testStandardBaudRates();
    testToStringFormat();
    printf("SECTION serialconfig: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
