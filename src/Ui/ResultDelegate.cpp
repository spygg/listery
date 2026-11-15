#include "Ui/ResultDelegate.h"

#include "Core/QtCompat.h"
#include "Ui/ResultColumns.h"
#include "Ui/ResultModel.h"

#include <QApplication>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHash>
#include <QIcon>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
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

/**
 * 结果行的目录文案。
 * 索引库里已经存了原生分隔符的 dir，直接用即可：重绘时对每一行构造 QFileInfo
 * 会去 stat 一次磁盘（可能还是慢速网络盘），列表滚动就会被拖慢。
 */
QString rowDirectory(const ExecutableRecord &record)
{
    if (!record.dir.isEmpty())
        return record.dir;

    const int slash = qMax(record.path.lastIndexOf(QLatin1Char('/')),
                           record.path.lastIndexOf(QLatin1Char('\\')));
    return slash > 0 ? record.path.left(slash) : QString();
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
    // 按「连续高亮 / 连续普通」切成若干段整段绘制。逐字符 drawText 的话一行 30 个字
    // 就是 30 次绘制调用 + 30 个临时 QString，列表滚动时这一笔开销很显眼。
    int i = 0;
    while (i < text.size()) {
        const bool highlighted = positions.contains(i);
        int end = i + 1;
        while (end < text.size() && positions.contains(end) == highlighted)
            ++end;
        const QString run = text.mid(i, end - i);
        painter->setPen(highlighted ? highlightColor : baseColor);
        painter->drawText(QPoint(x, baseline), run);
        x += textWidth(metrics, run);
        i = end;
    }
}
} // namespace

// ============================ 列视图共用几何 ============================

QVector<ResultColumn> parseColumnSpec(const QString &spec)
{
    QVector<ResultColumn> columns;
    if (spec.isEmpty())
        return columns;

    const QStringList parts =
        spec.split(QChar(0x1F), LISTERY_SKIP_EMPTY_PARTS);
    for (const QString &part : parts) {
        const QStringList fields =
            part.split(QLatin1Char(':'), LISTERY_KEEP_EMPTY_PARTS);
        if (fields.size() < 3 || fields.at(0).isEmpty())
            continue;
        ResultColumn column;
        column.label = fields.at(0);
        column.width = qMax(0, fields.at(1).toInt());
        column.rightAligned =
            fields.at(2).trimmed().compare(QLatin1String("r"), Qt::CaseInsensitive) == 0;
        // 第 4 段是排序键（可选），第 5 段是当前排序方向（可选）：
        // 老插件只写 3 段，解析后 sortKey 为空，表头保持只读。
        if (fields.size() >= 4)
            column.sortKey = fields.at(3).trimmed();
        if (fields.size() >= 5) {
            const QString order = fields.at(4).trimmed().toLower();
            if (order == QLatin1String("asc"))
                column.sortOrder = 1;
            else if (order == QLatin1String("desc"))
                column.sortOrder = 2;
        }
        columns.append(column);
    }
    return columns;
}

QVector<QRect> resultColumnRects(const QRect &rowRect,
                                 const QVector<ResultColumn> &columns)
{
    QVector<QRect> rects;
    const int count = columns.size();
    if (count <= 0)
        return rects;

    // 左侧留一小块徽标，右侧留出边距；列间固定间隙
    const int left = rowRect.left() + ResultLayout::kColumnLeftInset
                     + ResultLayout::kColumnBadgeSize + ResultLayout::kColumnGap;
    const int right = rowRect.right() - ResultLayout::kColumnRightInset;
    const int budget = qMax(0, right - left
                           - (count - 1) * ResultLayout::kColumnGap);

    QVector<int> widths(count, 0);
    int fixedTotal = 0;
    int flexCount = 0;
    for (int i = 0; i < count; ++i) {
        widths[i] = qMax(0, columns.at(i).width);
        if (widths[i] == 0)
            ++flexCount;
        else
            fixedTotal += widths[i];
    }

    // 面板比固定列之和还窄（小屏/多列）：按比例压缩固定列，而不是让后面的列跑出面板
    if (fixedTotal > budget && fixedTotal > 0) {
        const double scale = static_cast<double>(budget) / static_cast<double>(fixedTotal);
        fixedTotal = 0;
        for (int i = 0; i < count; ++i) {
            if (widths[i] > 0) {
                widths[i] = qMax(24, static_cast<int>(widths[i] * scale));
                fixedTotal += widths[i];
            }
        }
    }
    const int flexWidth = flexCount > 0
        ? qMax(24, (budget - fixedTotal) / flexCount)
        : 0;

    int x = left;
    for (int i = 0; i < count; ++i) {
        const int width = widths[i] == 0 ? flexWidth : widths[i];
        // 每列左右各留 4px 内边距：右对齐的数字列和下一列左对齐文字之间
        // 才不会只隔一个列间距、看起来粘在一起。表头与数据行共用这里，保持一致。
        rects.append(QRect(x, rowRect.top(), width, rowRect.height()).adjusted(4, 0, -4, 0));
        x += width + ResultLayout::kColumnGap;
    }
    return rects;
}

