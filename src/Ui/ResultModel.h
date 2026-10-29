#pragma once

#include "Core/SearchEngine.h"

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

    int count() const;
    const SearchResult &resultAt(int row) const;

private:
    QVector<SearchResult> m_results;
};
