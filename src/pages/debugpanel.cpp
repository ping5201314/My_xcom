#include "pages/debugpanel.h"

#include "debugcmd/commandeditor.h"
#include "debugcmd/debugcommand.h"
#include "serial/serialport.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QMessageBox>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

namespace {
constexpr int kLogMaxLines = 3000;     // TX 日志行数上限
constexpr int kSpecialCooldownMs = 35000; // 修复 M3：特殊模式指令发送后冷却 35s，
                                          // 覆盖设备 30s INITIATE 窗口（05c §5 风险3）
                                          // 加 5s 余量，期间屏蔽一切发送防二次 @W12

// 修复 M3：特殊模式指令判定——发送后设备进入专用模式并停止响应普通指令；
// @W12 触发 OTA 且同轮二次 @W12 会卡死设备只能断电（05c §5 风险3）。
// 新增此类指令时在此登记前缀。
bool isSpecialModeFrame(const QString& frame)
{
    return frame.startsWith(QStringLiteral("@W12:")) || frame.startsWith(QStringLiteral("@W20:"))
           || frame.startsWith(QStringLiteral("@W90:"));
}
} // namespace

DebugPanel::DebugPanel(SerialPort* port, QWidget* parent)
    : QWidget(parent)
    , m_port(port)
{
    buildUi();
    loadCommands();
    appendLog(QStringLiteral("调试指令面板就绪。双击指令直接发送；带参指令选中后在下方填写参数。"));
}

void DebugPanel::buildUi()
{
    // 用户反馈改版：面板放进主窗口左栏（发送设置下方），竖排紧凑布局——
    // 连接位置顶、指令树居中占大头、参数行、日志小窗口垫底（只作发送确认）
    auto* root = new QVBoxLayout(this);

    // 连接位选择（置顶）：R/C 指令帧 @Rxx:N / @Cxx:N 的末位（调试通道连接标志），
    // 默认 0；部分调试场景需要 :1（如 @R01:1 回显连接位）。W 指令值域固定 5 字节
    // 含 '.'，无此位
    auto* connRow = new QHBoxLayout;
    connRow->addWidget(new QLabel(QStringLiteral("连接位 (R/C)"), this));
    m_connectCombo = new QComboBox(this);
    m_connectCombo->addItem(QStringLiteral("0"));
    m_connectCombo->addItem(QStringLiteral("1"));
    m_connectCombo->setToolTip(QStringLiteral("R/C 指令帧末位 :0/:1，默认 0"));
    connRow->addWidget(m_connectCombo);
    connRow->addStretch(1);
    // 编辑指令表（用户反馈）：弹窗里添加/修改/删除指令，保存到 debug_commands.json
    auto* editBtn = new QPushButton(QStringLiteral("编辑指令"), this);
    editBtn->setToolTip(QStringLiteral("弹窗管理指令表：添加/修改/删除分组与指令"));
    connect(editBtn, &QPushButton::clicked, this, &DebugPanel::onEditCommands);
    connRow->addWidget(editBtn);
    root->addLayout(connRow);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabel(QStringLiteral("指令（双击发送）"));
    m_tree->setAlternatingRowColors(true);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &DebugPanel::onTreeSelectionChanged);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &DebugPanel::onTreeDoubleClicked);
    root->addWidget(m_tree, 1);

    // 参数区（仅带参指令显示）
    auto* paramRow = new QHBoxLayout;
    m_paramLabel = new QLabel(this);
    m_paramLabel->hide();
    m_paramEdit = new QLineEdit(this);
    m_paramEdit->hide();
    m_paramEdit->setPlaceholderText(QStringLiteral("参数值"));
    m_sendBtn = new QPushButton(QStringLiteral("发送选中指令"), this);
    connect(m_sendBtn, &QPushButton::clicked, this, &DebugPanel::onSendCurrent);
    paramRow->addWidget(m_paramLabel);
    paramRow->addWidget(m_paramEdit, 1);
    paramRow->addWidget(m_sendBtn);
    root->addLayout(paramRow);

    // TX 日志：小窗口垫底（固定高度，只作发送确认，不占大块空间）
    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(kLogMaxLines);
    m_logView->setFixedHeight(90);
    m_logView->setFont(QFont(QStringLiteral("Consolas")));
    root->addWidget(m_logView);

    // 修复 M3：特殊模式指令冷却定时器（发送后 35s 内屏蔽一切发送，防连点二次 @W12）
    m_cooldownTimer = new QTimer(this);
    m_cooldownTimer->setSingleShot(true);
    connect(m_cooldownTimer, &QTimer::timeout, this, [this] {
        appendLog(QStringLiteral("特殊模式指令冷却结束，发送已恢复"));
        applySendEnabled();
    });
}

// ---------------------------------------------------------------------------
// 指令表加载
// ---------------------------------------------------------------------------

