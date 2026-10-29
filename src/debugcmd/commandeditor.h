#pragma once

// 调试指令编辑对话框（用户反馈：面板上加按键，弹窗管理指令表）。
// 表格编辑：分组/名称/发送内容/带参数/默认参数/参数提示，支持添加、删除、
// 保存到固定位置指令表（.csv 或注入的 .json，按扩展名写）；面板在 Accepted 后重新加载指令树。
// 2026-10-22 用户反馈：去掉顶部保存路径提示（多余的提示）；保存仍写固定位置
// （换电脑流程 = 「加载表格」导入运输文件 → 保存进本机固定位置，面板自动加载）。

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
    /// （固定位置，跨构建持久）。关闭（含点 X）= 保存并退出（用户反馈）。
    explicit CommandEditorDialog(const QString& loadPath, const QString& savePath,
                                 QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override; // 关闭即保存，写失败留在窗口

private slots:
    void onAddRow();
    void onRemoveSelected();
    void onLoadTable(); // 「加载表格」：选 csv/json 全量替换编辑内容（只改内存）
    void onSave();

private:
    void fillTable();       // 从 model 填表
    bool collectAndValidate(QString* err); // 表格 → model（校验必填与参数位数）
    bool doSave();          // 校验 + 写盘，失败弹提示返回 false

    QString m_jsonPath; // 保存目标（固定位置）
    DebugCommandModel m_model;
    QTableWidget* m_table = nullptr;
    QPushButton* m_saveBtn = nullptr;
    bool m_saved = false; // 已保存过（防止 保存按钮 accept 后 closeEvent 重复写盘）
};
