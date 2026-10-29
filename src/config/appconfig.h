#pragma once

// 应用配置封装：QSettings(ini) 存 %APPDATA%/MyTeam/my_com.ini。
// 键名集中在此定义，避免字符串散落各处；提供类型化读写。

#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

class AppConfig
{
public:
    AppConfig();
    // 测试注入：显式给定 ini 文件路径（临时文件）时不再走 %APPDATA%，
    // 供单元测试做读写往返而不污染用户真实配置；空路径回退默认位置
    explicit AppConfig(const QString& iniPath);

    // 通用类型化读写
    QVariant value(const QString& key, const QVariant& def = QVariant()) const;
    void setValue(const QString& key, const QVariant& v);
    QString stringValue(const QString& key, const QString& def = QString()) const;
    int intValue(const QString& key, int def = 0) const;
    bool boolValue(const QString& key, bool def = false) const;
    QStringList stringListValue(const QString& key,
                                const QStringList& def = QStringList()) const;
    void setStringListValue(const QString& key, const QStringList& values);

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
    static const QString kShowTx;      // ui/showTx
    static const QString kAutoScroll;  // ui/autoScroll
    static const QString kEncoding;    // ui/encoding（GBK/UTF-8）
    static const QString kSendHex;     // ui/sendHex
    static const QString kLineEnding;  // ui/lineEnding（0无 1CRLF 2LF 3CR）
    static const QString kLoopInterval; // ui/loopInterval
    static const QString kSendHistory;  // ui/sendHistory（发送历史，最新在前，≤50 条）

    // ---- 固件升级参数 ----
    static const QString kUpgradeFile; // upgrade/filePath

    // ---- 主窗口几何（用户反馈：记住调整后的窗口大小） ----
    static const QString kWindowGeometry; // window/geometry（saveGeometry 的 QByteArray）

private:
    // QSettings 构造依赖 QCoreApplication 的 org/app 名，main.cpp 里已先行设置
    QSettings m_settings;
};

// 发送历史纯逻辑（可单测）：完全匹配去重移前 + 上限裁剪 + ini 往返。
// 放在 appconfig 同文件而非新增源文件：遵守并行开发约束（T8 可能改 CMakeLists，
// 不新增文件即可避免 CMakeLists 合并冲突）
class SendHistory
{
public:
    static constexpr int kMaxEntries = 50; // 上限写死（2026-10-20 拍板"约 50 条"）

    QStringList list() const { return m_items; }
    void setItems(const QStringList& items); // 载入（防御性截断到上限）
    // 完全匹配（区分大小写、含空格）视为相同：删旧位置、插到最前；超上限丢最旧
    void add(const QString& text);
    void load(const AppConfig& config); // 从 ini 读（键 AppConfig::kSendHistory）
    void save(AppConfig& config) const; // 写 ini 并即时落盘（关闭软件保留）

private:
    void trim(); // 超出上限丢最旧

    QStringList m_items; // 最新在前
};
