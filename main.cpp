// my_com 串口助手入口。
// QApplication 的 org/app 元数据必须在创建任何 QSettings（AppConfig）之前设置，
// 否则 ini 会写到错误位置。
#include <QApplication>
#include <QIcon>
#include "mainwindow.h"

namespace {
// 全局配色：与图标一致的深蓝主色 + 橙色点缀。控件动态属性选择器：
// QPushButton[primary="true"]（主操作，深蓝底）/ [danger="true"]（危险操作，红底）
const char kAppStyle[] = R"(
* { font-family: "Microsoft YaHei UI", "Segoe UI", "PingFang SC", sans-serif; font-size: 13px; }
QMainWindow, QDialog { background: #EEF1F6; }
QTabWidget::pane { border: 1px solid #D5DBE5; background: #FFFFFF; border-radius: 6px; }
QTabBar::tab { background: #E4E9F2; padding: 8px 22px; margin-right: 3px;
    border-top-left-radius: 6px; border-top-right-radius: 6px; color: #55607A; }
QTabBar::tab:selected { background: #FFFFFF; color: #2C4C8C; font-weight: bold;
    border-bottom: 2px solid #2C4C8C; }
QTabBar::tab:hover:!selected { color: #2C4C8C; }
QGroupBox { background: #FFFFFF; border: 1px solid #D5DBE5; border-radius: 8px;
    margin-top: 14px; padding-top: 6px; font-weight: bold; color: #33415C; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }
QPushButton { background: #FFFFFF; border: 1px solid #C3CBD8; border-radius: 5px;
    padding: 6px 14px; color: #33415C; }
QPushButton:hover { border-color: #2C4C8C; color: #2C4C8C; }
QPushButton:pressed { background: #E8EDF5; }
QPushButton:disabled { color: #A8B0C0; background: #F0F3F8; border-color: #DDE2EC; }
QPushButton[primary="true"] { background: #2C4C8C; color: #FFFFFF; border: 1px solid #2C4C8C;
    font-weight: bold; }
QPushButton[primary="true"]:hover { background: #3A5DA8; border-color: #3A5DA8; color: #FFFFFF; }
QPushButton[primary="true"]:disabled { background: #9AA8C8; border-color: #9AA8C8; color: #E8ECF5; }
QPushButton[danger="true"] { background: #C0392B; color: #FFFFFF; border: 1px solid #C0392B; }
QPushButton[danger="true"]:hover { background: #D44637; border-color: #D44637; color: #FFFFFF; }
QPushButton[danger="true"]:disabled { background: #DCA7A0; border-color: #DCA7A0; color: #F5E9E8; }
QComboBox, QSpinBox, QLineEdit { background: #FFFFFF; border: 1px solid #C3CBD8;
    border-radius: 5px; padding: 4px 8px; color: #33415C; }
QComboBox:hover, QSpinBox:hover { border-color: #8FA0C0; }
QComboBox:focus, QSpinBox:focus { border-color: #2C4C8C; }
QComboBox QAbstractItemView { background: #FFFFFF; border: 1px solid #C3CBD8;
    selection-background-color: #2C4C8C; selection-color: #FFFFFF; }
QPlainTextEdit { background: #FFFFFF; border: 1px solid #C3CBD8; border-radius: 6px;
    color: #33415C; }
QPlainTextEdit:focus { border-color: #2C4C8C; }
QCheckBox, QLabel { color: #33415C; }
QStatusBar { background: #E4E9F2; color: #55607A; }
QStatusBar::item { border: none; }
QToolBar { background: #FFFFFF; border: none; border-bottom: 1px solid #D5DBE5;
    spacing: 6px; padding: 4px 8px; }
QToolBar QToolButton { background: transparent; border: 1px solid transparent;
    border-radius: 5px; padding: 5px 12px; color: #33415C; }
QToolBar QToolButton:hover { background: #E8EDF5; color: #2C4C8C; }
QToolBar QToolButton:checked { background: #2C4C8C; color: #FFFFFF; }
QDockWidget { titlebar-close-icon: none; }
QDockWidget::title { background: #E4E9F2; padding: 6px 12px; color: #33415C;
    font-weight: bold; }
QProgressBar { border: 1px solid #C3CBD8; border-radius: 5px; text-align: center;
    background: #FFFFFF; }
QProgressBar::chunk { background: #2C4C8C; border-radius: 4px; }
#sidebar { background: #F2F5FA; border-right: 1px solid #D5DBE5; }
)";
} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("MyTeam"));
    QApplication::setApplicationName(QStringLiteral("my_com"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    app.setStyleSheet(QString::fromLatin1(kAppStyle));
    // 窗口/任务栏图标;exe 文件图标由 my_com.rc 提供
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/my_com.png")));

    MainWindow w;
    w.show();
    return app.exec();
}