ResultColumnHeader::ResultColumnHeader(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("resultColumnHeader"));
    setFixedHeight(ResultLayout::kColumnHeaderHeight);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
}

void ResultColumnHeader::setColumns(const QVector<ResultColumn> &columns)
{
    m_columns = columns;
    // 列定义每次查询都会重建：插件用第 4/5 段回传当前排序列与方向，
    // 表头据此直接画箭头（不依赖调用方再补一次 setSortIndicator）。
    QString indicatedKey;
    bool indicatedAscending = true;
    for (const ResultColumn &column : m_columns) {
        if (column.sortOrder == 1 || column.sortOrder == 2) {
            indicatedKey = column.sortKey;
            indicatedAscending = column.sortOrder == 1;
            break;
        }
    }
    bool keyStillPresent = false;
    for (const ResultColumn &column : m_columns) {
        if (!m_sortKey.isEmpty() && column.sortKey == m_sortKey)
            keyStillPresent = true;
    }
    if (!indicatedKey.isEmpty()) {
        m_sortKey = indicatedKey;
        m_ascending = indicatedAscending;
    } else if (!keyStillPresent) {
        m_sortKey.clear();
    }
    m_hoverColumn = -1;
    unsetCursor();
    update();
}

void ResultColumnHeader::setSortIndicator(const QString &sortKey, bool ascending)
{
    m_sortKey = sortKey;
    m_ascending = ascending;
    update();
}

QSize ResultColumnHeader::sizeHint() const
{
    return QSize(0, ResultLayout::kColumnHeaderHeight);
}

int ResultColumnHeader::columnAt(const QPoint &position) const
{
    const QVector<QRect> cells = resultColumnRects(rect(), m_columns);
    for (int i = 0; i < cells.size(); ++i) {
        if (cells.at(i).contains(position))
            return i;
    }
    return -1;
}

bool ResultColumnHeader::sortableAt(int column) const
{
    return column >= 0 && column < m_columns.size()
           && !m_columns.at(column).sortKey.isEmpty();
}

void ResultColumnHeader::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(0xFA, 0xFB, 0xFD));
    painter.setPen(QColor(0xE9, 0xED, 0xF3));
    painter.drawLine(rect().left(), rect().bottom(), rect().right(), rect().bottom());

    const QVector<QRect> cells = resultColumnRects(rect(), m_columns);
    if (cells.isEmpty())
        return;

    QFont font = painter.font();
    font.setPixelSize(11);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    const QFontMetrics metrics(font);

    for (int i = 0; i < cells.size() && i < m_columns.size(); ++i) {
        const ResultColumn &column = m_columns.at(i);
        const bool sortable = sortableAt(i);
        const bool active = sortable && column.sortKey == m_sortKey;
        const bool hovered = sortable && i == m_hoverColumn;

        if (hovered || (active && m_pressedColumn == i)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0xEE, 0xF3, 0xFB));
            painter.drawRoundedRect(cells.at(i).adjusted(0, 3, 0, -3), 6, 6);
        }

        QRect textRect = cells.at(i);
        if (sortable)
            textRect.setRight(textRect.right() - 14);

        painter.setPen(active ? QColor(0x1A, 0x6C, 0xF0)
                             : (hovered ? QColor(0x1F, 0x23, 0x29)
                                        : QColor(0x8A, 0x91, 0x9B)));
        const QString text =
            metrics.elidedText(column.label, Qt::ElideRight, textRect.width());
        const int flags = Qt::AlignVCenter
                          | (column.rightAligned ? Qt::AlignRight : Qt::AlignLeft);
        painter.drawText(textRect, flags, text);

        if (active) {
            // Everything 风格的排序箭头：升序 ▲、降序 ▼，画在列内右端。
            const int cx = cells.at(i).right() - 7;
            const int cy = cells.at(i).center().y();
            QPolygon arrow;
            if (m_ascending)
                arrow << QPoint(cx, cy - 4) << QPoint(cx - 4, cy + 2) << QPoint(cx + 4, cy + 2);
            else
                arrow << QPoint(cx, cy + 3) << QPoint(cx - 4, cy - 3) << QPoint(cx + 4, cy - 3);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x1A, 0x6C, 0xF0));
            painter.drawPolygon(arrow);
        }
    }
}

