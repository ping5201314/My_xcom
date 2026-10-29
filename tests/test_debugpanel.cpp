// DebugPanel 行为单元测试（T8 改造 + T11 补充 T13）：三列表格构建、固定位置路径注入、
// 指令表缺失/损坏 → 空白表格 + 占位提示（不回退内置最小指令集、无弹窗无红字）、
// %1 占位替换（3 位/5 位/非数字/空均按原样接受）、模板缺 %1 防御、
// 空 send 指令容错（静默早退、不崩溃、无 TX 日志）、
// T13 自动迁移的“不迁移”侧（注入路径即使 exe 旁有表也不复制）、
// T13 右键修改落盘链路（行更新 → saveTable → 重启面板内容仍在）。
//
// 测试手段说明（不改业务代码）：
// 1) 本 TU 先包含 Qt 相关头，再 #define private public 后包含 pages/debugpanel.h ——
//    仅使 DebugPanel 的 private 成员对本 TU 可见；不改类布局、不改 ODR 符号，
//    moc 生成的元对象在独立 TU 中按原头编译，二者一致。
// 2) SerialPort 用本 TU 定义的最小桩（DebugPanel 只经前向声明使用其指针）——
//    测试路径（空帧/非法参数早退）不会进入 doSend 的真实端口分支。
// 3) 平台 offscreen（qt_test_main 设置）；指令表路径一律注入 QTemporaryDir 临时目录
//    （T8 起固定 %APPDATA% 存储，测试必须隔离，不读写真实用户数据）。
//
// 框架约定同 my_com_tests：CHECK 宏 + 失败计数；入口 run_debugpanel_tests() 由 qt_test_main 调用。

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTemporaryDir>
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

QString jsonPath(const QTemporaryDir& dir)
{
    return dir.filePath(QStringLiteral("debug_commands.json"));
}

void writeFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(content);
}

} // namespace

static void testEmptyTableOnMissingJson()
{
    // 指令表缺失（临时目录无 json）→ 空白表格 + 占位提示，无内置最小指令集回退
    QTemporaryDir dir;
    DebugPanel p(nullptr, jsonPath(dir));
    p.show(); // offscreen 下窗口必须 show 后子控件 isVisible() 才有效
    CHECK(p.m_table->rowCount() == 0, "缺失：表格 0 行（无内置最小集回退）");
    CHECK(p.m_stack->currentWidget() == static_cast<QWidget*>(p.m_placeholder),
          "缺失：显示占位提示页而非表格页");
    CHECK(p.m_placeholder->text().contains(QStringLiteral("编辑指令"))
              && p.m_placeholder->text().contains(QStringLiteral("加载表格")),
          "缺失：占位提示引导编辑/导入");
    CHECK(p.m_logView->toPlainText().contains(QStringLiteral("未找到或无法解析指令表")),
          "缺失：日志中性提示未找到指令表");
    CHECK(!p.m_sendBtn->isEnabled(), "缺失：发送选中指令禁用");
    CHECK(!p.m_paramEdit->isVisible(), "缺失：无选中行不显示参数框");
}

static void testJsonLoadPanelTable()
{
    // 临时目录写入合法 JSON（带参指令）→ 面板加载并平铺为三列表格行
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
    QTemporaryDir dir;
    writeFile(jsonPath(dir), json);
    DebugPanel p(nullptr, jsonPath(dir));
    p.show();
    CHECK(p.m_table->columnCount() == 3, "JSON 载入：三列表格");
    CHECK(p.m_table->horizontalHeaderItem(0)->text() == QStringLiteral("分组")
              && p.m_table->horizontalHeaderItem(1)->text() == QStringLiteral("名称")
              && p.m_table->horizontalHeaderItem(2)->text() == QStringLiteral("发送内容"),
          "JSON 载入：表头 分组/名称/发送内容");
    CHECK(p.m_table->rowCount() == 2, "JSON 载入：2 条指令平铺为 2 行");
    CHECK(p.m_stack->currentWidget() == static_cast<QWidget*>(p.m_table),
          "JSON 载入：显示表格页而非占位页");
    if (p.m_table->rowCount() < 2)
        return;
    CHECK(p.m_table->item(0, 0)->text() == QStringLiteral("校准")
              && p.m_table->item(1, 0)->text() == QStringLiteral("校准"),
          "JSON 载入：分组列逐行显示");
    CHECK(p.m_table->item(0, 1)->text() == QStringLiteral("设定压力")
              && p.m_table->item(0, 2)->text() == QStringLiteral("@W05:%1."),
          "JSON 载入：名称/发送内容列正确");
    CHECK(p.m_logView->toPlainText().contains(QStringLiteral("已加载指令表")),
          "JSON 载入：日志提示已加载");
    const DebugCommand c = p.m_table->item(0, 0)->data(Qt::UserRole).value<DebugCommand>();
    CHECK(c.hasParam && c.paramDefault == QStringLiteral("0900"),
          "JSON 载入：带参指令字段进入行数据");
}

