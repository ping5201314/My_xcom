#include "pages/terminalpage.h"
#include "serial/serialport.h"
#include "serial/serialconfig.h"
#include "config/appconfig.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QEvent>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QTextCursor>
#include <QTime>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

namespace {
// 组合框按 data 值选中一项；找不到就退回第 0 项
void selectComboByData(QComboBox* combo, int data)
{
    const int idx = combo->findData(data);
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}
} // namespace

TerminalPage::TerminalPage(SerialPort* port, AppConfig* config, QWidget* parent)
    : QWidget(parent)
    , m_port(port)
    , m_config(config)
{
    buildUi();
    loadSettings();

    connect(m_port, &SerialPort::dataReceived, this, &TerminalPage::onData);
    connect(m_port, &SerialPort::errorOccurred, this, &TerminalPage::onError);
    // 串口被关闭（含拔线后自动关闭）时复位界面状态。
    // 修复 C2：升级占用期间引擎为切换串口参数会 close/open（ensurePortAt115200 内部
    // 先关再开），此时 closed 不应把 UI 置成"已关闭"并放开控件——否则用户按 UI 指引
    // 点"打开串口"就会以终端页参数重开端口，打乱正在进行的传输（违反 Q7/D15）。
    connect(m_port, &SerialPort::closed, this, [this] {
        if (m_upgradeBusy)
            return; // 占用期间的开关由 setUpgradeBusy 统一管理，结束时按真实状态恢复
        stopLoop();
        updateOpenUi(false);
        // 异常断开红字提示（用户反馈）：打开状态下报过错、随后被关闭 = 拔线/设备复位，
        // 与用户手动关闭区分（手动关闭在 onOpenClose 里清了标志）
        if (m_portFaultPending) {
            m_portFaultPending = false;
            appendRxText(QStringLiteral("[串口已断开] 串口异常断开，请检查连接后重新打开\n"),
                         QColor(QStringLiteral("#C0392B")));
        }
    });
}

void TerminalPage::setUpgradeBusy(bool busy)
{
    // 修复 C2：升级占用期间终端页完全禁用（需求 Q7、05c A9）——发送/循环发送/
    // 开关串口/端口参数全部不可交互，UI 明确显示"升级占用中"避免误导。
    if (m_upgradeBusy == busy)
        return;
    m_upgradeBusy = busy;
    if (busy) {
        // 先置占用标志再停循环：stopLoop 内部的 updateOpenUi 会按占用状态收敛控件
        stopLoop(); // 升级开始时若循环发送在跑，必须立即停（定时器照发会穿插字节破坏传输）
        m_openBtn->setToolTip(QStringLiteral("固件升级占用中，升级结束后自动恢复"));
    } else {
        m_openBtn->setToolTip(QString());
    }
    // 发送区控件随占用状态整体禁用/恢复
    m_txEdit->setEnabled(!busy);
    m_txModeCombo->setEnabled(!busy);
    m_eolCombo->setEnabled(!busy);
    m_sendBtn->setEnabled(!busy);
    m_intervalSpin->setEnabled(!busy);
    m_loopBtn->setEnabled(!busy);
    m_clearTxBtn->setEnabled(!busy);
    updateHistoryEnabled(); // 历史下拉随发送区一起禁用/恢复
    // 配置行 + 开关按钮：占用期间全禁用；结束时按真实串口状态恢复
    updateOpenUi(busy ? false : m_port->isOpen());
}

