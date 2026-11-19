#pragma once
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

// Watches a folder for new .torrent files (FDM: ForceSilentDownloadsFromTorrentFolder).
// A file is taken once it has stopped growing and really is a torrent: it is copied to
// `keepDir` (the engine reads it whenever the download starts, maybe much later), and the
// original is renamed to "<name>.torrent.added" so it isn't picked up again.
class TorrentFolderWatcher : public QObject {
    Q_OBJECT
public:
    explicit TorrentFolderWatcher(const QString& keepDir, QObject* parent = nullptr);

    void setFolder(const QString& folder); // "" stops watching
    QString folder() const { return m_folder; }
    void scanNow();

signals:
    void torrentAdded(const QString& copyPath, const QString& originalName);

private:
    void scan();

    QString m_keepDir, m_folder;
    QFileSystemWatcher m_fsw;
    QTimer m_poll;                  // backup for missed notifications, and to see files settle
    QHash<QString, qint64> m_sizes; // path -> size at the last look (settling)
};
