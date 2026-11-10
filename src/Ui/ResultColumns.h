#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

/** 结果列表的两种行高与列视图的几何参数（宿主与各渲染模块共用）。 */
namespace ResultLayout {
constexpr int kClassicRowHeight = 54;
constexpr int kColumnRowHeight = 30;
constexpr int kColumnHeaderHeight = 28;
constexpr int kColumnLeftInset = 20;
constexpr int kColumnBadgeSize = 20;
constexpr int kColumnGap = 10;
constexpr int kColumnRightInset = 14;
/** SDK 里列定义 / 单元格文本的分隔符（ASCII US）。 */
constexpr char kColumnSeparator = '\x1f';
} // namespace ResultLayout

struct ResultColumn
{
    QString label;
    /** 0 = 弹性列，>0 = 固定像素宽度。 */
    int width = 0;
    bool rightAligned = false;

    bool operator==(const ResultColumn &other) const
    {
        return width == other.width && rightAligned == other.rightAligned
               && label == other.label;
    }
};

/** 解析插件提交的列定义；非法项会被跳过，返回空表示不使用列视图。 */
QVector<ResultColumn> parseColumnSpec(const QString &spec);

/** 计算一行内各单元格的矩形。列视图与表头共用，保证像素级对齐。 */
QVector<QRect> resultColumnRects(const QRect &rowRect, const QVector<ResultColumn> &columns);

/**
 * 列视图表头：常驻在输入框下方、结果列表上方。
 * 不参与选中、不接收焦点，只画列名和分隔线；无需 Q_OBJECT（没有自己的信号槽）。
 */
class ResultColumnHeader : public QWidget
{
public:
    explicit ResultColumnHeader(QWidget *parent = nullptr);

    void setColumns(const QVector<ResultColumn> &columns);
    const QVector<ResultColumn> &columns() const { return m_columns; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVector<ResultColumn> m_columns;
};
