#pragma once
#include "core/SleepGuard.h"
#include "core/TorrentFolderWatcher.h"
#include "core/RemoteServer.h"
#include "core/Updater.h"
#include "core/MeteredGuard.h"
#include <QAbstractItemModel>
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class Database;
class Download;
class DownloadManager;
class DownloadsModel;
class DownloadsFilterModel;
class QQmlEngine;
class QJSEngine;

// Facade exposed to QML as the singleton `App`.
class AppController : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(App)
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel* downloads READ downloads CONSTANT)
    Q_PROPERTY(QObject* selected READ selected NOTIFY selectedChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    Q_PROPERTY(int selectionRev READ selectionRev NOTIFY selectionChanged) // bump to re-evaluate isSelected()
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString search READ search WRITE setSearch NOTIFY searchChanged)
    Q_PROPERTY(QVariantMap counts READ counts NOTIFY countsChanged)
    Q_PROPERTY(double totalSpeed READ totalSpeed NOTIFY statsChanged)
    Q_PROPERTY(double totalUpload READ totalUpload NOTIFY statsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY statsChanged)
    // Total download speed, one sample a second, oldest first (the last 2 minutes).
    Q_PROPERTY(QVariantList speedHistory READ speedHistory NOTIFY speedHistoryChanged)
    Q_PROPERTY(int trafficMode READ trafficMode WRITE setTrafficMode NOTIFY trafficModeChanged)
    Q_PROPERTY(bool trafficScheduled READ trafficScheduled NOTIFY trafficModeChanged)
    Q_PROPERTY(int whenDone READ whenDone WRITE setWhenDone NOTIFY whenDoneChanged)
    Q_PROPERTY(bool torrentSupport READ torrentSupport CONSTANT)
    Q_PROPERTY(bool browserAddSilently READ browserAddSilently WRITE setBrowserAddSilently NOTIFY settingsChanged)
    Q_PROPERTY(bool startHidden READ startHidden CONSTANT)
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY settingsChanged)
    Q_PROPERTY(bool compactView READ compactView WRITE setCompactView NOTIFY settingsChanged)
    Q_PROPERTY(bool closeButtonHidesApp READ closeButtonHidesApp WRITE setCloseButtonHidesApp NOTIFY settingsChanged)
    Q_PROPERTY(bool monitorClipboard READ monitorClipboard WRITE setMonitorClipboard NOTIFY settingsChanged)
    Q_PROPERTY(bool preventSleep READ preventSleep WRITE setPreventSleep NOTIFY settingsChanged)
    Q_PROPERTY(bool startWithWindows READ startWithWindows WRITE setStartWithWindows NOTIFY settingsChanged)
    Q_PROPERTY(QString userAgent READ userAgent WRITE setUserAgent NOTIFY settingsChanged)
    Q_PROPERTY(QString defaultUserAgent READ defaultUserAgent CONSTANT)
    Q_PROPERTY(int ipVersion READ ipVersion WRITE setIpVersion NOTIFY settingsChanged)
    Q_PROPERTY(int autoRemoveDays READ autoRemoveDays WRITE setAutoRemoveDays NOTIFY settingsChanged)
    Q_PROPERTY(int btListenPort READ btListenPort WRITE setBtListenPort NOTIFY settingsChanged)
    Q_PROPERTY(int zoom READ zoom WRITE setZoom NOTIFY settingsChanged)      // percent; applies after a restart
    Q_PROPERTY(int zoomInEffect READ zoomInEffect CONSTANT)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY settingsChanged) // "" = Windows' language
    Q_PROPERTY(QString languageInEffect READ languageInEffect CONSTANT)
    Q_PROPERTY(QVariantList languages READ languages CONSTANT) // [{code, name}]
    Q_PROPERTY(bool previousSessionCrashed READ previousSessionCrashed CONSTANT)
    // Remote control from the local network (off by default).
    Q_PROPERTY(bool remoteEnabled READ remoteEnabled WRITE setRemoteEnabled NOTIFY remoteChanged)
    Q_PROPERTY(int remotePort READ remotePort WRITE setRemotePort NOTIFY remoteChanged)
    Q_PROPERTY(QString remoteCode READ remoteCode NOTIFY remoteChanged)
    Q_PROPERTY(QStringList remoteAddresses READ remoteAddresses NOTIFY remoteChanged)
    Q_PROPERTY(QString remoteError READ remoteError NOTIFY remoteChanged)
    // Updates: state 0 idle, 1 checking, 2 update available, 3 downloading, 4 ready to install, 5 error
    Q_PROPERTY(QString updateUrl READ updateUrl WRITE setUpdateUrl NOTIFY updateChanged)
    Q_PROPERTY(bool autoCheckUpdates READ autoCheckUpdates WRITE setAutoCheckUpdates NOTIFY updateChanged)
    Q_PROPERTY(int updateState READ updateState NOTIFY updateChanged)
    Q_PROPERTY(QString updateStatus READ updateStatus NOTIFY updateChanged)
    Q_PROPERTY(bool chooseTorrentFiles READ chooseTorrentFiles WRITE setChooseTorrentFiles NOTIFY settingsChanged)
    Q_PROPERTY(bool antivirusCheck READ antivirusCheck WRITE setAntivirusCheck NOTIFY settingsChanged)
    Q_PROPERTY(int meteredPolicy READ meteredPolicy WRITE setMeteredPolicy NOTIFY settingsChanged) // 0 ask, 1 pause, 2 ignore
    Q_PROPERTY(QVariantList tags READ tags NOTIFY tagsChanged) // [{id, name, color, extensions, folder}]
    Q_PROPERTY(QString torrentWatchFolder READ torrentWatchFolder WRITE setTorrentWatchFolder NOTIFY settingsChanged) // "" = off
    Q_PROPERTY(QString sortKey READ sortKey NOTIFY sortChanged)             // added | name | size | status
    Q_PROPERTY(bool sortDescending READ sortDescending NOTIFY sortChanged)
    Q_PROPERTY(bool notifyOnComplete READ notifyOnComplete WRITE setNotifyOnComplete NOTIFY settingsChanged)
    Q_PROPERTY(bool soundOnComplete READ soundOnComplete WRITE setSoundOnComplete NOTIFY settingsChanged)
    Q_PROPERTY(QString defaultFolder READ defaultFolder WRITE setDefaultFolder NOTIFY settingsChanged)
    Q_PROPERTY(QStringList folderHistory READ folderHistory NOTIFY settingsChanged)
    Q_PROPERTY(bool bottomPanelOpen READ bottomPanelOpen WRITE setBottomPanelOpen NOTIFY settingsChanged)
    Q_PROPERTY(int bottomPanelTab READ bottomPanelTab WRITE setBottomPanelTab NOTIFY settingsChanged)
    Q_PROPERTY(double bottomPanelHeight READ bottomPanelHeight WRITE setBottomPanelHeight NOTIFY settingsChanged)
    Q_PROPERTY(QUrl settingsFileUrl READ settingsFileUrl CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QString devOpen READ devOpen CONSTANT) // FLUXDM_DEV_OPEN: open a dialog at startup (screenshots)

