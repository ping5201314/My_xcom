#include "debugcmd/debugcommand.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringDecoder>

namespace {

// RFC 4180 引号规则：含逗号/引号/换行的字段加引号包裹，内部引号翻倍
QString csvField(const QString& f)
{
    if (f.contains(QLatin1Char(',')) || f.contains(QLatin1Char('"'))
        || f.contains(QLatin1Char('\n')) || f.contains(QLatin1Char('\r')))
        return QLatin1Char('"') + QString(f).replace(QLatin1Char('"'), QStringLiteral("\"\""))
               + QLatin1Char('"');
    return f;
}

// CSV 字节流解码：BOM → UTF-8；否则严格 UTF-8 试解，无效按 GBK（ICU 名称查询，
// 与终端页同一套路；无 ICU 的裁剪版 Qt 回退 UTF-8 结果）。Excel 中文系统
// 「另存为 CSV」默认 GBK 无 BOM，必须兼容
QString decodeCsvBytes(const QByteArray& data)
{
    QByteArray raw = data;
    if (raw.startsWith("\xEF\xBB\xBF"))
        raw.remove(0, 3); // UTF-8 BOM
    if (raw.isEmpty())
        return QString();
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString s = utf8.decode(raw);
    if (!utf8.hasError())
        return s;
    QStringDecoder gbk("GBK");
    if (gbk.isValid())
        return gbk.decode(raw);
    return s;
}

// 一次遍历的 RFC 4180 行解析：引号字段可含逗号/换行、内部引号翻倍转义；
// 引号未闭合时残余作为最后一个字段（容错不报错）
QVector<QStringList> parseCsvRows(const QString& text)
{
    QVector<QStringList> rows;
    QStringList fields;
    QString field;
    bool inQuotes = false;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"'); // 翻倍引号 = 字面引号
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
        } else if (c == QLatin1Char('"') && field.isEmpty()) {
            inQuotes = true;
        } else if (c == QLatin1Char(',')) {
            fields.append(field);
            field.clear();
        } else if (c == QLatin1Char('\n')) {
            fields.append(field);
            field.clear();
            rows.append(fields);
            fields.clear();
        } else if (c != QLatin1Char('\r')) {
            field += c;
        }
    }
    if (!field.isEmpty() || !fields.isEmpty()) {
        fields.append(field);
        rows.append(fields);
    }
    return rows;
}

} // namespace

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

bool DebugCommandModel::loadCsv(const QString& csvPath)
{
    QFile f(csvPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QString text = decodeCsvBytes(f.readAll());
    const QVector<QStringList> rows = parseCsvRows(text);
    m_groups.clear();
    bool first = true;
    for (const QStringList& fields : rows) {
        // 完全空行跳过；列数不足 4（无带参数列）的行容错跳过，不报错
        if (fields.size() < 4)
            continue;
        if (first && fields.at(0).trimmed() == QStringLiteral("分组")) {
            first = false; // 表头行
            continue;
        }
        first = false;
        const QString group = fields.at(0).trimmed();
        DebugCommand c;
        c.name = fields.at(1).trimmed();
        c.send = fields.at(2).trimmed();
        const QString hp = fields.at(3).trimmed();
        c.hasParam = hp == QStringLiteral("是")
                     || hp.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                     || hp == QStringLiteral("1");
        if (fields.size() > 4)
            c.paramDefault = fields.at(4).trimmed();
        if (fields.size() > 5)
            c.paramHint = fields.at(5).trimmed();
        // 必填列（分组/名称/发送内容）为空的坏行容错跳过（Excel 手改常见尾行残留）
        if (group.isEmpty() || c.name.isEmpty() || c.send.isEmpty())
            continue;
        // 同组合并（保持出现顺序），与 setEntries 语义一致
        int idx = -1;
        for (int i = 0; i < m_groups.size(); ++i) {
            if (m_groups.at(i).name == group) {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            m_groups.append({group, {c}});
        else
            m_groups[idx].commands.append(c);
    }
    return true;
}

bool DebugCommandModel::saveCsv(const QString& csvPath) const
{
    // 与 JSON save 同款：目录不存在先建（首次使用 %APPDATA% 目录未创建过）
    const QString dir = QFileInfo(csvPath).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir))
        return false;
    QString text;
    text += QStringLiteral("分组,名称,发送内容,带参数,默认参数,参数提示\n");
    for (const Group& g : m_groups) {
        for (const DebugCommand& c : g.commands) {
            text += csvField(g.name) + QLatin1Char(',') + csvField(c.name) + QLatin1Char(',')
                    + csvField(c.send) + QLatin1Char(',')
                    + (c.hasParam ? QStringLiteral("是") : QStringLiteral("否")) + QLatin1Char(',')
                    + csvField(c.paramDefault) + QLatin1Char(',') + csvField(c.paramHint)
                    + QLatin1Char('\n');
        }
    }
    QFile f(csvPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write("\xEF\xBB\xBF"); // UTF-8 BOM：Excel 任意语言区域双击打开中文不乱码
    f.write(text.toUtf8());
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
