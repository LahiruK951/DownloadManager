#pragma once
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

// Programmatic app/tray icon: rounded blue tile with a white down arrow.
namespace AppIcon {
inline QPixmap render(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    QLinearGradient g(0, 0, 0, s);
    g.setColorAt(0, QColor(0x4c, 0x8d, 0xff));
    g.setColorAt(1, QColor(0x1f, 0x5f, 0xd6));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(s * 0.04, s * 0.04, s * 0.92, s * 0.92), s * 0.22, s * 0.22);

    QPainterPath arrow;
    arrow.moveTo(s * 0.42, s * 0.20);
    arrow.lineTo(s * 0.58, s * 0.20);
    arrow.lineTo(s * 0.58, s * 0.48);
    arrow.lineTo(s * 0.72, s * 0.48);
    arrow.lineTo(s * 0.50, s * 0.70);
    arrow.lineTo(s * 0.28, s * 0.48);
    arrow.lineTo(s * 0.42, s * 0.48);
    arrow.closeSubpath();
    p.setBrush(Qt::white);
    p.drawPath(arrow);
    p.drawRoundedRect(QRectF(s * 0.26, s * 0.75, s * 0.48, s * 0.07), s * 0.03, s * 0.03);
    return pm;
}

inline QIcon make()
{
    QIcon icon;
    for (int sz : {16, 24, 32, 48, 64, 128, 256})
        icon.addPixmap(render(sz));
    return icon;
}
} // namespace AppIcon
