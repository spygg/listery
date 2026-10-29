#include "Platform/Branding.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>

namespace Branding {

QIcon appIcon()
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient gradient(0, 0, 64, 64);
    gradient.setColorAt(0.0, QColor(0x3D, 0x7C, 0xFF));
    gradient.setColorAt(1.0, QColor(0x1C, 0x46, 0xC8));

    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(QRectF(3, 3, 58, 58), 15, 15);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 4.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawEllipse(QPointF(27, 27), 11, 11);
    painter.drawLine(QPointF(35.5, 35.5), QPointF(46, 46));
    painter.end();

    return QIcon(pixmap);
}

} // namespace Branding
