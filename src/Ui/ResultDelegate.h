#pragma once

#include <QStyledItemDelegate>

/** 自绘结果行：图标 + 高亮名称 + 灰色路径 + 使用次数。 */
class ResultDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit ResultDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