void TerminalPage::buildUi()
{
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // 页面根布局：必须套一层 QVBoxLayout——分隔条作为裸子控件挂在页面上时
    // 没有布局管理，窗口变大它不会跟着拉伸，右侧/下方留出大片空白（用户反馈
    // 的空白区根因）。布局把分隔条撑满整个页面。
    auto* pageLay = new QVBoxLayout(this);
    // 边缘不留白（用户要求）：三栏贴满窗口四边；区域之间的间隔由分隔条手柄
    // 和分组框内边距提供
    pageLay->setContentsMargins(0, 0, 0, 0);
    // 三栏分隔条：左栏(配置+发送) | 接收区(居中占剩余) | 右栏(调试指令，addRightPanel 追加)
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);
    pageLay->addWidget(m_splitter);

    // ---- 左栏: 端口配置 + 发送设置（与右栏同宽，对称；宽度由分隔条统一管理）----
    auto* sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setAttribute(Qt::WA_StyledBackground, true); // 纯 QWidget 的 QSS 背景需要此标志
    sidebar->setMinimumWidth(300);
    auto* sideLay = new QVBoxLayout(sidebar);
    sideLay->setContentsMargins(10, 10, 10, 10);
    sideLay->setSpacing(10);

    auto* cfgBox = new QGroupBox(QStringLiteral("端口配置"), sidebar);
    auto* cfgForm = new QFormLayout(cfgBox);
    cfgForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    cfgForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_portCombo = new QComboBox(cfgBox);
    m_portCombo->setToolTip(QStringLiteral("串口端口"));
    m_refreshBtn = new QPushButton(QStringLiteral("刷新"), cfgBox);
    m_refreshBtn->setToolTip(QStringLiteral("重新枚举可用串口"));
    auto* portRow = new QHBoxLayout;
    portRow->setSpacing(6);
    portRow->addWidget(m_portCombo, 1);
    portRow->addWidget(m_refreshBtn);
    cfgForm->addRow(QStringLiteral("端口"), portRow);
    m_baudCombo = new QComboBox(cfgBox);
    for (int b : standardBaudRates())
        m_baudCombo->addItem(QString::number(b), b);
    cfgForm->addRow(QStringLiteral("波特率"), m_baudCombo);
    m_dataCombo = new QComboBox(cfgBox);
    for (int d : { Data5, Data6, Data7, Data8 })
        m_dataCombo->addItem(QString::number(d), d);
    cfgForm->addRow(QStringLiteral("数据位"), m_dataCombo);
    m_parityCombo = new QComboBox(cfgBox);
    m_parityCombo->addItem(QStringLiteral("无"), ParityNone);
    m_parityCombo->addItem(QStringLiteral("奇校验"), ParityOdd);
    m_parityCombo->addItem(QStringLiteral("偶校验"), ParityEven);
    cfgForm->addRow(QStringLiteral("校验"), m_parityCombo);
    m_stopCombo = new QComboBox(cfgBox);
    m_stopCombo->addItem(QStringLiteral("1"), StopOne);
    m_stopCombo->addItem(QStringLiteral("2"), StopTwo);
    cfgForm->addRow(QStringLiteral("停止位"), m_stopCombo);
    m_flowCombo = new QComboBox(cfgBox);
    m_flowCombo->addItem(QStringLiteral("无"), FlowNone); // 流控固定“无”：OTA 文档 A9
    cfgForm->addRow(QStringLiteral("流控"), m_flowCombo);
    m_openBtn = new QPushButton(QStringLiteral("打开串口"), cfgBox);
    m_openBtn->setProperty("primary", true);
    cfgForm->addRow(m_openBtn);
    sideLay->addWidget(cfgBox);

    auto* txBox = new QGroupBox(QStringLiteral("发送设置"), sidebar);
    auto* txLay = new QVBoxLayout(txBox);
    auto* txRow1 = new QHBoxLayout;
    m_txModeCombo = new QComboBox(txBox);
    m_txModeCombo->addItem(QStringLiteral("文本"));
    m_txModeCombo->addItem(QStringLiteral("HEX"));
    m_txModeCombo->setToolTip(QStringLiteral("HEX 模式按 “01 02 0A” 解析（空格/0x 前缀可省）"));
    m_eolCombo = new QComboBox(txBox);
    m_eolCombo->addItem(QStringLiteral("无"));
    m_eolCombo->addItem(QStringLiteral("CRLF"));
    m_eolCombo->addItem(QStringLiteral("LF"));
    m_eolCombo->addItem(QStringLiteral("CR"));
    m_eolCombo->setToolTip(QStringLiteral("发送内容末尾追加的行尾（HEX 模式同样追加）"));
    txRow1->addWidget(new QLabel(QStringLiteral("输入"), txBox));
    txRow1->addWidget(m_txModeCombo, 1);
    txRow1->addWidget(new QLabel(QStringLiteral("行尾"), txBox));
    txRow1->addWidget(m_eolCombo, 1);
    txLay->addLayout(txRow1);
    m_txEdit = new QPlainTextEdit(txBox);
    m_txEdit->setFont(fixed);
    m_txEdit->setToolTip(QStringLiteral("回车=发送，Ctrl+回车=换行"));
    // 发送框随组伸展填满左栏（用户反馈：下面不要留白，界面饱和一点）
    txLay->addWidget(m_txEdit, 1);
    m_sendBtn = new QPushButton(QStringLiteral("发送"), txBox);
    m_sendBtn->setProperty("primary", true);
    m_loopBtn = new QPushButton(QStringLiteral("循环发送"), txBox);
    m_clearTxBtn = new QPushButton(QStringLiteral("清空发送框"), txBox);
    auto* txRow2 = new QHBoxLayout;
    txRow2->addWidget(m_sendBtn, 1);
    txRow2->addWidget(m_loopBtn, 1);
    txRow2->addWidget(m_clearTxBtn, 1);
    txLay->addLayout(txRow2);
    m_intervalSpin = new QSpinBox(txBox);
    m_intervalSpin->setRange(10, 100000);
    m_intervalSpin->setSingleStep(10);
    m_intervalSpin->setSuffix(QStringLiteral(" ms"));
    m_intervalSpin->setToolTip(QStringLiteral("循环发送间隔"));
    auto* txRow3 = new QHBoxLayout;
    txRow3->addWidget(new QLabel(QStringLiteral("循环间隔"), txBox));
    txRow3->addWidget(m_intervalSpin, 1);
    txLay->addLayout(txRow3);
    // 发送历史下拉（2026-10-20 拍板，10-21 用户调整位置：放到发送区最后面——
    // 发送框之后）：手动发送成功后自动记录（最新在前、最多 50 条，循环发送不记录），
    // 选中只回填、不自动发送
    auto* histRow = new QHBoxLayout;
    histRow->addWidget(new QLabel(QStringLiteral("历史"), txBox));
    m_historyCombo = new QComboBox(txBox);
    m_historyCombo->addItem(QStringLiteral("— 选择历史填入发送框 —")); // 占位首项
    m_historyCombo->setToolTip(QStringLiteral("发送历史：手动发送成功后自动记录"
                                              "（最新在前，最多 50 条，循环发送不记录）。"
                                              "选中填入发送框，不自动发送；HEX 内容需在 HEX 输入模式下发送才保持原义"));
    histRow->addWidget(m_historyCombo, 1);
    txLay->addLayout(histRow);
    sideLay->addWidget(txBox, 1);

    m_splitter->addWidget(sidebar);

    // ---- 中栏: 接收区 (占剩余宽度) ----
    auto* rxBox = new QGroupBox(QStringLiteral("接收区"), this);
    auto* rxLay = new QVBoxLayout(rxBox);
    // 单行工具栏（用户要求定稿）：显示/编码/勾选框/清空/保存日志 全部在左，
    // RX 计数贴右。配合下方 m_rxView->setMinimumWidth 保护：拖动分隔条时
    // 中间不会被压到控件溢出错位（压不动会转挤两侧栏）
    auto* rxTool = new QHBoxLayout;
    m_viewCombo = new QComboBox(rxBox);
    m_viewCombo->addItem(QStringLiteral("文本"));
    m_viewCombo->addItem(QStringLiteral("HEX"));
    m_viewCombo->setToolTip(QStringLiteral("接收显示模式（文本/HEX）"));
    m_encodingCombo = new QComboBox(rxBox);
    m_encodingCombo->addItem(QStringLiteral("GBK"));
    m_encodingCombo->addItem(QStringLiteral("UTF-8"));
    m_encodingCombo->setToolTip(
        QStringLiteral("文本模式编解码（发送与接收共用）。GBK 走 ICU，无内置 GBK 时回退 UTF-8"));
    m_tsCheck = new QCheckBox(QStringLiteral("时间戳"), rxBox);
    m_tsCheck->setToolTip(QStringLiteral("每行前缀 [HH:mm:ss.mmm]"));
    m_scrollCheck = new QCheckBox(QStringLiteral("自动滚动"), rxBox);
    m_pauseCheck = new QCheckBox(QStringLiteral("暂停显示"), rxBox);
    m_pauseCheck->setToolTip(QStringLiteral("暂停时数据仍接收并计数，恢复后一次性补显"));
    m_showTxCheck = new QCheckBox(QStringLiteral("显示发送"), rxBox);
    m_showTxCheck->setToolTip(QStringLiteral("接收区用青色显示发送的帧（人在界面发与 AI 中转发）；关闭只显示接收"));
    m_watchCheck = new QCheckBox(QStringLiteral("监视AI会话"), rxBox);
    m_watchCheck->setToolTip(
        QStringLiteral("AI 通过命令行控制串口时，这里实时显示它的收发（不占用串口）"));
    connect(m_watchCheck, &QCheckBox::toggled, this, &TerminalPage::onWatchToggle);
    m_watchTimer = new QTimer(this);
    m_watchTimer->setInterval(500);
    connect(m_watchTimer, &QTimer::timeout, this, &TerminalPage::onWatchTick);
    // AI 中转轮询：串口打开期间一直跑，消费 AI 的转发请求（COM 口独占下的协同调试）
    m_relayTimer = new QTimer(this);
    m_relayTimer->setInterval(500);
    connect(m_relayTimer, &QTimer::timeout, this, &TerminalPage::onRelayTick);
    m_clearRxBtn = new QPushButton(QStringLiteral("清空"), rxBox);
    // 用户反馈：保存日志功能要保留——按钮文字保持「保存日志」明确可见
    m_saveBtn = new QPushButton(QStringLiteral("保存日志"), rxBox);
    m_saveBtn->setToolTip(QStringLiteral("保存接收日志到文件"));

    rxTool->addWidget(m_viewCombo);
    rxTool->addWidget(m_encodingCombo);
    rxTool->addWidget(m_tsCheck);
    rxTool->addWidget(m_scrollCheck);
    rxTool->addWidget(m_pauseCheck);
    rxTool->addWidget(m_showTxCheck);
    rxTool->addWidget(m_watchCheck);
    rxTool->addWidget(m_clearRxBtn);
    rxTool->addWidget(m_saveBtn);
    rxTool->addStretch(1);

    rxLay->addLayout(rxTool);

    m_rxView = new QPlainTextEdit(rxBox);
    m_rxView->setReadOnly(true);
    m_rxView->setFont(fixed);
    m_rxView->setLineWrapMode(QPlainTextEdit::NoWrap); // 串口原始数据不折行，HEX 行不被拦断
    m_rxView->setMaximumBlockCount(5000); // 行数上限：防长时间挂机内存膨胀
    rxLay->addWidget(m_rxView);

    m_splitter->addWidget(rxBox);

    // ---- 信号 ----
    m_loopTimer = new QTimer(this);
    connect(m_refreshBtn, &QPushButton::clicked, this, &TerminalPage::onRefreshPorts);
    connect(m_openBtn, &QPushButton::clicked, this, &TerminalPage::onOpenClose);
    connect(m_sendBtn, &QPushButton::clicked, this, &TerminalPage::onSend);
    connect(m_loopBtn, &QPushButton::clicked, this, &TerminalPage::onLoopToggle);
    connect(m_loopTimer, &QTimer::timeout, this, &TerminalPage::onLoopTimeout);
    connect(m_clearRxBtn, &QPushButton::clicked, this, [this] {
        m_rxView->clear();
        m_lineStart = true; // 清空后插入点回到行首
    });
    connect(m_clearTxBtn, &QPushButton::clicked, this, [this] { m_txEdit->clear(); });
    // 用 activated 而非 currentIndexChanged：只在用户选中时触发，回填后程序复位到
    // 占位首项不会误触——同一历史项可重复选择
    connect(m_historyCombo, &QComboBox::activated, this, &TerminalPage::onHistorySelected);
    connect(m_saveBtn, &QPushButton::clicked, this, &TerminalPage::onSaveLog);
    connect(m_viewCombo, &QComboBox::currentIndexChanged, this, &TerminalPage::onViewModeChanged);
    connect(m_encodingCombo, &QComboBox::currentTextChanged, this, [this](const QString&) {
        rebuildDecoder();
        saveSettings();
    });
    connect(m_pauseCheck, &QCheckBox::toggled, this, [this](bool paused) {
        if (!paused && !m_pauseBuf.isEmpty()) {
            // 恢复显示：把暂停期间攒的数据一次性补显
            QByteArray buf;
            buf.swap(m_pauseBuf);
            displayChunk(buf);
        }
    });
    // 选项变更即落盘（写入量极小）
    connect(m_tsCheck, &QCheckBox::toggled, this, [this](bool) { saveSettings(); });
    connect(m_scrollCheck, &QCheckBox::toggled, this, [this](bool) { saveSettings(); });
    connect(m_showTxCheck, &QCheckBox::toggled, this, [this](bool) { saveSettings(); });
    connect(m_txModeCombo, &QComboBox::currentIndexChanged, this, [this](int) { saveSettings(); });
    connect(m_eolCombo, &QComboBox::currentIndexChanged, this, [this](int) { saveSettings(); });
    connect(m_intervalSpin, &QSpinBox::valueChanged, this, [this](int) { saveSettings(); });
    m_txEdit->installEventFilter(this);
}

