#pragma once

// 串口配置结构：本项目工具链没有 QtSerialPort 模块，因此用自定义结构 + Win32 常量映射。
// 枚举与 Win32 常量（CBR_115200 等）的映射函数声明在这里，实现放 cpp（避免 windows.h 扩散）。

#include <QMetaType>
#include <QString>
#include <QVector>

// 波特率档位：枚举值直接等于线速率数值（Win32 DCB.BaudRate 的 CBR_xxx 常量正是这些数值）
enum BaudRate {
    B1200 = 1200,
    B2400 = 2400,
    B4800 = 4800,
    B9600 = 9600,
    B14400 = 14400,
    B19200 = 19200,
    B38400 = 38400,
    B57600 = 57600,
    B115200 = 115200,
    B128000 = 128000,
    B230400 = 230400,
    B256000 = 256000,
    B460800 = 460800,
    B500000 = 500000,
    B576000 = 576000,
    B921600 = 921600
};

enum DataBits { Data5 = 5, Data6 = 6, Data7 = 7, Data8 = 8 };

// 校验位枚举值与 Win32 NOPARITY/ODDPARITY/EVENPARITY（0/1/2）一致，可直传
enum Parity { ParityNone = 0, ParityOdd = 1, ParityEven = 2 };

enum StopBits { StopOne = 0, StopTwo = 1 };

enum FlowControl { FlowNone = 0 };

/// 串口打开配置。int 存储枚举值是为了便于与 QSettings/组合框 data 直接互转。
struct SerialConfig {
    QString portName;          // "COM3"
    int baud = B115200;        // BaudRate
    int dataBits = Data8;      // DataBits
    int parity = ParityNone;   // Parity
    int stopBits = StopOne;    // StopBits
    int flowControl = FlowNone; // FlowControl

    /// 状态栏摘要："COM3 115200,8N1"
    QString toString() const;
};

/// 常用波特率列表（终端页下拉使用），升序
QVector<int> standardBaudRates();

/// 枚举 → Win32 DCB 常量映射。返回 int 仅为不把 windows.h 引入本头文件，
/// 调用处按需转 DWORD/BYTE。未知值处理：
///   - 波特率按数值直传（修复 SF-3：CBR_xxx 即数值本身，DCB.BaudRate 接受任意
///     DWORD，且 UI 下拉只提供档位，运行期不可达未知值；直传对 DCB 无害）；
///   - 数据位/校验/停止位未知值退回最保守默认（8 位/无校验/1 停止位）。
int baudToWin32(int baud);       // CBR_xxx 与数值一一对应（CBR_115200 == 115200），未知值直传
int dataBitsToWin32(int bits);   // 5~8 直传，未知回退 8
int parityToWin32(int parity);   // NOPARITY/ODDPARITY/EVENPARITY == 0/1/2，未知回退 NOPARITY
int stopBitsToWin32(int stop);   // StopOne→ONESTOPBIT(0)，StopTwo→TWOSTOPBITS(2)，未知回退 1 停止位

Q_DECLARE_METATYPE(SerialConfig)