void DebugPanel::loadCommands()
{
    // 指令表位置（用户反馈：数据要跟着工程走，不放到 %APPDATA%）：
    // exe 旁的 debug_commands.json（构建时从 commands/ 复制过来）。
    // 编辑保存也是同一个文件——改完想进源码就把它复制回 commands/ 提交。
    m_commandPath = QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("debug_commands.json"));

    DebugCommandModel model;
    if (model.load(m_commandPath) && !model.groups().isEmpty()) {
        for (const QString& group : model.groups())
            addGroup(group, model.commandsOf(group));
        appendLog(QStringLiteral("已加载指令表：%1（%2 个分组）")
                      .arg(m_commandPath)
                      .arg(model.groups().size()));
        return;
    }
    appendLog(QStringLiteral("指令表 %1 缺失或损坏，使用内置最小指令集").arg(m_commandPath));
    QVector<DebugCommand> fallback;
    DebugCommand c;
    c.name = QStringLiteral("版本信息");
    c.send = QStringLiteral("@R20:0");
    c.hasParam = false;
    fallback.append(c);
    addGroup(QStringLiteral("信息查询"), fallback);
}

void DebugPanel::onEditCommands()
{
    // 保存到与加载相同的位置（exe 旁，即工程路径）——用户反馈：数据跟着工程走，
    // 不放 %APPDATA%；想进源码就把改好的文件复制回 commands/debug_commands.json
    CommandEditorDialog dlg(m_commandPath, m_commandPath, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_tree->clear();
        loadCommands(); // 重新加载并重建指令树
        appendLog(QStringLiteral("指令表已保存并重新加载"));
    }
}

void DebugPanel::addGroup(const QString& group, const QVector<DebugCommand>& cmds)
{
    auto* groupItem = new QTreeWidgetItem(m_tree, QStringList{ group });
    groupItem->setFlags(Qt::ItemIsEnabled); // 分组不可选中发送
    for (const DebugCommand& c : cmds) {
        auto* item = new QTreeWidgetItem(groupItem, QStringList{ c.name });
        item->setData(0, Qt::UserRole, QVariant::fromValue(c));
        if (c.hasParam)
            item->setToolTip(0, QStringLiteral("%1\n参数：%2")
                                    .arg(c.send, c.paramHint.isEmpty() ? QStringLiteral("参数值")
                                                                       : c.paramHint));
        else
            item->setToolTip(0, c.send);
    }
    groupItem->setExpanded(true);
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void DebugPanel::onTreeSelectionChanged()
{
    const auto* item = m_tree->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid()) {
        m_paramLabel->hide();
        m_paramEdit->hide();
        m_sendBtn->setEnabled(false);
        return;
    }
    const DebugCommand cmd = item->data(0, Qt::UserRole).value<DebugCommand>();
    // 修复 M3：发送按钮可用性受特殊模式冷却约束（升级期间不再禁用，用户反馈）
    m_sendBtn->setEnabled(!m_cooldownTimer->isActive());
    if (cmd.hasParam) {
        m_paramLabel->setText(QStringLiteral("参数（%1）").arg(cmd.paramHint));
        m_paramLabel->show();
        m_paramEdit->setText(cmd.paramDefault);
        m_paramEdit->show();
    } else {
        m_paramLabel->hide();
        m_paramEdit->hide();
    }
}

void DebugPanel::onTreeDoubleClicked(QTreeWidgetItem* item, int column)
{
    Q_UNUSED(column);
    if (!item || !item->data(0, Qt::UserRole).isValid())
        return;
    m_tree->setCurrentItem(item); // 触发参数区刷新
    onSendCurrent();
}

void DebugPanel::onSendCurrent()
{
    const auto* item = m_tree->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid())
        return;
    const DebugCommand cmd = item->data(0, Qt::UserRole).value<DebugCommand>();
    const QString frame = currentFrameText();
    if (frame.isEmpty())
        return; // 参数非法，currentFrameText 已提示
    doSend(frame, cmd.name);
}


QString DebugPanel::currentFrameText()
{
    const auto* item = m_tree->currentItem();
    if (!item || !item->data(0, Qt::UserRole).isValid())
        return QString();
    const DebugCommand cmd = item->data(0, Qt::UserRole).value<DebugCommand>();
    if (!cmd.hasParam) {
        QString frame = cmd.send;
        // R/C 指令的连接位可切换：帧形如 @R20:0，末位改写为所选连接位；
        // W 指令（值域固定 5 字节且含 '.'）无此位，原样发送
        if ((frame.startsWith(QStringLiteral("@R")) || frame.startsWith(QStringLiteral("@C")))
            && frame.size() >= 6 && frame.at(frame.size() - 2) == QLatin1Char(':')) {
            const QChar last = frame.at(frame.size() - 1);
            if (last == QLatin1Char('0') || last == QLatin1Char('1'))
                frame[frame.size() - 1] = m_connectCombo->currentIndex() == 1
                                              ? QLatin1Char('1')
                                              : QLatin1Char('0');
        }
        return frame;
    }
    // 参数不做位数/格式限制（用户要求）：本工具是通用串口助手，不绑定 Finca 的
    // 5 字节值域约定。参数原样替换 %1；模板没有 %1 就原样发送
    const QString param = m_paramEdit->text();
    QString frame = cmd.send;
    if (frame.contains(QStringLiteral("%1")))
        frame.replace(QStringLiteral("%1"), param);
    return frame;
}

