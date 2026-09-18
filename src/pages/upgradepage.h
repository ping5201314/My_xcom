#pragma once

// 固件升级页：静态界面骨架在 upgradepage.ui（Qt Designer 可视化编辑，AUTOUIC 编译
// 生成 ui_upgradepage.h）；动态逻辑在本类：文件校验、进度、日志、横幅、忙碌禁用。
// 运行中：开始按钮禁用、取消可用；busyChanged 转发给主窗口联动禁用终端页（05c D15/A9）。

#include <QWidget>

#include "config/appconfig.h"

namespace Ui {
class UpgradePage;
}

class SerialPort;
class UpgradeEngine;
class QLineEdit;
class QPushButton;
class QLabel;
class QProgressBar;
class QPlainTextEdit;

class UpgradePage : public QWidget
{
    Q_OBJECT
public:
    explicit UpgradePage(SerialPort* port, QWidget* parent = nullptr);
    ~UpgradePage() override;

signals:
    /// 升级忙状态（含冷却期），供主窗口禁用调试面板发送等
    void upgradeBusyChanged(bool busy);

private slots:
    void onBrowse();
    void onStart();
    void onCancel();
    void onSaveLog(); // 手动保存过程日志（文件对话框自选位置；用户要求不自动保存）
    void onPhaseChanged(const QString& phase);
    void onProgressChanged(int done, int total);
    void onLog(const QString& msg);
    void onFinished(bool ok, const QString& reason);
    void onBusyChanged(bool busy);

private:
    /// 刷新文件信息与校验标注（名称长度/大小/存在性，不合格项红字）
    void updateFileInfo();
    void appendLog(const QString& line); // 上限 5000 行 + 自动滚动
    void setBanner(bool ok, const QString& text);
    void saveSettings();

    SerialPort* m_port;
    UpgradeEngine* m_engine = nullptr; // 工作线程自持（无父级），本页负责析构
    AppConfig m_config;                // 文件路径持久化
    Ui::UpgradePage* ui = nullptr;     // .ui 骨架（setupUi 生成控件，本类析构）

    // 以下成员是 .ui 里控件的"别名"：setupUi 后从 ui 取回，动态逻辑沿用 m_ 前缀
    QLineEdit* m_fileEdit = nullptr;
    QPushButton* m_browseBtn = nullptr;
    QLabel* m_fileInfo = nullptr; // 文件校验信息（富文本，不合格项红字）
    QPushButton* m_startBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_phaseLabel = nullptr;
    QPlainTextEdit* m_logView = nullptr;
    QLabel* m_banner = nullptr;
    QPushButton* m_saveLogBtn = nullptr; // 手动保存日志按钮（.ui 定义）

    bool m_busy = false;
    QString m_lastFilePath; // 与 m_config 同步的当前文件路径
};
