#include "debugcmd/commandeditor.h"

#include <algorithm>

#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
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
        QStringLiteral("分组与指令可自由增改；带参数指令的 send 用 %1 占位（如 @W21:%1.），"
                       "默认参数必须是 4 位数字（W 值域固定 5 字节且必须含 '.'）。"),
        this));

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
    m_saveBtn = new QPushButton(QStringLiteral("保存"), this);
    m_saveBtn->setProperty("primary", true);
    // 用户反馈：关闭即保存——「关闭」按钮与窗口 X 都走保存流程
    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    closeBtn->setToolTip(QStringLiteral("关闭并保存到用户目录"));
    connect(addBtn, &QPushButton::clicked, this, &CommandEditorDialog::onAddRow);
    connect(delBtn, &QPushButton::clicked, this, &CommandEditorDialog::onRemoveSelected);
    connect(m_saveBtn, &QPushButton::clicked, this, &CommandEditorDialog::onSave);
    connect(closeBtn, &QPushButton::clicked, this, &CommandEditorDialog::onSave);
    btnRow->addWidget(addBtn);
    btnRow->addWidget(delBtn);
    btnRow->addStretch(1);
    btnRow->addWidget(m_saveBtn);
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    // 加载当前指令表（从面板实际加载的文件读；失败则表格为空、可从头新建）
    if (m_model.load(loadPath))
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

bool CommandEditorDialog::collectAndValidate(QString* err)
{
    QVector<DebugCommandModel::Entry> es;
    static const QRegularExpression digits4(QStringLiteral("^\\d{4}$"));
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
        if (e.group.isEmpty() || e.cmd.name.isEmpty() || e.cmd.send.isEmpty()) {
            *err = QStringLiteral("第 %1 行：分组/名称/发送内容不能为空").arg(r + 1);
            return false;
        }
        if (e.cmd.hasParam && !digits4.match(e.cmd.paramDefault).hasMatch()) {
            *err = QStringLiteral("第 %1 行「%2」：带参数指令的默认参数必须恰好 4 位数字")
                       .arg(r + 1)
                       .arg(e.cmd.name);
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
    if (!m_model.save(m_jsonPath)) {
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
