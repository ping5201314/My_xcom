#include "cli/climode.h"
#include "version.h"

#include "serial/serialconfig.h"
#include "serial/serialport.h"
#include "serial/relayport.h"
#include "upgrade/upgradeengine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

FILE* g_log = nullptr;   // 镜像输出：AI 环境里控制台挂接可能失败，日志文件保证可读
FILE* g_trace = nullptr; // 共享监视文件（%TEMP%/my_com_trace.log，追加写）：
                         // GUI「监视AI会话」实时显示 AI 的收发，人不占串口也能看

// 统一输出：stdout + cli.log + 共享监视文件三写
void out(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    if (g_log)
        std::fputs(buf, g_log);
    if (g_trace)
        std::fputs(buf, g_trace);
    std::fflush(stdout);
    if (g_log)
        std::fflush(g_log);
    if (g_trace)
        std::fflush(g_trace);
}

// 只写 stdout + cli.log、**不回写共享监视文件**——用于打印监视文件自身的内容
//（relay 的尾部回显若回写 trace，会被 GUI 监视再次解析成 GUI-RX 行 → 重复显示）
void outNoTrace(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    if (g_log)
        std::fputs(buf, g_log);
    std::fflush(stdout);
    if (g_log)
        std::fflush(g_log);
}

// GUI 子系统的 exe 从控制台启动时 stdout 默认不挂接——把父控制台接回来
void attachConsole()
{
#ifdef Q_OS_WIN
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONERR$", "w", stderr);
    }
#endif
}

void printHex(const QByteArray& data, const char* tag)
{
    char hex[8];
    out("%s (%d 字节): ", tag, static_cast<int>(data.size()));
    for (unsigned char c : data) {
        std::snprintf(hex, sizeof(hex), "%02X ", c);
        out("%s", hex);
    }
    out(" | ");
    for (unsigned char c : data)
        out("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
    out("\n");
    // 可读文本行（给人在 GUI 监视窗口直接读）：ASCII 原样、\r/\n 转行、
    // 连续换行折叠——设备回显行尾是 \r\r\n，折叠后不留空行
    QByteArray text;
    for (unsigned char c : data) {
        if (c == '\r' || c == '\n') {
            if (!text.endsWith('\n'))
                text.append('\n');
        } else if (c >= 0x20 && c < 0x7F) {
            text.append(static_cast<char>(c));
        }
    }
    if (!text.trimmed().isEmpty())
        out("%s文本:\n%s\n", tag, text.trimmed().constData());
}

int usage()
{
    out("my_com 命令行模式（供 AI/脚本控制串口）:\n");
    out("  my_com.exe cli send  <帧文本>  --port COM5 [--baud 115200] [--wait-ms 3000]\n");
    out("  my_com.exe cli listen          --port COM5 [--baud 115200] [--seconds 5]\n");
    out("  my_com.exe cli upgrade <bin>   [--port COM5]   <- 不带 --port 经串口助手中转(GUI 连接时)\n");
    out("  my_com.exe cli relay-send <帧文本> [--wait-ms 5000]   ← GUI 占用串口时经它代发\n");
    return 1;
}

SerialConfig makeConfig(const QString& port, int baud)
{
    SerialConfig cfg;
    cfg.portName = port;
    cfg.baud = baud;
    cfg.dataBits = Data8;
    cfg.parity = ParityNone;
    cfg.stopBits = StopOne;
    cfg.flowControl = FlowNone; // 串口升级要求无流控
    return cfg;
}

// 尝试中转代发（GUI 已连串口时经它发，任何端口都行）：成功返回 true 并完成
// 回显输出；失败（GUI 未运行/串口未开）返回 false，无痕迹（错误只进 cli.log）
bool tryRelay(const QString& frame, int waitMs)
{
    const QString reqPath = QDir::tempPath() + QStringLiteral("/my_com_ai_tx.txt");
    QFile f(reqPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(frame.toUtf8());
    f.close();
    bool consumed = false;
    for (int i = 0; i < 20; ++i) {
        QThread::msleep(100);
        if (!QFile::exists(reqPath)) {
            consumed = true;
            break;
        }
    }
    if (!consumed) {
        QFile::remove(reqPath);
        outNoTrace("CLI-INFO: 中转不可用（GUI 未运行或串口未打开），回退直连\n");
        return false;
    }
    out("CLI-RELAY-SENT: %s（由串口助手代发）\n", qPrintable(frame));
    QThread::msleep(waitMs);
    QFile trace(QDir::tempPath() + QStringLiteral("/my_com_trace.log"));
    if (trace.open(QIODevice::ReadOnly)) {
        const qint64 sz = trace.size();
        trace.seek(sz > 2048 ? sz - 2048 : 0);
        outNoTrace("=== 监视文件尾部 ===\n%s", trace.readAll().constData());
    }
    out("CLI-OK\n");
    return true;
}

int cmdSend(int argc, char* argv[])
{
    QString frame;
    QString port;
    int baud = 115200;
    int waitMs = 3000;
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc)
            port = QString::fromLocal8Bit(argv[++i]);
        else if (!std::strcmp(argv[i], "--baud") && i + 1 < argc)
            baud = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--wait-ms") && i + 1 < argc)
            waitMs = std::atoi(argv[++i]);
        else if (frame.isEmpty())
            frame = QString::fromLocal8Bit(argv[i]);
    }
    if (frame.isEmpty())
        return usage();

    if (port.isEmpty()) {
        // 自动模式（用户要求：多数场景先人工 GUI 后 AI）：
        // 1) 中转优先——GUI 已连串口时经它代发，与端口号无关
        // 2) 中转不可用 → 回退直连 GUI 配置（%APPDATA% ini）里记住的端口
        if (tryRelay(frame, waitMs))
            return 0;
        QSettings ini(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("MyTeam"),
                      QStringLiteral("my_com"));
        port = ini.value(QStringLiteral("serial/lastPort")).toString();
        if (port.isEmpty()) {
            out("CLI-ERROR: GUI 未运行且无历史端口记录，请指定 --port\n");
            return 1;
        }
        out("CLI-INFO: 直连 %s（GUI 未占用串口）\n", qPrintable(port));
    }

    SerialPort sp;
    if (!sp.open(makeConfig(port, baud))) {
        out("CLI-ERROR: 打开 %s 失败\n", qPrintable(port));
        return 1;
    }
    sp.beginExclusive();
    const QByteArray bytes = frame.toUtf8();
    const qint64 n = sp.writeSync(bytes, 2000);
    out("TX (%d/%d 字节): %s\n", static_cast<int>(n), static_cast<int>(bytes.size()),
                qPrintable(frame));
    printHex(bytes, "TX-HEX");

    // 收一段时间的回显（分片读，数据一停就提前结束）
    QByteArray reply;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < waitMs) {
        const QByteArray r = sp.readBytes(512, 100);
        reply.append(r);
        if (!r.isEmpty() && reply.size() >= 512)
            break; // 收到足够数据，提前收工
    }
    sp.endExclusive();
    printHex(reply, "RX");
    out("CLI-OK\n");
    return 0;
}