void TerminalPage::loadSettings()
{
    // 先填端口列表，再恢复上次的选择
    onRefreshPorts();
    const QString lastPort = m_config->stringValue(AppConfig::kLastPort);
    if (!lastPort.isEmpty()) {
        for (int i = 0; i < m_portCombo->count(); ++i) {
            if (m_portCombo->itemText(i).section(QLatin1Char(' '), 0, 0) == lastPort) {
                m_portCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    selectComboByData(m_baudCombo, m_config->intValue(AppConfig::kLastBaud, B115200));
    selectComboByData(m_dataCombo, m_config->intValue(AppConfig::kLastDataBits, Data8));
    selectComboByData(m_parityCombo, m_config->intValue(AppConfig::kLastParity, ParityNone));
    selectComboByData(m_stopCombo, m_config->intValue(AppConfig::kLastStopBits, StopOne));
    selectComboByData(m_flowCombo, m_config->intValue(AppConfig::kLastFlow, FlowNone));
    m_viewCombo->setCurrentIndex(m_config->boolValue(AppConfig::kHexView, false) ? 1 : 0);
    m_encodingCombo->setCurrentText(
        m_config->stringValue(AppConfig::kEncoding, QStringLiteral("GBK")));
    rebuildDecoder();
    m_tsCheck->setChecked(m_config->boolValue(AppConfig::kTimestamp, false));
    m_scrollCheck->setChecked(m_config->boolValue(AppConfig::kAutoScroll, true));
    m_showTxCheck->setChecked(m_config->boolValue(AppConfig::kShowTx, true));
    m_txModeCombo->setCurrentIndex(m_config->boolValue(AppConfig::kSendHex, false) ? 1 : 0);
    m_eolCombo->setCurrentIndex(m_config->intValue(AppConfig::kLineEnding, 1)); // 默认 CRLF
    m_intervalSpin->setValue(m_config->intValue(AppConfig::kLoopInterval, 1000));
    // 发送历史：启动时从 ini 载入（键 ui/sendHistory，最新在前）
    m_history.load(*m_config);
    rebuildHistoryCombo();
}

void TerminalPage::saveSettings()
{
    const SerialConfig cfg = currentConfig();
    m_config->setValue(AppConfig::kLastPort, cfg.portName);
    m_config->setValue(AppConfig::kLastBaud, cfg.baud);
    m_config->setValue(AppConfig::kLastDataBits, cfg.dataBits);
    m_config->setValue(AppConfig::kLastParity, cfg.parity);
    m_config->setValue(AppConfig::kLastStopBits, cfg.stopBits);
    m_config->setValue(AppConfig::kLastFlow, cfg.flowControl);
    m_config->setValue(AppConfig::kHexView, m_viewCombo->currentIndex() == 1);
    m_config->setValue(AppConfig::kTimestamp, m_tsCheck->isChecked());
    m_config->setValue(AppConfig::kAutoScroll, m_scrollCheck->isChecked());
    m_config->setValue(AppConfig::kShowTx, m_showTxCheck->isChecked());
    m_config->setValue(AppConfig::kEncoding, m_encodingCombo->currentText());
    m_config->setValue(AppConfig::kSendHex, m_txModeCombo->currentIndex() == 1);
    m_config->setValue(AppConfig::kLineEnding, m_eolCombo->currentIndex());
    m_config->setValue(AppConfig::kLoopInterval, m_intervalSpin->value());
}

void TerminalPage::rebuildDecoder()
{
    m_encoding = m_encodingCombo->currentText();
    m_leadByte = 0; // 编码切换后，旧编码的残尾半字节已无意义
    // Qt6 无 Core5Compat：GBK 走 ICU 名称查询，取不到（无 ICU 的裁剪版 Qt）回退 UTF-8
    QStringDecoder dec(m_encoding.toLatin1().constData());
    if (!dec.isValid())
        dec = QStringDecoder(QStringDecoder::Utf8);
    m_decoder = std::move(dec);
}

SerialConfig TerminalPage::currentConfig() const
{
    SerialConfig cfg;
    // “COM3 (USB-SERIAL CH340)” → 取空格前字段作为端口名
    cfg.portName = m_portCombo->currentText().section(QLatin1Char(' '), 0, 0);
    cfg.baud = m_baudCombo->currentData().toInt();
    cfg.dataBits = m_dataCombo->currentData().toInt();
    cfg.parity = m_parityCombo->currentData().toInt();
    cfg.stopBits = m_stopCombo->currentData().toInt();
    cfg.flowControl = m_flowCombo->currentData().toInt();
    return cfg;
}

bool TerminalPage::parseHexInput(const QString& text, QByteArray* out, QString* err) const
{
    // 允许 "01 02 0A"、"01020A"、"0x01 0x02" 混用；非法字节报错且不发送
    QString t = text.simplified();
    t.remove(QLatin1Char(' '));
    t.remove(QStringLiteral("0x"));
    t.remove(QStringLiteral("0X"));
    if (t.size() % 2 != 0) {
        *err = QStringLiteral("HEX 输入必须是成对的十六进制字节（如 “01 02 0A”），当前为 %1 个字符。")
                   .arg(t.size());
        return false;
    }
    out->clear();
    out->reserve(t.size() / 2);
    for (int i = 0; i < t.size(); i += 2) {
        bool ok = false;
        const int v = t.mid(i, 2).toInt(&ok, 16);
        if (!ok) {
            *err = QStringLiteral("“%1” 不是合法的十六进制字节。").arg(t.mid(i, 2));
            return false;
        }
        out->append(static_cast<char>(v));
    }
    return true;
}

QByteArray TerminalPage::sendLineEnding() const
{
    switch (m_eolCombo->currentIndex()) {
    case 1: return QByteArray("\r\n", 2);
    case 2: return QByteArray("\n", 1);
    case 3: return QByteArray("\r", 1);
    default: return {};
    }
}

int TerminalPage::minimumNeededWidth() const
{
    // 分隔条的 minimumSizeHint = 各栏 minimumSizeHint 之和 + 手柄宽，
    // 即"三栏不挤压错位所需的最小宽度"——窗口拖动下限直接取它（用户要求）
    return m_splitter ? m_splitter->minimumSizeHint().width() : 1150;
}

void TerminalPage::writeTrace(const QString& line)
{
    // 共享监视文件（与 CLI 同一路径）：用户 GUI 的收发也记录进去，
    // AI 读这个文件就能知道人在界面上发了什么、设备回了什么
    const QString path = QDir::tempPath() + QStringLiteral("/my_com_trace.log");
    QFile f(path);
    const bool tooBig = QFileInfo::exists(path) && QFileInfo(path).size() > 512 * 1024;
    if (!f.open(QIODevice::Append | (tooBig ? QIODevice::Truncate : QIODevice::Append)))
        return;
    f.write(QStringLiteral("[%1] %2\n")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), line)
                .toUtf8());
}

void TerminalPage::onRelayTick()
{
    // 升级中转会话期间提速轮询（CLI 逐帧等待消费，500ms 会让整轮超预算），
    // 平时恢复 500ms 常态
    const QString flagPath = QDir::tempPath() + QStringLiteral("/my_com_ai_relay.flag");
    const int wantMs = QFile::exists(flagPath) ? 30 : 500;
    if (m_relayTimer->interval() != wantMs)
        m_relayTimer->setInterval(wantMs);

    // ① 新协议（升级中转向）：.part 全量数据 + .rdy 就绪标记，CLI 写完 .part 才落 .rdy，
    //    本页只在 .rdy 存在时读 .part，杜绝半包；读后代发并删除两文件
    const QString partPath = QDir::tempPath() + QStringLiteral("/my_com_ai_tx.part");
    const QString rdyPath = QDir::tempPath() + QStringLiteral("/my_com_ai_tx.rdy");
    if (QFile::exists(rdyPath)) {
        QByteArray frame;
        QFile part(partPath);
        if (part.open(QIODevice::ReadOnly)) {
            frame = part.readAll();
            part.close();
        }
        QFile::remove(rdyPath);
        QFile::remove(partPath);
        if (frame.isEmpty() || !m_port->isOpen())
            return;
        writeTrace(QStringLiteral("AI-RELAY-TX: %1 字节").arg(frame.size()));
        m_port->write(frame);
        return;
    }

    // ② 旧协议（relay-send 兼容）：AI 的 CLI 把文本帧写进 %TEMP%/my_com_ai_tx.txt，
    //    本页用自己已打开的串口代发——COM 口独占下实现"AI 与人在同一串口协同调试"，
    //    设备回复走本页正常接收路径（GUI-RX 进共享记录 + 接收区显示）
    const QString path = QDir::tempPath() + QStringLiteral("/my_com_ai_tx.txt");
    QFile f(path);
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray frame = f.readAll().trimmed();
    f.close();
    QFile::remove(path); // 先删后发：消费语义，避免重复发送
    if (frame.isEmpty() || !m_port->isOpen())
        return;
    writeTrace(QStringLiteral("AI-RELAY-TX: %1").arg(QString::fromUtf8(frame)));
    m_port->write(frame);
}

void TerminalPage::onWatchToggle(bool on)
{
    if (on) {
        // 监视 AI 会话：AI 的 CLI 把收发追加写进 %TEMP%/my_com_trace.log，
        // 这里每 500ms 读增量上屏——本页不打开串口，与 AI 同时使用互不冲突
        QFile f(QDir::tempPath() + QStringLiteral("/my_com_trace.log"));
        m_watchPos = (f.open(QIODevice::ReadOnly)) ? f.size() : 0;
        appendRxText(QStringLiteral("[AI监视] 已开启：AI 会话的收发将实时显示（本页不占用串口）\n"),
                     QColor(QStringLiteral("#7B68EE")));
        m_watchTimer->start();
    } else {
        m_watchTimer->stop();
        appendRxText(QStringLiteral("[AI监视] 已关闭\n"),
                     QColor(QStringLiteral("#7B68EE")));
    }
}

void TerminalPage::onWatchTick()
{
    QFile f(QDir::tempPath() + QStringLiteral("/my_com_trace.log"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    if (f.size() < m_watchPos) {
        m_watchPos = f.size(); // 文件被截断重开：跳到末尾
        return;
    }
    if (!f.seek(m_watchPos))
        return;
    const QByteArray added = f.read(f.size() - m_watchPos);
    m_watchPos = f.pos();
    if (added.isEmpty())
        return;
    // 按行上屏（用户要求：别每行都带 [AI]，用颜色区分）：
    //   - AI 自己的动作行（AI-RELAY-TX/CLI-*）→ 带 [AI] 前缀
    //   - 设备回复内容行（GUI-RX）→ 无前缀，紫色
    //   - 人在界面发的行（GUI-TX）→ 无前缀，青色
    //   - 会话标记 → 灰色 [AI]
    //   - 其余（历史碎片/杂行）→ 白名单外，直接丢弃
    // 时间戳跟随「时间戳」勾选框：剥掉共享记录自带的时间戳，
    // appendRxText 会在勾选时统一加 [HH:mm:ss.mmm]，没勾就不显示
    static const QRegularExpression tsRe(
        QStringLiteral("^\\[\\d{2}:\\d{2}:\\d{2}\\.\\d{3}\\]\\s*"));
    const QColor kAiPurple(QStringLiteral("#7B68EE"));
    const QColor kTxTeal(QStringLiteral("#1F7A8C"));
    const QColor kAiGray(QStringLiteral("#9A9A9A"));
    const QStringList lines = QString::fromUtf8(added).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        QString clean = line.trimmed();
        if (clean.isEmpty())
            continue;
        clean.remove(tsRe); // remove(QRegularExpression) 非 const，用副本
        if (clean.startsWith(QStringLiteral("GUI-RX:"))) {
            m_watchInText = false;
            const QString content = clean.mid(7).trimmed();
            if (!content.isEmpty())
                appendRxText(content + QLatin1Char('\n'), kAiPurple);
        } else if (clean.startsWith(QStringLiteral("GUI-TX:"))) {
            m_watchInText = false;
            if (m_showTxCheck->isChecked()) {
                const QString content = clean.mid(7).trimmed();
                if (!content.isEmpty())
                    appendRxText(content + QLatin1Char('\n'), kTxTeal);
            }
        } else if (clean.startsWith(QStringLiteral("AI-RELAY-TX:"))) {
            m_watchInText = false;
            if (m_showTxCheck->isChecked())
                appendRxText(QStringLiteral("[AI] %1\n").arg(clean), kAiPurple);
        } else if (clean.startsWith(QStringLiteral("=== AI 会话"))) {
            m_watchInText = false;
            appendRxText(QStringLiteral("[AI] %1\n").arg(clean), kAiGray);
        } else if (clean.startsWith(QStringLiteral("CLI-ERROR"))) {
            m_watchInText = false; // 错误信息不进监视显示（用户要求）——AI 自己读 cli.log
        } else if (clean.startsWith(QStringLiteral("CLI-"))) {
            m_watchInText = false;
            appendRxText(QStringLiteral("[AI] %1\n").arg(clean), kAiGray);
        } else if (clean.startsWith(QStringLiteral("TX ("))) {
            // AI 发送行："TX (6/6 字节): @R20:0" → "[AI] TX: @R20:0"
            m_watchInText = false;
            if (m_showTxCheck->isChecked()) {
                const int p = clean.indexOf(QStringLiteral("): "));
                appendRxText(QStringLiteral("[AI] TX: %1\n")
                                 .arg(p >= 0 ? clean.mid(p + 3) : clean),
                             kAiPurple);
            }
        } else if (clean.startsWith(QStringLiteral("RX文本:"))) {
            m_watchInText = true; // 进入内容块：后续无前缀行都是设备回复文本
        } else if (clean.startsWith(QStringLiteral("RX ("))
                   || clean.startsWith(QStringLiteral("TX-HEX"))) {
            m_watchInText = false; // HEX 噪音行丢弃
        } else if (m_watchInText) {
            // RX文本 内容块内的行：无前缀、紫色（如 FMC PID / Project name 等）
            appendRxText(clean + QLatin1Char('\n'), kAiPurple);
        }
        // 其余（历史碎片/杂行）不显示，保持监视窗口干净
    }
}

void TerminalPage::addRightPanel(QWidget* widget)
{
    // 追加为右侧第三栏（用户反馈：调试指令放右边更顺手）——分隔条可拖宽，
    // 接收区居中始终完整可见，边发指令边看回显
    if (!m_splitter)
        return;
    widget->setMinimumWidth(300);
    m_splitter->addWidget(widget);
    m_splitter->setStretchFactor(0, 0); // 左栏保持自身宽度
    m_splitter->setStretchFactor(1, 1); // 接收区占剩余
    m_splitter->setStretchFactor(2, 0); // 右栏保持自身宽度
    // 左右两栏初始同宽 290（对称）；接收区居中占大头（用户反馈：接收区太窄，
    // 两侧收到 290 给中间多让出 80px）
    m_splitter->setSizes({290, 560, 290});
}

void TerminalPage::onOpenClose()
{
    if (m_port->isOpen()) {
        m_portFaultPending = false; // 手动关闭不算异常断开，closed 处理不再红字提示
        writeTrace(QStringLiteral("GUI-CLOSED"));
        m_port->close();
        return; // UI 复位由 closed 信号统一处理
    }
    m_portFaultPending = false; // 重新打开前清掉旧错误标记
    const SerialConfig cfg = currentConfig();
    if (cfg.portName.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法打开"),
                             QStringLiteral("未选择串口，请先点击“刷新”枚举可用端口。"));
        return;
    }
    if (m_port->open(cfg)) {
        // 共享记录：让 AI 知道 GUI 当前占用的串口（协同调试时端口变化也能感知）
        writeTrace(QStringLiteral("GUI-OPEN: %1 @%2").arg(cfg.portName).arg(cfg.baud));
        // 记住本次成功参数，下次启动自动填入
        m_config->setValue(AppConfig::kLastPort, cfg.portName);
        m_config->setValue(AppConfig::kLastBaud, cfg.baud);
        m_config->setValue(AppConfig::kLastDataBits, cfg.dataBits);
        m_config->setValue(AppConfig::kLastParity, cfg.parity);
        m_config->setValue(AppConfig::kLastStopBits, cfg.stopBits);
        m_config->setValue(AppConfig::kLastFlow, cfg.flowControl);
        updateOpenUi(true);
    }
    // 打开失败的细节由 errorOccurred 信号统一弹窗
}

void TerminalPage::onRefreshPorts()
{
    const QString old = m_portCombo->currentText();
    const QString oldPort = old.section(QLatin1Char(' '), 0, 0);
    m_portCombo->clear();
    m_portCombo->addItems(SerialPort::availablePorts());
    if (m_portCombo->count() == 0)
        return;
    // 尽量保持原选择；原选择不在时回到上次成功打开的端口；再不行取第 0 项
    int idx = -1;
    for (int i = 0; i < m_portCombo->count(); ++i) {
        if (m_portCombo->itemText(i).section(QLatin1Char(' '), 0, 0) == oldPort) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        const QString last = m_config->stringValue(AppConfig::kLastPort);
        for (int i = 0; i < m_portCombo->count(); ++i) {
            if (m_portCombo->itemText(i).section(QLatin1Char(' '), 0, 0) == last) {
                idx = i;
                break;
            }
        }
    }
    m_portCombo->setCurrentIndex(idx >= 0 ? idx : 0);
}

void TerminalPage::onSend()
{
    doSend(false);
}

void TerminalPage::doSend(bool silent)
{
    // 修复 C2 防御：占用期间即使有事件漏进（如回车直发）也直接忽略
    if (m_upgradeBusy)
        return;
    if (!m_port->isOpen()) {
        // 不弹窗（用户反馈）：红字内联进接收区提示
        if (!silent)
            appendRxText(QStringLiteral("[发送失败] 串口未打开，请先打开串口\n"),
                         QColor(QStringLiteral("#C0392B")));
        stopLoop(); // 循环发送中拔线/关串口：停循环，避免反复报错
        return;
    }
    QByteArray payload;
    if (m_txModeCombo->currentIndex() == 1) {
        // HEX 模式：非法输入给出红字提示且不发送
        QString err;
        if (!parseHexInput(m_txEdit->toPlainText(), &payload, &err)) {
            if (!silent)
                appendRxText(QStringLiteral("[发送失败] %1\n").arg(err),
                             QColor(QStringLiteral("#C0392B")));
            stopLoop();
            return;
        }
    } else {
        // 文本模式：与接收显示共用编码（GBK/UTF-8）
        QStringEncoder enc(m_encoding.toLatin1().constData());
        if (!enc.isValid())
            enc = QStringEncoder(QStringEncoder::Utf8);
        payload = enc.encode(m_txEdit->toPlainText());
    }
    payload.append(sendLineEnding());
    if (payload.isEmpty())
        return; // 空输入且未选行尾：不动作
    const qint64 n = m_port->write(payload);
    if (n <= 0)
        return; // 失败细节由 errorOccurred 上报
    if (!silent) {
        // 手动发送成功才记历史（2026-10-20 拍板）：循环发送（silent）逐次都不记；
        // 失败路径（未开串口/HEX 非法/写失败/空输入）在上面已 return，走不到这里。
        // 存发送框原文——HEX 模式存 HEX 文本本身，回填不切换输入模式
        m_history.add(m_txEdit->toPlainText());
        m_history.save(*m_config); // 即时落盘：关闭软件后保留
        rebuildHistoryCombo();
    }
    // 共享记录：AI 能读到用户在 GUI 里发送的内容（去掉行尾控制字符）
    {
        QByteArray body = payload;
        while (body.endsWith('\r') || body.endsWith('\n'))
            body.chop(1);
        writeTrace(QStringLiteral("GUI-TX: %1").arg(QString::fromUtf8(body)));
        if (m_showTxCheck->isChecked()) {
            // 跟随接收区的 文本/HEX 显示模式(用户要求:发送显示与接收视图同步)
            if (m_viewCombo->currentIndex() == 1) {
                appendRxText(QString::fromLatin1(payload.toHex(' ').toUpper())
                                 + QLatin1Char('\n'),
                             QColor(QStringLiteral("#1F7A8C")));
            } else {
                QStringDecoder txDec(m_encoding.toLatin1().constData());
                if (!txDec.isValid())
                    txDec = QStringDecoder(QStringDecoder::Utf8);
                appendRxText(txDec.decode(QByteArrayView(payload.constData(), payload.size()))
                                 + QLatin1Char('\n'),
                             QColor(QStringLiteral("#1F7A8C")));
            }
        }
    }
    emit txBytesWritten(n);
}

void TerminalPage::rebuildHistoryCombo()
{
    // 第 0 项固定为占位提示，随后是历史条目（最新在前）。重建后复位到占位项，
    // 与 activated 信号配合：同一历史项可重复选择
    m_historyCombo->clear();
    m_historyCombo->addItem(QStringLiteral("— 选择历史填入发送框 —"));
    const QStringList items = m_history.list();
    for (int i = 0; i < items.size(); ++i) {
        m_historyCombo->addItem(items.at(i));
        // 长条目/多行条目在框里会被省略，tooltip 显示完整内容
        m_historyCombo->setItemData(i + 1, items.at(i), Qt::ToolTipRole);
    }
    m_historyCombo->setCurrentIndex(0);
}

void TerminalPage::onHistorySelected(int idx)
{
    if (idx <= 0)
        return; // 占位首项
    // 选中 → 回填发送框，不自动发送（用户再点“发送”或回车）；随即复位到占位项
    m_txEdit->setPlainText(m_historyCombo->itemText(idx));
    m_historyCombo->setCurrentIndex(0);
}

void TerminalPage::updateHistoryEnabled()
{
    // 升级占用与循环发送运行期间禁用历史下拉（选中会改写发送框，避免与运行中
    // 状态纠缠——需求：发送禁用期间历史下拉跟随禁用）
    m_historyCombo->setEnabled(!m_upgradeBusy && !m_loopTimer->isActive());
}

void TerminalPage::onLoopToggle()
{
    if (m_loopTimer->isActive()) {
        stopLoop();
        return;
    }
    if (!m_port->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("无法发送"),
                             QStringLiteral("串口未打开，请先打开串口。"));
        return;
    }
    m_loopTimer->setInterval(m_intervalSpin->value());
    m_loopTimer->start();
    m_loopBtn->setText(QStringLiteral("停止发送"));
    // 循环期间锁配置：避免中途切换端口/参数造成状态混乱
    m_intervalSpin->setEnabled(false);
    updateOpenUi(true);
    m_openBtn->setEnabled(false);
    m_refreshBtn->setEnabled(false);
    updateHistoryEnabled(); // 循环运行中历史下拉禁用（回填会打断正在循环发送的内容）
}

