#pragma once
#include <QHash>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include "core/Types.h"

// A download "module" (FDM: downloadswww, downloadsbt, downloadsm3u, ...).
// Engines run their transfers off the GUI thread and report back via signals.
class DownloadEngine : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~DownloadEngine() override = default;

    virtual QString moduleUid() const = 0;
    virtual bool canHandle(const QUrl& url) const = 0;
    virtual void start(const TaskParams& params) = 0;
    virtual void stop(qint64 id) = 0;
    virtual void setSpeedLimit(qint64 bytesPerSec) = 0;   // 0 = unlimited
    virtual void setMaxConnections(int perDownload) = 0;
    virtual void setMaxConnectionsPerServer(int /*n*/) {} // 0 = no limit
    virtual void setUploadLimit(qint64 /*bytesPerSec*/) {}
    virtual void setSeedRatio(double /*ratio*/) {}
    virtual void setFileWanted(qint64 /*id*/, int /*index*/, bool /*wanted*/) {}
    virtual void setTaskSpeedLimit(qint64 /*id*/, qint64 /*bytesPerSec*/) {}
    // BitTorrent: start a task that waited for file selection, with these file indexes wanted.
    virtual void chooseFiles(qint64 /*id*/, const QList<int>& /*wanted*/) {}
    virtual void setSequential(qint64 /*id*/, bool /*on*/) {}
    // Live details for the UI (BitTorrent: {peers: [...], trackers: [...]}).
    virtual QVariantMap details(qint64 /*id*/) const { return {}; }
    // Stops everything synchronously and returns final resume data per task.
    virtual QHash<qint64, QByteArray> shutdown() = 0;

signals:
    void meta(qint64 id, const QString& fileName, const QString& filePath, qint64 total, bool resumable);
    void stats(const TaskStats& stats);
    void resumeData(qint64 id, const QByteArray& data);
    void finished(qint64 id, const QString& filePath);
    void failed(qint64 id, const QString& error, const QByteArray& resumeData);
    void stopped(qint64 id, const QByteArray& resumeData);
    void log(qint64 id, const QString& line);
    // The task is paused until chooseFiles(): files = [{index, name, size}]
    void filesAwaiting(qint64 id, const QVariantList& files);
};