void ResultColumnHeader::mouseMoveEvent(QMouseEvent *event)
{
    const int column = columnAt(event->pos());
    if (column != m_hoverColumn) {
        m_hoverColumn = column;
        update();
    }
    if (sortableAt(column))
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
    QWidget::mouseMoveEvent(event);
}

void ResultColumnHeader::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressedColumn = columnAt(event->pos());
        update();
    }
    QWidget::mousePressEvent(event);
}

void ResultColumnHeader::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int column = columnAt(event->pos());
        const int pressed = m_pressedColumn;
        m_pressedColumn = -1;
        update();
        if (column >= 0 && column == pressed && sortableAt(column))
            emit columnClicked(column);
    }
    QWidget::mouseReleaseEvent(event);
}

void ResultColumnHeader::leaveEvent(QEvent *event)
{
    m_hoverColumn = -1;
    unsetCursor();
    update();
    QWidget::leaveEvent(event);
}

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

    // 列视图（Everything 风格）：整张表一旦启用列定义，所有插件行都用紧凑单行样式，
    // 命令类结果没有单元格数据时退化成「标题 / 副标题」占前两列，避免行高与绘制不一致。
    if (pluginRow && model->hasColumns()) {
        QStringList cells = result.pluginCells;
        if (cells.isEmpty()) {
            cells << result.pluginTitle << result.pluginSubtitle;
            while (cells.size() < model->columns().size())
                cells << QString();
        }

        const int badgeTop = rowRect.top() + (rowRect.height() - ResultLayout::kColumnBadgeSize) / 2;
        const QRect badgeRect(rowRect.left() + ResultLayout::kColumnLeftInset, badgeTop,
                              ResultLayout::kColumnBadgeSize, ResultLayout::kColumnBadgeSize);
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(0x1A, 0x6C, 0xF0));
        painter->drawRoundedRect(badgeRect, 4, 4);
        QFont badgeFont(option.font);
        badgeFont.setPixelSize(8);
        badgeFont.setWeight(QFont::Bold);
        painter->setFont(badgeFont);
        painter->setPen(Qt::white);
        painter->drawText(badgeRect, Qt::AlignCenter,
                         result.pluginBadge.isEmpty() ? QStringLiteral("·")
                                                      : result.pluginBadge.left(2));

        const QVector<QRect> rects = resultColumnRects(rowRect, model->columns());
        QFont cellFont(option.font);
        cellFont.setPixelSize(12);
        for (int i = 0; i < rects.size() && i < cells.size(); ++i) {
            QFont font = cellFont;
            if (i == 0)
                font.setWeight(QFont::DemiBold);
            painter->setFont(font);
            painter->setPen(i == 0 ? QColor(0x1F, 0x23, 0x29) : QColor(0x5A, 0x62, 0x6E));
            const QFontMetrics metrics(font);
            const QString text = metrics.elidedText(cells.at(i), Qt::ElideRight,
                                                   rects.at(i).width());
            const int flags = Qt::AlignVCenter
                              | (model->columns().at(i).rightAligned ? Qt::AlignRight
                                                                     : Qt::AlignLeft);
            painter->drawText(rects.at(i), flags, text);
        }
        painter->restore();
        return;
    }

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
    const QString subtitle = pluginRow ? result.pluginSubtitle : rowDirectory(result.record);
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
                              ? tr("Enter 复制")
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
                          tr("%1 次").arg(result.record.launchCount));
    }

    painter->restore();
}

QSize ResultDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option)
    const ResultModel *model = qobject_cast<const ResultModel *>(index.model());
    const bool columnMode = model != nullptr && model->hasColumns();
    return QSize(0, columnMode ? ResultLayout::kColumnRowHeight : kRowHeight);
}
