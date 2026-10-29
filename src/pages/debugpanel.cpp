#include "pages/debugpanel.h"

#include "debugcmd/commandeditor.h"
#include "debugcmd/debugcommand.h"
#include "serial/serialport.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QMessageBox>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

namespace {
constexpr int kLogMaxLines = 3000;     // TX 日志行数上限
constexpr int kSpecialCooldownMs = 35000; // 修复 M3：特殊模式指令发送后冷却 35s，
                                          // 覆盖设备 30s INITIATE 窗口（05c §5 风险3）
                                          // 加 5s 余量，期间屏蔽一切发送防二次 @W12

constexpr int kColGroupWidth = 110; // 分组列宽（用户拍板三列表格：分组/名称/发送内容）
constexpr int kColNameWidth = 150;  // 名称列宽；发送内容列 Stretch 占满剩余

// 修复 M3：特殊模式指令判定——发送后设备进入专用模式并停止响应普通指令；
// @W12 触发 OTA 且同轮二次 @W12 会卡死设备只能断电（05c §5 风险3）。
// 新增此类指令时在此登记前缀。
bool isSpecialModeFrame(const QString& frame)
{
    return frame.startsWith(QStringLiteral("@W12:")) || frame.startsWith(QStringLiteral("@W20:"))
           || frame.startsWith(QStringLiteral("@W90:"));
}

// 单条指令编辑对话框（2026-10-20 用户需求：表格行右键「编辑此指令…」直接修改，
// 确定即保存到固定位置指令表；与全表编辑器 CommandEditorDialog 互补）
class CommandEditDialog : public QDialog
{
public:
    CommandEditDialog(const QString& group, const DebugCommand& cmd, QWidget* parent,
                      const QString& title = QStringLiteral("编辑指令"))
        : QDialog(parent)
    {
        setWindowTitle(title); // 添加场景标题「添加指令」（三轮审查 D5）
        auto* form = new QFormLayout(this);
        m_groupEdit = new QLineEdit(group, this);
        m_nameEdit = new QLineEdit(cmd.name, this);
        m_sendEdit = new QLineEdit(cmd.send, this);
        m_hasParamCombo = new QComboBox(this);
        m_hasParamCombo->addItem(QStringLiteral("否"));
        m_hasParamCombo->addItem(QStringLiteral("是"));
        m_hasParamCombo->setCurrentIndex(cmd.hasParam ? 1 : 0);
        m_defaultEdit = new QLineEdit(cmd.paramDefault, this);
        m_hintEdit = new QLineEdit(cmd.paramHint, this);
        form->addRow(QStringLiteral("分组"), m_groupEdit);
        form->addRow(QStringLiteral("名称"), m_nameEdit);
        form->addRow(QStringLiteral("发送内容"), m_sendEdit);
        form->addRow(QStringLiteral("带参数"), m_hasParamCombo);
        form->addRow(QStringLiteral("默认参数"), m_defaultEdit);
        form->addRow(QStringLiteral("参数提示"), m_hintEdit);
        form->addRow(new QLabel(
            QStringLiteral("带参数指令的发送内容用 %1 占位（如 @W21:%1.）"), this));

        auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        form->addRow(box);
        connect(box, &QDialogButtonBox::accepted, this, [this] {
            if (m_groupEdit->text().trimmed().isEmpty() || m_nameEdit->text().trimmed().isEmpty()
                || m_sendEdit->text().trimmed().isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("无法保存"),
                                     QStringLiteral("分组/名称/发送内容不能为空"));
                return;
            }
            accept();
        });
        connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
        box->button(QDialogButtonBox::Ok)->setDefault(true); // S5：回车触发确定
    }

    QString group() const { return m_groupEdit->text().trimmed(); }
    DebugCommand command() const
    {
        DebugCommand c;
        c.name = m_nameEdit->text().trimmed();
        c.send = m_sendEdit->text().trimmed();
        c.hasParam = m_hasParamCombo->currentIndex() == 1;
        c.paramDefault = m_defaultEdit->text().trimmed(); // M8：与主编辑器一致，全列 trim
        c.paramHint = m_hintEdit->text().trimmed();
        return c;
    }

private:
    QLineEdit* m_groupEdit;
    QLineEdit* m_nameEdit;
    QLineEdit* m_sendEdit;
    QComboBox* m_hasParamCombo;
    QLineEdit* m_defaultEdit;
    QLineEdit* m_hintEdit;
};
} // namespace

DebugPanel::DebugPanel(SerialPort* port, QWidget* parent)
    : DebugPanel(port, QString(), parent)
{
}

