// DebugCommandModel 单元测试（T4）：合法 JSON 分组/带参字段解析、缺失文件返回 false
//（面板侧据此回退内置默认集）、损坏 JSON / 非对象 JSON / groups 缺失与类型错误不崩溃、
// 空 send 指令的模型层容错。临时 JSON 用 QTemporaryDir 写入，作用域结束自动清理 ——
// 确定性、不污染项目目录。
// 框架约定同 my_com_tests：CHECK 宏 + 失败计数；入口 run_debugcmd_tests() 由 qt_test_main 调用。

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QStringList>
#include <QVector>

#include <cstdio>

#include "debugcmd/debugcommand.h"

static int g_failures = 0;
static int g_passes = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            ++g_passes;                                                     \
            printf("PASS: %s\n", msg);                                      \
        } else {                                                            \
            ++g_failures;                                                   \
            printf("FAIL: %s\n", msg);                                      \
        }                                                                   \
    } while (0)

namespace {

// 把内容写入临时目录并返回文件路径；QTemporaryDir 由调用方持有（作用域内有效）
QString writeTempJson(const QByteArray& content, const char* name)
{
    static QTemporaryDir dir; // 本文件所有用例共用一个临时目录，进程结束统一清理
    const QString path = dir.filePath(QLatin1String(name));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(content);
    f.close();
    return path;
}

} // namespace

static void testValidJsonGroupsAndParams()
{
    const QByteArray json = R"({
        "groups": [
            {
                "group": "基础回显",
                "commands": [
                    { "name": "通道回显", "send": "@R01:0", "desc": "打印连接位 0/1" },
                    { "name": "触发OTA",  "send": "@W12:0000." }
                ]
            },
            {
                "group": "校准",
                "commands": [
                    { "name": "设定压力", "send": "@W05:%1.", "hasParam": true,
                      "paramDefault": "0900", "paramHint": "萃取压力(0.01bar)，4位数字" }
                ]
            }
        ]
    })";
    const QString path = writeTempJson(json, "cmd_ok.json");
    CHECK(!path.isEmpty(), "临时 JSON 写入成功");
    if (path.isEmpty())
        return;

    DebugCommandModel m;
    CHECK(m.load(path), "合法 JSON 加载成功");
    CHECK(m.groups() == QStringList({ QStringLiteral("基础回显"), QStringLiteral("校准") }),
          "分组名按 JSON 顺序解析");

    const QVector<DebugCommand> g0 = m.commandsOf(QStringLiteral("基础回显"));
    CHECK(g0.size() == 2, "分组 基础回显 2 条指令");
    if (g0.size() == 2) {
        CHECK(g0[0].name == QStringLiteral("通道回显") && g0[0].send == QStringLiteral("@R01:0")
                  && !g0[0].hasParam,
              "无参指令 name/send/hasParam 正确");
        CHECK(g0[1].name == QStringLiteral("触发OTA") && g0[1].send == QStringLiteral("@W12:0000."),
              "OTA 触发帧指令正确（值域 5 字节含 '.'）");
    }
    const QVector<DebugCommand> g1 = m.commandsOf(QStringLiteral("校准"));
    CHECK(g1.size() == 1, "分组 校准 1 条指令");
    if (g1.size() == 1) {
        CHECK(g1[0].name == QStringLiteral("设定压力"), "带参指令 name 正确");
        CHECK(g1[0].send == QStringLiteral("@W05:%1.") && g1[0].hasParam,
              "带参指令 %1 占位模板与 hasParam 正确");
        CHECK(g1[0].paramDefault == QStringLiteral("0900")
                  && g1[0].paramHint == QStringLiteral("萃取压力(0.01bar)，4位数字"),
              "带参指令 paramDefault/paramHint 正确");
    }
    CHECK(m.commandsOf(QStringLiteral("不存在的组")).isEmpty(), "未知分组返回空（不崩溃）");
}

static void testMissingFileReturnsFalse()
{
    // 缺失文件 → load false（DebugPanel 以 load false 触发内置默认集回退，面板测试覆盖）。
    // 用“写后即删”的临时路径，保证确定性缺失（不依赖盘符是否存在）。
    const QString path = writeTempJson("{}", "cmd_gone.json");
    QFile::remove(path);
    DebugCommandModel m;
    CHECK(!m.load(path), "缺失文件 → load false");
    CHECK(m.groups().isEmpty(), "缺失文件后 groups 为空");
}

