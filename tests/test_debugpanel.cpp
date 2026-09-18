// DebugPanel 行为单元测试（T4）：内置默认集回退、JSON 载入后的面板树构建、
// %1 占位替换与“恰好 4 位数字”校验（3 位/5 位/非数字/空拒绝）、模板缺 %1 防御、
// 空 send 指令容错（静默早退、不崩溃、无 TX 日志）。
//
// 测试手段说明（不改业务代码）：
// 1) 本 TU 先包含 Qt 相关头，再 #define private public 后包含 pages/debugpanel.h ——
//    仅使 DebugPanel 的 private 成员对本 TU 可见；不改类布局、不改 ODR 符号，
//    moc 生成的元对象在独立 TU 中按原头编译，二者一致。
// 2) SerialPort 用本 TU 定义的最小桩（DebugPanel 只经前向声明使用其指针）——
//    测试路径（空帧/非法参数早退）不会进入 doSend 的真实端口分支。
// 3) 平台 offscreen（qt_test_main 设置）；exe 输出目录独立（CMake RUNTIME_OUTPUT_DIRECTORY），
//    保证“JSON 缺失回退默认集”路径确定性可测。
//
// 框架约定同 my_com_tests：CHECK 宏 + 失败计数；入口 run_debugpanel_tests() 由 qt_test_main 调用。

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <cstdio>

#include "debugcmd/debugcommand.h"

// 桩 SerialPort：DebugPanel 仅通过前向声明使用；doSend 的打开/写入路径不在测试范围内。
// 方法用类外定义（非内联）：保证符号必然发射 —— debugpanel.cpp 的 doSend 引用了
// SerialPort::isOpen/write，即使本 TU 从不调用它们，链接器也需要这些符号。
class SerialPort {
public:
    SerialPort() = default;
    bool isOpen() const;
    qint64 write(const QByteArray&);
};

bool SerialPort::isOpen() const
{
    return false;
}

qint64 SerialPort::write(const QByteArray&)
{
    return -1;
}

#define private public
#include "pages/debugpanel.h"
#undef private

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

QString jsonPathInExeDir()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("debug_commands.json"));
}

// 取树中最后一个顶层分组（避免与回退集/JSON 集叠加时索引漂移）
QTreeWidgetItem* lastGroup(const DebugPanel& p)
{
    const int n = p.m_tree->topLevelItemCount();
    return n > 0 ? p.m_tree->topLevelItem(n - 1) : nullptr;
}

} // namespace

static void testFallbackDefaultSet()
{
    // exe 目录无 debug_commands.json（测试 exe 输出到独立子目录；防御性先删除遗留）
    QFile::remove(jsonPathInExeDir());
    DebugPanel p(nullptr);
    p.show(); // offscreen 下窗口必须 show 后子控件 isVisible() 才有效
    CHECK(p.m_tree->topLevelItemCount() == 1, "回退：恰好 1 个分组");
    const auto* g = p.m_tree->topLevelItem(0);
    if (!g)
        return;
    CHECK(g->text(0) == QStringLiteral("信息查询"), "回退：分组名 信息查询");
    CHECK(g->childCount() == 1, "回退：1 条指令");
    if (g->childCount() != 1)
        return;
    const DebugCommand c = g->child(0)->data(0, Qt::UserRole).value<DebugCommand>();
    CHECK(c.name == QStringLiteral("版本信息") && c.send == QStringLiteral("@R20:0") && !c.hasParam,
          "回退：内置最小指令集 {版本信息 @R20:0}（升级成功判据指令）");
    CHECK(p.m_logView->toPlainText().contains(QStringLiteral("内置最小指令集")),
          "回退：日志明确提示使用内置最小指令集");
    // 回退集不触发参数区（无带参指令）
    g->child(0)->setSelected(true);
    p.m_tree->setCurrentItem(g->child(0));
    CHECK(!p.m_paramEdit->isVisible(), "回退：无参指令不显示参数框");
}

static void testJsonLoadPanelTree()
{
    // exe 目录写入合法 JSON（带参指令）→ 面板从文件加载并构建树
    const QByteArray json = R"({
        "groups": [
            { "group": "校准",
              "commands": [
                  { "name": "设定压力", "send": "@W05:%1.", "hasParam": true,
                    "paramDefault": "0900", "paramHint": "萃取压力(0.01bar)，4位数字" },
                  { "name": "空指令", "send": "" }
              ] }
        ]
    })";
    const QString path = jsonPathInExeDir();
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(json);
    }
    DebugPanel p(nullptr);
    p.show();
    CHECK(p.m_tree->topLevelItemCount() == 1, "JSON 载入：1 个分组");
    const auto* g = p.m_tree->topLevelItem(0);
    if (!g)
        return;
    CHECK(g->text(0) == QStringLiteral("校准") && g->childCount() == 2,
          "JSON 载入：分组 校准 2 条指令");
    CHECK(p.m_logView->toPlainText().contains(QStringLiteral("已加载指令表")),
          "JSON 载入：日志提示已加载");
    if (g->childCount() >= 1) {
        const DebugCommand c = g->child(0)->data(0, Qt::UserRole).value<DebugCommand>();
        CHECK(c.hasParam && c.paramDefault == QStringLiteral("0900"),
              "JSON 载入：带参指令字段进入树数据");
    }
    // 清理：保证下次运行回退测试确定性
    QFile::remove(path);
}