DebugPanel::DebugPanel(SerialPort* port, const QString& commandsPath, QWidget* parent)
    : QWidget(parent)
    , m_port(port)
{
    buildUi();
    loadCommands(commandsPath);
    appendLog(QStringLiteral("调试指令面板就绪。双击行直接发送；带参指令选中后在下方填写参数。"));
}

void DebugPanel::buildUi()
{
    // 用户反馈改版：面板放进主窗口左栏（发送设置下方），竖排紧凑布局——
    // 连接位置顶、指令表格居中占大头、参数行、日志小窗口垫底（只作发送确认）
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
    // 编辑指令表（用户反馈）：弹窗里添加/修改/删除指令，保存到固定位置指令表文件
    auto* editBtn = new QPushButton(QStringLiteral("编辑指令"), this);
    editBtn->setToolTip(QStringLiteral("弹窗管理指令表：添加/修改/删除分组与指令"));
    connect(editBtn, &QPushButton::clicked, this, &DebugPanel::onEditCommands);
    connRow->addWidget(editBtn);
    root->addLayout(connRow);

    // 指令表格（用户拍板：分组树改三列表格 分组/名称/发送内容，指令按 JSON 顺序平铺，
    // 分组列逐行显示，双击行发送）。与空白占位页叠进 QStackedWidget：0 行时切占位提示
    m_stack = new QStackedWidget(this);
    m_table = new QTableWidget(m_stack);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels({QStringLiteral("分组"), QStringLiteral("名称"),
                                        QStringLiteral("发送内容")});
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); // 只读：增删改走「编辑指令」
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_table->setColumnWidth(0, kColGroupWidth);
    m_table->setColumnWidth(1, kColNameWidth);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            &DebugPanel::onTableSelectionChanged);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            &DebugPanel::onTableCellDoubleClicked);
    // 右键菜单：弹「编辑此指令…」直接修改该行并保存（2026-10-20 用户需求）
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableWidget::customContextMenuRequested, this,
            &DebugPanel::onTableContextMenu);
    m_stack->addWidget(m_table);

    // 空白占位提示（缺失/损坏时不弹窗不红字，用户拍板）：灰色居中弱提示，
    // 引导点「编辑指令」新建或进编辑窗「加载表格」导入
    m_placeholder = new QLabel(
        QStringLiteral("暂无指令。\n点「编辑指令」新建指令表，或在编辑窗口点「加载表格」"
                       "导入 CSV/JSON 指令表"),
        m_stack);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setStyleSheet(QStringLiteral("color: #8891A5;"));
    // 空表右键（2026-10-22 用户需求 + 三轮审查 D1）：0 行时占位页盖住表格、右键
    // 事件到不了表格——给占位页挂与表格空白处同款菜单（先菜单后对话框，直接
    // 退出菜单不弹添加窗口，2026-10-22 用户反馈）
    m_placeholder->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_placeholder, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) { showAddCommandMenu(m_placeholder->mapToGlobal(pos)); });
    m_stack->addWidget(m_placeholder);
    root->addWidget(m_stack, 1);

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

