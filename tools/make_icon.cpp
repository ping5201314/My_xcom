// make_icon.cpp — my_com 应用图标生成工具
// 用法: make_icon.exe <输出目录>
// 产出: <输出目录>/my_com.png (256x256) + my_com.ico (16/24/32/48/64/128/256 多尺寸)
// 设计: 深蓝圆角底 + 咖啡杯 + 信号弧 —— "给咖啡机的串口命令"
// 重生成: g++ 本文件链接 Qt6Gui/Qt6Core 后运行即可 (无 UI 依赖, 不需要 QApplication)
#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QVector>
#include <cstdio>

namespace {

// 画 256x256 主图
QImage renderMaster()
{
    const int S = 256;
    QImage img(S, S, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    // ---- 背景: 深蓝圆角方块, 纵向渐变 ----
    QLinearGradient bg(0, 0, 0, S);
    bg.setColorAt(0.0, QColor("#2C4C8C"));
    bg.setColorAt(1.0, QColor("#152A52"));
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, 0, S, S), 60, 60);

    // ---- 信号弧 (右上): 圆点 + 3 条弧, 橙色 ----
    const QPointF dot(192, 48);
    p.setBrush(QColor("#FFB15C"));
    p.drawEllipse(dot, 10, 10);
    QPen arcPen(QColor("#FF9F43"), 12, Qt::SolidLine, Qt::RoundCap);
    p.setPen(arcPen);
    p.setBrush(Qt::NoBrush);
    const qreal radii[3] = {27.0, 48.0, 69.0};
    for (qreal r : radii)
        p.drawArc(QRectF(dot.x() - r, dot.y() - r, 2 * r, 2 * r), 45 * 16, 90 * 16);

    // ---- 碟 (先画, 被杯身压住上沿) ----
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#EFE6D4"));
    p.drawEllipse(QPointF(108, 208), 64, 17);
    p.setBrush(QColor("#D9CDB4"));
    p.drawEllipse(QPointF(108, 211), 64, 15);

    // ---- 杯把 (右侧圆环段) ----
    QPen handlePen(QColor("#F8F2E7"), 14, Qt::SolidLine, Qt::RoundCap);
    p.setPen(handlePen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(168 - 30, 138 - 30, 60, 60), 20 * 16, 280 * 16);

    // ---- 杯身: 顶窄底宽的圆角梯形 ----
    QPainterPath cup;
    cup.moveTo(56, 106);            // 左上
    cup.quadTo(56, 98, 64, 98);     // 左上圆角
    cup.lineTo(152, 98);            // 顶边
    cup.quadTo(160, 98, 160, 106);  // 右上圆角
    cup.lineTo(160, 178);           // 右侧直下
    cup.quadTo(160, 200, 138, 200); // 右下大圆角
    cup.lineTo(78, 200);
    cup.quadTo(56, 200, 56, 178);   // 左下大圆角
    cup.closeSubpath();
    p.setBrush(QColor("#F8F2E7"));
    p.setPen(QPen(QColor("#D9CDB4"), 3));
    p.drawPath(cup);

    // ---- 咖啡液面 ----
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#5C4030"));
    p.drawEllipse(QPointF(108, 106), 51, 12);
    p.setBrush(QColor("#6E5240"));
    p.drawEllipse(QPointF(108, 104), 33, 7);

    // ---- 蒸汽: 两笔淡白曲线 (左上, 半透明) ----
    QPen steamPen(QColor(255, 255, 255, 130), 7, Qt::SolidLine, Qt::RoundCap);
    p.setPen(steamPen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(78 - 16, 56 - 16, 32, 32), 60 * 16, 120 * 16);
    p.drawArc(QRectF(112 - 20, 48 - 20, 40, 40), 50 * 16, 130 * 16);

    p.end();
    return img;
}

// 多尺寸 ICO: PNG 内嵌 (Vista+ 支持), 宽/高 256 记 0
bool writeIco(const QImage& master, const QString& path)
{
    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    struct Entry { QByteArray png; int size; };
    QVector<Entry> entries;
    for (int s : sizes) {
        QImage scaled = master.scaled(s, s, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (!scaled.save(&buf, "PNG"))
            return false;
        entries.append({png, s});
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    QDataStream out(&f);
    out.setByteOrder(QDataStream::LittleEndian);
    out << quint16(0) << quint16(1) << quint16(entries.size());  // 保留/类型/数量
    quint32 offset = 6 + 16 * entries.size();
    for (const Entry& e : entries) {
        out << quint8(e.size == 256 ? 0 : e.size) << quint8(e.size == 256 ? 0 : e.size);
        out << quint8(0) << quint8(0);        // 调色板数/保留
        out << quint16(1) << quint16(32);     // 平面数/位深
        out << quint32(e.png.size()) << quint32(offset);
        offset += e.png.size();
    }
    for (const Entry& e : entries)
        f.write(e.png);
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    const QImage master = renderMaster();
    bool ok = master.save(dir + "/my_com.png", "PNG");
    ok = writeIco(master, dir + "/my_com.ico") && ok;
    if (!ok) {
        std::fprintf(stderr, "make_icon: 写出失败, 目标目录 %s\n", qPrintable(dir));
        return 1;
    }
    std::printf("make_icon: %s/my_com.png + my_com.ico (7 sizes) 生成完毕\n",
                qPrintable(dir));
    return 0;
}