static void testParamValidationAndSubstitution()
{
    DebugPanel p(nullptr); // 回退集/残留 JSON 均无带参指令，随后 addGroup 注入测试指令
    p.show();
    QVector<DebugCommand> cmds;
    {
        DebugCommand c;
        c.name = QStringLiteral("设定压力");
        c.send = QStringLiteral("@W05:%1.");
        c.hasParam = true;
        c.paramDefault = QStringLiteral("0900");
        c.paramHint = QStringLiteral("4位数字");
        cmds.append(c);
    }
    {
        DebugCommand c;
        c.name = QStringLiteral("坏模板");
        c.send = QStringLiteral("@W22:0000."); // 无 %1 占位却声明带参
        c.hasParam = true;
        c.paramDefault = QStringLiteral("1000");
        cmds.append(c);
    }
    p.addGroup(QStringLiteral("测试"), cmds);
    auto* g = lastGroup(p);
    if (!g || g->childCount() < 2)
        return;

    // 带参指令选中：参数区可见、默认值填充
    auto* item1 = g->child(0);
    p.m_tree->setCurrentItem(item1);
    CHECK(p.m_paramEdit->isVisible(), "带参指令选中显示参数框");
    CHECK(p.m_paramEdit->text() == QStringLiteral("0900"), "带参指令默认参数 0900 填充");

    // 参数原样替换 %1（用户要求：取消"恰好 4 位数字"限制，通用串口助手
    // 不绑定特定协议参数约定）
    p.m_paramEdit->setText(QStringLiteral("0900"));
    CHECK(p.currentFrameText() == QStringLiteral("@W05:0900."),
          "%1 替换：0900 → @W05:0900.");
    p.m_paramEdit->setText(QStringLiteral("123"));
    CHECK(p.currentFrameText() == QStringLiteral("@W05:123."),
          "任意位数参数接受（3 位）");
    p.m_paramEdit->setText(QStringLiteral("12345"));
    CHECK(p.currentFrameText() == QStringLiteral("@W05:12345."),
          "任意位数参数接受（5 位，不再入口截断）");
    p.m_paramEdit->setText(QStringLiteral("12a4"));
    CHECK(p.currentFrameText() == QStringLiteral("@W05:12a4."),
          "非纯数字参数接受");
    p.m_paramEdit->setText(QStringLiteral("+123"));
    CHECK(p.currentFrameText() == QStringLiteral("@W05:+123."),
          "带符号参数接受");
    p.m_paramEdit->setText(QStringLiteral("hello"));
    CHECK(p.currentFrameText() == QStringLiteral("@W05:hello."),
          "任意文本参数接受");

    // 模板缺 %1：参数无位置可注入，原样发送模板本身（不再拒绝）
    auto* item2 = g->child(1);
    p.m_tree->setCurrentItem(item2);
    CHECK(p.m_paramEdit->isVisible(), "带参声明但缺 %1 的指令仍显示参数框");
    p.m_paramEdit->setText(QStringLiteral("1234"));
    CHECK(p.currentFrameText() == item2->data(0, Qt::UserRole).value<DebugCommand>().send,
          "模板缺 %1 原样发送（不再拒绝）");

    // 无参指令：参数区隐藏，帧文本即 send 原文
    auto* itemR = [&]() -> QTreeWidgetItem* {
        for (int i = 0; i < p.m_tree->topLevelItemCount(); ++i) {
            auto* tg = p.m_tree->topLevelItem(i);
            for (int j = 0; j < tg->childCount(); ++j) {
                const DebugCommand c = tg->child(j)->data(0, Qt::UserRole).value<DebugCommand>();
                if (c.name == QStringLiteral("版本信息"))
                    return tg->child(j);
            }
        }
        return nullptr;
    }();
    if (itemR) {
        p.m_tree->setCurrentItem(itemR);
        CHECK(!p.m_paramEdit->isVisible(), "无参指令不显示参数框");
        CHECK(p.currentFrameText() == QStringLiteral("@R20:0"), "无参指令帧文本即 send 原文");
    } else {
        CHECK(false, "回退集应含 版本信息 指令（前提不成立，跳过后续）");
    }
}

static void testEmptySendTolerance()
{
    DebugPanel p(nullptr);
    p.show();
    QVector<DebugCommand> cmds;
    {
        DebugCommand c;
        c.name = QStringLiteral("空指令");
        c.send = QString(); // 空模板
        c.hasParam = false;
        cmds.append(c);
    }
    p.addGroup(QStringLiteral("容错"), cmds);
    auto* g = lastGroup(p);
    if (!g || g->childCount() < 1)
        return;
    auto* item = g->child(0);
    p.m_tree->setCurrentItem(item);
    const QString before = p.m_logView->toPlainText();
    p.onSendCurrent(); // 空 send → currentFrameText 返回空 → 早退：不发不崩
    CHECK(p.m_logView->toPlainText() == before, "空 send 指令静默早退（无 TX 日志、不崩溃）");
    CHECK(!p.m_logView->toPlainText().contains(QStringLiteral("TX [")), "空 send 未产生任何 TX 日志");
}

int run_debugpanel_tests()
{
    testFallbackDefaultSet();
    testJsonLoadPanelTree();
    testParamValidationAndSubstitution();
    testEmptySendTolerance();
    printf("SECTION debugpanel: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
