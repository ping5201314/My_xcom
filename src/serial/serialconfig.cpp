#include "serial/serialconfig.h"

// Win32 常量（ONESTOPBIT/TWOSTOPBITS）只在实现里用，别让 windows.h 污染头文件
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

QVector<int> standardBaudRates()
{
    return { B1200,  B2400,  B4800,   B9600,   B14400,  B19200,
             B38400, B57600, B115200, B128000, B230400, B256000,
             B460800, B500000, B576000, B921600 };
}

QString SerialConfig::toString() const
{
    static const char parityChars[] = { 'N', 'O', 'E' };
    const char p = (parity >= ParityNone && parity <= ParityEven) ? parityChars[parity] : '?';
    // 状态栏风格："COM3 115200,8N1"
    // 修复 SF-1：显示停止位**个数**（1/2），而不是枚举值（StopOne=0/StopTwo=1）——
    // 枚举值是 Win32 ONESTOPBIT/TWOSTOPBITS 语义，直接打印会出现 8N0（不存在的线路配置）。
    // 与 stopBitsToWin32 的映射口径一致：StopTwo 显示 2，其余（含未知值回退）显示 1。
    return QStringLiteral("%1 %2,%3%4%5")
        .arg(portName)
        .arg(baud)
        .arg(dataBits)
        .arg(QLatin1Char(p))
        .arg(stopBits == StopTwo ? 2 : 1);
}

int baudToWin32(int baud)
{
    // CBR_xxx 宏就是数值本身（如 CBR_115200 == 115200），直接回传
    return baud;
}

int dataBitsToWin32(int bits)
{
    return (bits >= 5 && bits <= 8) ? bits : 8;
}

int parityToWin32(int parity)
{
    // Win32: NOPARITY=0, ODDPARITY=1, EVENPARITY=2 —— 与枚举定义一致
    return (parity >= ParityNone && parity <= ParityEven) ? parity : ParityNone;
}

int stopBitsToWin32(int stop)
{
    // Win32: ONESTOPBIT=0, TWOSTOPBITS=2 —— 注意 StopTwo(1) 要映射到 2
    return (stop == StopTwo) ? TWOSTOPBITS : ONESTOPBIT;
}
