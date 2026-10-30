#include "Platform/Branding.h"

#include <QPixmap>

namespace Branding {

QIcon appIcon()
{
    // 预渲染多个原生尺寸，保证窗口/托盘在任意 DPI 下都清晰（不依赖运行时缩放）
    static const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256, 512};
    QIcon icon;
    for (int s : sizes) {
        QPixmap pixmap(s, s);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paintListeryIcon(&painter, pixmap.size());
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace Branding
