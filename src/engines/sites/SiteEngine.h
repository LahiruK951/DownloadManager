#pragma once
#include <QPointer>
#include <QProcess>
#include <map>
#include <memory>

#include "core/DownloadEngine.h"

// Module "downloadsms" (FDM's name for media-site downloads): video/audio pages on sites like
// YouTube or Vimeo, through yt-dlp (FDM uses its own site plugins; yt-dlp is the maintained
// equivalent). Best video + audio, merged with ffmpeg when available. Resume relies on
// yt-dlp continuing its .part files.
class SiteEngine : public DownloadEngine {
    Q_OBJECT
public:
    explicit SiteEngine(QObject* parent = nullptr);
    ~SiteEngine() override;

    QString moduleUid() const override { return QStringLiteral("downloadsms"); }
    bool canHandle(const QUrl& url) const override;
    void start(const TaskParams& params) override;
    void stop(qint64 id) override;
    void setSpeedLimit(qint64 bytesPerSec) override { m_limit = bytesPerSec; }
    void setMaxConnections(int perDownload) override { m_maxConn = qBound(1, perDownload, 8); }
    QHash<qint64, QByteArray> shutdown() override;

    static bool isSupportedSite(const QUrl& url);
    // yt-dlp arguments for a task (tested directly).
    static QStringList arguments(const TaskParams& p, int connections, qint64 speedLimit);
    // One line of yt-dlp output -> what it means (tested directly).
    struct Line {
        enum Kind { None, Progress, Destination, Final, Error, AlreadyThere } kind = None;
        qint64 done = 0, total = -1;
        double speed = 0;
        QString path, text;
    };
    static Line parse(const QString& line);

private:
    struct Task {
        qint64 id = 0;
        TaskParams p;
        QPointer<QProcess> proc;
        QByteArray outBuf, errBuf;
        QString currentFile, finalFile;
        QString lastError;
        qint64 previousStreams = 0; // bytes of streams already finished (video, then audio)
        qint64 curDone = 0, curTotal = -1;
        double speed = 0;
        bool metaSent = false;
        bool stopping = false;
    };
    void onOutput(Task& t, const QByteArray& chunk);
    void onFinished(qint64 id, int code, QProcess::ExitStatus st);

    std::map<qint64, std::unique_ptr<Task>> m_tasks;
    qint64 m_limit = 0;
    int m_maxConn = 4;
};