public:
    AppController(DownloadManager* mgr, const QString& dataDir, QObject* parent = nullptr);
    ~AppController() override;

    static inline AppController* s_instance = nullptr;
    static AppController* create(QQmlEngine*, QJSEngine*);

    QAbstractItemModel* downloads() const;
    QObject* selected() const;
    QString filter() const;
    void setFilter(const QString& f);
    QString search() const;
    void setSearch(const QString& s);
    QVariantMap counts() const { return m_counts; }
    double totalSpeed() const { return m_totalSpeed; }
    QVariantList speedHistory() const;
    Q_INVOKABLE QVariantMap usage() const; // see DownloadManager::usage()
    double totalUpload() const { return m_totalUpload; }
    int activeCount() const { return m_activeCount; }
    int trafficMode() const;
    void setTrafficMode(int m);
    bool trafficScheduled() const;
    int whenDone() const { return m_whenDone; }
    void setWhenDone(int a);
    bool torrentSupport() const;
    bool browserAddSilently() const;
    void setBrowserAddSilently(bool v);
    bool startHidden() const;

    QString theme() const;
    void setTheme(const QString& t);
    bool compactView() const;
    void setCompactView(bool v);
    bool closeButtonHidesApp() const;
    void setCloseButtonHidesApp(bool v);
    bool monitorClipboard() const;
    bool preventSleep() const;
    bool startWithWindows() const;
    QString userAgent() const;
    QString defaultUserAgent() const;
    int ipVersion() const;
    int autoRemoveDays() const;
    int btListenPort() const;
    int zoom() const;
    void setZoom(int percent);
    int zoomInEffect() const;
    QString language() const;
    void setLanguage(const QString& code);
    QString languageInEffect() const { return m_languageAtStart; }
    QVariantList languages() const;
    Q_INVOKABLE void restartApp();
    bool previousSessionCrashed() const;
    bool remoteEnabled() const;
    void setRemoteEnabled(bool on);
    int remotePort() const;
    void setRemotePort(int port);
    QString remoteCode() const;
    QStringList remoteAddresses() const;
    QString remoteError() const { return m_remoteError; }
    Q_INVOKABLE void newRemoteCode();
    QString updateUrl() const;
    void setUpdateUrl(const QString& url);
    bool autoCheckUpdates() const;
    void setAutoCheckUpdates(bool on);
    int updateState() const { return m_updateState; }
    QString updateStatus() const { return m_updateStatus; }
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadUpdate();
    Q_INVOKABLE void installUpdate();
    // Report a problem: the text that would be shared, and ways to share it (nothing is sent).
    Q_INVOKABLE QString problemReport() const;
    Q_INVOKABLE void copyProblemReport();
    Q_INVOKABLE QString saveProblemReport(const QUrl& file); // error text or ""
    Q_INVOKABLE void openLogsFolder() const;
    bool chooseTorrentFiles() const;
    bool antivirusCheck() const;
    int meteredPolicy() const;
    void setMeteredPolicy(int p);
    Q_INVOKABLE void pauseOnMetered(bool remember);
    Q_INVOKABLE void keepDownloadingOnMetered(bool remember);
    void setAntivirusCheck(bool on);
    QVariantList tags() const;
    QString torrentWatchFolder() const;
    void setTorrentWatchFolder(const QString& folder);
    void setChooseTorrentFiles(bool on);
    QString sortKey() const;
    bool sortDescending() const;
    Q_INVOKABLE void setSort(const QString& key, bool descending);
    void setPreventSleep(bool on);
    void setStartWithWindows(bool on);
    void setUserAgent(const QString& ua);
    void setIpVersion(int v);
    void setAutoRemoveDays(int days);
    void setBtListenPort(int port); // used from the next start
    void setMonitorClipboard(bool v);
    bool notifyOnComplete() const;
    void setNotifyOnComplete(bool v);
    bool soundOnComplete() const;
    void setSoundOnComplete(bool v);
    QString defaultFolder() const;
    void setDefaultFolder(const QString& f);
    QStringList folderHistory() const;
    bool bottomPanelOpen() const;
    void setBottomPanelOpen(bool v);
    int bottomPanelTab() const;
    void setBottomPanelTab(int v);
    double bottomPanelHeight() const;
    void setBottomPanelHeight(double v);
    QUrl settingsFileUrl() const;
    QString version() const;
    QString devOpen() const { return qEnvironmentVariable("FLUXDM_DEV_OPEN"); }

    // --- actions
    // existingFile: FileNamer::ExistingFile for these downloads, -1 = the default from Settings
    Q_INVOKABLE QString addDownloads(const QString& text, const QString& folder, const QString& fileName, bool start,
                                     const QVariantMap& schedule = {}, int existingFile = -1, const QString& checksum = {});
    Q_INVOKABLE QString verifyChecksum(qint64 id, const QString& expected); // "" = started
    Q_INVOKABLE bool isChecksum(const QString& text) const; // a hash of a known length is in it
    Q_INVOKABLE int countLinks(const QString& text) const;
    // Links in `text` that are already in the list: {count, id, title, state} of the first one; empty if none.
    Q_INVOKABLE QVariantMap duplicateOf(const QString& text) const;
    Q_INVOKABLE bool isTorrentLink(const QString& text) const;
    Q_INVOKABLE QString linkModule(const QString& text) const; // module uid of the first link, "" if none
    Q_INVOKABLE void stopSeeding(qint64 id);
    Q_INVOKABLE void setFileWanted(qint64 id, int index, bool wanted);

    // --- scheduling
    Q_INVOKABLE QVariantList trafficRules() const;
    Q_INVOKABLE void setTrafficRules(const QVariantList& rules);
    Q_INVOKABLE QVariantMap downloadSchedule(qint64 id) const;
    Q_INVOKABLE void setDownloadSchedule(qint64 id, bool enabled, const QVariantMap& window);
    Q_INVOKABLE QVariantMap defaultSchedule() const;
    Q_INVOKABLE QString scheduleText(const QVariantMap& window) const;
    Q_INVOKABLE void performWhenDone(int action);

    // --- BitTorrent settings
    Q_INVOKABLE int uploadLimitKBps() const;
    Q_INVOKABLE void setUploadLimitKBps(int kbps);
    Q_INVOKABLE double seedRatio() const;
    Q_INVOKABLE void setSeedRatio(double r);
    Q_INVOKABLE int autoRetryLimit() const;
    Q_INVOKABLE QString exportDownloads(const QUrl& file) const;   // returns a status message
    Q_INVOKABLE QString importDownloads(const QUrl& file);
    Q_INVOKABLE bool fdmAvailable() const;   // Free Download Manager's list exists on this PC
    Q_INVOKABLE QString importFromFdm();     // status text
    Q_INVOKABLE QString categoryFolder(const QString& category) const;     // "" = use the default folder
    Q_INVOKABLE void setCategoryFolder(const QString& category, const QString& folder);
    Q_INVOKABLE QString folderFor(const QString& links) const;             // best folder for the first link
    Q_INVOKABLE QString categoryOf(const QString& links) const;
    Q_INVOKABLE void setAutoRetryLimit(int n);
    Q_INVOKABLE bool torrentAssociated() const;
    Q_INVOKABLE void setTorrentAssociated(bool on);

    // --- browser integration (native messaging host + extension)
    Q_INVOKABLE bool browserIntegrated() const;
    Q_INVOKABLE QString setBrowserIntegrated(bool on); // returns error text or ""
    Q_INVOKABLE QString extensionDir() const;
    Q_INVOKABLE void openExtensionDir() const;
    Q_INVOKABLE QString firefoxExtensionDir() const;
    Q_INVOKABLE void openFirefoxExtensionDir() const;
    Q_INVOKABLE QString suggestedName(const QString& url) const;
    QString handleBrowserRequest(const QVariantMap& request); // returns error text or ""
    Q_INVOKABLE void select(qint64 id);              // single selection (plain click)
    Q_INVOKABLE void toggleSelect(qint64 id);        // Ctrl+click
    Q_INVOKABLE void selectRange(qint64 id);         // Shift+click, from the current item
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isSelected(qint64 id) const { return m_sel.contains(id); }
    Q_INVOKABLE QVariantList selectedIds() const;
    Q_INVOKABLE void pauseSelected();
    Q_INVOKABLE void resumeSelected();
    Q_INVOKABLE void removeSelected(bool deleteFiles);
    Q_INVOKABLE void copySelectedLinks();
    Q_INVOKABLE QString openSelected();              // opens the finished files; status text or ""
    Q_INVOKABLE void setPrioritySelected(int priority);
    Q_INVOKABLE void moveInQueue(qint64 id, const QString& where);   // top | up | down | bottom
    Q_INVOKABLE void moveSelectionInQueue(const QString& where);
    int selectionCount() const { return int(m_sel.size()); }
    int selectionRev() const { return m_selRev; }
    Q_INVOKABLE void pause(qint64 id);
    Q_INVOKABLE void resume(qint64 id);
    Q_INVOKABLE void togglePause(qint64 id);
    Q_INVOKABLE void restart(qint64 id);
    Q_INVOKABLE void remove(qint64 id, bool deleteFiles);
    Q_INVOKABLE void setPriority(qint64 id, int priority);
    Q_INVOKABLE void setDownloadSpeedLimit(qint64 id, int kbps);
    Q_INVOKABLE QString moveSelected(const QUrl& folder); // error text or ""
    Q_INVOKABLE bool canMove(qint64 id) const;
    Q_INVOKABLE bool convertStreams() const;
    Q_INVOKABLE void setConvertStreams(bool on);
    Q_INVOKABLE QString ffmpegSetting() const;
    Q_INVOKABLE void setFfmpegSetting(const QString& path);
    Q_INVOKABLE QString ffmpegFound() const; // resolved path or ""
    Q_INVOKABLE QString ytdlpSetting() const;
    Q_INVOKABLE void setYtdlpSetting(const QString& path);
    Q_INVOKABLE QString ytdlpFound() const;
    Q_INVOKABLE void pauseAll();
    Q_INVOKABLE void resumeAll();
    Q_INVOKABLE QString openFile(qint64 id);         // error text or ""; falls back to "Open with"
    Q_INVOKABLE QString openWith(qint64 id);         // Windows "Open with" chooser; error text or ""
    Q_INVOKABLE void openFolder(qint64 id);
    Q_INVOKABLE void openPage(qint64 id);            // the web page the download came from
    Q_INVOKABLE bool hasPage(qint64 id) const;
    Q_INVOKABLE void copyLink(qint64 id);
    Q_INVOKABLE void copyPath(qint64 id);
    Q_INVOKABLE QString rename(qint64 id, const QString& newName); // error text or ""
    Q_INVOKABLE bool canRename(qint64 id) const;
    Q_INVOKABLE QString loginServer(qint64 id) const;  // "https://host:443"
    Q_INVOKABLE QString loginUser(qint64 id) const;    // known user name for that server, or ""
    Q_INVOKABLE void signIn(qint64 id, const QString& user, const QString& password, bool remember);
    Q_INVOKABLE int savedLoginCount() const;
    Q_INVOKABLE int existingFileDefault() const;
    Q_INVOKABLE void setExistingFileDefault(int reaction);
    Q_INVOKABLE bool fileExistsIn(const QString& folder, const QString& name) const;
    // Adds without the dialog (browser, command line, remote page). A link that is already being
    // downloaded (queued, running, paused) isn't added twice: the existing one is selected instead.
    QString addSilently(const QString& text, const QString& folder, const QString& fileName);
    Q_INVOKABLE QString expectedName(const QString& text) const; // likely file name of a single link
    Q_INVOKABLE void resolveExisting(qint64 id, int reaction); // answer to "file already exists"
    Q_INVOKABLE void chooseFiles(qint64 id, const QVariantList& wantedIndexes);
    Q_INVOKABLE void setSequential(qint64 id, bool on);
    Q_INVOKABLE QVariantMap torrentDetails(qint64 id) const; // {peers: [...], trackers: [...]}
    // User tags. saveTag: id 0 creates; returns the id (0 if the name is empty or taken).
    Q_INVOKABLE int saveTag(int id, const QString& name, const QString& color, const QString& extensions, const QString& folder);
    Q_INVOKABLE void removeTag(int id);
    Q_INVOKABLE void setTag(qint64 downloadId, int tagId, bool on);
    Q_INVOKABLE void setSelectionTag(int tagId, bool on);
    Q_INVOKABLE int selectionTagState(int tagId) const; // 0 none, 1 some, 2 all selected have it
    Q_INVOKABLE bool tagNameTaken(const QString& name, int exceptId) const;
    // MP3/MP4 converter. Options: {vbr, bitrate, quality, folder}; remembered per format.
    Q_INVOKABLE QString convertFile(qint64 id, const QString& format, const QVariantMap& options);
    Q_INVOKABLE QVariantMap convertDefaults(const QString& format) const;
    Q_INVOKABLE bool canConvert(qint64 id, const QString& format) const;
    // Torrent audio/video: download in order and open the partial file once its start is there.
    Q_INVOKABLE void playWhileDownloading(qint64 id);
    // Network: {mode 0 system / 1 none / 2 manual, type http|socks5, host, port, user, hasPassword}
    Q_INVOKABLE QVariantMap proxySettings() const;
    // password: "" with keepPassword keeps the saved one. Returns an error text or "".
    Q_INVOKABLE QString setProxySettings(int mode, const QString& type, const QString& host, int port,
                                         const QString& user, const QString& password, bool keepPassword);
    Q_INVOKABLE int maxConnectionsPerServer() const;
    Q_INVOKABLE void setMaxConnectionsPerServer(int n);
    Q_INVOKABLE bool canPlayWhileDownloading(qint64 id) const;
    Q_INVOKABLE void forgetLogins();
    Q_INVOKABLE void trustCertificate(qint64 id);
    Q_INVOKABLE QString mirrors(qint64 id) const;                      // one per line
    Q_INVOKABLE QString setMirrors(qint64 id, const QString& text);    // error text or ""
    Q_INVOKABLE int trustedHostCount() const;
    Q_INVOKABLE void forgetTrustedHosts();
    Q_INVOKABLE void quit();
    Q_INVOKABLE void notifyHiddenToTray();

    // --- traffic mode settings
    Q_INVOKABLE QVariantMap tumSettings(int mode) const;
    Q_INVOKABLE void setTumSettings(int mode, int maxDownloads, int maxConnections, int speedLimitKBps);
    Q_INVOKABLE QString tumDescription(int mode) const;

    // --- helpers
    Q_INVOKABLE QString clipboardUrls() const;
    Q_INVOKABLE QString fmtSize(qint64 bytes) const;
    Q_INVOKABLE QString fmtSpeed(double bytesPerSec) const;
    Q_INVOKABLE QString fmtEta(qint64 seconds) const;
    Q_INVOKABLE QString fmtDate(const QDateTime& dt) const;
    Q_INVOKABLE QUrl toUrl(const QString& localPath) const;
    Q_INVOKABLE QString urlToLocal(const QUrl& url) const;

    void handleExternalArgs(const QStringList& args);
    void requestShow() { emit showWindowRequested(); }

