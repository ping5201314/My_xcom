#include "debugcmd/commandeditor.h"

#include <algorithm>

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Column {
    ColGroup = 0,
    ColName,
    ColSend,
    ColHasParam,
    ColDefault,
    ColHint,
    ColCount
};
} // namespace

CommandEditorDialog::CommandEditorDialog(const QString& loadPath, const QString& savePath,
                                         QWidget* parent)
    : QDialog(parent)
    , m_jsonPath(savePath)
{
    setWindowTitle(QStringLiteral("编辑调试指令"));
    resize(860, 480);

    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(
        QStringLiteral("分组与指令可自由增改；带参数指令的 send 用 %1 占位（如 @W21:%1.）。"
                       "参数不做位数/格式限制（通用串口助手，不绑定特定协议）。"),
        this));
    // 2026-10-22 用户反馈：去掉保存路径提示（多余的提示）；「加载表格」的语义
    // 由按钮 tooltip 说明，窗口更清爽

    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels({QStringLiteral("分组"), QStringLiteral("名称"),
                                        QStringLiteral("发送内容"), QStringLiteral("带参数"),
                                        QStringLiteral("默认参数"), QStringLiteral("参数提示")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(ColSend, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->verticalHeader()->setVisible(false);
    root->addWidget(m_table, 1);

    auto* btnRow = new QHBoxLayout;
    auto* addBtn = new QPushButton(QStringLiteral("添加指令"), this);
    auto* delBtn = new QPushButton(QStringLiteral("删除选中"), this);
    // 换电脑导入入口：选 json 替换当前编辑内容，只改内存，保存/关闭才写固定用户目录
    auto* loadBtn = new QPushButton(QStringLiteral("加载表格"), this);
    loadBtn->setToolTip(QStringLiteral("从 CSV/JSON 文件导入并替换当前编辑内容（保存/关闭才写盘）"));
    m_saveBtn = new QPushButton(QStringLiteral("保存"), this);
    m_saveBtn->setProperty("primary", true);
    // 用户反馈：关闭即保存——「关闭」按钮与窗口 X 都走保存流程
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    closeBtn->setToolTip(QStringLiteral("关闭并保存到用户目录"));
    connect(addBtn, &QPushButton::clicked, this, &CommandEditorDialog::onAddRow);
    connect(delBtn, &QPushButton::clicked, this, &CommandEditorDialog::onRemoveSelected);
    connect(loadBtn, &QPushButton::clicked, this, &CommandEditorDialog::onLoadTable);
    connect(m_saveBtn, &QPushButton::clicked, this, &CommandEditorDialog::onSave);
    connect(closeBtn, &QPushButton::clicked, this, &CommandEditorDialog::onSave);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(loadBtn);
    btnRow->addWidget(m_saveBtn);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    // 加载当前指令表（从面板实际加载的文件读；失败则表格为空、可从头新建）。
    // 按扩展名解析：固定位置为 .csv（2026-10-21 拍板），注入/旧 json 路径仍走 json
    const bool isCsv = loadPath.toLower().endsWith(QStringLiteral(".csv"));
    if (isCsv ? m_model.loadCsv(loadPath) : m_model.load(loadPath))
        fillTable();
}

void CommandEditorDialog::closeEvent(QCloseEvent* event)
{
    // 用户反馈：关闭即保存（含窗口 X）。已保存过直接放行；否则先校验写盘，
    // 内容非法或写盘失败则留在窗口里让用户修正/重试
    if (m_saved) {
        event->accept();
        return;
    }
    if (!doSave()) {
        event->ignore();
        return;
    }
    m_saved = true;
    event->accept();
}

void CommandEditorDialog::fillTable()
{
    const auto entries = m_model.entries();
    m_table->setRowCount(entries.size());
    for (int r = 0; r < entries.size(); ++r) {
        const DebugCommand& c = entries[r].cmd;
        m_table->setItem(r, ColGroup, new QTableWidgetItem(entries[r].group));
        m_table->setItem(r, ColName, new QTableWidgetItem(c.name));
        m_table->setItem(r, ColSend, new QTableWidgetItem(c.send));
        // 带参数列：下拉框（否/是）
        auto* combo = new QComboBox(m_table);
        combo->addItem(QStringLiteral("否"));
        combo->addItem(QStringLiteral("是"));
        combo->setCurrentIndex(c.hasParam ? 1 : 0);
        m_table->setCellWidget(r, ColHasParam, combo);
        m_table->setItem(r, ColDefault, new QTableWidgetItem(c.paramDefault));
        m_table->setItem(r, ColHint, new QTableWidgetItem(c.paramHint));
    }
}

void CommandEditorDialog::onAddRow()
{
    // 新行默认填进最后一行的分组（没有则"新分组"），便于批量添加同组指令
    const int last = m_table->rowCount() - 1;
    const QString group = last >= 0 && m_table->item(last, ColGroup)
                              ? m_table->item(last, ColGroup)->text()
                              : QStringLiteral("新分组");
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    m_table->setItem(r, ColGroup, new QTableWidgetItem(group));
    m_table->setItem(r, ColName, new QTableWidgetItem(QStringLiteral("新指令")));
    m_table->setItem(r, ColSend, new QTableWidgetItem(QStringLiteral("@R20:0")));
    auto* combo = new QComboBox(m_table);
    combo->addItem(QStringLiteral("否"));
    combo->addItem(QStringLiteral("是"));
    m_table->setCellWidget(r, ColHasParam, combo);
    m_table->setItem(r, ColDefault, new QTableWidgetItem(QStringLiteral("0000")));
    m_table->setItem(r, ColHint, new QTableWidgetItem());
    m_table->setCurrentCell(r, ColName); // 落到名称列，可直接改名
    m_table->editItem(m_table->item(r, ColName));
}

void CommandEditorDialog::onRemoveSelected()
{
    // 从大到小删，避免行号偏移
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    QList<int> rList;
    for (const QModelIndex& idx : rows)
        rList << idx.row();
    std::sort(rList.begin(), rList.end(), std::greater<int>());
    for (int r : rList)
        m_table->removeRow(r);
}

void CommandEditorDialog::onLoadTable()
{
    // 换电脑导入（用户 2026-10-20 拍板）：初始目录 = exe 旁（构建后模板种子所在）。
    // 2026-10-21 起同时支持 CSV（用户拍板，Excel 编辑）与 JSON（老表兼容）
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("加载指令表"), QCoreApplication::applicationDirPath(),
        QStringLiteral("指令表 (*.csv *.json)"));
    if (path.isEmpty())
        return; // 用户取消，不做任何变化
    // 按扩展名解析；失败时弹警告、当前编辑内容保持不变。
    // CSV 无法区分"合法空表"与"内容全坏"（都解析出 0 行），保守按失败处理：
    // 选错文件（二进制/无关 csv）不会静默清空编辑表格（T14 二轮审查 Major）；
    // 合法空表 = JSON {} 或编辑器内自行删行，不必经「加载表格」导入
    const bool isCsvPath = path.toLower().endsWith(QStringLiteral(".csv"));
    const bool ok = isCsvPath ? (m_model.loadCsv(path) && !m_model.groups().isEmpty())
                              : m_model.load(path);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("加载失败"),
                             QStringLiteral("无法读取所选文件（不存在、格式损坏或无有效指令行）："
                                            "\n%1\n当前编辑内容保持不变。")
                                 .arg(path));
        return;
    }
    // 全量替换 6 列编辑内容；只改内存，保存/关闭才写固定用户目录
    fillTable();
}