int cmdListen(int argc, char* argv[])
{
    QString port;
    int baud = 115200;
    int seconds = 5;
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc)
            port = QString::fromLocal8Bit(argv[++i]);
        else if (!std::strcmp(argv[i], "--baud") && i + 1 < argc)
            baud = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
    }
    if (port.isEmpty())
        return usage();

    QCoreApplication app(argc, argv);
    SerialPort sp;
    if (!sp.open(makeConfig(port, baud))) {
        out("CLI-ERROR: 打开 %s 失败\n", qPrintable(port));
        return 1;
    }
    out("CLI-LISTEN: %s @%d 监听 %d 秒...\n", qPrintable(port), baud, seconds);
    QObject::connect(&sp, &SerialPort::dataReceived, [](const QByteArray& data) {
        printHex(data, "RX");
    });
    QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    app.exec();
    out("CLI-OK\n");
    return 0;
}

int cmdRelaySend(int argc, char* argv[])
{
    // 中转发送：COM 口被 GUI 占用时，把帧写进请求文件，由运行中的串口助手
    // 用它的串口代发（协同调试：AI 与人在同一串口互不影响）
    QString frame;
    int waitMs = 5000;
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--wait-ms") && i + 1 < argc)
            waitMs = std::atoi(argv[++i]);
        else if (frame.isEmpty())
            frame = QString::fromLocal8Bit(argv[i]);
    }
    if (frame.isEmpty())
        return usage();
    if (tryRelay(frame, waitMs))
        return 0;
    out("CLI-ERROR: 中转请求未被消费——请确认串口助手正在运行且串口已打开\n");
    return 1;
}

