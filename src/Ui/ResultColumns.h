#pragma once

#include <QPoint>
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
/** 用户拖动表头时的列宽下限。 */
constexpr int kMinColumnWidth = 48;
/** SDK 里列定义 / 单元格文本的分隔符（ASCII US）。 */
constexpr char kColumnSeparator = '\x1f';
} // namespace ResultLayout

struct ResultColumn
{
    QString label;
    /** 0 = 弹性列，>0 = 固定像素宽度。 */
    int width = 0;
    bool rightAligned = false;
    /** 插件自定义的排序键；为空表示该列不可点击排序（老插件/纯展示列）。 */
    QString sortKey;
    /** 0 = 当前未按该列排序；1 = 升序；2 = 降序（插件在 spec 里声明）。 */
    int sortOrder = 0;

    bool operator==(const ResultColumn &other) const
    {
        return width == other.width && rightAligned == other.rightAligned
               && label == other.label && sortKey == other.sortKey
               && sortOrder == other.sortOrder;
    }
};

/** 解析插件提交的列定义；非法项会被跳过，返回空表示不使用列视图。 */
QVector<ResultColumn> parseColumnSpec(const QString &spec);

/** 计算一行内各单元格的矩形。列视图与表头共用，保证像素级对齐。 */
QVector<QRect> resultColumnRects(const QRect &rowRect, const QVector<ResultColumn> &columns);

/**
 * 列视图表头：常驻在输入框下方、结果列表上方。
 * 不参与选中、不接收焦点；点击带排序键的列发出 columnClicked，
 * 当前排序列右侧画一个箭头（升序向上、降序向下），参照 Everything。
 * 列与列之间的缝隙可以左右拖动改宽度，松手后发出 columnsResized。
 */
class ResultColumnHeader : public QWidget
{
    Q_OBJECT

public:
    explicit ResultColumnHeader(QWidget *parent = nullptr);

    void setColumns(const QVector<ResultColumn> &columns);
    const QVector<ResultColumn> &columns() const { return m_columns; }

    /** 按排序键设置箭头；key 为空或列不存在时清除箭头。 */
    void setSortIndicator(const QString &sortKey, bool ascending);
    QString sortKey() const { return m_sortKey; }
    bool ascending() const { return m_ascending; }

    QSize sizeHint() const override;

signals:
    /** 点击了可排序列，参数是列下标。 */
    void columnClicked(int column);
    /**
     * 拖动列边界时给出新的整组列宽。
     * committed=false 是拖拽中的预览，true 是松手后的最终结果（调用方据此持久化）。
     */
    void columnsResized(const QVector<ResultColumn> &columns, bool committed);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int columnAt(const QPoint &position) const;
    int dividerAt(const QPoint &position) const;
    bool sortableAt(int column) const;

    QVector<ResultColumn> m_columns;
    QString m_sortKey;
    bool m_ascending = true;
    int m_hoverColumn = -1;
    int m_pressedColumn = -1;
    int m_hoverDivider = -1;
    int m_resizeColumn = -1;
    int m_resizeStartX = 0;
    int m_resizeStartWidth = 0;
    int m_resizeRightWidth = 0;
    bool m_resizeRightFixed = false;
    bool m_resizeMoved = false;
};