static void testParamValidationAndSubstitution()
{
    // 路径注入空表临时目录：仅测试注入的指令，不依赖任何外部 json
    QTemporaryDir dir;
    DebugPanel p(nullptr, jsonPath(dir));
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
    {
        DebugCommand c;
        c.name = QStringLiteral("版本信息");
        c.send = QStringLiteral("@R20:0");
        cmds.append(c);
    }
    p.addGroup(QStringLiteral("测试"), cmds);
    if (p.m_table->rowCount() < 3)
        return;

    // 带参指令行选中：参数区可见、默认值填充
    p.m_table->setCurrentCell(0, 0);
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
    p.m_table->setCurrentCell(1, 0);
    CHECK(p.m_paramEdit->isVisible(), "带参声明但缺 %1 的指令仍显示参数框");
    p.m_paramEdit->setText(QStringLiteral("1234"));
    CHECK(p.currentFrameText() == p.m_table->item(1, 0)->data(Qt::UserRole)
                                      .value<DebugCommand>()
                                      .send,
          "模板缺 %1 原样发送（不再拒绝）");

    // 无参指令：参数区隐藏，帧文本即 send 原文
    p.m_table->setCurrentCell(2, 0);
    CHECK(!p.m_paramEdit->isVisible(), "无参指令不显示参数框");
    CHECK(p.currentFrameText() == QStringLiteral("@R20:0"), "无参指令帧文本即 send 原文");
}

static void testNoAutoMigrationWithInjectedPath()
{
    // T13 自动迁移只发生在“默认 %APPDATA% 路径”（pathOverride 为空）：
    // 路径注入时即使 exe 旁存在 debug_commands.json 也不复制（测试确定性约定）。
    // 这里在 exe 旁（applicationDirPath）临时放一个“迁移标记”表——若误做迁移，
    // 注入路径会被复制出该文件、表格会出现标记指令，断言即红。
    QTemporaryDir dir;
    const QString exeSide = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("debug_commands.json"));
    QByteArray backup;
    {
        QFile f(exeSide);
        if (f.open(QIODevice::ReadOnly)) {
            backup = f.readAll();
            f.close();
        }
    }
    writeFile(exeSide,
              R"({"groups":[{"group":"迁移标记","commands":[{"name":"不应出现","send":"@R99:0"}]}]})");
    DebugPanel p(nullptr, jsonPath(dir));
    p.show();
    CHECK(!QFile::exists(jsonPath(dir)), "注入路径不做迁移：临时路径保持缺失（exe 旁有表也不复制）");
    CHECK(p.m_table->rowCount() == 0, "注入路径不做迁移：表格 0 行（未误加载 exe 旁标记表）");
    CHECK(p.m_logView->toPlainText().contains(QStringLiteral("未找到或无法解析指令表")),
          "注入路径不做迁移：日志仍为中性提示（无迁移日志）");
    // 清理 exe 旁标记文件，恢复原状
    if (backup.isEmpty())
        QFile::remove(exeSide);
    else
        writeFile(exeSide, backup);
}

