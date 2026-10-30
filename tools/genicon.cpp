// 生成 Listery 的文件图标（多分辨率 PNG），用于打包进 .ico。
// 复用 Branding::paintListeryIcon，保证文件图标与运行时图标完全一致。
// 编译（MinGW 例）：
//   g++ genicon.cpp -I.. -I<Qt5 include> -I<Qt5/QtWidgets> -I<Qt5/QtGui> -I<Qt5/QtCore> \
//       -fPIC -std=c++17 $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core) -o genicon
// 运行：./genicon <out_dir>

#include "Platform/Branding.h"

#include <QGuiApplication>
#include <QDir>
#include <QDebug>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    const QString outDir = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QDir().mkpath(outDir);

    // 文件图标常用尺寸（含 256 大图，供任务栏/开始菜单使用）
    const int sizes[] = {16, 24, 32, 40, 48, 64, 96, 128, 256};
    for (int s : sizes) {
        QPixmap pixmap(s, s);
        pixmap.fill(Qt::transparent);
        {
            QPainter painter(&pixmap);
            Branding::paintListeryIcon(&painter, pixmap.size());
        }
        const QString path = QStringLiteral("%1/appicon-%2.png").arg(outDir).arg(s);
        if (!pixmap.save(path, "PNG")) {
            qWarning("无法保存图标: %s", qPrintable(path));
            return 1;
        }
        qInfo("生成 %s", qPrintable(path));
    }
    qInfo("完成，输出目录: %s", qPrintable(outDir));
    return 0;
}