void TerminalPage::stopLoop()
{
    m_loopTimer->stop();
    m_loopBtn->setText(QStringLiteral("循环发送"));
    m_intervalSpin->setEnabled(true);
    updateOpenUi(m_port->isOpen());
    updateHistoryEnabled();
}

void TerminalPage::onLoopTimeout()
{
    doSend(true); // 循环发送：出错静默（弹窗会打断循环）
}

void TerminalPage::onData(const QByteArray& data)
{
    if (data.isEmpty())
        return;
    // 升级中转会话期间：把设备回包的原始字节镜像到 RX 文件，CLI 升级轮询读取
    // （用户要求：GUI 连接串口时不影响终端使用——升级无需再关 GUI 串口）
    if (QFile::exists(QDir::tempPath() + QStringLiteral("/my_com_ai_relay.flag"))) {
        const QString rxPath = QDir::tempPath() + QStringLiteral("/my_com_ai_rx.txt");
        QFile rx(rxPath);
        if (rx.open(QIODevice::ReadWrite | QIODevice::Append)) {
            if (rx.size() > 1024 * 1024)
                rx.resize(0); // 超限截断（CLI 侧检测到文件变短会从 0 重新读）
            rx.write(data);
            rx.close();
        }
    }
    // 共享记录：AI 能读到设备回给 GUI 的数据。按"完整行"聚合后再写文件——
    // 设备回显按片到达（慢打印时一片只有几字节），逐片上屏会碎成断句（用户反馈）
    {
        QString text;
        bool lastNl = false;
        for (unsigned char c : data) {
            if (c == '\r' || c == '\n') {
                if (!lastNl)
                    text.append(QLatin1Char('\n'));
                lastNl = true;
            } else if (c >= 0x20 && c < 0x7F) {
                text.append(QLatin1Char(c));
                lastNl = false;
            }
        }
        m_traceRxBuf.append(text);
        for (;;) {
            const int nl = m_traceRxBuf.indexOf(QLatin1Char('\n'));
            if (nl < 0)
                break;
            const QString line = m_traceRxBuf.left(nl).trimmed();
            m_traceRxBuf.remove(0, nl + 1);
            if (!line.isEmpty())
                writeTrace(QStringLiteral("GUI-RX: %1").arg(line));
        }
    }
    if (m_pauseCheck->isChecked()) {
        m_pauseBuf.append(data); // 暂停：只攒不显示
        return;
    }
    displayChunk(data);
}

