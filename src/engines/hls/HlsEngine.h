#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QMap>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <map>
#include <memory>

#include "core/DownloadEngine.h"
#include "engines/hls/M3u8.h"

class QNetworkReply;

// Module "downloadsm3u": HLS (.m3u8) video streams. Picks the best variant,
// downloads segments in parallel, decrypts AES-128 and writes them in order
// into one .ts (or fMP4) file. Runs on the GUI thread (QNetworkAccessManager is async).
class HlsEngine : public DownloadEngine {
    Q_OBJECT
public:
    explicit HlsEngine(QObject* parent = nullptr);
    ~HlsEngine() override;

    QString moduleUid() const override { return QStringLiteral("downloadsm3u"); }
    bool canHandle(const QUrl& url) const override;
    void start(const TaskParams& params) override;
    void stop(qint64 id) override;
    void setSpeedLimit(qint64 bytesPerSec) override { m_limit = bytesPerSec; }
    void setMaxConnections(int perDownload) override { m_maxConn = qBound(1, perDownload, 6); }
    void setTaskSpeedLimit(qint64 id, qint64 bytesPerSec) override;
    QHash<qint64, QByteArray> shutdown() override;

    static QString outputBaseName(const QUrl& playlistUrl);

private:
    struct Task {
        qint64 id = 0;
        TaskParams p;
        QUrl mediaUrl;
        M3u8Playlist pl;
        QHash<int, QByteArray> keys;      // keyIndex -> 16-byte key
        QSet<int> keysLoading;
        int nextToRequest = 0;
        int nextToWrite = 0;
        QMap<int, QByteArray> ready;      // finished, not yet written (out of order)
        QSet<int> inFlight;
        QHash<int, int> retries;
        QList<QPointer<QNetworkReply>> replies;
        QFile file;
        QString fileName, finalPath, partPath;
        bool resumed = false;
        bool initWritten = false;
        bool dirty = false;
        qint64 written = 0;
        qint64 tickBytes = 0;
        qint64 pacedBytes = 0;            // bytes received since pacing started
        QElapsedTimer pace;
        double speed = 0;
        bool speedInit = false;
    };

    Task* task(qint64 id);
    QNetworkRequest request(const Task& t, const QUrl& url) const;
    QNetworkReply* get(Task& t, const QUrl& url);
    void fetchPlaylist(qint64 id, const QUrl& url, bool isMedia);
    void onMediaPlaylist(Task& t);
    void pump(qint64 id);
    void onSegment(qint64 id, int index, QNetworkReply* reply);
    void flush(Task& t);
    void finish(Task& t);
    void fail(Task& t, const QString& error);
    void emitStats();
    QByteArray makeResumeData(const Task& t) const;
    void release(Task& t);

    std::map<qint64, std::unique_ptr<Task>> m_tasks;
    QNetworkAccessManager m_nam;
    QTimer m_timer;
    qint64 m_limit = 0;
    int m_maxConn = 4;
    int m_tick = 0;
};
