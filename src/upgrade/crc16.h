#pragma once

// CRC-16/XMODEM：纯 C++，不依赖 Qt（供协议库单元测试独立链接）。
// 参数：poly 0x1021、初值 0、MSB 先（非反射）；线上按高字节在前发送。
// 标准校验向量："123456789" → 0x31C3。

#include <cstddef>
#include <cstdint>

namespace mycom {

class Crc16
{
public:
    static uint16_t xmodem(const uint8_t* data, size_t len);
};

} // namespace mycom
