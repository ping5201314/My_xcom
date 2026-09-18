#include "pages/upgradepage.h"

#include "ui_upgradepage.h" // AUTOUIC 由 upgradepage.ui 生成

#include "serial/serialport.h"
#include "upgrade/upgradeengine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>

namespace {
constexpr int kLogMaxLines = 5000; // 日志行数上限（B1 日志是排障命脉，保留足够上下文）
} // namespace

UpgradePage::UpgradePage(SerialPort* port, QWidget* parent)
    : QWidget(parent)
    , m_port(port)
    , ui(new Ui::UpgradePage)
{
    // .ui 只搭静态骨架；setupUi 创建控件并按 objectName 存进 ui->xxx。
    // 把成员指针别名到 ui 里的控件，后续动态逻辑沿用 m_ 前缀（改动最小）
    ui->setupUi(this);
    m_fileEdit = ui->m_fileEdit;
    m_browseBtn = ui->m_browseBtn;
    m_fileInfo = ui->m_fileInfo;
    m_startBtn = ui->m_startBtn;
    m_cancelBtn = ui->m_cancelBtn;
    m_progress = ui->m_progress;
    m_phaseLabel = ui->m_phaseLabel;
    m_banner = ui->m_banner;
    m_logView = ui->m_logView;
    m_saveLogBtn = ui->m_saveLogBtn;
    // 主题色动态属性放代码里设置（Designer 重新保存 .ui 时自定义属性可能丢失）
    m_startBtn->setProperty("primary", true);
    m_cancelBtn->setProperty("danger", true);

    // 按钮连接放代码里（Qt6 uic 会把 .ui 的 connections 生成为 &QWidget::xxx
    // 编译期函数指针，槽在子类里会编译不过，见 upgradepage.ui 的说明）
    connect(m_browseBtn, &QPushButton::clicked, this, &UpgradePage::onBrowse);
    connect(m_startBtn, &QPushButton::clicked, this, &UpgradePage::onStart);
    connect(m_cancelBtn, &QPushButton::clicked, this, &UpgradePage::onCancel);
    connect(m_saveLogBtn, &QPushButton::clicked, this, &UpgradePage::onSaveLog);

    // 引擎自持工作线程；信号跨线程（自动队列）接回 UI
    m_engine = new UpgradeEngine(port);
    connect(m_engine, &UpgradeEngine::phaseChanged, this, &UpgradePage::onPhaseChanged);
    connect(m_engine, &UpgradeEngine::progressChanged, this, &UpgradePage::onProgressChanged);
    connect(m_engine, &UpgradeEngine::logMessage, this, &UpgradePage::onLog);
    connect(m_engine, &UpgradeEngine::finished, this, &UpgradePage::onFinished);
    connect(m_engine, &UpgradeEngine::busyChanged, this, &UpgradePage::onBusyChanged);

    // 恢复上次的文件路径
    m_lastFilePath = m_config.stringValue(AppConfig::kUpgradeFile);
    if (!m_lastFilePath.isEmpty())
        m_fileEdit->setText(m_lastFilePath);
    updateFileInfo();
}

UpgradePage::~UpgradePage()
{
    // 先删引擎：其析构会请求取消并等工作线程退出，之后串口才可能被销毁（端口由主窗口持有）
    delete m_engine;
    m_engine = nullptr;
    // ui 只负责释放自己的结构体；控件本身由 Qt 父子关系随页面销毁
    delete ui;
    ui = nullptr;
}

void UpgradePage::onBrowse()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择固件文件"), m_lastFilePath, QStringLiteral("固件镜像 (*.bin);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    m_lastFilePath = path;
    m_fileEdit->setText(path);
    updateFileInfo();
}

void UpgradePage::updateFileInfo()
{
    const QString path = m_fileEdit->text();
    if (path.isEmpty()) {
        m_fileInfo->setText(QStringLiteral("<span style='color:#e74c3c;'>未选择文件</span>"));
        return;
    }
    const QFileInfo fi(path);
    QStringList parts;
    QStringList bad;

    // 存在性
    if (!fi.exists() || !fi.isFile())
        bad << QStringLiteral("文件不存在或不可读");
    // 名称长度：超 31 字节不再拒绝（用户反馈）——发送时自动截断为安全名
    //（文件名不参与写 Flash，05c C3），这里只给橙色提示
    const QString base = fi.fileName();
    const int nameBytes = base.toUtf8().size();
    bool nameTooLong = false;
    if (base.isEmpty())
        bad << QStringLiteral("文件名为空");
    else if (nameBytes > UpgradeEngine::kMaxFileNameLen)
        nameTooLong = true;
    // 大小（A6：≤120K，122K 硬上限）
    const qint64 size = fi.size();
    if (size <= 0)
        bad << QStringLiteral("文件为空");
    else if (size > UpgradeEngine::kMaxFileSize)
        bad << QStringLiteral("文件过大（%1 字节，需 ≤120K；122K 是设备硬上限）").arg(size);

    parts << QStringLiteral("<b>%1</b>").arg(base.toHtmlEscaped());
    if (size > 0)
        parts << QStringLiteral("%1 字节（%2 块）").arg(size).arg((size + 127) / 128);
    if (nameTooLong)
        parts << QStringLiteral("<span style='color:#e67e22;'>文件名超长（%1 字节），发送时自动截断，不影响升级</span>")
                     .arg(nameBytes);
    if (bad.isEmpty())
        parts << QStringLiteral("<span style='color:#27ae60;'>校验通过 ✓</span>");
    else
        for (const QString& b : bad)
            parts << QStringLiteral("<span style='color:#e74c3c;'>✗ %1</span>").arg(b.toHtmlEscaped());
    m_fileInfo->setText(parts.join(QStringLiteral("　|　")));
}

