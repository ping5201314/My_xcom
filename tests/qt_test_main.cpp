// Qt 层单元测试入口（T4）：DebugCommandModel / SerialConfig / DebugPanel 行为测试。
// 与 my_com_tests（纯 C++ 协议库）分离：本目标链接 Qt6::Widgets（内含 Core），
// mycom_proto 保持纯 C++ 定位不变。
// 平台用 offscreen（无窗口弹出、CI 可重复）；QT_QPA_PLATFORM 必须在 QApplication 构造前设置。

#include <QApplication>
#include <QByteArray>

#include <cstdio>

int run_debugcmd_tests();
int run_serialconfig_tests();
int run_debugpanel_tests();

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);

    int fails = 0;
    fails += run_debugcmd_tests();
    fails += run_serialconfig_tests();
    fails += run_debugpanel_tests();
    if (fails)
        printf("RESULT: %d FAILED\n", fails);
    else
        printf("RESULT: ALL PASSED\n");
    return fails ? 1 : 0;
}
