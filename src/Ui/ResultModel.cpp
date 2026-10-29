#include "Ui/ResultModel.h"

ResultModel::ResultModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int ResultModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_results.size();
}

QVariant ResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_results.size())
        return QVariant();

    const SearchResult &result = m_results.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return result.record.displayName;
    case PathRole:
        return result.record.path;
    case DirRole:
        return result.record.dir;
    case ExtRole:
        return result.record.ext;
    case UsageRole:
        return result.record.launchCount;
    case ScoreRole:
        return result.score;
    case Qt::ToolTipRole:
        return result.record.path;
    default:
        break;
    }
    return QVariant();
}

void ResultModel::setResults(const QVector<SearchResult> &results)
{
    beginResetModel();
    m_results = results;
    endResetModel();
}

void ResultModel::clearResults()
{
    if (m_results.isEmpty())
        return;
    beginResetModel();
    m_results.clear();
    endResetModel();
}

int ResultModel::count() const
{
    return m_results.size();
}

const SearchResult &ResultModel::resultAt(int row) const
{
    static const SearchResult empty;
    if (row < 0 || row >= m_results.size())
        return empty;
    return m_results.at(row);
}
