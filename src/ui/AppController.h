#pragma once
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
    Q_INVOKABLE QString addDownloads(const QString& text, const QString& folder, const QString& fileName, bool start,
                                     const QVariantMap& schedule = {});
    Q_INVOKABLE int countLinks(const QString& text) const;
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
    Q_INVOKABLE void forgetLogins();
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
    void trafficModeChanged();
    void settingsChanged();
    void showWindowRequested();
    void addDialogRequested(const QString& urls);
    void downloadCompleted(const QString& title, const QString& filePath);
    void hiddenToTray();
    void whenDoneChanged();
    void whenDoneCountdown(int action);
    void notice(const QString& text); // short message for the toast
    void loginRequired(QObject* download); // a Download; QObject* keeps this header free of core includes

private:
    Download* byId(qint64 id) const;
    QString doOpen(qint64 id);
    void watch(Download* d);
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
    QPointer<Download> m_selected;
    QList<qint64> m_sel; // multi-selection (includes the current item)
    int m_selRev = 0;
    void selectionUpdated();
    QVariantMap m_counts;
    QTimer m_recountTimer;
    double m_totalSpeed = 0;
    double m_totalUpload = 0;
    int m_activeCount = 0;
    int m_whenDone = 0; // 0 nothing, 1 sleep, 2 hibernate, 3 shut down, 4 exit FluxDM
    QString m_lastClip;
    QHash<QString, QVariantMap> m_browserMeta; // url -> referrer/cookies/userAgent/fileName from the browser
    bool m_trayHintShown = false;
};
