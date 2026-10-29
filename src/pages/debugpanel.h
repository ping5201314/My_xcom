#pragma once

// 调试指令面板：加载固定位置 %APPDATA%\MyTeam\my_com\debug_commands.csv
// （2026-10-20 用户拍板存储位置，2026-10-21 拍板换 CSV 格式：Excel 双击可编辑，
// 6 列「分组,名称,发送内容,带参数,默认参数,参数提示」；老版 json 自动迁移为 csv，
// exe 旁种子 json 仅作迁移源与编辑窗「加载表格」导入素材），三列表格
// （分组/名称/发送内容）展示 @R/@W/@C 指令；选中后带参指令显示参数输入框，
// 双击行直接用默认参数发送。
// 布局（用户反馈改版，竖排紧凑）：连接位选择（:0/:1）置顶 → 指令表格占大头 →
// 参数行 → 底部小日志窗口（固定高度，仅作发送确认）。
// 指令表缺失或损坏：表格空白 + 居中占位提示（引导「编辑指令」新建或「加载表格」
// 导入），不弹窗不红字、不回退内置最小指令集。
// 自动迁移：固定位置缺失且 exe 旁存在指令表时复制过来一次（老版本用户命令无缝续用）。
// 右键菜单（2026-10-20 起,2026-10-22 扩展）：指令行 = 「编辑此指令…」「删除此指令」；
// 空白处 = 「添加命令…」；空表时占位页右键弹同款菜单（0 行时表格被占位页覆盖,
// 三轮审查 D1）。先菜单后对话框,直接退出菜单不弹窗;三个操作均立即保存到固定位置。
// 串口未打开时点发送只提示「串口未打开」，不崩溃；升级忙时由主窗口联动禁用发送。

#include <QColor>
#include <QVector>
#include <QWidget>

class SerialPort;
struct DebugCommand;
class QTableWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QComboBox;
class QStackedWidget;
class QTimer;
class QPoint;

class DebugPanel : public QWidget
{
    Q_OBJECT
public:
    // 常规构造：指令表从固定 %APPDATA%\MyTeam\my_com\debug_commands.csv 加载
    explicit DebugPanel(SerialPort* port, QWidget* parent = nullptr);
    // commandsPath 非空时作为指令表 JSON 路径（单元测试注入临时目录，隔离真实
    // %APPDATA%）；留空 = 固定 %APPDATA%\MyTeam\my_com\debug_commands.csv（注入 .json 路径时按 json 解析）
    DebugPanel(SerialPort* port, const QString& commandsPath, QWidget* parent = nullptr);

private slots:
    void onTableSelectionChanged();
    void onTableCellDoubleClicked(int row, int column);
    void onSendCurrent();
    void onEditCommands(); // 弹窗编辑指令表（添加/修改/删除），保存后重新加载
    void onTableContextMenu(const QPoint& pos); // 表格右键菜单（2026-10-20 起，10-22 扩展）
    void showAddCommandMenu(const QPoint& globalPos); // 「添加命令…」菜单（空白处/占位页共用）

private:
    void buildUi();
    // pathOverride 空 = 固定 %APPDATA% 位置；测试可注入临时路径
    void loadCommands(const QString& pathOverride = QString());
    void addGroup(const QString& group, const QVector<DebugCommand>& cmds); // 平铺追加表格行
    int currentCommandRow() const; // 当前选中且带指令数据的行号（无选中/无数据 = -1）
    void updatePlaceholder(); // 0 行显示占位提示页，否则显示表格页
    void editCommandAt(int row); // 右键「编辑此指令…」：弹单条编辑对话框，确定即更新并保存
    void deleteCommandAt(int row); // 右键「删除此指令」：确认后删行并保存
    void addCommandAtEnd();      // 右键空白「添加命令…」：弹单条对话框，确定即追加并保存
    void appendCommandRow(const QString& group, const DebugCommand& cmd); // 追加一行（加载/右键添加共用）
    bool saveTable();            // 从表格行重建模型写回 m_commandPath（单条修改后立即落盘）；返回是否写盘成功
    QString currentFrameText(); // 校验参数并替换 %1，返回最终帧文本（非法返回空并提示）
    void doSend(const QString& frame, const QString& name); // 打开检查 + 写入 + TX 日志
    // 时间戳 + 内容，3000 行上限，自动滚动；color 非空时该行用指定颜色（错误提示红字）
    void appendLog(const QString& line, const QColor& color = {});
    QString hexOf(const QByteArray& data) const;
    void applySendEnabled(); // 修复 M3：综合"升级忙"与"特殊模式冷却"两个因素设置控件可用性

    SerialPort* m_port;

    QString m_commandPath; // 指令表 JSON 路径（loadCommands 时记录，编辑对话框用）

    QStackedWidget* m_stack = nullptr; // 表格页 / 空白占位页（0 行时切换）
    QTableWidget* m_table = nullptr;
    QLabel* m_placeholder = nullptr;   // 空白占位提示（缺失/损坏时显示）
    QLabel* m_paramLabel = nullptr;
    QLineEdit* m_paramEdit = nullptr;
    QPushButton* m_sendBtn = nullptr;
    QComboBox* m_connectCombo = nullptr; // R/C 指令连接位选择（:0/:1），默认 0
    QPlainTextEdit* m_logView = nullptr;
    QTimer* m_cooldownTimer = nullptr; // 修复 M3：特殊模式指令发送后的冷却定时器
    bool m_menuOpen = false; // 右键菜单防重入（同一右键只允许一个菜单/对话框）
};