bool CommandEditorDialog::collectAndValidate(QString* err)
{
    QVector<DebugCommandModel::Entry> es;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const auto text = [this, r](int col) {
            const QTableWidgetItem* it = m_table->item(r, col);
            return it ? it->text().trimmed() : QString();
        };
        DebugCommandModel::Entry e;
        e.group = text(ColGroup);
        e.cmd.name = text(ColName);
        e.cmd.send = text(ColSend);
        auto* combo = qobject_cast<QComboBox*>(m_table->cellWidget(r, ColHasParam));
        e.cmd.hasParam = combo && combo->currentIndex() == 1;
        e.cmd.paramDefault = text(ColDefault);
        e.cmd.paramHint = text(ColHint);
        // 只保留基础完整性校验（空字段会存出损坏的 JSON）；参数格式不做限制
        //（用户要求：通用串口助手，不绑定特定协议的参数约定）
        if (e.group.isEmpty() || e.cmd.name.isEmpty() || e.cmd.send.isEmpty()) {
            *err = QStringLiteral("第 %1 行：分组/名称/发送内容不能为空").arg(r + 1);
            return false;
        }
        es.append(e);
    }
    m_model.setEntries(es);
    return true;
}

bool CommandEditorDialog::doSave()
{
    QString err;
    if (!collectAndValidate(&err)) {
        QMessageBox::warning(this, QStringLiteral("无法保存"), err);
        return false;
    }
    // 防呆（真机教训：用户误删全部行后关闭即保存，指令表被存成空表）：
    // 空表保存前必须明确确认
    if (m_model.entries().isEmpty()) {
        const auto r = QMessageBox::question(
            this, QStringLiteral("指令表为空"),
            // 面板已无内置最小集回退，空表保存后就是空白表格
            QStringLiteral("当前指令表没有任何指令，保存后调试面板将无任何指令（空白表格）。\n"
                           "确定要保存空表吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes)
            return false;
    }
    // 按扩展名写盘：固定位置为 .csv（2026-10-21 用户拍板，Excel 可编辑），
    // 测试注入的 json 路径仍走 json
    const bool isCsvPath = m_jsonPath.toLower().endsWith(QStringLiteral(".csv"));
    if (!(isCsvPath ? m_model.saveCsv(m_jsonPath) : m_model.save(m_jsonPath))) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("无法写入：%1").arg(m_jsonPath));
        return false;
    }
    return true;
}

void CommandEditorDialog::onSave()
{
    if (!doSave())
        return;
    m_saved = true;
    accept(); // 面板在 Accepted 后重新加载指令树
}
