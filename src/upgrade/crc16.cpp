#include "upgrade/crc16.h"

namespace mycom {

// 位级实现，避免静态表：73KB 固件镜像约 60 万次迭代，毫秒级即可算完，
// 相对 9~12s 的传输总时长完全不是瓶颈；实现简单、无表错风险。
uint16_t Crc16::xmodem(const uint8_t* data, size_t len)
{
    uint16_t crc = 0; // 初值 0
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            const bool carry = (crc & 0x8000) != 0;
            crc = static_cast<uint16_t>(crc << 1);
            if (carry)
                crc ^= 0x1021; // poly 0x1021（CCITT，XMODEM 变体）
        }
    }
    return crc;
}

} // namespace mycom
