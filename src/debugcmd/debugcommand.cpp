#include "debugcmd/debugcommand.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

bool DebugCommandModel::load(const QString& jsonPath)
{
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    m_groups.clear();
    const QJsonArray groups = doc.object().value(QLatin1String("groups")).toArray();
    for (const QJsonValue& gv : groups) {
        const QJsonObject go = gv.toObject();
        Group g;
        g.name = go.value(QLatin1String("group")).toString();
        const QJsonArray cmds = go.value(QLatin1String("commands")).toArray();
        for (const QJsonValue& cv : cmds) {
            const QJsonObject co = cv.toObject();
            DebugCommand c;
            c.name = co.value(QLatin1String("name")).toString();
            c.send = co.value(QLatin1String("send")).toString();
            c.hasParam = co.value(QLatin1String("hasParam")).toBool();
            c.paramDefault = co.value(QLatin1String("paramDefault")).toString();
            c.paramHint = co.value(QLatin1String("paramHint")).toString();
            g.commands.append(c);
        }
        m_groups.append(g);
    }
    return true;
}

bool DebugCommandModel::save(const QString& jsonPath) const
{
    // 目标目录可能不存在（如 %APPDATA%/MyTeam/my_com 首次使用时还没创建过，
    // 直接写文件会报"无法写入"——用户真机反馈过），先建目录再写
    const QString dir = QFileInfo(jsonPath).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir))
        return false;
    // 按当前分组结构重建 JSON（pretty 缩进，保留手工编辑可读性），原子写盘
    QJsonArray groups;
    for (const Group& g : m_groups) {
        QJsonObject go;
        go.insert(QLatin1String("group"), g.name);
        QJsonArray cmds;
        for (const DebugCommand& c : g.commands) {
            QJsonObject co;
            co.insert(QLatin1String("name"), c.name);
            co.insert(QLatin1String("send"), c.send);
            co.insert(QLatin1String("hasParam"), c.hasParam);
            co.insert(QLatin1String("paramDefault"), c.paramDefault);
            co.insert(QLatin1String("paramHint"), c.paramHint);
            cmds.append(co);
        }
        go.insert(QLatin1String("commands"), cmds);
        groups.append(go);
    }
    QJsonObject root;
    root.insert(QLatin1String("groups"), groups);

    QFile f(jsonPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QStringList DebugCommandModel::groups() const
{
    QStringList r;
    for (const Group& g : m_groups)
        r << g.name;
    return r;
}

QVector<DebugCommand> DebugCommandModel::commandsOf(const QString& group) const
{
    for (const Group& g : m_groups) {
        if (g.name == group)
            return g.commands;
    }
    return {};
}

QVector<DebugCommandModel::Entry> DebugCommandModel::entries() const
{
    QVector<Entry> r;
    for (const Group& g : m_groups) {
        for (const DebugCommand& c : g.commands)
            r.append({g.name, c});
    }
    return r;
}

void DebugCommandModel::setEntries(const QVector<Entry>& es)
{
    // 按条目顺序重建分组：同分组名的条目合并（保持出现顺序），分组名不同才另起
    m_groups.clear();
    QStringList order; // 分组首次出现顺序
    for (const Entry& e : es) {
        const int idx = order.indexOf(e.group);
        if (idx < 0) {
            order.append(e.group);
            m_groups.append({e.group, {e.cmd}});
        } else {
            m_groups[idx].commands.append(e.cmd);
        }
    }
}
