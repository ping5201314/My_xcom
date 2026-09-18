#pragma once

// 调试指令面板：加载 commands/debug_commands.json（exe 目录，构建后由 CMake 复制），
// 按分组展示 @R/@W/@C 指令；点选后带参指令显示参数输入框（校验恰好 4 位数字），
// 双击直接用默认参数发送。
// 布局（用户反馈改版，竖排紧凑）：连接位选择（:0/:1）置顶 → 指令树占大头 →
// 参数行 → 底部小日志窗口（固定高度，仅作发送确认）。
// 串口未打开时点发送只提示「串口未打开」，不崩溃；升级忙时由主窗口联动禁用发送。

#include <QVector>
#include <QWidget>
#include <QColor>

class SerialPort;
struct DebugCommand;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QComboBox;
class QTimer;

class DebugPanel : public QWidget
{
    Q_OBJECT
public:
    explicit DebugPanel(SerialPort* port, QWidget* parent = nullptr);

private slots:
    void onTreeSelectionChanged();
    void onTreeDoubleClicked(QTreeWidgetItem* item, int column);
    void onSendCurrent();
    void onEditCommands(); // 弹窗编辑指令表（添加/修改/删除），保存后重新加载

private:
    void buildUi();
    void loadCommands(); // 从 exe 目录加载 JSON，失败回退内置最小指令集
    void addGroup(const QString& group, const QVector<DebugCommand>& cmds);
    QString currentFrameText(); // 校验参数并替换 %1，返回最终帧文本（非法返回空并提示）
    void doSend(const QString& frame, const QString& name); // 打开检查 + 写入 + TX 日志
    // 时间戳 + 内容，3000 行上限，自动滚动；color 非空时该行用指定颜色（错误提示红字）
    void appendLog(const QString& line, const QColor& color = {});
    QString hexOf(const QByteArray& data) const;
    void applySendEnabled(); // 修复 M3：综合"升级忙"与"特殊模式冷却"两个因素设置控件可用性

    SerialPort* m_port;

    QString m_commandPath; // 指令表 JSON 路径（loadCommands 时记录，编辑对话框用）

    QTreeWidget* m_tree = nullptr;
    QLabel* m_paramLabel = nullptr;
    QLineEdit* m_paramEdit = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QComboBox* m_connectCombo = nullptr; // R/C 指令连接位选择（:0/:1），默认 0
    QPlainTextEdit* m_logView = nullptr;
    QTimer* m_cooldownTimer = nullptr; // 修复 M3：特殊模式指令发送后的冷却定时器
};
