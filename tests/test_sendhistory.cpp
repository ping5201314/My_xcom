// SendHistory 单元测试（T11 新增，覆盖 T10 纯逻辑类）：完全匹配去重移前（区分大小写、
// 含空格）、上限 50 裁剪（add 与 setItems 两条路径）、空输入不记录、ini 往返（QSettings
// 数组格式，逗号/换行/中文条目保真）、缺键默认空、保存空历史清键。
// AppConfig 用 iniPath 构造注入临时 ini（与 debugpanel 路径注入同理），不读写真实
// %APPDATA%。框架约定同 my_com_qt_tests：CHECK 宏 + 失败计数；入口
// run_sendhistory_tests() 由 qt_test_main 调用。

#include <QFile>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

#include "config/appconfig.h"

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

static void testDedupMoveToFront()
{
    SendHistory h;
    h.add(QStringLiteral("a"));
    h.add(QStringLiteral("b"));
    h.add(QStringLiteral("a")); // 重复 → 删旧位置、插到最前
    CHECK(h.list() == QStringList({QStringLiteral("a"), QStringLiteral("b")}),
          "去重移前：重复条目移到最前且不重复");
    h.add(QStringLiteral("A"));
    CHECK(h.list() == QStringList({QStringLiteral("A"), QStringLiteral("a"),
                                   QStringLiteral("b")}),
          "完全匹配：区分大小写（A 与 a 视为不同）");
    h.add(QStringLiteral(" a"));
    CHECK(h.list() == QStringList({QStringLiteral(" a"), QStringLiteral("A"),
                                   QStringLiteral("a"), QStringLiteral("b")}),
          "完全匹配：含空格参与比较（' a' 与 'a' 不同）");
    h.add(QStringLiteral("b"));
    CHECK(h.list() == QStringList({QStringLiteral("b"), QStringLiteral(" a"),
                                   QStringLiteral("A"), QStringLiteral("a")}),
          "去重移前：旧位置删除后其余顺序保持");
}

static void testMaxFiftyAdd()
{
    SendHistory h;
    for (int i = 0; i < 55; ++i)
        h.add(QStringLiteral("条目%1").arg(i));
    CHECK(h.list().size() == 50, "add 超上限：裁剪到 50 条");
    CHECK(h.list().first() == QStringLiteral("条目54"), "add 超上限：最新条目在前");
    CHECK(h.list().last() == QStringLiteral("条目5"), "add 超上限：最旧 5 条被丢弃");
    CHECK(!h.list().contains(QStringLiteral("条目4")), "add 超上限：条目0~4 全部不在");
    h.add(QStringLiteral("条目5")); // 最旧仍存在的条目再发一次 → 移前
    CHECK(h.list().first() == QStringLiteral("条目5") && h.list().size() == 50,
          "去重移前与上限并存：旧条目再发移前且总量不超上限");
}

static void testEmptyInputIgnored()
{
    SendHistory h;
    h.add(QString());
    CHECK(h.list().isEmpty(), "空字符串输入不记录");
    h.add(QStringLiteral("x"));
    h.add(QString());
    CHECK(h.list() == QStringList({QStringLiteral("x")}), "空输入与正常条目混发：只记非空");
    h.add(QStringLiteral("   "));
    CHECK(h.list().first() == QStringLiteral("   "), "纯空白非空字符串照常记录（isEmpty 语义）");
}

static void testSetItemsTruncation()
{
    SendHistory h;
    QStringList items;
    for (int i = 0; i < 60; ++i)
        items << QStringLiteral("i%1").arg(i);
    h.setItems(items);
    CHECK(h.list().size() == 50, "setItems 防御性截断到 50");
    CHECK(h.list().first() == QStringLiteral("i0") && h.list().last() == QStringLiteral("i49"),
          "setItems 截断保留前 50 条（载入顺序即最新在前）");
    h.setItems({});
    CHECK(h.list().isEmpty(), "setItems 空列表 → 空历史");
}

static void testIniRoundTrip()
{
    QTemporaryDir dir;
    const QString iniPath = dir.filePath(QStringLiteral("my_com.ini"));
    {
        AppConfig cfg(iniPath);
        SendHistory h;
        h.add(QStringLiteral("含,逗号 的条目"));
        h.add(QStringLiteral("第一行\n第二行"));
        h.add(QStringLiteral("中文指令 @R20:0"));
        h.add(QStringLiteral("plain"));
        h.save(cfg);
    }
    // 新 AppConfig 实例从同一 ini 读回（模拟重启软件）
    {
        AppConfig cfg2(iniPath);
        SendHistory h2;
        h2.load(cfg2);
        CHECK(h2.list() == QStringList({QStringLiteral("plain"), QStringLiteral("中文指令 @R20:0"),
                                        QStringLiteral("第一行\n第二行"),
                                        QStringLiteral("含,逗号 的条目")}),
              "ini 往返：逗号/换行/中文条目逐条保真且顺序正确（最新在前）");
    }
    // ini 文件落盘格式：数组键 + 值原文（逗号不会被 join 破坏）
    QFile f(iniPath);
    if (f.open(QIODevice::ReadOnly)) {
        const QString txt = QString::fromUtf8(f.readAll());
        CHECK(txt.contains(QStringLiteral("size=")), "ini 文件用数组格式存历史（size 键存在）");
        CHECK(txt.contains(QStringLiteral("含,逗号 的条目")), "ini 文件里含逗号条目原文可见");
        f.close();
    } else {
        CHECK(false, "ini 文件可读");
    }
    // 保存空历史 → 键被清掉，重启读回为空（不残留旧值）
    {
        AppConfig cfg3(iniPath);
        SendHistory h3;
        h3.load(cfg3);
        h3.setItems({});
        h3.save(cfg3);
    }
    {
        AppConfig cfg4(iniPath);
        SendHistory h4;
        h4.load(cfg4);
        CHECK(h4.list().isEmpty(), "保存空历史后重启读回为空（键已清，不残留）");
    }
}

static void testMissingKeyDefaultsEmpty()
{
    QTemporaryDir dir;
    const QString iniPath = dir.filePath(QStringLiteral("fresh.ini")); // 全新 ini：无 ui/sendHistory 键
    AppConfig cfg(iniPath);
    SendHistory h;
    h.load(cfg);
    CHECK(h.list().isEmpty(), "缺键（新 ini）→ 历史默认空");
}

int run_sendhistory_tests()
{
    testDedupMoveToFront();
    testMaxFiftyAdd();
    testEmptyInputIgnored();
    testSetItemsTruncation();
    testIniRoundTrip();
    testMissingKeyDefaultsEmpty();
    printf("SECTION sendhistory: %d PASSED, %d FAILED\n", g_passes, g_failures);
    return g_failures;
}
