#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <map>
#include <memory>
#include <vector>

#include "core/DownloadEngine.h"
#include "engines/dash/Mpd.h"

class QNetworkReply;

// Module "downloadsfragments" (FDM's name): MPEG-DASH (.mpd) videos on demand. Takes the best
// video and audio representations, downloads both tracks' segments in parallel and in order
// into part files, then combines them into one file with ffmpeg (stream copy, no re-encoding).
// Without ffmpeg the video and the audio are kept as two files.
class DashEngine : public DownloadEngine {
    Q_OBJECT
public:
    explicit DashEngine(QObject* parent = nullptr);
    ~DashEngine() override;

    QString moduleUid() const override { return QStringLiteral("downloadsfragments"); }
    bool canHandle(const QUrl& url) const override;
    void start(const TaskParams& params) override;
    void stop(qint64 id) override;
    void setSpeedLimit(qint64 bytesPerSec) override { m_limit = bytesPerSec; }
    void setMaxConnections(int perDownload) override { m_maxConn = qBound(1, perDownload, 8); }
    void setTaskSpeedLimit(qint64 id, qint64 bytesPerSec) override;
    QHash<qint64, QByteArray> shutdown() override;

private:
    struct Track {
        MpdRepresentation rep;
        QList<MpdSegment> items; // init (if any) + media segments, written in this order
        QString partPath;
        QFile file;
        int nextToRequest = 0;
        int nextToWrite = 0;
        QMap<int, QByteArray> ready;
        QSet<int> inFlight;
        QHash<int, int> retries;
        qint64 written = 0;
        bool done() const { return nextToWrite >= items.size(); }
    };
    struct Task {
        qint64 id = 0;
        TaskParams p;
        QUrl mpdUrl;
        std::vector<std::unique_ptr<Track>> tracks; // video first, then audio
        QString fileName, finalPath;
        QJsonObject resume;          // saved state being resumed (empty = fresh start)
        QList<QPointer<QNetworkReply>> replies;
        QPointer<QProcess> mux;
        qint64 estimate = -1;        // total size from bandwidth x duration
        qint64 limit = 0;            // per-download cap
        qint64 tickBytes = 0;
        qint64 windowBytes = 0;      // for speed limiting (per second)
        QElapsedTimer window;
        double speed = 0;
        bool stopping = false;
    };

    Task* task(qint64 id);
    QNetworkRequest request(const Task& t, const MpdSegment& s) const;
    void onManifest(qint64 id, QNetworkReply* reply);
    void pump(qint64 id);
    void onSegment(qint64 id, int track, int index, QNetworkReply* reply);
    void finishTracks(Task& t);
    void fail(Task& t, const QString& error);
    void emitStats();
    QByteArray makeResumeData(const Task& t) const;
    void release(Task& t);
    qint64 received(const Task& t) const;

    std::map<qint64, std::unique_ptr<Task>> m_tasks;
    QNetworkAccessManager m_nam;
    QTimer m_timer;
    qint64 m_limit = 0;
    int m_maxConn = 6;
    int m_tick = 0;
};
