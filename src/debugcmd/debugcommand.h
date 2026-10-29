#pragma once

// 调试指令表：结构与 JSON 加载（第二阶段调试指令面板使用本模型展示/发送）。
// 指令语义见 Finca/APP/Application/debug.c（@R/@W/@C 命令集，USART0 调试口）。

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

/// 单条调试指令
struct DebugCommand {
    QString name;          // 显示名，如“版本信息”
    QString send;          // 发送模板，如 "@R20:0"；带参指令用 "@W%1:0000." 形式占位
    bool hasParam = false;
    QString paramDefault;
    QString paramHint;
};

// 调试面板把指令对象存进 QTreeWidgetItem 的 UserRole，需要元类型注册
Q_DECLARE_METATYPE(DebugCommand)

/// 调试指令表模型：从指令表文件（CSV 为主，JSON 兼容）加载分组与指令
class DebugCommandModel
{
public:
    /// 扁平条目（编辑对话框用）：分组名 + 指令
    struct Entry {
        QString group;
        DebugCommand cmd;
    };

    bool load(const QString& jsonPath);
    bool save(const QString& jsonPath) const; // 按分组重建 JSON 写盘（编辑对话框用）
    // CSV 指令表（2026-10-21 用户拍板，用 Excel 直接编辑）：
    // 6 列「分组,名称,发送内容,带参数,默认参数,参数提示」，带参数写 是/否；
    // 写盘带 UTF-8 BOM（Excel 任意区域双击中文不乱码），读入自动识别
    // UTF-8(BOM/无 BOM)/GBK（Excel 中文系统另存为 CSV 的默认编码）与 RFC4180 引号字段
    bool loadCsv(const QString& csvPath);
    bool saveCsv(const QString& csvPath) const;
    QStringList groups() const;
    QVector<DebugCommand> commandsOf(const QString& group) const;
    QVector<Entry> entries() const;              // 全量导出（含分组名）
    void setEntries(const QVector<Entry>& es);   // 全量替换（编辑后回写，再 save）

private:
    struct Group {
        QString name;
        QVector<DebugCommand> commands;
    };
    QVector<Group> m_groups;
};
