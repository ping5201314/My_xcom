#include "mainwindow.h"
#include "version.h"

#include "config/appconfig.h"
#include "pages/debugpanel.h"
#include "pages/terminalpage.h"
#include "pages/upgradepage.h"
#include "serial/serialconfig.h"
#include "serial/serialport.h"

#include <QCloseEvent>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_config(new AppConfig)
    , m_port(new SerialPort(this))
{
    setWindowTitle(QStringLiteral("my_com 串口助手 ") + QStringLiteral(MYCOM_VERSION_STR));

    // 两个页签：串口助手（左栏含调试指令面板）/ 固件升级。
    // 调试指令不再单独开窗或侧栏：放进串口助手页左栏「发送设置」下方，与主界面一体
    // （用户反馈），接收区始终完整可见，可一边发指令一边看回显
    auto* tabs = new QTabWidget(this);
    m_terminal = new TerminalPage(m_port, m_config, tabs);
    m_debugPanel = new DebugPanel(m_port, tabs);
    m_terminal->addRightPanel(m_debugPanel); // 右栏：调试指令（用户反馈右侧更顺手）
    m_upgradePage = new UpgradePage(m_port, tabs);
    tabs->addTab(m_terminal, QStringLiteral("串口助手"));
    tabs->addTab(m_upgradePage, QStringLiteral("固件升级"));
    setCentralWidget(tabs);

    // 状态栏：左侧串口状态（可伸展），右侧常驻收发计数
    m_portStatus = new QLabel(QStringLiteral("串口未打开"), this);
    m_rxCount = new QLabel(QStringLiteral("RX: 0"), this);
    m_txCount = new QLabel(QStringLiteral("TX: 0"), this);
    statusBar()->addWidget(m_portStatus, 1);
    statusBar()->addPermanentWidget(m_rxCount);
    statusBar()->addPermanentWidget(m_txCount);

    connect(m_port, &SerialPort::opened, this, [this](const SerialConfig& cfg) {
        m_portStatus->setText(QStringLiteral("已连接：%1").arg(cfg.toString()));
    });
    connect(m_port, &SerialPort::closed, this, [this] {
        m_portStatus->setText(QStringLiteral("串口未打开"));
    });
    // 状态栏 RX 计数走 dataReceived（独占升级期间不进终端页，也不计在这里）
    connect(m_port, &SerialPort::dataReceived, this, [this](const QByteArray& data) {
        m_rxTotal += data.size();
        m_rxCount->setText(QStringLiteral("RX: %1").arg(m_rxTotal));
    });
    connect(m_terminal, &TerminalPage::txBytesWritten, this, [this](qint64 n) {
        m_txTotal += n;
        m_txCount->setText(QStringLiteral("TX: %1").arg(m_txTotal));
    });
    // 用户反馈：删除"升级期间禁用调试指令发送"的联动——升级进行中调试面板照常可用；
    // 用户自行把控传输期间不发命令（@W12 特殊模式指令仍有确认框+35s 冷却兜底）。
    // 修复 C2：升级忙同时联动终端页——发送/循环发送/开关串口/参数全部禁用（05c A9）
    connect(m_upgradePage, &UpgradePage::upgradeBusyChanged, m_terminal,
            &TerminalPage::setUpgradeBusy);
    // Suggestion 1：镜像升级忙状态，busy 时关闭窗口先确认
    connect(m_upgradePage, &UpgradePage::upgradeBusyChanged, this, [this](bool busy) {
        m_upgradeBusy = busy;
        // 状态栏明确显示升级占用状态：用户此前误把面板日志里的历史提示
        // 当成"仍被禁用"，加一个实时指示（busy 含冷却与版本确认轮询期）
        m_portStatus->setText(busy ? QStringLiteral("固件升级进行中…（升级结束后自动恢复）")
                                   : (m_port->isOpen()
                                          ? QStringLiteral("已连接：%1").arg(m_port->config().toString())
                                          : QStringLiteral("串口未打开")));
    });
    connect(m_port, &SerialPort::errorOccurred, this, [this](const QString& msg) {
        statusBar()->showMessage(msg, 8000);
    });

    // 默认尺寸与拖动下限一致（用户要求）：三栏最小总宽 + 20px 边框余量，
    // 动态跟随接收区工具栏的最小宽
    const int baseWidth = m_terminal->minimumNeededWidth() + 20;
    setMinimumSize(baseWidth, 700);
    resize(baseWidth, 740);
    // 用户反馈：记住上次关闭时的窗口大小与位置（首次运行用上面的默认值）
    const QByteArray geo = m_config->value(AppConfig::kWindowGeometry).toByteArray();
    if (!geo.isEmpty())
        restoreGeometry(geo);
}

MainWindow::~MainWindow()
{
    // AppConfig 是裸指针成员（非 QObject），需手动释放
    delete m_config;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 退出前把终端页的端口参数/显示选项落盘（AppConfig 在成员里最后析构）
    m_terminal->saveSettings();
    // 记住窗口大小与位置（用户反馈：下次打开和调整过的一样）
    m_config->setValue(AppConfig::kWindowGeometry, saveGeometry());
    // Suggestion 1：升级进行中关闭窗口先确认——退出会中断传输（设备约 3s 后自愈，
    // 无害但可能白传一轮），误关窗口的概率远高于有意退出
    if (m_upgradeBusy) {
        const auto r = QMessageBox::question(
            this, QStringLiteral("确认退出"),
            QStringLiteral("固件升级正在进行中，退出将中断升级"
                           "（设备约 3s 后自动退出升级状态）。确定退出？"));
        if (r != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }
    QMainWindow::closeEvent(event);
}