void TerminalPage::onError(const QString& msg)
{
    // 串口错误只内联提示：红字写进接收区 + 主窗口状态栏展示，不弹模态框——
    // 拔线等连续错误场景下弹窗会挡操作且难看（用户反馈：不要弹窗）
    // 打开状态下报错 = 大概率随后被关闭（拔线/设备复位），closed 处理里红字提示断开
    m_portFaultPending = m_port->isOpen();
    appendRxText(QStringLiteral("[串口错误] %1\n").arg(msg),
                 QColor(QStringLiteral("#C0392B"))); // 错误行红色
}

void TerminalPage::onSaveLog()
{
    const QString def = QStringLiteral("mycom_%1.log")
                            .arg(QDateTime::currentDateTime().toString(
                                QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存接收日志"), def, QStringLiteral("文本文件 (*.txt *.log)"));
    if (path.isEmpty())
        return;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入文件：%1").arg(f.errorString()));
        return;
    }
    f.write(m_rxView->toPlainText().toUtf8());
    if (!f.commit())
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("写入文件失败：%1").arg(f.errorString()));
}

void TerminalPage::onViewModeChanged(int idx)
{
    m_encodingCombo->setEnabled(idx == 0); // 编码只在文本模式有意义
    if (!m_hexBuf.isEmpty()) {
        // 切换显示模式时把 HEX 残余行补完，不丢数据（补 \n 防与后续文本粘连）
        appendRxText(QString::fromLatin1(m_hexBuf.toHex(' ').toUpper())
                     + QLatin1Char('\n'));
        m_hexBuf.clear();
    }
}

