#pragma once
#include <QHash>
#include <QList>
#include <QPair>
#include <QObject>
#include <QTimer>

#include "core/Download.h"
#include "core/Tag.h"
#include "core/MediaConvert.h"
#include "core/ProxySettings.h"
#include "core/FdmImport.h"
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
    void persistRecord(Download* d) { persist(d); }
    // Manual queue order among unfinished downloads (FDM: user-defined order). where:
    // "top", "up", "down", "bottom". Several at once keep their relative order.
    void moveInQueue(const QList<Download*>& ds, const QString& where);
    QList<Download*> queueOrder() const; // unfinished downloads, first to start first
    void setSchedule(Download* d, bool enabled, const ScheduleWindow& w);
    void setFileWanted(Download* d, int index, bool wanted);
    void setSpeedLimit(Download* d, int kbps);
    QString moveTo(Download* d, const QString& newDir); // returns error text or ""; may finish asynchronously
    static bool canMove(const Download* d);
    // Rename a finished file on disk (not torrents: libtorrent owns their names). Error text or "".
    QString rename(Download* d, const QString& newName);
    static bool canRename(const Download* d);

    // Server logins, per "scheme://host:port". Kept for this session; saved when `remember`.
    static QString loginServer(const QString& url);
    bool loginFor(const QString& url, QString* user, QString* password) const;
    void setLogin(const QString& url, const QString& user, const QString& password, bool remember);
    int savedLoginCount() const;
    // Untrusted certificates the user accepted, per host.
    void trustCertificate(Download* d); // remember its host and try again
    QStringList trustedHosts() const;
    void forgetTrustedHosts();
    void forgetLogins();

    // Stream post-processing: remux HLS .ts to .mp4 with ffmpeg (FDM ships ffmpeg; we find one).
    bool convertStreams() const;
    void setConvertStreams(bool on);
    QString ffmpegSetting() const;
    QString ytdlpSetting() const;
    void setYtdlpSetting(const QString& path);
    QString ytdlpPath() const; // setting, FLUXDM_YTDLP, next to FluxDM, PATH; "" if none
    void setFfmpegSetting(const QString& path);
    QString ffmpegPath() const; // resolved: setting, FLUXDM_FFMPEG, then PATH; "" if none
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
    // What to do when a file with the same name exists (global default for new downloads).
    int existingFileDefault() const;
    void setExistingFileDefault(int reaction);
    void setExistingFileReaction(Download* d, int reaction);

    // BitTorrent: ask which files to download before starting (default on).
    bool chooseTorrentFiles() const;
    void setChooseTorrentFiles(bool on);
    void chooseFiles(Download* d, const QList<int>& wanted);
    void setSequential(Download* d, bool on);
    QVariantMap details(Download* d) const;

    // User tags
    QList<Tag> tags() const { return m_tags; }
    Tag tag(int id) const;
    int saveTag(Tag t); // create (id 0) or update; returns the id
    void removeTag(int id);
    void setTag(Download* d, int tagId, bool on);
    QList<int> tagsFor(const QString& fileName) const; // tags whose extensions match
    QString tagFolderFor(const QString& fileName) const; // folder of the first matching tag that has one

    // MP3/MP4 converter for finished audio/video files (keeps the original). Error text or "".
    QString convertFile(Download* d, const ConvertOptions& o);
    static QString convertibleFile(const Download* d); // the media file to convert, "" if none

    // Antivirus check of finished files (Windows attachment services; on by default).
    // Free Download Manager's list: finished ones as completed entries, the rest paused (they
    // start over: FDM's partial data can't be reused). Returns {added, skipped as duplicates}.
    QPair<int, int> importFdm(const QList<FdmImport::Item>& items);
    bool antivirusCheck() const;
    void setAntivirusCheck(bool on);

    // Network: proxy for all web traffic and the per-server connection cap.
    ProxySettings proxySettings() const;
    void setProxySettings(const ProxySettings& ps);
    QString proxyFor(const QString& url) const; // TaskParams::proxy for this link
    int maxConnectionsPerServer() const;
    void setMaxConnectionsPerServer(int n);
    static void applyQtProxy(const ProxySettings& ps);
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
    void tagsChanged();
    void queueReordered();
    void conversionFinished(Download* d, const QString& output, const QString& error);
    void antivirusChecked(Download* d, bool blocked, const QString& message);
    void storageError(const QString& message); // the database could not be written (rate-limited)

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
    void convertToMp4(Download* d);
    void openDownloadedTorrent(Download* d);
    void checkWithAntivirus(Download* d);
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
    qint64 m_lastStorageError = 0; // ms since epoch of the last storageError signal
    QHash<QString, QPair<QString, QString>> m_sessionLogins; // server -> user, password
    QList<Tag> m_tags;
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
