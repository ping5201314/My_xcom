#pragma once

// 调试指令编辑对话框（用户反馈：面板上加按键，弹窗管理指令表）。
// 表格编辑：分组/名称/发送内容/带参数/默认参数/参数提示，支持添加、删除、
// 保存到 debug_commands.json；面板在 Accepted 后重新加载指令树。

#include <QDialog>

#include "debugcmd/debugcommand.h"

class QTableWidget;
class QPushButton;
class QCloseEvent;

class CommandEditorDialog : public QDialog
{
    Q_OBJECT
public:
    /// loadPath：读取当前指令表（面板实际加载的文件）；savePath：保存目标
    /// （用户目录，跨构建持久）。关闭（含点 X）= 保存并退出（用户反馈）。
    explicit CommandEditorDialog(const QString& loadPath, const QString& savePath,
                                 QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override; // 关闭即保存，写失败留在窗口

private slots:
    void onAddRow();
    void onRemoveSelected();
    void onSave();

private:
    void fillTable();       // 从 model 填表
    bool collectAndValidate(QString* err); // 表格 → model（校验必填与参数位数）
    bool doSave();          // 校验 + 写盘，失败弹提示返回 false

    QString m_jsonPath; // 保存目标（用户目录）
    DebugCommandModel m_model;
    QTableWidget* m_table = nullptr;
    QPushButton* m_saveBtn = nullptr;
    bool m_saved = false; // 已保存过（防止 保存按钮 accept 后 closeEvent 重复写盘）
};