void DebugPanel::loadCommands(const QString& pathOverride)
{
    // 指令表固定位置（用户 2026-10-20 拍板存储位置，2026-10-21 拍板换 CSV 格式）：
    // %APPDATA%\MyTeam\my_com\debug_commands.csv（main.cpp 已设 org=MyTeam、app=my_com）。
    // 修复 I3：Qt 6 的 AppConfigLocation 在 Windows 指向 %LOCALAPPDATA%，与 ini 所在的
    // Roaming 分家——改用 AppDataLocation（Roaming，与 QSettings 落盘一致）。
    // 编辑保存写同一位置；换电脑 = 拷 csv + 编辑窗「加载表格」导入。
    // pathOverride 非空时用之（单元测试注入临时目录，不碰真实用户数据）。
    m_commandPath = pathOverride.isEmpty()
                        ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                              .filePath(QStringLiteral("debug_commands.csv"))
                        : pathOverride;

    // 自动迁移（pathOverride 非空即测试注入，不做迁移保持确定性）：
    // csv 缺失时，把老版指令表转过来——①固定位置旧 json（2026-10-20~10-21 间版本）
    // ②exe 旁种子 json（老发行版）。转成 CSV 后老 json 保留不动。
    if (pathOverride.isEmpty() && !QFile::exists(m_commandPath)) {
        const QString oldJson =
            QFileInfo(m_commandPath).dir().filePath(QStringLiteral("debug_commands.json"));
        const QString exeSide = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("debug_commands.json"));
        const QString source = QFile::exists(oldJson)
                                   ? oldJson
                                   : (exeSide != oldJson && QFile::exists(exeSide) ? exeSide
                                                                                   : QString());
        if (!source.isEmpty()) {
            DebugCommandModel conv;
            if (conv.load(source) && !conv.groups().isEmpty() && conv.saveCsv(m_commandPath))
                appendLog(QStringLiteral("已把旧指令表迁移为 CSV：%1 → %2")
                              .arg(source, m_commandPath));
            else
                appendLog(QStringLiteral("旧指令表迁移失败（读取失败/空表/写盘失败）：%1，"
                                         "表格保持为空")
                              .arg(source),
                          QColor(QStringLiteral("#C0392B")));
        }
    }

    DebugCommandModel model;
    // 注入路径可能仍是 json（兼容/测试）：按扩展名选择解析
    const bool isCsvPath = m_commandPath.toLower().endsWith(QStringLiteral(".csv"));
    const bool loaded = isCsvPath ? model.loadCsv(m_commandPath) : model.load(m_commandPath);
    if (loaded && !model.groups().isEmpty()) {
        for (const QString& group : model.groups())
            addGroup(group, model.commandsOf(group));
        appendLog(QStringLiteral("已加载指令表：%1（%2 个分组）")
                      .arg(m_commandPath)
                      .arg(model.groups().size()));
        updatePlaceholder();
        return;
    }
    // 缺失/损坏/合法空表：表格空白 + 占位提示，不弹窗不红字、不再回退内置最小
    // 指令集（用户拍板）。空表与缺失分开描述（M7），日志如实反映实情
    appendLog(loaded
                  ? QStringLiteral("指令表 %1 无有效指令行（为空或列格式不符），表格为空"
                                   "（可点「编辑指令」新建或导入；CSV 需 6 列格式，见使用说明）")
                        .arg(m_commandPath)
                  : QStringLiteral("未找到或无法解析指令表 %1，表格为空（可点「编辑指令」"
                                   "新建或导入；CSV 需 6 列格式，见使用说明）")
                        .arg(m_commandPath));
    updatePlaceholder();
}

void DebugPanel::onEditCommands()
{
    // 保存到与加载相同的位置（固定 %APPDATA% 用户目录，2026-10-20 用户拍板）；
    // exe 旁种子模板仅作编辑窗「加载表格」的导入素材
    CommandEditorDialog dlg(m_commandPath, m_commandPath, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_table->setRowCount(0); // 清行（保留表头与列配置）后按新指令表重建
        loadCommands(m_commandPath); // 显式回传同一路径（M5：注入路径构造时也重读同一文件）
        appendLog(QStringLiteral("指令表已保存并重新加载"));
    }
}

void DebugPanel::onTableContextMenu(const QPoint& pos)
{
    // 右键菜单（2026-10-22 用户需求扩展）：指令行 = 编辑此指令… / 删除此指令；
    // 空白处 = 添加命令…（先菜单后对话框，退出菜单不弹窗）。冷却期间表格禁用，
    // 菜单整体不可用
    const int row = m_table->rowAt(pos.y());
    const QTableWidgetItem* it = (row >= 0) ? m_table->item(row, 0) : nullptr;
    const bool validRow = it && it->data(Qt::UserRole).isValid();
    if (!validRow) {
        showAddCommandMenu(m_table->viewport()->mapToGlobal(pos));
        return;
    }
    if (m_menuOpen)
        return; // 同一右键只允许一个菜单（防御重复事件，用户反馈"退出后自动弹窗"）
    m_table->setCurrentCell(row, 0); // 选中被右键的行，参数区跟随刷新
    m_menuOpen = true;
    QMenu menu(this);
    QAction* editAct = menu.addAction(QStringLiteral("编辑此指令…"));
    QAction* delAct = menu.addAction(QStringLiteral("删除此指令"));
    QAction* chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    m_menuOpen = false;
    if (chosen == editAct)
        editCommandAt(row);
    else if (chosen == delAct)
        deleteCommandAt(row);
}

void DebugPanel::showAddCommandMenu(const QPoint& globalPos)
{
    // 单一「添加命令…」菜单（表格空白处 / 空表占位页共用）：必须在菜单里选择才弹
    // 添加对话框，直接退出菜单什么都不做（2026-10-22 用户反馈）
    if (m_menuOpen)
        return;
    m_menuOpen = true;
    QMenu menu(this);
    QAction* addAct = menu.addAction(QStringLiteral("添加命令…"));
    if (menu.exec(globalPos) == addAct)
        addCommandAtEnd();
    m_menuOpen = false;
}