void UpgradePage::onStart()
{
    if (m_busy)
        return; // 忙（含冷却）：按钮本已禁用，双保险
    const QString path = m_fileEdit->text();
    // 页面前置校验：与引擎重复校验保持一致（引擎仍会复核，双保险）
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists() || !fi.isFile()) {
        setBanner(false, QStringLiteral("请先选择有效的 .bin 固件文件。"));
        return;
    }
    if (fi.size() <= 0 || fi.size() > UpgradeEngine::kMaxFileSize) {
        setBanner(false,
                  QStringLiteral("文件大小 %1 字节，超出允许范围（≤120K，122K 是设备硬上限）。")
                      .arg(fi.size()));
        return;
    }
    // 文件名超长不再拦截：引擎/发送器会截断头包文件名（不影响升级，05c C3）
    saveSettings();
    setBanner(true, QString()); // 隐藏旧横幅
    appendLog(QStringLiteral("========== 开始升级请求 =========="));
    // 引擎在工作线程：队列调用 start，绝不阻塞 UI。
    // 注意：参数必须在 UI 线程取值后传值捕获 —— lambda 在工作线程执行，绝不能碰 QWidget。
    const QString lastPort = m_config.stringValue(AppConfig::kLastPort);
    QMetaObject::invokeMethod(
        m_engine,
        [engine = m_engine, path, lastPort] { engine->start(path, lastPort); },
        Qt::QueuedConnection);
}

void UpgradePage::onCancel()
{
    if (!m_busy)
        return;
    appendLog(QStringLiteral("发送取消请求…（传输中则 ≤1s 内生效；取消后需等 4s 冷却）"));
    // 直接调用（引擎 slot 线程安全：仅原子标志 + sender.abort）；若走队列，
    // 工作线程阻塞在 run() 里时永远收不到，必须直连语义。
    m_engine->cancel();
    m_cancelBtn->setEnabled(false); // 防重复点击；busyChanged(false) 时恢复
}

void UpgradePage::onPhaseChanged(const QString& phase)
{
    m_phaseLabel->setText(phase);
}

void UpgradePage::onProgressChanged(int done, int total)
{
    if (total < 0) {
        // 忙碌型阶段（等待/复位/冷却）：range(0,0) 忙碌样式
        m_progress->setRange(0, 0);
        m_progress->setFormat(QStringLiteral("等待中…"));
        return;
    }
    m_progress->setRange(0, total);
    m_progress->setValue(done);
    m_progress->setFormat(QStringLiteral("%v/%m 块 (%p%)"));
}

void UpgradePage::onLog(const QString& msg)
{
    appendLog(msg);
}

void UpgradePage::onFinished(bool ok, const QString& reason)
{
    setBanner(ok, reason);
    appendLog(QStringLiteral("========== %1 ==========")
                  .arg(ok ? QStringLiteral("升级成功") : QStringLiteral("升级结束")));
    appendLog(reason);
    // 用户要求：不再自动保存日志——需要时点「保存日志」按钮自己选位置存
}

void UpgradePage::onSaveLog()
{
    // 手动保存过程日志：文件对话框自选位置（用户反馈：自己来保存，不要软件自动存）
    const QString def = QStringLiteral("mycom_upgrade_%1.log")
                            .arg(QDateTime::currentDateTime().toString(
                                QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存升级日志"), def, QStringLiteral("日志文件 (*.log);;文本文件 (*.txt)"));
    if (path.isEmpty())
        return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QStringLiteral("警告：日志保存失败（%1）：%2").arg(path, f.errorString()));
        return;
    }
    f.write(m_logView->toPlainText().toUtf8());
    appendLog(QStringLiteral("升级日志已保存：%1").arg(path));
}

void UpgradePage::onBusyChanged(bool busy)
{
    m_busy = busy;
    m_startBtn->setEnabled(!busy);
    m_browseBtn->setEnabled(!busy);
    m_fileEdit->setEnabled(!busy);
    m_cancelBtn->setEnabled(busy);
    if (!busy)
        m_progress->setRange(0, 0); // 回到待命忙碌样式
    emit upgradeBusyChanged(busy);
}

void UpgradePage::appendLog(const QString& line)
{
    // 上限 5000 行（setMaximumBlockCount 自动裁掉最旧块）+ 自动滚动（日志是排障命脉，B1）
    m_logView->appendPlainText(line);
    m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
}

void UpgradePage::setBanner(bool ok, const QString& text)
{
    if (text.isEmpty()) {
        m_banner->hide();
        return;
    }
    m_banner->setText(text);
    // 成功绿 / 失败红（含原因与建议），白字醒目
    m_banner->setStyleSheet(ok
        ? QStringLiteral("background:#27ae60;color:white;padding:10px;border-radius:6px;font-weight:bold;")
        : QStringLiteral("background:#e74c3c;color:white;padding:10px;border-radius:6px;font-weight:bold;"));
    m_banner->show();
}

void UpgradePage::saveSettings()
{
    m_config.setValue(AppConfig::kUpgradeFile, m_fileEdit->text());
}
