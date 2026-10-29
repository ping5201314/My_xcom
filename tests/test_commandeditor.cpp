// CommandEditorDialog 单元测试（T11 新增，覆盖 T9「加载表格」导入链路的非对话框部分）：
// 构造加载 → fillTable 全量填充 6 列 → 表格改值 → collectAndValidate 校验 → doSave
// 落盘 → 新对话框/新模型读回一致。文件选择器（QFileDialog 原生窗口）无法在 offscreen
// 下自动化，由 GUI 冒烟人工点验（T11 报告）；此处验证其后的「全量替换 + 关闭保存」逻辑。
// 空表校验分支（doSave 弹确认框）与失败弹窗不在本 TU 内触发（模态阻塞 offscreen），
// 只测 collectAndValidate 的纯逻辑部分。
// 框架约定同 my_com_qt_tests：CHECK 宏 + 失败计数；入口 run_commandeditor_tests() 由
// qt_test_main 调用。路径全部注入 QTemporaryDir，不读写真实 %APPDATA%。

#include <QComboBox>
#include <QFile>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cstdio>

// 仅使 CommandEditorDialog 的 private 成员对本 TU 可见（与 test_debugpanel 同手法），
// 不改类布局/ODR 符号
#define private public
#include "debugcmd/commandeditor.h"
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
QString writeJson(const QTemporaryDir& dir, const char* name, const QByteArray& content)
{
    const QString path = dir.filePath(QLatin1String(name));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(content);
    return path;
}
} // namespace

static void testLoadFillModifySave()
{
    // 编辑器从 loadPath 加载 2 条指令 → 表格 6 列填充 → 改名称与 send → doSave →
    // 落盘文件为修改后内容（等效「加载表格导入 → 关闭保存」的落盘结果）
    QTemporaryDir dir;
    const QString loadPath = writeJson(dir, "editor_load.json", R"({
        "groups": [ { "group": "信息查询",
            "commands": [
                { "name": "版本信息", "send": "@R20:0" },
                { "name": "设定压力", "send": "@W05:%1.", "hasParam": true,
                  "paramDefault": "0900", "paramHint": "4位数字" } ] } ]
    })");
    const QString savePath = dir.filePath(QStringLiteral("editor_save.json"));
    {
        CommandEditorDialog dlg(loadPath, savePath);
        CHECK(dlg.m_table->rowCount() == 2, "构造加载：表格 2 行（fillTable 全量填充）");
        CHECK(dlg.m_table->columnCount() == 6, "表格 6 列（分组/名称/发送/带参/默认/提示）");
        if (dlg.m_table->rowCount() < 2)
            return;
        CHECK(dlg.m_table->item(0, 1)->text() == QStringLiteral("版本信息")
                  && dlg.m_table->item(0, 2)->text() == QStringLiteral("@R20:0"),
              "行 0 名称/发送内容正确");
        auto* combo = qobject_cast<QComboBox*>(dlg.m_table->cellWidget(1, 3));
        CHECK(combo && combo->currentIndex() == 1, "行 1 带参数列下拉 = 是");
        CHECK(dlg.m_table->item(1, 4)->text() == QStringLiteral("0900")
                  && dlg.m_table->item(1, 5)->text() == QStringLiteral("4位数字"),
              "行 1 默认参数/参数提示正确");
        // 模拟用户改表：改名称与发送内容
        dlg.m_table->item(0, 1)->setText(QStringLiteral("版本信息2"));
        dlg.m_table->item(0, 2)->setText(QStringLiteral("@R21:0"));
        CHECK(dlg.doSave(), "修改后 doSave 成功（非空表无确认弹窗）");
    }
    // 新实例从 savePath 读回 = 修改后内容
    {
        CommandEditorDialog dlg2(savePath, savePath);
        CHECK(dlg2.m_table->rowCount() == 2, "保存产物可被新编辑器实例加载");
        if (dlg2.m_table->rowCount() < 2)
            return;
        CHECK(dlg2.m_table->item(0, 1)->text() == QStringLiteral("版本信息2")
                  && dlg2.m_table->item(0, 2)->text() == QStringLiteral("@R21:0"),
              "读回内容为修改后内容（跨实例一致）");
        CHECK(dlg2.m_table->item(1, 4)->text() == QStringLiteral("0900"),
              "未改动的行字段原样保留");
    }
}