// ---------------------------------------------------------------------------
// 发送与日志
// ---------------------------------------------------------------------------

void DebugPanel::doSend(const QString& frame, const QString& name)
{
    // 修复 M3：特殊模式指令（@W12/@W20/@W90）发送前弹模态确认框（模态即挡连点），
    // 发送成功后进入 35s 冷却屏蔽一切发送——冷却覆盖设备 30s INITIATE 窗口，
    // 防止二次 @W12 叠加卡死设备（05c §5 风险3）。冷却判断放在打开检查之前，
    // 无论串口状态如何都要挡住重复触发。
    if (isSpecialModeFrame(frame)) {
        if (m_cooldownTimer->isActive()) {
            appendLog(QStringLiteral("特殊模式指令冷却中（%1s），已忽略本次发送（指令：%2）")
                          .arg(m_cooldownTimer->remainingTime() / 1000 + 1)
                          .arg(name));
            return;
        }
        const auto r = QMessageBox::question(
            this, QStringLiteral("特殊模式指令"),
            QStringLiteral("“%1”将使设备进入特殊模式：\n"
                           "• @W12 触发 OTA 升级：设备进入 30s 升级等待窗口，期间不响应普通指令；"
                           "同轮二次 @W12 会卡死设备，只能断电恢复（05c §5 风险3）\n"
                           "• @W20/@W90 创建专用模式任务，设备停止常规通讯\n\n"
                           "确定发送？")
                .arg(name));
        if (r != QMessageBox::Yes) {
            appendLog(QStringLiteral("已取消发送特殊模式指令：%1").arg(name));
            return;
        }
    }
    // 串口未打开：红字提示（用户反馈），不崩溃
    if (!m_port->isOpen()) {
        appendLog(QStringLiteral("串口未打开，发送失败（指令：%1）").arg(name),
                  QColor(QStringLiteral("#C0392B")));
        return;
    }
    const QByteArray bytes = frame.toLatin1();
    const qint64 n = m_port->write(bytes);
    if (n != bytes.size()) {
        // 修复 Minor 2：末位占位符应为 %2（期望字节数），此前重复 %1 显示"写 5/10 字节：5"
        appendLog(QStringLiteral("发送失败（写 %1/%2 字节）：%2").arg(n).arg(bytes.size()),
                  QColor(QStringLiteral("#C0392B")));
        return;
    }
    // TX 日志：时间戳 + 指令名 + HEX
    appendLog(QStringLiteral("TX [%1] %2 | %3").arg(name, hexOf(bytes), frame));
    // 修复 M3：特殊模式指令发送成功 → 冷却 35s（覆盖设备 30s INITIATE 窗口）
    if (isSpecialModeFrame(frame)) {
        m_cooldownTimer->start(kSpecialCooldownMs);
        applySendEnabled();
        appendLog(QStringLiteral("特殊模式指令已发送，发送控件冷却 35s（覆盖设备 30s "
                                 "INITIATE 窗口，防二次 @W12 卡死设备，05c 风险3）"));
    }
}

QString DebugPanel::hexOf(const QByteArray& data) const
{
    QString s;
    for (unsigned char c : data)
        s += QString::asprintf("%02X ", c);
    return s.trimmed();
}

void DebugPanel::appendLog(const QString& line, const QColor& color)
{
    // 上限 3000 行（setMaximumBlockCount 自动裁旧）+ 自动滚动；
    // 支持着色（错误提示红字，用户反馈）
    QTextCursor cur(m_logView->document());
    cur.movePosition(QTextCursor::End);
    QTextCharFormat fmt;
    if (color.isValid())
        fmt.setForeground(color);
    cur.insertText(QStringLiteral("[%1] %2\n")
                       .arg(QDateTime::currentDateTime().toString(
                                QStringLiteral("HH:mm:ss.zzz")),
                            line),
                   fmt);
    m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
}

void DebugPanel::applySendEnabled()
{
    // 修复 M3：特殊模式冷却期间屏蔽发送（防连点二次 @W12 卡死设备，05c 风险3）。
    // 升级期间不再禁用面板（用户反馈：删除"传输期间禁发"功能，用户自行把控）
    const bool on = !m_cooldownTimer->isActive();
    m_tree->setEnabled(on);
    m_connectCombo->setEnabled(on);
    m_sendBtn->setEnabled(on && m_tree->currentItem()
                           && m_tree->currentItem()->data(0, Qt::UserRole).isValid());
}
