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
class ExternalTools;

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
    // A download of the same address already in the list (newest first); the #fragment is ignored.
    Download* findByUrl(const QString& url) const;
    // Compares a finished file with a published checksum (MD5/SHA-1/SHA-256/SHA-512) on a worker
    // thread; `expected` "" re-checks the one stored with the download. Returns an error text.
    QString verifyChecksum(Download* d, const QString& expected = {});
    void setExpectedChecksum(Download* d, const QString& text); // checked when the download finishes
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
    // A new link for an unfinished web download (the old one expired). The part already downloaded
    // is kept and continued if the new link serves a file of the same size. `page`, when given,
    // becomes the download page. Error text or "".
    QString changeAddress(Download* d, const QString& url, const QString& page = {});
    static bool canChangeAddress(const Download* d);
    // Resume data that makes the engine use the record's URL and check it is the same file.
    static QByteArray withNewAddress(const QByteArray& resumeData);

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
    // Archives (.zip, .7z, .rar, .tar.*) unpacked into a folder next to them when they finish.
    bool extractArchives() const;
    void setExtractArchives(bool on);
    QString extractorPath() const;                // 7-Zip if installed, else Windows' tar.exe; "" = none
    static bool isArchive(const QString& path);
    bool canExtract(const Download* d) const;     // finished archive the extractor can open
    QString extract(Download* d);                 // starts it; returns an error text
    // A Metalink file: one download per entry, with its mirrors and checksum. Returns them.
    QList<Download*> addFromMetalink(const QString& metalinkPath, const QString& destDir, bool start, const QString& referrer = {});
    // "Create torrent": a .torrent next to a finished file, then seeding it. Returns an error text.
    bool canCreateTorrent(const Download* d) const;
    QString createTorrent(Download* d);
    void setConvertStreams(bool on);
    QString ffmpegSetting() const;
    QString ytdlpSetting() const;
    void setYtdlpSetting(const QString& path);
    QString ytdlpPath() const; // setting, FLUXDM_YTDLP, FluxDM's own (tools), next to FluxDM, PATH; "" if none
    void setFfmpegSetting(const QString& path);
    QString ffmpegPath() const; // setting, FLUXDM_FFMPEG, FluxDM's own (tools), next to FluxDM, PATH; "" if none
    // The ffmpeg/yt-dlp copies FluxDM ships and updates (none in tests).
    void setTools(ExternalTools* tools) { m_tools = tools; }
    ExternalTools* tools() const { return m_tools; }
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
    QString userAgent() const;              // the setting; "" = defaultUserAgent()
    void setUserAgent(const QString& ua);
    int ipVersion() const;                  // 0 any, 4, 6
    void setIpVersion(int v);
    int autoRemoveDays() const;             // finished downloads leave the list after N days; 0 = never
    void setAutoRemoveDays(int days);
    int removeOldFinished();
    // Bytes downloaded / uploaded: {todayDown, todayUp, monthDown, monthUp, totalDown, totalUp}.
    QVariantMap usage() const;
    void countTraffic(qint64 down, qint64 up, double speed);
    void flushUsage(); // writes what was counted since the last flush (every minute and on exit)
    int btListenPort() const;               // BitTorrent listening port, used from the next start
    void setBtListenPort(int port);                // returns how many were removed (files stay on disk)

signals:
    void downloadAdded(Download* d);
    void downloadRemoved(Download* d);
    void downloadFinished(Download* d);
    void statsUpdated(); // at most every 100 ms, however many downloads report
    void trafficModeChanged();
    void allDownloadsFinished();
    void tagsChanged();
    void queueReordered();
    void conversionFinished(Download* d, const QString& output, const QString& error);
    void antivirusChecked(Download* d, bool blocked, const QString& message);
    void extracted(Download* d, const QString& folder, const QString& error);
    void torrentCreated(const QString& torrentPath, const QString& error);
    void storageError(const QString& message); // the database could not be written (rate-limited)
    void checksumChecked(Download* d, bool match, const QString& text);

public slots:
    // Downloads waiting for disk space continue once there is enough (also run every 15 s).
    void recheckDiskSpace();

private:
    void schedule();
    void evaluateTrafficRules();
    bool startDownload(Download* d); // false: not started (no engine, not enough disk space)
    void stopDownload(Download* d);
    void persist(Download* d);
    void applyLimits();
    void checkAllDone();
    void runAutoRetries();
    void checkMissingFiles();
    void convertToMp4(Download* d);
    void openDownloadedTorrent(Download* d);
    void openDownloadedMetalink(Download* d);
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
    ExternalTools* m_tools = nullptr;
    qint64 m_lastStorageError = 0; // ms since epoch of the last storageError signal
    qint64 m_pendingDown = 0, m_pendingUp = 0; // counted, not yet in usageStats
    bool m_checkingFiles = false; // checkMissingFiles() is running on its thread
    QString m_pendingDay;
    QHash<QString, QPair<QString, QString>> m_sessionLogins; // server -> user, password
    QList<Tag> m_tags;
    QList<DownloadEngine*> m_engines;
    QList<Download*> m_list; // newest first
    QHash<qint64, Download*> m_byId; // same downloads, for find()
    QTimer m_statsTimer; // folds the per-task stats signals into one statsUpdated per tick
    void statsChanged() { if (!m_statsTimer.isActive()) m_statsTimer.start(); }
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