int cmdUpgrade(int argc, char* argv[])
{
    QString file;
    QString port;
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc)
            port = QString::fromLocal8Bit(argv[++i]);
        else if (file.isEmpty())
            file = QString::fromLocal8Bit(argv[i]);
    }
    if (file.isEmpty())
        return usage();
    if (!QFileInfo::exists(file)) {
        out("CLI-ERROR: 文件不存在: %s\n", qPrintable(file));
        return 1;
    }

    QCoreApplication app(argc, argv);
    // 单次升级尝试:engine.start 经队列调用在工作线程执行,finished 时退出事件循环
    const auto runAttempt = [&](IUpgradePort* transport, const QString& portName) -> int {
        UpgradeEngine engine(transport);
        int exitCode = 1;
        QObject::connect(&engine, &UpgradeEngine::logMessage,
                         [](const QString& msg) { out("%s\n", qPrintable(msg));  });
        QObject::connect(&engine, &UpgradeEngine::finished, &app,
                         [&](bool ok, const QString& reason) {
            out("%s: %s\n", ok ? "CLI-UPGRADE-OK" : "CLI-UPGRADE-FAIL", qPrintable(reason));
            exitCode = ok ? 0 : 1;
            app.quit();
        });
        // start() 经队列调用在工作线程执行;日志经信号回主线程打印
        QMetaObject::invokeMethod(&engine, [&engine, file, portName] {
            engine.start(file, portName);
        }, Qt::QueuedConnection);
        app.exec();
        return exitCode;
    };

    // 不带 --port:中转升级模式(经串口助手代发代收,GUI 连接串口时互不影响;
    // 用户要求:GUI 连接不影响终端使用)。中转失败(助手未运行/未开串口)自动回退
    // 直连历史端口,直连再失败则给出明确指引。
    if (port.isEmpty()) {
        RelayPort rp;
        out("CLI-INFO: 中转升级模式——经串口助手代发代收(GUI 保持连接不受影响)\n");
        // 引擎校验端口名非空才开串口:中转模式传占位名,RelayPort 忽略参数只建会话
        const int rc = runAttempt(&rp, QStringLiteral("RELAY"));
        if (rc == 0)
            return 0;
        out("CLI-INFO: 中转失败(串口助手未运行或未打开串口),回退直连模式\n");
        QSettings ini(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("MyTeam"),
                      QStringLiteral("my_com"));
        port = ini.value(QStringLiteral("serial/lastPort")).toString();
        if (port.isEmpty()) {
            out("CLI-ERROR: 无历史端口记录——请打开串口助手(保持串口打开)或指定 --port COMx 重试\n");
            return rc;
        }
    }

    SerialPort sp;
    const int rc = runAttempt(&sp, port);
    if (rc != 0)
        out("CLI-INFO: 直连失败(串口可能被 GUI 占用)。中转需串口助手运行且串口已打开;直连需先关闭 GUI 串口\n");
    return rc;
}


} // namespace

int runCliMode(int argc, char* argv[])
{
    attachConsole();
    // 日志镜像：exe 旁 cli.log。GUI 子系统的 exe 在无控制台环境（如 AI 终端）
    // stdout 不可见，AI/脚本读这个文件拿结果
    const QString logPath = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath()
                            + QStringLiteral("/cli.log");
    g_log = std::fopen(logPath.toLocal8Bit().constData(), "w");
    // 共享监视文件：追加写；超过 512KB 截断重开（防长期无人清理膨胀）
    const QString tracePath = QDir::tempPath() + QStringLiteral("/my_com_trace.log");
    if (QFileInfo::exists(tracePath) && QFileInfo(tracePath).size() > 512 * 1024)
        g_trace = std::fopen(tracePath.toLocal8Bit().constData(), "w");
    else
        g_trace = std::fopen(tracePath.toLocal8Bit().constData(), "a");
    out("=== AI 会话开始 %s（my_com " MYCOM_VERSION_STR "）===\n", qPrintable(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    // 去掉 "cli" 本身：剩余 argv = 子命令 + 参数
    if (argc < 3)
        return usage();
    const QString sub = QString::fromLocal8Bit(argv[2]);
    const int restArgc = argc - 3;
    char** restArgv = argv + 3;
    int rc = 1;
    if (sub == QStringLiteral("send"))
        rc = cmdSend(restArgc, restArgv);
    else if (sub == QStringLiteral("listen"))
        rc = cmdListen(restArgc, restArgv);
    else if (sub == QStringLiteral("upgrade"))
        rc = cmdUpgrade(restArgc, restArgv);
    else if (sub == QStringLiteral("relay-send"))
        rc = cmdRelaySend(restArgc, restArgv);
    else
        rc = usage();
    out("=== AI 会话结束 ===\n");
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    if (g_trace) {
        std::fclose(g_trace);
        g_trace = nullptr;
    }
    return rc;
}
