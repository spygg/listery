#pragma once

#include <QIcon>
#include <QPainter>
#include <QSize>

namespace Branding {

/** 程序图标（矢量绘制，运行时即时生成，可任意尺寸渲染）。 */
QIcon appIcon();

/**
 * 在给定尺寸的画布上绘制程序图标（蓝色渐变圆角方块 + 白色放大镜）。
 * 设计基准为 64×64，调用方负责按目标尺寸缩放，保证任意分辨率下都清晰。
 * 同时被运行时 appIcon() 与 tools/genicon 用来生成文件图标（.ico）复用。
 */
inline void paintListeryIcon(QPainter *painter, const QSize &size)
{
    const qreal s = qMin(size.width(), size.height());
    if (s <= 0)
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->scale(s / 64.0, s / 64.0);

    // 蓝色渐变圆角方块（左上偏亮、右下偏深）
    QLinearGradient gradient(0, 0, 64, 64);
    gradient.setColorAt(0.0, QColor(0x4A, 0x90, 0xFF));
    gradient.setColorAt(1.0, QColor(0x1C, 0x46, 0xC8));

    painter->setPen(Qt::NoPen);
    painter->setBrush(gradient);
    painter->drawRoundedRect(QRectF(2, 2, 60, 60), 14, 14);

    // 白色放大镜：圆框 + 手柄
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(Qt::white, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->drawEllipse(QPointF(27, 27), 11, 11);
    painter->drawLine(QPointF(34.8, 34.8), QPointF(46, 46));

    painter->restore();
}

} // namespace Branding
