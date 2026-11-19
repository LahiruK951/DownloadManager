#include "ui/DownloadsModel.h"

#include "core/Download.h"
#include "core/DownloadManager.h"

DownloadsModel::DownloadsModel(DownloadManager* mgr, QObject* parent)
    : QAbstractListModel(parent), m_items(mgr->downloads())
{
    for (Download* d : m_items)
        track(d);
    connect(mgr, &DownloadManager::downloadAdded, this, [this](Download* d) {
        beginInsertRows({}, 0, 0);
        m_items.prepend(d);
        endInsertRows();
        track(d);
    });
    connect(mgr, &DownloadManager::downloadRemoved, this, [this](Download* d) {
        const int row = int(m_items.indexOf(d));
        if (row < 0)
            return;
        beginRemoveRows({}, row, row);
        m_items.removeAt(row);
        endRemoveRows();
        disconnect(d, nullptr, this, nullptr);
    });
}

void DownloadsModel::track(Download* d)
{
    connect(d, &Download::stateChanged, this, [this, d] { rowChanged(d); });
    connect(d, &Download::titleChanged, this, [this, d] { rowChanged(d); });
    connect(d, &Download::tagsChanged, this, [this, d] { rowChanged(d); });
    connect(d, &Download::categoryChanged, this, [this, d] { rowChanged(d); });
}

void DownloadsModel::rowChanged(Download* d)
{
    const int row = int(m_items.indexOf(d));
    if (row >= 0)
        emit dataChanged(index(row), index(row), {StateRole, CategoryRole, TitleRole});
}

int DownloadsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant DownloadsModel::data(const QModelIndex& index, int role) const
{
    Download* d = m_items.value(index.row());
    if (!d)
        return {};
    switch (role) {
    case DownloadRole: return QVariant::fromValue(static_cast<QObject*>(d));
    case StateRole: return d->state();
    case CategoryRole: return d->category();
    case TitleRole:
    case Qt::DisplayRole: return d->title();
    }
    return {};
}

QHash<int, QByteArray> DownloadsModel::roleNames() const
{
    return {{DownloadRole, "download"}, {StateRole, "state"}, {CategoryRole, "category"}, {TitleRole, "title"}};
}

// ------------------------------------------------------------------ filter

DownloadsFilterModel::DownloadsFilterModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    connect(this, &QAbstractItemModel::rowsInserted, this, &DownloadsFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &DownloadsFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &DownloadsFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &DownloadsFilterModel::countChanged);
}

void DownloadsFilterModel::setFilter(const QString& f)
{
    if (m_filter == f)
        return;
    m_filter = f;
    invalidateRowsFilter();
    emit countChanged();
}

void DownloadsFilterModel::setSearch(const QString& s)
{
    if (m_search == s)
        return;
    m_search = s;
    invalidateRowsFilter();
    emit countChanged();
}

qint64 DownloadsFilterModel::idAt(int row) const
{
    auto* src = static_cast<DownloadsModel*>(sourceModel());
    const QModelIndex si = mapToSource(index(row, 0));
    Download* d = src ? src->at(si.row()) : nullptr;
    return d ? d->id() : -1;
}

bool DownloadsFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex&) const
{
    auto* src = static_cast<DownloadsModel*>(sourceModel());
    Download* d = src->at(sourceRow);
    if (!d)
        return false;
    if (m_filter == QLatin1String("active")) {
        if (d->completed())
            return false;
    } else if (m_filter == QLatin1String("completed")) {
        if (!d->completed())
            return false;
    } else if (m_filter.startsWith(QLatin1String("tag:"))) {
        if (!d->hasTag(m_filter.mid(4).toInt()))
            return false;
    } else if (m_filter != QLatin1String("all")) {
        if (d->category() != m_filter)
            return false;
    }
    if (!m_search.isEmpty())
        return d->title().contains(m_search, Qt::CaseInsensitive) || d->url().contains(m_search, Qt::CaseInsensitive);
    return true;
}

void DownloadsFilterModel::setSortOrder(const QString& key, bool descending)
{
    static const QStringList keys = {QStringLiteral("added"), QStringLiteral("name"), QStringLiteral("size"), QStringLiteral("status")};
    m_sortKey = keys.contains(key) ? key : QStringLiteral("added");
    m_sortDesc = descending;
    sort(0, m_sortDesc ? Qt::DescendingOrder : Qt::AscendingOrder);
    invalidate(); // same column and order can still mean a different key
}

// Status order: what's moving first, finished last.
static int statusRank(const Download* d)
{
    switch (d->state()) {
    case Download::Starting:
    case Download::Downloading: return 0;
    case Download::Queued: return 1;
    case Download::Paused: return 2;
    case Download::Error: return 3;
    default: return 4; // completed
    }
}

bool DownloadsFilterModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    auto* src = static_cast<DownloadsModel*>(sourceModel());
    const Download* a = src->at(left.row());
    const Download* b = src->at(right.row());
    if (!a || !b)
        return false;
    int c = 0;
    if (m_sortKey == QLatin1String("name"))
        c = QString::localeAwareCompare(a->title(), b->title());
    else if (m_sortKey == QLatin1String("size"))
        c = a->total() < b->total() ? -1 : (a->total() > b->total() ? 1 : 0);
    else if (m_sortKey == QLatin1String("status"))
        c = statusRank(a) - statusRank(b);
    if (m_sortKey == QLatin1String("added"))
        return a->id() < b->id(); // ids grow with time
    if (c != 0)
        return c < 0;
    // Tie-break for the other keys: newest first, whichever direction was picked.
    const bool olderFirst = a->id() < b->id();
    return m_sortDesc ? olderFirst : !olderFirst;
}

int DownloadsFilterModel::rowOf(qint64 id) const
{
    for (int r = 0; r < rowCount(); ++r)
        if (idAt(r) == id)
            return r;
    return -1;
}