void DebugPanel::addCommandAtEnd()
{
    // 右键空白处「添加命令…」：弹单条编辑对话框（默认分组 = 最后一行分组，方便
    // 连续添加同组指令），确定即追加行尾并落盘（与右键修改同款"改完不丢"）
    QString group = QStringLiteral("新分组");
    for (int r = m_table->rowCount() - 1; r >= 0; --r) {
        if (m_table->item(r, 0)) {
            group = m_table->item(r, 0)->text();
            break;
        }
    }
    CommandEditDialog dlg(group, DebugCommand(), this, QStringLiteral("添加指令"));
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString g = dlg.group();
    const DebugCommand c = dlg.command();
    appendCommandRow(g, c);
    updatePlaceholder(); // 0 行 → 1 行：从占位提示页切回表格页
    m_table->setCurrentCell(m_table->rowCount() - 1, 0);
    if (saveTable())
        appendLog(QStringLiteral("已添加并保存指令：%1 / %2").arg(g, c.name));
    else
        appendLog(QStringLiteral("指令已添加但保存失败：%1 / %2（写盘失败，重启后丢失）")
                      .arg(g, c.name),
                  QColor(QStringLiteral("#C0392B")));
    onTableSelectionChanged(); // 参数区跟随新行刷新
}

void DebugPanel::deleteCommandAt(int row)
{
    // 右键「删除此指令」：确认后删行并落盘（删除不可恢复，弹确认与特殊模式/空表
    // 确认同一风格）；删空后切回占位提示页
    const QTableWidgetItem* it = m_table->item(row, 0);
    if (!it)
        return;
    const QString name =
        m_table->item(row, 1) ? m_table->item(row, 1)->text() : QString();
    const auto r = QMessageBox::question(
        this, QStringLiteral("删除指令"),
        QStringLiteral("确定删除指令「%1」？删除后立即保存，不可恢复。").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes)
        return;
    m_table->removeRow(row);
    updatePlaceholder();
    if (saveTable())
        appendLog(QStringLiteral("已删除并保存指令：%1").arg(name));
    else
        appendLog(QStringLiteral("指令已删除但保存失败：%1（写盘失败，重启后恢复）").arg(name),
                  QColor(QStringLiteral("#C0392B")));
    onTableSelectionChanged(); // 参数区跟随新选中行/清空
}

void DebugPanel::editCommandAt(int row)
{
    // 弹单条编辑对话框（分组 + 6 字段）；确定后更新行与 UserRole 并立即落盘，
    // 保证"右键改完关闭软件不丢"（2026-10-20 用户需求）
    QTableWidgetItem* it = m_table->item(row, 0);
    if (!it || !it->data(Qt::UserRole).isValid())
        return;
    const QString oldGroup = it->text();
    const DebugCommand oldCmd = it->data(Qt::UserRole).value<DebugCommand>();
    CommandEditDialog dlg(oldGroup, oldCmd, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QString group = dlg.group();
    const DebugCommand cmd = dlg.command();
    it->setText(group);
    it->setData(Qt::UserRole, QVariant::fromValue(cmd));
    m_table->item(row, 1)->setText(cmd.name);
    QTableWidgetItem* sendItem = m_table->item(row, 2);
    sendItem->setText(cmd.send);
    // tooltip 与 addGroup 同规则：完整帧 + 参数提示（列窄显示不全时仍可读全）
    if (cmd.hasParam)
        sendItem->setToolTip(QStringLiteral("%1\n参数：%2")
                                 .arg(cmd.send,
                                      cmd.paramHint.isEmpty() ? QStringLiteral("参数值")
                                                              : cmd.paramHint));
    else
        sendItem->setToolTip(cmd.send);
    // M3：写盘失败必须如实提示（此前无条件报成功，失败时提示相反、重启修改丢失）
    if (saveTable())
        appendLog(QStringLiteral("已修改并保存指令：%1 / %2").arg(group, cmd.name));
    else
        appendLog(QStringLiteral("指令已修改但保存失败：%1 / %2（写盘失败，重启后修改丢失）")
                      .arg(group, cmd.name),
                  QColor(QStringLiteral("#C0392B")));
    // M6：更新行后手动刷新参数区——改默认参数后参数框仍显示旧值、立即双击发送会发旧参数
    onTableSelectionChanged();
}

bool DebugPanel::saveTable()
{
    // 从表格行重建模型并写回 m_commandPath：隐藏字段（hasParam/paramDefault/
    // paramHint）都存在第 0 列 UserRole 里，行遍历即可完整还原；目录 mkpath 由
    // model.save/saveCsv 保证（首次使用 %APPDATA% 目录尚未创建时也不会失败）
    DebugCommandModel model;
    QVector<DebugCommandModel::Entry> es;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QTableWidgetItem* it = m_table->item(r, 0);
        if (!it || !it->data(Qt::UserRole).isValid())
            continue;
        DebugCommandModel::Entry e;
        e.group = it->text();
        e.cmd = it->data(Qt::UserRole).value<DebugCommand>();
        es.append(e);
    }
    model.setEntries(es);
    // 按扩展名写盘：固定位置为 .csv（2026-10-21 用户拍板），注入的 json 路径仍走 json
    return m_commandPath.toLower().endsWith(QStringLiteral(".csv"))
               ? model.saveCsv(m_commandPath)
               : model.save(m_commandPath);
}

