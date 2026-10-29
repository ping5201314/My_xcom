#pragma once

// 串口助手页：端口配置 + 接收显示（文本/HEX、时间戳、自动滚动、暂停、保存日志）
// + 发送（文本/HEX、行尾、循环发送）。
// 编码：Qt6 无 Core5Compat，GBK 走 ICU 的 QStringConverter::encodingForName("GBK")，
// 取不到时回退 UTF-8。接收拆包时的多字节字符残尾（前导字节）在页内暂存拼接。

#include <QWidget>

#include <QStringConverter>
#include <QByteArray>
#include <QColor>

#include <optional>

#include "config/appconfig.h" // SendHistory 值成员需要完整定义（升级页同款 include 风格）

class SerialPort;
class QComboBox;
class QPushButton;
class QPlainTextEdit;
class QCheckBox;
class QLabel;
class QSpinBox;
class QTimer;
class QSplitter;
struct SerialConfig;

class TerminalPage : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalPage(SerialPort* port, AppConfig* config, QWidget* parent = nullptr);

    /// 把端口参数/显示选项落盘（主窗口退出前调用）
    void saveSettings();

    /// 往右侧追加面板（调试指令面板：三栏布局 左配置/中接收/右指令，可拖宽，
    /// 不遮挡接收区——用户反馈右侧更顺手）
    void addRightPanel(QWidget* widget);

    /// 三栏的最小总宽（左右栏最小值 + 接收区工具栏最小宽 + 分隔条手柄），
    /// 主窗口用它设置拖动下限（用户要求：按接收区最小值定窗口最小值）
    int minimumNeededWidth() const;

    /// 把本页的收发写进共享监视文件（%TEMP%/my_com_trace.log，追加）：
    /// AI 能读到用户在 GUI 里的每一次发送与收到的数据（双向协作）
    void writeTrace(const QString& line);


public slots:
    /// 修复 C2：升级占用期间完全禁用终端页交互（端口配置行/开关串口/发送/循环发送等），
    /// 按钮显示"升级占用中"避免误导；正在运行的循环发送自动停止。
    /// 占用期间引擎为切换串口参数触发的 opened/closed 信号不会翻转本页 UI。
    void setUpgradeBusy(bool busy);

signals:
    void txBytesWritten(qint64 bytes); // 发送成功字节数，供主窗口状态栏计数

private slots:
    void onOpenClose();
    void onRefreshPorts();
    void onSend();
    void onLoopToggle();
    void onLoopTimeout();
    void onData(const QByteArray& data);
    void onError(const QString& msg);
    void onSaveLog();
    void onViewModeChanged(int idx);
    void onHistorySelected(int idx); // 历史下拉选中：回填发送框并复位到占位首项
    void onWatchToggle(bool on); // 监视 AI 会话：读共享跟踪文件，不占串口
    void onWatchTick();          // 500ms 轮询跟踪文件增量并上屏

private:
    void buildUi();
    void loadSettings();
    void rebuildDecoder();
    void rebuildHistoryCombo();  // 用 m_history 重建下拉（占位首项 + 历史条目）
    void updateHistoryEnabled(); // 升级占用/循环运行期间禁用历史下拉
    SerialConfig currentConfig() const;
    bool parseHexInput(const QString& text, QByteArray* out, QString* err) const;
    QByteArray sendLineEnding() const;
    void doSend(bool silent);
    void stopLoop();
    void updateOpenUi(bool open);
    void displayChunk(const QByteArray& chunk); // 接收数据进显示管线
    // 时间戳/滚动/配色统一处理；overrideColor 非空时强制该颜色（错误行用）
    void appendRxText(const QString& text, const QColor& overrideColor = {});
    QString decodeChunk(const QByteArray& chunk); // 编码解码 + 拆包残尾处理

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    SerialPort* m_port;
    AppConfig* m_config;
    QSplitter* m_splitter = nullptr; // 三栏分隔条（addRightPanel 追加右栏用）

    // ---- 端口配置行 ----
    QComboBox* m_portCombo;
    QComboBox* m_baudCombo;
    QComboBox* m_dataCombo;
    QComboBox* m_parityCombo;
    QComboBox* m_stopCombo;
    QComboBox* m_flowCombo;
    QPushButton* m_refreshBtn;
    QPushButton* m_openBtn;

    // ---- 接收区 ----
    QPlainTextEdit* m_rxView;
    QComboBox* m_viewCombo;     // 文本/HEX
    QComboBox* m_encodingCombo; // GBK/UTF-8
    QCheckBox* m_tsCheck;
    QCheckBox* m_scrollCheck;
    QCheckBox* m_pauseCheck;
    QCheckBox* m_watchCheck;  // 监视 AI 会话（读 %TEMP%/my_com_trace.log，不占串口）
    QCheckBox* m_showTxCheck; // 显示发送数据（接收区青色显示人发/AI 发的帧）
    QTimer* m_watchTimer = nullptr; // 监视轮询定时器
    qint64 m_watchPos = 0;    // 跟踪文件已读位置
    QTimer* m_relayTimer = nullptr; // AI 中转轮询：读 %TEMP%/my_com_ai_tx.txt 并代发
    void onRelayTick();       // 中转请求消费（AI 的指令经本 GUI 的串口发出）
    QString m_traceRxBuf;     // GUI-RX 共享记录的行聚合缓冲（完整行才写文件）
    bool m_watchInText = false; // 监视解析状态：正在 RX文本 内容块内（跨 tick 保持）
    QPushButton* m_clearRxBtn;
    QPushButton* m_saveBtn;

    // ---- 发送区 ----
    QComboBox* m_historyCombo; // 发送历史下拉（首项占位提示；选中只回填不发送）
    QPlainTextEdit* m_txEdit;
    QComboBox* m_txModeCombo; // 文本/HEX
    QComboBox* m_eolCombo;    // 无/CRLF/LF/CR
    QPushButton* m_sendBtn;
    QSpinBox* m_intervalSpin;
    QPushButton* m_loopBtn;
    QPushButton* m_clearTxBtn;
    QTimer* m_loopTimer;

    // ---- 状态 ----
    // 发送历史：手动发送成功才记录（循环发送不记），去重移前/上限 50，ini 持久化
    SendHistory m_history;
    bool m_upgradeBusy = false; // 修复 C2：升级占用期间忽略 closed/opened 信号、禁用全部交互
    QByteArray m_pauseBuf;  // 暂停显示期间攒的数据
    QByteArray m_hexBuf;    // HEX 显示凑 16 字节一行的残余
    char m_leadByte = 0;    // GBK/UTF-8 多字节字符被分包时的残尾前导字节
    QString m_encoding = QStringLiteral("GBK");
    // 打开状态下报错 → 随后被关闭 = 异常断开（closed 时红字提示；手动关闭会清掉）
    bool m_portFaultPending = false;
    // 流式显示状态：插入点是否在行首。数据按包分片到达，时间戳只在真实行首打一次，
    // 而不是每个包一行（appendPlainText 的每包一段正是"碎片切行+空行"的根因）
    bool m_lineStart = true;
    // 接收解码器（ICU 取 GBK，失败回退 UTF-8）。用 QStringDecoder 而非基类：
    // Qt 6.8 里 decode() 只在派生类上，且基类析构是 protected 不能放进 optional
    std::optional<QStringDecoder> m_decoder;
};