void TerminalPage::updateOpenUi(bool open)
{
    // 修复 C2：升级占用期间配置行与开关按钮一律禁用并显示"升级占用中"，
    // 即使引擎切换串口发出 opened/closed 也不恢复，避免用户按 UI 指引重开端口
    // AI 中转轮询跟随真实串口状态：打开期间消费 AI 转发请求，关闭则停
    if (open && !m_relayTimer->isActive())
        m_relayTimer->start();
    else if (!open)
        m_relayTimer->stop();
    m_openBtn->setText(m_upgradeBusy ? QStringLiteral("升级占用中")
                                     : (open ? QStringLiteral("关闭串口")
                                             : QStringLiteral("打开串口")));
    const bool cfgEnabled = !open && !m_upgradeBusy;
    m_openBtn->setEnabled(!m_upgradeBusy);
    m_portCombo->setEnabled(cfgEnabled);
    m_baudCombo->setEnabled(cfgEnabled);
    m_dataCombo->setEnabled(cfgEnabled);
    m_parityCombo->setEnabled(cfgEnabled);
    m_stopCombo->setEnabled(cfgEnabled);
    m_flowCombo->setEnabled(cfgEnabled);
    m_refreshBtn->setEnabled(cfgEnabled);
}

void TerminalPage::displayChunk(const QByteArray& chunk)
{
    if (chunk.isEmpty())
        return;
    if (m_viewCombo->currentIndex() == 1) {
        // HEX：凑满 16 字节一行（接收是按包到达的，行边界与包边界无关）。
        // 流式插入不自动换行，每行补 \n 保证 16 字节行不粘连
        m_hexBuf.append(chunk);
        while (m_hexBuf.size() >= 16) {
            const QByteArray line = m_hexBuf.left(16);
            m_hexBuf.remove(0, 16);
            appendRxText(QString::fromLatin1(line.toHex(' ').toUpper())
                         + QLatin1Char('\n'));
        }
    } else {
        appendRxText(decodeChunk(chunk));
    }
}

