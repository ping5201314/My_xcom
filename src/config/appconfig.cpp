#include "config/appconfig.h"

const QString AppConfig::kLastPort = QStringLiteral("serial/lastPort");
const QString AppConfig::kLastBaud = QStringLiteral("serial/lastBaud");
const QString AppConfig::kLastDataBits = QStringLiteral("serial/lastDataBits");
const QString AppConfig::kLastParity = QStringLiteral("serial/lastParity");
const QString AppConfig::kLastStopBits = QStringLiteral("serial/lastStopBits");
const QString AppConfig::kLastFlow = QStringLiteral("serial/lastFlowControl");

const QString AppConfig::kHexView = QStringLiteral("ui/hexView");
const QString AppConfig::kTimestamp = QStringLiteral("ui/timestamp");
const QString AppConfig::kShowTx = QStringLiteral("ui/showTx");
const QString AppConfig::kAutoScroll = QStringLiteral("ui/autoScroll");
const QString AppConfig::kEncoding = QStringLiteral("ui/encoding");
const QString AppConfig::kSendHex = QStringLiteral("ui/sendHex");
const QString AppConfig::kLineEnding = QStringLiteral("ui/lineEnding");
const QString AppConfig::kLoopInterval = QStringLiteral("ui/loopInterval");
const QString AppConfig::kSendHistory = QStringLiteral("ui/sendHistory");

const QString AppConfig::kUpgradeFile = QStringLiteral("upgrade/filePath");

const QString AppConfig::kWindowGeometry = QStringLiteral("window/geometry");

AppConfig::AppConfig()
    : m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("MyTeam"),
                 QStringLiteral("my_com"))
{
}

// 测试注入：给定路径直接开该 ini 文件；空路径回退默认 %APPDATA% 位置
AppConfig::AppConfig(const QString& iniPath)
    : m_settings(!iniPath.isEmpty()
                     ? QSettings(iniPath, QSettings::IniFormat)
                     : QSettings(QSettings::IniFormat, QSettings::UserScope,
                                 QStringLiteral("MyTeam"), QStringLiteral("my_com")))
{
}

QVariant AppConfig::value(const QString& key, const QVariant& def) const
{
    return m_settings.value(key, def);
}

void AppConfig::setValue(const QString& key, const QVariant& v)
{
    m_settings.setValue(key, v);
    // 立即落盘：写入频率很低（选项变更/退出时），换取断电/崩溃不丢配置
    m_settings.sync();
}

QString AppConfig::stringValue(const QString& key, const QString& def) const
{
    return m_settings.value(key, def).toString();
}

int AppConfig::intValue(const QString& key, int def) const
{
    bool ok = false;
    const int v = m_settings.value(key, def).toInt(&ok);
    return ok ? v : def;
}

bool AppConfig::boolValue(const QString& key, bool def) const
{
    return m_settings.value(key, def).toBool();
}

QStringList AppConfig::stringListValue(const QString& key, const QStringList& def) const
{
    // QStringList 直接经 QVariant 存 INI 会退化成逗号拼接的单个 QString
    // （QVariant::toString 对列表是 join(',')，且 INI 无类型信息，读回变字符串），
    // 含逗号/换行的历史条目会被破坏。改用 QSettings 数组格式（key/size + key/N/value）
    // 存取列表，内容完全保真。
    // beginReadArray/setArrayIndex 会动游标（非 const），这里用同路径的临时实例读，
    // 保持本方法 const 且不触碰 m_settings 的游标状态
    QSettings reader(m_settings.fileName(), QSettings::IniFormat);
    const int size = reader.beginReadArray(key);
    if (size == 0) {
        reader.endArray();
        return def;
    }
    QStringList out;
    out.reserve(size);
    for (int i = 0; i < size; ++i) {
        reader.setArrayIndex(i);
        out.append(reader.value(QStringLiteral("value")).toString());
    }
    reader.endArray();
    return out;
}

void AppConfig::setStringListValue(const QString& key, const QStringList& values)
{
    m_settings.remove(key); // 清掉旧值（含手工改成标量形式的情况），数组重写不留残键
    m_settings.beginWriteArray(key);
    for (int i = 0; i < values.size(); ++i) {
        m_settings.setArrayIndex(i);
        m_settings.setValue(QStringLiteral("value"), values.at(i));
    }
    m_settings.endArray();
    m_settings.sync(); // 与 setValue 一致：即时落盘（写入频率低，仅手动发送成功时）
}

void SendHistory::setItems(const QStringList& items)
{
    m_items = items;
    trim(); // ini 被手工编辑放大时防御性截断
}

void SendHistory::add(const QString& text)
{
    if (text.isEmpty())
        return; // 空输入不记录（需求 5.3）
    m_items.removeAll(text); // 去重：完全匹配（区分大小写、含空格）视为相同
    m_items.prepend(text);   // 相同内容移到最前
    trim();                  // 超上限丢最旧
}

void SendHistory::load(const AppConfig& config)
{
    setItems(config.stringListValue(AppConfig::kSendHistory));
}

void SendHistory::save(AppConfig& config) const
{
    config.setStringListValue(AppConfig::kSendHistory, m_items);
}

void SendHistory::trim()
{
    while (m_items.size() > kMaxEntries)
        m_items.removeLast();
}