signals:
    void selectedChanged();
    void selectionChanged();
    void filterChanged();
    void searchChanged();
    void countsChanged();
    void statsChanged();
    void speedHistoryChanged();
    void trafficModeChanged();
    void settingsChanged();
    void showWindowRequested();
    void addDialogRequested(const QString& urls);
    void downloadCompleted(const QString& title, const QString& filePath);
    void hiddenToTray();
    void whenDoneChanged();
    void whenDoneCountdown(int action);
    void notice(const QString& text); // short message for the toast
    void sortChanged();
    void remoteChanged();
    void updateChanged();
    void tagsChanged();
    void fileConflict(QObject* download);  // "ask" found an existing file
    void certificateRequired(QObject* download); // untrusted TLS certificate
    void meteredQuestion(int activeDownloads);
    void filesRequired(QObject* download); // torrent waits for the user to pick files
    void loginRequired(QObject* download); // a Download; QObject* keeps this header free of core includes

private:
    Download* byId(qint64 id) const;
    QString doOpen(qint64 id);
    void watch(Download* d);
    QVariantMap playableFile(const Download* d) const;
    QString doOpenWith(qint64 id);
    void scheduleRecount();
    void recount();
    void updateStats();
    void rememberFolder(const QString& dir);
    void onClipboardChanged();

    DownloadManager* m_mgr;
    DownloadsModel* m_model;
    DownloadsFilterModel* m_proxy;
    QSettings m_ui;
    SleepGuard m_sleepGuard; // keeps the PC awake while downloading
    TorrentFolderWatcher* m_torrentWatcher = nullptr;
    RemoteServer* m_remote = nullptr;
    Updater* m_updater = nullptr;
    MeteredGuard* m_metered = nullptr;
    int m_updateState = 0;
    QString m_updateStatus, m_updateFile;
    void setUpdate(int state, const QString& status);
    QString m_remoteError;
    QString m_languageAtStart;
    void applyRemote();
    QList<qint64> m_playWhenReady; // playWhileDownloading() waiting for enough data
    QPointer<Download> m_selected;
    QList<qint64> m_sel; // multi-selection (includes the current item)
    int m_selRev = 0;
    void selectionUpdated();
    QVariantMap m_counts;
    QTimer m_recountTimer;
    double m_totalSpeed = 0;
    QList<double> m_speedHistory;
    QTimer m_speedSampler;
    double m_totalUpload = 0;
    int m_activeCount = 0;
    int m_whenDone = 0; // 0 nothing, 1 sleep, 2 hibernate, 3 shut down, 4 exit FluxDM
    QString m_lastClip;
    QHash<QString, QVariantMap> m_browserMeta; // url -> referrer/cookies/userAgent/fileName from the browser
    bool m_trayHintShown = false;
};
