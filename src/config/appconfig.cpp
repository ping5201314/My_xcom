#include "config/appconfig.h"

const QString AppConfig::kLastPort = QStringLiteral("serial/lastPort");
const QString AppConfig::kLastBaud = QStringLiteral("serial/lastBaud");
const QString AppConfig::kLastDataBits = QStringLiteral("serial/lastDataBits");
const QString AppConfig::kLastParity = QStringLiteral("serial/lastParity");
const QString AppConfig::kLastStopBits = QStringLiteral("serial/lastStopBits");
const QString AppConfig::kLastFlow = QStringLiteral("serial/lastFlowControl");

const QString AppConfig::kHexView = QStringLiteral("ui/hexView");
const QString AppConfig::kTimestamp = QStringLiteral("ui/timestamp");
const QString AppConfig::kAutoScroll = QStringLiteral("ui/autoScroll");
const QString AppConfig::kEncoding = QStringLiteral("ui/encoding");
const QString AppConfig::kSendHex = QStringLiteral("ui/sendHex");
const QString AppConfig::kLineEnding = QStringLiteral("ui/lineEnding");
const QString AppConfig::kLoopInterval = QStringLiteral("ui/loopInterval");

const QString AppConfig::kUpgradeFile = QStringLiteral("upgrade/filePath");

const QString AppConfig::kWindowGeometry = QStringLiteral("window/geometry");

AppConfig::AppConfig()
    : m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("MyTeam"),
                 QStringLiteral("my_com"))
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
