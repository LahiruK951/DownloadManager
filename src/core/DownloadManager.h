#pragma once
#include <QList>
#include <QObject>
#include <QTimer>

#include "core/Download.h"
#include "core/Schedule.h"
#include "core/TrafficMode.h"

class Database;
class DownloadEngine;

// Owns all downloads, runs the queue according to the traffic mode and the
// schedules, and routes each download to its engine module.
class DownloadManager : public QObject {
    Q_OBJECT
public:
    explicit DownloadManager(Database* db, QObject* parent = nullptr);
    ~DownloadManager() override;

    void addEngine(DownloadEngine* e);
    void load();
    bool canHandle(const QString& url) const;
    QString moduleFor(const QString& url) const;
    Download* add(const QString& url, const QString& destDir, const QString& fileName, bool start,
                  const QString& referrer = {}, const QString& cookies = {}, const QString& userAgent = {},
                  const ScheduleWindow* schedule = nullptr);
    Download* find(qint64 id) const;
    const QList<Download*>& downloads() const { return m_list; }

    void pause(Download* d);
    void resume(Download* d);
    void restart(Download* d);
    void remove(Download* d, bool deleteFiles);
    void setPriority(Download* d, int priority);
    void setSchedule(Download* d, bool enabled, const ScheduleWindow& w);
    void setFileWanted(Download* d, int index, bool wanted);
    void setSpeedLimit(Download* d, int kbps);
    QString moveTo(Download* d, const QString& newDir); // returns error text or ""; may finish asynchronously
    static bool canMove(const Download* d);
    void stopSeeding(Download* d);
    void pauseAll();
    void resumeAll();

    // --- traffic modes (FDM "TUM"): manual choice + weekly rules ("snail mode schedule")
    int trafficMode() const { return m_effectiveMode; }
    int manualTrafficMode() const { return m_manualMode; }
    bool trafficModeScheduled() const { return m_activeRule >= 0 && m_activeRule != m_overriddenRule; }
    void setTrafficMode(int mode); // user action
    TrafficMode::Limits limits(int mode) const;
    void setLimits(int mode, const TrafficMode::Limits& l);
    QList<TrafficRule> trafficRules() const { return m_rules; }
    void setTrafficRules(const QList<TrafficRule>& rules);

    // --- BitTorrent
    qint64 uploadLimitKBps() const;
    void setUploadLimitKBps(qint64 kbps);
    double seedRatio() const;
    int autoRetryLimit() const;
    void setAutoRetryLimit(int n);
    static bool isRetriable(const QString& error);
    void setSeedRatio(double r);

    void shutdown();
    static QString defaultUserAgent();

signals:
    void downloadAdded(Download* d);
    void downloadRemoved(Download* d);
    void downloadFinished(Download* d);
    void statsUpdated();
    void trafficModeChanged();
    void allDownloadsFinished();

private:
    void schedule();
    void evaluateTrafficRules();
    void startDownload(Download* d);
    void stopDownload(Download* d);
    void persist(Download* d);
    void applyLimits();
    void checkAllDone();
    void runAutoRetries();
    void checkMissingFiles();
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
    int m_manualMode = TrafficMode::High;
    int m_effectiveMode = TrafficMode::High;
    QList<TrafficRule> m_rules;
    int m_activeRule = -1;
    int m_overriddenRule = -1;
    QTimer m_clock;
    bool m_hadActive = false;
    int m_tick = 0;
    bool m_shuttingDown = false;
};
