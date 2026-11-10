#pragma once

#include "Core/SearchEngine.h"
#include "Ui/ResultColumns.h"

#include <QAbstractListModel>

/** 搜索结果模型：把 SearchEngine 的结果直接喂给 QListView。 */
class ResultModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        DirRole,
        ExtRole,
        UsageRole,
        ScoreRole
    };

    explicit ResultModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    void setResults(const QVector<SearchResult> &results);
    void clearResults();

    /** 整张表一份列定义，不重复塞进每一行；空 = 经典两行样式。 */
    void setColumns(const QVector<ResultColumn> &columns);
    const QVector<ResultColumn> &columns() const { return m_columns; }
    bool hasColumns() const { return !m_columns.isEmpty(); }

    int count() const;
    const SearchResult &resultAt(int row) const;

private:
    QVector<SearchResult> m_results;
    QVector<ResultColumn> m_columns;
};
