#pragma once

// 应用配置封装：QSettings(ini) 存 %APPDATA%/MyTeam/my_com.ini。
// 键名集中在此定义，避免字符串散落各处；提供类型化读写。

#include <QSettings>
#include <QString>
#include <QVariant>

class AppConfig
{
public:
    AppConfig();

    // 通用类型化读写
    QVariant value(const QString& key, const QVariant& def = QVariant()) const;
    void setValue(const QString& key, const QVariant& v);
    QString stringValue(const QString& key, const QString& def = QString()) const;
    int intValue(const QString& key, int def = 0) const;
    bool boolValue(const QString& key, bool def = false) const;

    // ---- 串口参数（终端页） ----
    static const QString kLastPort;     // serial/lastPort
    static const QString kLastBaud;     // serial/lastBaud
    static const QString kLastDataBits; // serial/lastDataBits
    static const QString kLastParity;   // serial/lastParity
    static const QString kLastStopBits; // serial/lastStopBits
    static const QString kLastFlow;     // serial/lastFlowControl

    // ---- 显示/发送选项（终端页） ----
    static const QString kHexView;     // ui/hexView
    static const QString kTimestamp;   // ui/timestamp
    static const QString kAutoScroll;  // ui/autoScroll
    static const QString kEncoding;    // ui/encoding（GBK/UTF-8）
    static const QString kSendHex;     // ui/sendHex
    static const QString kLineEnding;  // ui/lineEnding（0无 1CRLF 2LF 3CR）
    static const QString kLoopInterval; // ui/loopInterval

    // ---- 固件升级参数 ----
    static const QString kUpgradeFile; // upgrade/filePath

    // ---- 主窗口几何（用户反馈：记住调整后的窗口大小） ----
    static const QString kWindowGeometry; // window/geometry（saveGeometry 的 QByteArray）

private:
    // QSettings 构造依赖 QCoreApplication 的 org/app 名，main.cpp 里已先行设置
    QSettings m_settings;
};