static void testMissingLoadEmptyEditor()
{
    // loadPath 缺失 → 表格空（可从头新建，与面板空表流程衔接）
    QTemporaryDir dir;
    const QString missing = dir.filePath(QStringLiteral("nope.json"));
    CommandEditorDialog dlg(missing, dir.filePath(QStringLiteral("out.json")));
    CHECK(dlg.m_table->rowCount() == 0, "loadPath 缺失：编辑器空表（不崩溃不弹窗）");
}

static void testValidateRejectsEmptyFields()
{
    // collectAndValidate：空分组/空名称/空 send 返回 false 并给出行号
    QTemporaryDir dir;
    const QString loadPath = writeJson(dir, "v_load.json", R"({
        "groups": [ { "group": "g", "commands": [ { "name": "x", "send": "@R20:0" } ] } ]
    })");
    {
        CommandEditorDialog dlg(loadPath, dir.filePath(QStringLiteral("v_save.json")));
        dlg.m_table->item(0, 1)->setText(QString()); // 名称清空
        QString err;
        CHECK(!dlg.collectAndValidate(&err), "空名称 → 校验失败");
        CHECK(err.contains(QStringLiteral("第 1 行")), "错误提示含行号");
        // 注意:此状态下的 doSave 会弹「无法保存」模态警告框,offscreen 无法关闭,
        // 不做调用(校验失败早退由 collectAndValidate 的返回保证,doSave 首行即调用它)
    }
    // 空表（0 行）校验通过 → 与空表保存流程（确认后落空表）的模型层一致
    {
        CommandEditorDialog dlg(dir.filePath(QStringLiteral("missing.json")),
                                dir.filePath(QStringLiteral("v_save2.json")));
        QString err;
        CHECK(dlg.collectAndValidate(&err), "空表校验通过（确认弹窗在 doSave，非本层职责）");
        CHECK(dlg.m_model.entries().isEmpty(), "空表校验后模型为空");
    }
}

static void testCsvSavePath()
{
    // 2026-10-21 CSV 拍板：savePath 为 .csv 时 doSave 走 saveCsv（BOM+6列表头），
    // 新编辑器实例按 csv 读回内容一致（固定位置即 csv 路径的落地链路）
    QTemporaryDir dir;
    // 注：内容含 )" 序列，用自定义定界符 R"CSV(...)CSV" 防 raw string 提前终止
    const QString loadPath = writeJson(dir, "c_load.json", R"CSV({
        "groups": [ { "group": "校准",
            "commands": [ { "name": "标定电压", "send": "@W21:%1.", "hasParam": true,
                            "paramDefault": "0230", "paramHint": "电压(0.1V)" } ] } ]
    })CSV");
    const QString savePath = dir.filePath(QStringLiteral("c_save.csv"));
    {
        CommandEditorDialog dlg(loadPath, savePath);
        CHECK(dlg.m_table->rowCount() == 1, "json 加载 1 行（跨格式导入场景）");
        CHECK(dlg.doSave(), "savePath 为 .csv → doSave 成功");
    }
    QFile f(savePath);
    CHECK(f.open(QIODevice::ReadOnly) && f.readAll().startsWith("\xEF\xBB\xBF"),
          "落盘为 UTF-8 BOM CSV");
    {
        CommandEditorDialog dlg2(savePath, savePath);
        CHECK(dlg2.m_table->rowCount() == 1, "csv 产物可被编辑器读回");
        if (dlg2.m_table->rowCount() < 1)
            return;
        CHECK(dlg2.m_table->item(0, 1)->text() == QStringLiteral("标定电压"),
              "csv 读回名称一致");
        auto* combo = qobject_cast<QComboBox*>(dlg2.m_table->cellWidget(0, 3));
        CHECK(combo && combo->currentIndex() == 1, "csv 读回带参数 = 是");
        CHECK(dlg2.m_table->item(0, 4)->text() == QStringLiteral("0230"),
              "csv 读回默认参数一致");
    }
}

int run_commandeditor_tests()
{
    testLoadFillModifySave();
    testMissingLoadEmptyEditor();
    testValidateRejectsEmptyFields();
    testCsvSavePath();
    printf("SECTION commandeditor: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
