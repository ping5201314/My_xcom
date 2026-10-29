#pragma once

#include <QByteArray>

class SerialConfig;

// 升级引擎所用的串口抽象接口。
// SerialPort（真实串口）与 RelayPort（GUI 占串口时的文件中转）都实现此接口，
// 使 CLI 升级在 GUI 连接串口时也能工作（用户要求：GUI 连接不影响终端使用）。
class IUpgradePort
{
public:
    virtual ~IUpgradePort() = default;

    virtual bool open(const SerialConfig& cfg) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual SerialConfig config() const = 0;
    virtual qint64 write(const QByteArray& data) = 0;
    virtual qint64 writeSync(const QByteArray& data, int timeoutMs) = 0;
    virtual QByteArray readBytes(qint64 maxSize, int timeoutMs) = 0;
    virtual void beginExclusive(bool purge = true) = 0;
    virtual void endExclusive() = 0;
};
