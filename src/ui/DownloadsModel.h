#pragma once
#include <QAbstractListModel>
#include <QSortFilterProxyModel>

class Download;
class DownloadManager;

// Flat list of Download objects (newest first). Delegates bind to `download.*`.
class DownloadsModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { DownloadRole = Qt::UserRole + 1, StateRole, CategoryRole, TitleRole };

    explicit DownloadsModel(DownloadManager* mgr, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Download* at(int row) const { return m_items.value(row); }

private:
    void track(Download* d);
    void rowChanged(Download* d);
    QList<Download*> m_items;
};

// "All / Active / Completed / <tag>" filter + search box + sort order.
class DownloadsFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    explicit DownloadsFilterModel(QObject* parent = nullptr);
    void setFilter(const QString& f);
    void setSearch(const QString& s);
    QString filter() const { return m_filter; }
    QString search() const { return m_search; }
    // key: "added" (default), "name", "size", "status" or "queue"; ties fall back to newest first.
    void setSortOrder(const QString& key, bool descending);
    QString sortKey() const { return m_sortKey; }
    bool sortDescending() const { return m_sortDesc; }
    int count() const { return rowCount(); }
    Q_INVOKABLE qint64 idAt(int row) const;
    Q_INVOKABLE int rowOf(qint64 id) const;

signals:
    void countChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    QString m_filter = QStringLiteral("all");
    QString m_search;
    QString m_sortKey = QStringLiteral("added");
    bool m_sortDesc = true;
};