static void testRightClickEditPersists()
{
    // T13 右键「编辑此指令…」：editCommandAt 弹模态对话框（CommandEditDialog 是
    // debugpanel.cpp 匿名命名空间类型，外部无法实例化；exec 在 offscreen 下也无法自动
    // 驱动——该交互由 GUI 冒烟人工点验）。此处直接验证其落盘链路：
    // 行更新（分组/名称/发送内容/UserRole 全字段，与 editCommandAt 同序）→ saveTable()
    // → 磁盘 JSON 更新 → 新面板重建后行内容一致（等效“右键改完 → 关闭软件 → 重启仍在”）。
    const QByteArray json = R"({
        "groups": [
            { "group": "校准",
              "commands": [
                  { "name": "设定压力", "send": "@W05:%1.", "hasParam": true,
                    "paramDefault": "0900", "paramHint": "4位数字" } ] }
        ]
    })";
    QTemporaryDir dir;
    writeFile(jsonPath(dir), json);
    {
        DebugPanel p(nullptr, jsonPath(dir));
        p.show();
        CHECK(p.m_table->rowCount() == 1, "右键修改前置：1 条指令已载入");
        if (p.m_table->rowCount() < 1)
            return;
        DebugCommand c;
        c.name = QStringLiteral("标定电压");
        c.send = QStringLiteral("@W21:%1.");
        c.hasParam = true;
        c.paramDefault = QStringLiteral("0230");
        c.paramHint = QStringLiteral("电压(0.1V)");
        p.m_table->item(0, 0)->setText(QStringLiteral("校准2"));
        p.m_table->item(0, 0)->setData(Qt::UserRole, QVariant::fromValue(c));
        p.m_table->item(0, 1)->setText(c.name);
        p.m_table->item(0, 2)->setText(c.send);
        p.saveTable(); // 右键修改的落盘链路
    }
    // “重启”面板（同路径新实例）→ 修改后的内容仍在
    {
        DebugPanel p2(nullptr, jsonPath(dir));
        p2.show();
        CHECK(p2.m_table->rowCount() == 1, "右键修改保存后：重启面板仍有 1 条指令");
        if (p2.m_table->rowCount() < 1)
            return;
        CHECK(p2.m_table->item(0, 0)->text() == QStringLiteral("校准2")
                  && p2.m_table->item(0, 1)->text() == QStringLiteral("标定电压")
                  && p2.m_table->item(0, 2)->text() == QStringLiteral("@W21:%1."),
              "重启后面板行内容为修改后内容（分组/名称/发送内容）");
        const DebugCommand c2 = p2.m_table->item(0, 0)->data(Qt::UserRole).value<DebugCommand>();
        CHECK(c2.hasParam && c2.paramDefault == QStringLiteral("0230")
                  && c2.paramHint == QStringLiteral("电压(0.1V)"),
              "重启后 UserRole 隐藏字段（hasParam/默认参数/参数提示）一致");
    }
    // 磁盘 JSON 也是新内容（独立模型读回交叉验证）
    DebugCommandModel m;
    CHECK(m.load(jsonPath(dir)), "磁盘 JSON 可重新加载");
    CHECK(m.groups() == QStringList({QStringLiteral("校准2")})
              && m.commandsOf(QStringLiteral("校准2")).size() == 1
              && m.commandsOf(QStringLiteral("校准2"))[0].name == QStringLiteral("标定电压"),
          "磁盘 JSON 已更新为新分组/新指令");
}

static void testEmptySendTolerance()
{
    QTemporaryDir dir;
    DebugPanel p(nullptr, jsonPath(dir));
    p.show();
    QVector<DebugCommand> cmds;
    {
        DebugCommand c;
        c.name = QStringLiteral("空指令");
        c.send = QString(); // 空模板
        cmds.append(c);
    }
    p.addGroup(QStringLiteral("容错"), cmds);
    if (p.m_table->rowCount() < 1)
        return;
    p.m_table->setCurrentCell(0, 0);
    const QString before = p.m_logView->toPlainText();
    p.onSendCurrent(); // 空 send → currentFrameText 返回空 → 早退：不发不崩
    CHECK(p.m_logView->toPlainText() == before, "空 send 指令静默早退（无 TX 日志、不崩溃）");
    CHECK(!p.m_logView->toPlainText().contains(QStringLiteral("TX [")), "空 send 未产生任何 TX 日志");
}

static void testCsvTableLoadAndSave()
{
    // CSV 主路径（2026-10-21 用户拍板，Excel 可编辑）：注入 .csv → 三列表格加载 →
    // 右键修改落盘链路（saveTable 按扩展名写 csv）→ 重启面板读回一致
    QTemporaryDir dir;
    const QString csv = dir.filePath(QStringLiteral("debug_commands.csv"));
    writeFile(csv, QStringLiteral("分组,名称,发送内容,带参数,默认参数,参数提示\n"
                                  "基础,版本信息,@R20:0,否,,\n"
                                  "校准,标定电压,@W21:%1.,是,0230,电压(0.1V)\n")
                       .toUtf8());
    {
        DebugPanel p(nullptr, csv);
        p.show();
        CHECK(p.m_table->rowCount() == 2, "CSV 载入：2 条指令平铺为 2 行");
        if (p.m_table->rowCount() < 2)
            return;
        CHECK(p.m_table->item(0, 0)->text() == QStringLiteral("基础")
                  && p.m_table->item(1, 0)->text() == QStringLiteral("校准"),
              "CSV 载入：分组列正确");
        CHECK(p.m_table->item(0, 2)->text() == QStringLiteral("@R20:0")
                  && p.m_table->item(1, 2)->text() == QStringLiteral("@W21:%1."),
              "CSV 载入：发送内容列正确");
        const DebugCommand c1 = p.m_table->item(1, 0)->data(Qt::UserRole).value<DebugCommand>();
        CHECK(c1.hasParam && c1.paramDefault == QStringLiteral("0230")
                  && c1.paramHint == QStringLiteral("电压(0.1V)"),
              "CSV 载入：带参隐藏字段进入 UserRole");
        // 右键修改落盘链路：改名称 → saveTable 按 .csv 写盘。
        // 与 editCommandAt 同序：单元格 + UserRole 一起更新（saveTable 从 UserRole 取数）
        DebugCommand c0 = p.m_table->item(0, 0)->data(Qt::UserRole).value<DebugCommand>();
        c0.name = QStringLiteral("版本信息2");
        p.m_table->item(0, 0)->setData(Qt::UserRole, QVariant::fromValue(c0));
        p.m_table->item(0, 1)->setText(c0.name);
        CHECK(p.saveTable(), "CSV 路径 saveTable 成功");
    }
    {
        DebugPanel p2(nullptr, csv);
        p2.show();
        CHECK(p2.m_table->rowCount() == 2, "CSV 保存后：重启面板仍有 2 条指令");
        if (p2.m_table->rowCount() < 2)
            return;
        CHECK(p2.m_table->item(0, 1)->text() == QStringLiteral("版本信息2"),
              "CSV 保存后：重启读回修改后名称");
    }
    // 磁盘仍是合法 CSV（BOM + 表头）
    QFile f(csv);
    CHECK(f.open(QIODevice::ReadOnly) && f.readAll().startsWith("\xEF\xBB\xBF"),
          "CSV 落盘带 UTF-8 BOM");
}

