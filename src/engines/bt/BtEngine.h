#pragma once
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <map>
#include <memory>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>

#include "core/DownloadEngine.h"

namespace libtorrent {
class session;
}
class QNetworkReply;

// Module "downloadsbt": magnet links, .torrent files (local or http) via libtorrent.
// libtorrent runs its own threads; this object polls alerts/status on the GUI thread.
class BtEngine : public DownloadEngine {
    Q_OBJECT
public:
    // offline: listen on loopback only, no DHT/LSD/UPnP (tests, dev profiles)
    explicit BtEngine(const QString& stateDir, int listenPort = 0, bool offline = false, QObject* parent = nullptr);
    ~BtEngine() override;
    // Makes a .torrent for a file or folder (no trackers: peers find it through DHT; a web seed
    // URL can be added). Slow for big files (it hashes everything): call it on a worker thread.
    static bool createTorrent(const QString& path, const QString& torrentPath, const QString& webSeed, QString* error);

    QString moduleUid() const override { return QStringLiteral("downloadsbt"); }
    bool canHandle(const QUrl& url) const override;
    void start(const TaskParams& params) override;
    void stop(qint64 id) override;
    void setSpeedLimit(qint64 bytesPerSec) override;
    void setMaxConnections(int perDownload) override;
    void setUploadLimit(qint64 bytesPerSec) override;
    void setSeedRatio(double ratio) override;
    void setFileWanted(qint64 id, int index, bool wanted) override;
    void setTaskSpeedLimit(qint64 id, qint64 bytesPerSec) override;
    void chooseFiles(qint64 id, const QList<int>& wanted) override;
    void setSequential(qint64 id, bool on) override;
    QVariantMap details(qint64 id) const override;
    QHash<qint64, QByteArray> shutdown() override;

    int listenPort() const;
    void addPeer(qint64 id, const QString& host, int port); // manual peer (FDM: "Add peer")

private:
    struct Task {
        qint64 id = 0;
        libtorrent::torrent_handle h;
        QString destDir;
        bool seedOnly = false;
        qint64 speedLimit = 0;
        bool metaEmitted = false;
        bool finishedEmitted = false;   // libtorrent says finished, flush requested
        bool finishReported = false;    // data flushed, finished() emitted
        bool stopping = false;
        bool pickFiles = false;     // pause for file selection once the file list is known
        bool awaitingFiles = false; // paused, waiting for chooseFiles()
        bool sequential = false;
        QByteArray lastResume;
        QVariantList lastFiles;
        QPointer<QNetworkReply> fetch;
    };

    void addTorrent(Task& t, libtorrent::add_torrent_params atp);
    void poll();
    void handleAlerts();
    void emitStats(bool full);
    void emitMeta(Task& t);
    void removeTask(qint64 id, bool removeFromSession = true);
    void failTask(Task& t, const QString& error);
    Task* byHandle(const libtorrent::torrent_handle& h);
    QString finalPath(const Task& t) const;
    QVariantList fileList(const Task& t) const; // [{index, name, size}] without pad files
    void maybeAwaitFiles(Task& t);

    std::unique_ptr<libtorrent::session> m_ses;
    std::map<qint64, Task> m_tasks;
    QTimer m_timer;
    QNetworkAccessManager m_nam;
    QString m_stateDir;
    double m_ratio = 1.0;
    int m_tick = 0;
    bool m_down = false;
};
