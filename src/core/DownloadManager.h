#pragma once
#include <QList>
#include <QObject>

#include "core/Download.h"
#include "core/TrafficMode.h"

class Database;
class DownloadEngine;

// Owns all downloads, runs the queue according to the traffic mode and
// routes each download to its engine module.
class DownloadManager : public QObject {
    Q_OBJECT
public:
    explicit DownloadManager(Database* db, QObject* parent = nullptr);
    ~DownloadManager() override;

    void load();
    bool canHandle(const QString& url) const;
    Download* add(const QString& url, const QString& destDir, const QString& fileName, bool start,
                  const QString& referrer = {}, const QString& cookies = {}, const QString& userAgent = {});
    Download* find(qint64 id) const;
    const QList<Download*>& downloads() const { return m_list; }

    void pause(Download* d);
    void resume(Download* d);
    void restart(Download* d);
    void remove(Download* d, bool deleteFiles);
    void setPriority(Download* d, int priority);
    void pauseAll();
    void resumeAll();

    int trafficMode() const { return m_mode; }
    void setTrafficMode(int mode);
    TrafficMode::Limits limits(int mode) const;
    void setLimits(int mode, const TrafficMode::Limits& l);

    void shutdown();

    static QString defaultUserAgent();

signals:
    void downloadAdded(Download* d);
    void downloadRemoved(Download* d);
    void downloadFinished(Download* d);
    void statsUpdated();

private:
    void schedule();
    void startDownload(Download* d);
    void stopDownload(Download* d);
    void persist(Download* d);
    void applyLimits();
    DownloadEngine* engineFor(const QString& url) const;
    DownloadEngine* engineByUid(const QString& uid) const;

    void onMeta(qint64 id, const QString& fileName, const QString& filePath, qint64 total, bool resumable);
    void onStats(const TaskStats& st);
    void onResumeData(qint64 id, const QByteArray& data);
    void onFinished(qint64 id, const QString& filePath);
    void onFailed(qint64 id, const QString& error, const QByteArray& resumeData);
    void onStopped(qint64 id, const QByteArray& resumeData);
    void onLog(qint64 id, const QString& line);

    Database* m_db;
    QList<DownloadEngine*> m_engines;
    QList<Download*> m_list; // newest first
    qint64 m_nextId = 0;
    int m_mode = TrafficMode::High;
    bool m_shuttingDown = false;
};