static void testRightClickAddDeleteChain()
{
    // 右键「添加命令/删除此指令」落盘链路（2026-10-22 用户需求）：菜单与确认框为
    // 模态交互由 GUI 冒烟人工点验，此处验证其后的数据链路——
    // appendCommandRow（与 addCommandAtEnd 同序）→ saveTable → 重启面板读回；
    // 删空全部行 → saveTable → 重启面板空白 + 占位页（删除到空语义）
    QTemporaryDir dir;
    const QString csv = dir.filePath(QStringLiteral("debug_commands.csv"));
    writeFile(csv, QStringLiteral("分组,名称,发送内容,带参数,默认参数,参数提示\n"
                                  "基础,版本信息,@R20:0,否,,\n")
                       .toUtf8());
    // ① 添加：追加 1 行 → 落盘 → 重启读回 2 行
    {
        DebugPanel p(nullptr, csv);
        p.show();
        CHECK(p.m_table->rowCount() == 1, "添加链路前置：载入 1 条指令");
        DebugCommand c;
        c.name = QStringLiteral("标定电压");
        c.send = QStringLiteral("@W21:%1.");
        c.hasParam = true;
        c.paramDefault = QStringLiteral("0230");
        c.paramHint = QStringLiteral("电压(0.1V)");
        p.appendCommandRow(QStringLiteral("校准"), c);
        p.updatePlaceholder();
        CHECK(p.m_table->rowCount() == 2, "追加行后表格 2 行");
        CHECK(p.saveTable(), "添加后 saveTable 成功");
    }
    {
        DebugPanel p2(nullptr, csv);
        p2.show();
        CHECK(p2.m_table->rowCount() == 2, "添加后重启面板：2 行仍在");
        if (p2.m_table->rowCount() < 2)
            return;
        CHECK(p2.m_table->item(1, 0)->text() == QStringLiteral("校准")
                  && p2.m_table->item(1, 1)->text() == QStringLiteral("标定电压"),
              "添加后重启面板：新行内容正确");
    }
    // ② 删除到空：逐行 removeRow（与 deleteCommandAt 同路径，含选中迁移）→ 落盘 →
    // 重启面板空白 + 占位页（等效删除后表格空语义；三轮审查 D3 更贴近实况）
    {
        DebugPanel p3(nullptr, csv);
        p3.show();
        while (p3.m_table->rowCount() > 0)
            p3.m_table->removeRow(0);
        p3.updatePlaceholder();
        CHECK(p3.m_stack->currentWidget() == static_cast<QWidget*>(p3.m_placeholder),
              "删空后切回占位提示页");
        CHECK(p3.saveTable(), "删空后 saveTable 成功（保存空表）");
    }
    {
        DebugPanel p4(nullptr, csv);
        p4.show();
        CHECK(p4.m_table->rowCount() == 0, "删空后重启面板：0 行（空表生效）");
        CHECK(p4.m_stack->currentWidget() == static_cast<QWidget*>(p4.m_placeholder),
              "删空后重启面板：显示占位提示页");
    }
}

int run_debugpanel_tests()
{
    testEmptyTableOnMissingJson();
    testJsonLoadPanelTable();
    testCsvTableLoadAndSave();
    testRightClickAddDeleteChain();
    testParamValidationAndSubstitution();
    testEmptySendTolerance();
    testNoAutoMigrationWithInjectedPath();
    testRightClickEditPersists();
    printf("SECTION debugpanel: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