void DebugPanel::addGroup(const QString& group, const QVector<DebugCommand>& cmds)
{
    // 指令按 JSON 顺序平铺为表格行（无分组层级行）：分组列逐行显示，信息不丢失
    for (const DebugCommand& c : cmds)
        appendCommandRow(group, c);
}

void DebugPanel::appendCommandRow(const QString& group, const DebugCommand& c)
{
    // 追加一行（加载与右键「添加命令」共用）：指令对象存第 0 列 UserRole，
    // 选中/发送/参数逻辑统一取该行数据
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    auto* groupItem = new QTableWidgetItem(group);
    groupItem->setData(Qt::UserRole, QVariant::fromValue(c));
    m_table->setItem(r, 0, groupItem);
    m_table->setItem(r, 1, new QTableWidgetItem(c.name));
    auto* sendItem = new QTableWidgetItem(c.send);
    // 发送内容列 tooltip：完整帧 + 参数提示（列窄显示不全时仍可读全）
    if (c.hasParam)
        sendItem->setToolTip(QStringLiteral("%1\n参数：%2")
                                 .arg(c.send,
                                      c.paramHint.isEmpty() ? QStringLiteral("参数值")
                                                            : c.paramHint));
    else
        sendItem->setToolTip(c.send);
    m_table->setItem(r, 2, sendItem);
}

void DebugPanel::updatePlaceholder()
{
    // 0 行显示占位提示页，否则显示表格页；同步刷新发送控件可用性
    m_stack->setCurrentWidget(m_table->rowCount() == 0 ? static_cast<QWidget*>(m_placeholder)
                                                       : static_cast<QWidget*>(m_table));
    applySendEnabled();
}

int DebugPanel::currentCommandRow() const
{
    const int row = m_table->currentRow();
    if (row < 0)
        return -1;
    const QTableWidgetItem* it = m_table->item(row, 0);
    return (it && it->data(Qt::UserRole).isValid()) ? row : -1;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void DebugPanel::onTableSelectionChanged()
{
    const int row = currentCommandRow();
    if (row < 0) {
        m_paramLabel->hide();
        m_paramEdit->hide();
        m_sendBtn->setEnabled(false);
        return;
    }
    const DebugCommand cmd = m_table->item(row, 0)->data(Qt::UserRole).value<DebugCommand>();
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

void DebugPanel::onTableCellDoubleClicked(int row, int column)
{
    Q_UNUSED(column);
    // 双击任意单元格 = 原树双击（选中该行并发送）；空白区双击不触发本信号
    if (row < 0)
        return;
    const QTableWidgetItem* it = m_table->item(row, 0);
    if (!it || !it->data(Qt::UserRole).isValid())
        return;
    m_table->setCurrentCell(row, 0); // 触发参数区刷新
    onSendCurrent();
}

void DebugPanel::onSendCurrent()
{
    const int row = currentCommandRow();
    if (row < 0)
        return;
    const DebugCommand cmd = m_table->item(row, 0)->data(Qt::UserRole).value<DebugCommand>();
    const QString frame = currentFrameText();
    if (frame.isEmpty())
        return; // 参数非法，currentFrameText 已提示
    doSend(frame, cmd.name);
}


QString DebugPanel::currentFrameText()
{
    const int row = currentCommandRow();
    if (row < 0)
        return QString();
    const DebugCommand cmd = m_table->item(row, 0)->data(Qt::UserRole).value<DebugCommand>();
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
    m_table->setEnabled(on);
    m_connectCombo->setEnabled(on);
    m_sendBtn->setEnabled(on && currentCommandRow() >= 0);
}
