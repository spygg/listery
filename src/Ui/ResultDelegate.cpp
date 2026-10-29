#include "Ui/ResultDelegate.h"

#include "Ui/ResultModel.h"

#include <QApplication>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionViewItem>

namespace {
const int kRowHeight = 54;
const int kIconSize = 32;

QFileIconProvider *iconProvider()
{
    static QFileIconProvider provider;
    return &provider;
}

QIcon iconForPath(const QString &path)
{
    static QHash<QString, QIcon> cache;
    const auto it = cache.constFind(path);
    if (it != cache.constEnd())
        return it.value();

    QIcon icon;
    if (!path.isEmpty())
        icon = iconProvider()->icon(QFileInfo(path));
    if (icon.isNull())
        icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    if (cache.size() > 400)
        cache.clear();
    cache.insert(path, icon);
    return icon;
}

int textWidth(const QFontMetrics &metrics, const QString &text)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    return metrics.horizontalAdvance(text);
#else
    return metrics.width(text);
#endif
}

void drawHighlightedText(QPainter *painter,
                         const QRect &rect,
                         const QString &text,
                         const QVector<int> &positions,
                         const QColor &baseColor,
                         const QColor &highlightColor,
                         const QFont &font)
{
    const QFontMetrics metrics(font);
    painter->setFont(font);

    if (textWidth(metrics, text) > rect.width() || positions.isEmpty()) {
        painter->setPen(baseColor);
        painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter,
                          metrics.elidedText(text, Qt::ElideRight, rect.width()));
        return;
    }

    int x = rect.left();
    const int baseline = rect.top() + (rect.height() + metrics.ascent() - metrics.descent()) / 2;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        painter->setPen(positions.contains(i) ? highlightColor : baseColor);
        painter->drawText(QPoint(x, baseline), QString(ch));
        x += textWidth(metrics, QString(ch));
    }
}
} // namespace

ResultDelegate::ResultDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void ResultDelegate::paint(QPainter *painter,
                           const QStyleOptionViewItem &option,
                           const QModelIndex &index) const
{
    const ResultModel *model = qobject_cast<const ResultModel *>(index.model());
    if (!model)
        return;

    const SearchResult &result = model->resultAt(index.row());
    const QRect rowRect = option.rect;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (option.state & QStyle::State_Selected) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xE7, 0xF0, 0xFE));
        painter->drawRoundedRect(rowRect.adjusted(8, 3, -8, -3), 9, 9);
    } else if (option.state & QStyle::State_MouseOver) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0xF4, 0xF6, 0xF9));
        painter->drawRoundedRect(rowRect.adjusted(8, 3, -8, -3), 9, 9);
    }

    const int iconLeft = rowRect.left() + 20;
    const QRect iconRect(iconLeft, rowRect.top() + (rowRect.height() - kIconSize) / 2,
                         kIconSize, kIconSize);

    const bool pluginRow = result.isPluginResult();
    if (pluginRow) {
        // 插件行没有文件图标：画一个带徽标文字的圆角色块（如“译”）
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x1A, 0x6C, 0xF0));
        painter->drawRoundedRect(iconRect, 8, 8);
        QFont badgeFont(option.font);
        badgeFont.setPixelSize(14);
        badgeFont.setWeight(QFont::Bold);
        painter->setFont(badgeFont);
        painter->setPen(Qt::white);
        painter->drawText(iconRect, Qt::AlignCenter,
                          result.pluginBadge.isEmpty() ? QStringLiteral("·") : result.pluginBadge);
    } else {
        iconForPath(result.record.path).paint(painter, iconRect, Qt::AlignCenter,
                                              QIcon::Normal, QIcon::Off);
    }

    const int textLeft = iconRect.right() + 14;
    const int rightReserved = result.record.launchCount > 0 ? 96 : 28;
    const int textWidthAvailable = qMax(60, rowRect.width() - textLeft - rightReserved);

    QFont nameFont(option.font);
    nameFont.setPixelSize(15);
    nameFont.setWeight(QFont::DemiBold);

    const QString title = pluginRow ? result.pluginTitle : result.record.displayName;
    const QRect nameRect(textLeft, rowRect.top() + 6, textWidthAvailable, 22);
    drawHighlightedText(painter, nameRect, title, pluginRow ? QVector<int>() : result.highlights,
                        QColor(0x1F, 0x23, 0x29), QColor(0x1A, 0x6C, 0xF0), nameFont);

    QFont pathFont(option.font);
    pathFont.setPixelSize(12);
    const QFontMetrics pathMetrics(pathFont);
    painter->setFont(pathFont);
    painter->setPen(QColor(0x8A, 0x91, 0x9B));
    const QRect pathRect(textLeft, rowRect.top() + 27, textWidthAvailable, 18);
    const QString subtitle = pluginRow
                                 ? result.pluginSubtitle
                                 : QDir::toNativeSeparators(QFileInfo(result.record.path).absolutePath());
    painter->drawText(pathRect, Qt::AlignLeft | Qt::AlignVCenter,
                      pathMetrics.elidedText(subtitle, Qt::ElideMiddle, pathRect.width()));

    if (pluginRow) {
        // 右侧提示这一行回车会做什么
        QFont actionFont(option.font);
        actionFont.setPixelSize(11);
        painter->setFont(actionFont);
        painter->setPen(QColor(0x9A, 0xA1, 0xAB));
        const QRect actionRect(rowRect.right() - 108, rowRect.top(), 92, rowRect.height());
        painter->drawText(actionRect, Qt::AlignRight | Qt::AlignVCenter,
                          result.pluginAction.startsWith(QStringLiteral("copy:"))
                              ? QStringLiteral("Enter 复制")
                              : QString());
        painter->setPen(QColor(0x8A, 0x91, 0x9B));
    }

    if (!pluginRow && result.record.launchCount > 0) {
        QFont usageFont(option.font);
        usageFont.setPixelSize(11);
        painter->setFont(usageFont);
        painter->setPen(QColor(0x9A, 0xA1, 0xAB));
        const QRect usageRect(rowRect.right() - 92, rowRect.top(), 76, rowRect.height());
        painter->drawText(usageRect, Qt::AlignRight | Qt::AlignVCenter,
                          QStringLiteral("%1 次").arg(result.record.launchCount));
    }

    painter->restore();
}

QSize ResultDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)
    return QSize(0, kRowHeight);
}