static void testCorruptedJsonNoCrash()
{
    DebugCommandModel m;
    // ① 语法损坏
    {
        const QString path = writeTempJson("{ this is not json !!!", "cmd_bad.json");
        CHECK(!path.isEmpty() && !m.load(path), "损坏 JSON → load false");
        CHECK(m.groups().isEmpty(), "损坏 JSON 后 groups 为空（不崩溃）");
    }
    // ② 合法 JSON 但非对象（数组）
    {
        const QString path = writeTempJson("[1,2,3]", "cmd_arr.json");
        CHECK(!path.isEmpty() && !m.load(path), "非对象 JSON（数组）→ load false");
    }
    // ③ 对象但 groups 缺失：load 视为“空指令表”成功（面板以 groups 空触发回退）
    {
        const QString path = writeTempJson("{}", "cmd_empty.json");
        DebugCommandModel m2;
        CHECK(m2.load(path) && m2.groups().isEmpty(), "groups 缺失 → load true 且 groups 空");
    }
    // ④ groups 类型错误（字符串而非数组）
    {
        const QString path = writeTempJson("{\"groups\": \"hello\"}", "cmd_badtype.json");
        DebugCommandModel m3;
        CHECK(m3.load(path) && m3.groups().isEmpty(), "groups 非数组 → 不崩溃且 groups 空");
    }
    // ⑤ 组对象缺字段/指令对象缺字段：不崩溃，缺省为空字符串
    {
        const QString path = writeTempJson(
            "{\"groups\": [{\"commands\": [{\"name\": \"x\"}]}]}", "cmd_sparse.json");
        DebugCommandModel m4;
        CHECK(m4.load(path), "稀疏字段 JSON 加载成功");
        CHECK(m4.groups() == QStringList({ QString() }), "缺 group 名 → 空字符串分组名");
        const QVector<DebugCommand> cmds = m4.commandsOf(QString());
        CHECK(cmds.size() == 1 && cmds[0].name == QStringLiteral("x")
                  && cmds[0].send.isEmpty() && !cmds[0].hasParam,
              "缺 send/hasParam 字段 → 空 send、hasParam=false（不崩溃）");
    }
}

static void testEmptySendTolerance()
{
    // 模型层不拦截空 send：容错在发送侧（DebugPanel::currentFrameText 空帧早退，面板测试覆盖）。
    // 模型职责是忠实解析，空 send 指令可正常展示（用户点发送时被安全跳过）。
    const QString path = writeTempJson(
        "{\"groups\": [{\"group\": \"g\", \"commands\": [{\"name\": \"空指令\", \"send\": \"\"}]}]}",
        "cmd_emptysend.json");
    DebugCommandModel m;
    CHECK(m.load(path), "含空 send 指令的 JSON 加载成功");
    const QVector<DebugCommand> cmds = m.commandsOf(QStringLiteral("g"));
    CHECK(cmds.size() == 1 && cmds[0].send.isEmpty() && cmds[0].name == QStringLiteral("空指令"),
          "空 send 指令被完整解析（name 保留、send 为空）");
}

static void testSaveLoadRoundTrip()
{
    // 编辑对话框链路：load → entries/setEntries → save → 新模型 load 后逐字段一致。
    // 分组按条目出现顺序重建，同名分组合并（编辑表格的"分组"列自由填写语义）。
    DebugCommandModel m;
    QVector<DebugCommandModel::Entry> es;
    {
        DebugCommand c;
        c.name = QStringLiteral("版本信息");
        c.send = QStringLiteral("@R20:0");
        es.append({QStringLiteral("基础"), c});
    }
    {
        DebugCommand c;
        c.name = QStringLiteral("标定电压");
        c.send = QStringLiteral("@W21:%1.");
        c.hasParam = true;
        c.paramDefault = QStringLiteral("0230");
        c.paramHint = QStringLiteral("电压(0.1V)");
        es.append({QStringLiteral("校准"), c});
    }
    {
        DebugCommand c;
        c.name = QStringLiteral("通道回显");
        c.send = QStringLiteral("@R01:0");
        es.append({QStringLiteral("基础"), c}); // 与第一条同组，应合并
    }
    m.setEntries(es);

    const QString path = writeTempJson(QByteArray(), "cmd_roundtrip.json");
    CHECK(m.save(path), "setEntries 后 save 到临时文件成功");

    DebugCommandModel m2;
    CHECK(m2.load(path), "save 产物可重新 load");
    CHECK(m2.groups() == QStringList({QStringLiteral("基础"), QStringLiteral("校准")}),
          "分组按出现顺序重建（基础在前、校准在后）");
    const QVector<DebugCommand> base = m2.commandsOf(QStringLiteral("基础"));
    CHECK(base.size() == 2 && base[0].name == QStringLiteral("版本信息")
              && base[1].name == QStringLiteral("通道回显"),
          "同名分组合并且指令顺序保留");
    const QVector<DebugCommand> cal = m2.commandsOf(QStringLiteral("校准"));
    CHECK(cal.size() == 1 && cal[0].hasParam && cal[0].paramDefault == QStringLiteral("0230")
              && cal[0].paramHint == QStringLiteral("电压(0.1V)") && cal[0].send == QStringLiteral("@W21:%1."),
          "带参指令字段逐项往返一致");
}

int run_debugcmd_tests()
{
    testValidJsonGroupsAndParams();
    testMissingFileReturnsFalse();
    testCorruptedJsonNoCrash();
    testEmptySendTolerance();
    testSaveLoadRoundTrip();
    printf("SECTION debugcmd: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