void TerminalPage::appendRxText(const QString& text, const QColor& overrideColor)
{
    if (text.isEmpty())
        return;
    // 统一换行：设备控制台层会把每个换行扩成 \r\r\n（0d 0d 0a，实测），
    // 先把连续 \r 折叠成一个，再 \r\n → \n、裸 \r → \n——否则每个行尾都
    // 多出一行空行（用户反馈"莫名其妙的换行"的根因）。折叠后设备真正的空行
    // （\r\r\n\r\r\n）保留为一个空行，不再放大
    QString body = text;
    static const QRegularExpression crRun(QStringLiteral("\\r+"));
    body.replace(crRun, QLatin1String("\r"));
    body.replace(QStringLiteral("\r\n"), QLatin1String("\n"));
    body.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    // 颜色：正文按显示模式区分（文本青绿 / HEX 蓝），错误行由调用方给红；
    // 时间戳灰色不抢眼
    const QColor content = overrideColor.isValid()
        ? overrideColor
        : (m_viewCombo->currentIndex() == 1 ? QColor(QStringLiteral("#1E6FB8"))
                                            : QColor(QStringLiteral("#1E7A46")));

    QScrollBar* sb = m_rxView->verticalScrollBar();
    const int oldPos = sb->value();
    const bool atEnd = (oldPos >= sb->maximum() - 2); // 容差 2px，贴底即视为在末尾

    // 修复：流式插入，不按包强制开新段落。数据是 ~2ms 一片到达的，段落边界只应
    // 跟真实换行走（appendPlainText 每次调用都另起一段 = 每个碎片被切成一行）
    QTextCursor cur(m_rxView->document());
    cur.movePosition(QTextCursor::End);
    if (m_tsCheck->isChecked() && m_lineStart) {
        QTextCharFormat tsFmt;
        tsFmt.setForeground(QColor(QStringLiteral("#8A8A8A")));
        cur.insertText(QStringLiteral("[%1] ").arg(
                           QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"))),
                       tsFmt);
    }
    QTextCharFormat bodyFmt;
    bodyFmt.setForeground(content);
    cur.insertText(body, bodyFmt);

    if (m_scrollCheck->isChecked() || atEnd)
        sb->setValue(sb->maximum()); // 跟随滚动
    else
        sb->setValue(oldPos); // 用户上翻时保持位置，不打断阅读

    m_lineStart = body.endsWith(QLatin1Char('\n'));
}

QString TerminalPage::decodeChunk(const QByteArray& chunk)
{
    QByteArray buf;
    if (m_leadByte) {
        buf.append(m_leadByte); // 拼上上一包留下的残尾前导字节
        m_leadByte = 0;
    }
    buf.append(chunk);
    // 包尾若停在一个多字节字符的前导字节上，拆出来暂存，等下一包拼完整
    if (!buf.isEmpty()) {
        const uchar last = static_cast<uchar>(buf.at(buf.size() - 1));
        const bool gbkLead = (m_encoding == QStringLiteral("GBK") && last >= 0x81 && last <= 0xFE);
        const bool utf8Lead = (m_encoding != QStringLiteral("GBK") && last >= 0xC2 && last <= 0xF4);
        if (gbkLead || utf8Lead) {
            m_leadByte = static_cast<char>(last);
            buf.chop(1);
        }
    }
    if (buf.isEmpty())
        return {};
    if (!m_decoder)
        return QString::fromUtf8(buf); // 兜底（rebuildDecoder 保证有值，此处仅防御）
    return m_decoder->decode(QByteArrayView(buf.constData(), buf.size()));
}

bool TerminalPage::eventFilter(QObject* obj, QEvent* event)
{
    // 回车=发送（Ctrl+回车=换行），匹配常见串口工具的操作习惯
    if (obj == m_txEdit && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
            && !(ke->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier))) {
            doSend(false);
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}
