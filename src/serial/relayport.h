#pragma once

// 中转传输层：COM 口被 GUI 独占时，CLI 升级经 GUI 代发代收。
//
// 文件协议（全部位于 %TEMP%）：
//   my_com_ai_relay.flag   会话标记：存在 = GUI 把收到的原始字节镜像到 RX 文件
//   my_com_ai_tx.part      CLI 写入的待发送字节（写完才落 .rdy 标记，防半包）
//   my_com_ai_tx.rdy       就绪标记：GUI 每轮询到它即读 .part 全量代发并删除两文件
//   my_com_ai_rx.txt       设备回包镜像：GUI onData 追加写，CLI 轮询增量读
// 与旧 relay-send（my_com_ai_tx.txt 文本帧）并存，互不干扰。

#include <QByteArray>
#include <QString>

#include "serial/iupgradeport.h"
#include "serial/serialconfig.h" // 值成员 m_cfg 需要完整类型


class RelayPort : public IUpgradePort
{
public:
    RelayPort() = default;
    ~RelayPort() override;

    bool open(const SerialConfig& cfg) override;   // 忽略串口参数（GUI 已持有串口），仅建立会话
    void close() override;                          // 清理会话标记与临时文件
    bool isOpen() const override;
    SerialConfig config() const override;
    qint64 write(const QByteArray& data) override;  // 非阻塞写 = writeSync(2s)
    qint64 writeSync(const QByteArray& data, int timeoutMs) override;
    QByteArray readBytes(qint64 maxSize, int timeoutMs) override;
    void beginExclusive(bool purge = true) override;   // 中转模式无独占概念，空操作
    void endExclusive() override;                      // 空操作

    static QString txPartPath();
    static QString txRdyPath();
    static QString rxPath();
    static QString flagPath();

private:
    bool m_open = false;
    SerialConfig m_cfg;
    qint64 m_rxOff = 0;   // RX 文件已消费偏移（文件被截断时归零）
};
