#pragma once

// 主窗口：两个页签——「串口助手」（三栏：左配置+发送 / 中接收区 / 右调试指令面板）
// 与「固件升级」（用户反馈：页签切换比独立窗口顺手）。
// 共享唯一 SerialPort 实例；状态栏显示串口状态与收发计数。

#include <QMainWindow>

class SerialPort;
class AppConfig;
class TerminalPage;
class DebugPanel;
class UpgradePage;
class QLabel;
class QCloseEvent;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    AppConfig* m_config;      // 先声明：成员按声明逆序析构，配置活得最久
    SerialPort* m_port;       // 各页面共享的唯一串口实例
    TerminalPage* m_terminal;
    DebugPanel* m_debugPanel; // 由终端页左栏托管显示（addToSidebar）
    UpgradePage* m_upgradePage;

    QLabel* m_portStatus; // 状态栏：串口状态（可伸展区）
    QLabel* m_rxCount;    // 状态栏：RX 计数（常驻右侧）
    QLabel* m_txCount;    // 状态栏：TX 计数
    qint64 m_rxTotal = 0;
    qint64 m_txTotal = 0;
    bool m_upgradeBusy = false; // 升级忙状态镜像（Suggestion 1：busy 时关闭窗口弹确认）
};
