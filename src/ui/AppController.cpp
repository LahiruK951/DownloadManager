#include "ui/AppController.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <powrprof.h>
#include <shlobj.h>
#endif

#include "core/BatchLinks.h"
#include "core/Download.h"
#include "core/DownloadManager.h"
#include "core/FileNamer.h"
#include "ui/DownloadsModel.h"

namespace {
const QStringList kCategories = {QStringLiteral("Video"), QStringLiteral("Music"), QStringLiteral("Archive"),
                                  QStringLiteral("Docs"), QStringLiteral("Programs"), QStringLiteral("Pictures"),
                                  QStringLiteral("Other")};

// Extensions that make a copied link worth offering for download (clipboard monitor).
const QStringList kCatchExtensions = {
    QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"), QStringLiteral("gz"), QStringLiteral("xz"),
    QStringLiteral("bz2"), QStringLiteral("tar"), QStringLiteral("iso"), QStringLiteral("exe"), QStringLiteral("msi"),
    QStringLiteral("apk"), QStringLiteral("dmg"), QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
    QStringLiteral("mov"), QStringLiteral("webm"), QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("wav"),
    QStringLiteral("m4a"), QStringLiteral("pdf"), QStringLiteral("epub"), QStringLiteral("deb"), QStringLiteral("rpm"),
    QStringLiteral("img"), QStringLiteral("bin"), QStringLiteral("torrent")};
} // namespace

AppController::AppController(DownloadManager* mgr, const QString& dataDir, QObject* parent)
    : QObject(parent),
      m_mgr(mgr),
      m_model(new DownloadsModel(mgr, this)),
      m_proxy(new DownloadsFilterModel(this)),
      m_ui(QDir(dataDir).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat)
{
    m_proxy->setSourceModel(m_model);

    m_recountTimer.setSingleShot(true);
    m_recountTimer.setInterval(150);
    connect(&m_recountTimer, &QTimer::timeout, this, &AppController::recount);

    connect(m_mgr, &DownloadManager::downloadAdded, this, [this](Download* d) {
        connect(d, &Download::stateChanged, this, &AppController::scheduleRecount);
        connect(d, &Download::titleChanged, this, &AppController::scheduleRecount);
        scheduleRecount();
    });
    connect(m_mgr, &DownloadManager::downloadRemoved, this, [this](Download* d) {
        if (m_selected == d) {
            m_selected = nullptr;
            emit selectedChanged();
        }
        scheduleRecount();
    });
    connect(m_mgr, &DownloadManager::downloadFinished, this, [this](Download* d) {
        emit downloadCompleted(d->title(), d->filePath());
    });
    connect(m_mgr, &DownloadManager::statsUpdated, this, &AppController::updateStats);
    connect(m_mgr, &DownloadManager::trafficModeChanged, this, &AppController::trafficModeChanged);
    connect(m_mgr, &DownloadManager::allDownloadsFinished, this, [this] {
        if (m_whenDone != 0)
            emit whenDoneCountdown(m_whenDone);
    });
    for (Download* d : m_mgr->downloads()) {
        connect(d, &Download::stateChanged, this, &AppController::scheduleRecount);
        connect(d, &Download::titleChanged, this, &AppController::scheduleRecount);
    }
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &AppController::onClipboardChanged);
    m_lastClip = QGuiApplication::clipboard()->text().trimmed(); // don't react to what was there before start
    recount();
}

AppController::~AppController()
{
    if (s_instance == this)
        s_instance = nullptr;
}

AppController* AppController::create(QQmlEngine*, QJSEngine*)
{
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

Download* AppController::byId(qint64 id) const { return m_mgr->find(id); }

// ------------------------------------------------------------------ properties

QAbstractItemModel* AppController::downloads() const { return m_proxy; }
QObject* AppController::selected() const { return m_selected.data(); }
QString AppController::filter() const { return m_proxy->filter(); }
QString AppController::search() const { return m_proxy->search(); }

void AppController::setFilter(const QString& f)
{
    if (f == m_proxy->filter())
        return;
    m_proxy->setFilter(f);
    emit filterChanged();
}

void AppController::setSearch(const QString& s)
{
    if (s == m_proxy->search())
        return;
    m_proxy->setSearch(s);
    emit searchChanged();
}

int AppController::trafficMode() const { return m_mgr->trafficMode(); }
void AppController::setTrafficMode(int m) { m_mgr->setTrafficMode(m); }
bool AppController::trafficScheduled() const { return m_mgr->trafficModeScheduled(); }

void AppController::setWhenDone(int a)
{
    a = qBound(0, a, 4);
    if (a == m_whenDone)
        return;
    m_whenDone = a;
    emit whenDoneChanged();
}

bool AppController::torrentSupport() const { return !m_mgr->moduleFor(QStringLiteral("magnet:?xt=urn:btih:0000000000000000000000000000000000000000")).isEmpty(); }

#define UI_SETTING(getter, setter, Type, key, def, conv)                 \
    Type AppController::getter() const { return m_ui.value(QStringLiteral(key), def).conv(); } \
    void AppController::setter(Type v)                                   \
    {                                                                    \
        if (getter() == v)                                               \
            return;                                                      \
        m_ui.setValue(QStringLiteral(key), v);                           \
        emit settingsChanged();                                          \
    }

UI_SETTING(compactView, setCompactView, bool, "compactView", false, toBool)
UI_SETTING(closeButtonHidesApp, setCloseButtonHidesApp, bool, "closeButtonHidesApp", true, toBool)
UI_SETTING(monitorClipboard, setMonitorClipboard, bool, "monitorClipboard", true, toBool)
UI_SETTING(notifyOnComplete, setNotifyOnComplete, bool, "notifyOnComplete", true, toBool)
UI_SETTING(bottomPanelOpen, setBottomPanelOpen, bool, "bottomPanelOpened", true, toBool)
UI_SETTING(bottomPanelTab, setBottomPanelTab, int, "bottomPanelCurrentTab", 0, toInt)
UI_SETTING(bottomPanelHeight, setBottomPanelHeight, double, "bottomPanelHeight", 230.0, toDouble)
#undef UI_SETTING

QString AppController::theme() const
{
    const QString forced = qEnvironmentVariable("FLUXDM_THEME"); // dev/screenshots
    return !forced.isEmpty() ? forced : m_ui.value(QStringLiteral("theme"), QStringLiteral("system")).toString();
}
void AppController::setTheme(const QString& t)
{
    if (t == theme())
        return;
    m_ui.setValue(QStringLiteral("theme"), t);
    emit settingsChanged();
}

QString AppController::defaultFolder() const
{
    return m_ui.value(QStringLiteral("defaultFolder"),
                      QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)))
        .toString();
}

void AppController::setDefaultFolder(const QString& f)
{
    if (f == defaultFolder())
        return;
    m_ui.setValue(QStringLiteral("defaultFolder"), QDir::toNativeSeparators(f));
    emit settingsChanged();
}

QStringList AppController::folderHistory() const
{
    QStringList h = m_ui.value(QStringLiteral("folderHistory")).toStringList();
    const QString def = defaultFolder();
    h.removeAll(def);
    h.prepend(def);
    return h;
}

void AppController::rememberFolder(const QString& dir)
{
    QStringList h = m_ui.value(QStringLiteral("folderHistory")).toStringList();
    const QString n = QDir::toNativeSeparators(dir);
    h.removeAll(n);
    h.prepend(n);
    while (h.size() > 10)
        h.removeLast();
    m_ui.setValue(QStringLiteral("folderHistory"), h);
    emit settingsChanged();
}

QUrl AppController::settingsFileUrl() const { return QUrl::fromLocalFile(m_ui.fileName()); }
QString AppController::version() const { return QStringLiteral(FLUXDM_VERSION); }

// ------------------------------------------------------------------ actions

QString AppController::addDownloads(const QString& text, const QString& folder, const QString& fileName, bool start,
                                    const QVariantMap& schedule)
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList urls = BatchLinks::expandAll(text.split(ws, Qt::SkipEmptyParts));
    if (urls.isEmpty())
        return tr("Enter at least one link.");
    QString dir = folder.trimmed();
    if (dir.isEmpty())
        dir = defaultFolder();
    if (!QDir().mkpath(dir))
        return tr("Cannot create folder %1").arg(dir);
    for (const QString& u : urls)
        if (!m_mgr->canHandle(u))
            return tr("Unsupported link: %1").arg(u);

    const bool sched = schedule.value(QStringLiteral("enabled")).toBool();
    const ScheduleWindow w = ScheduleWindow::fromMap(schedule);
    if (sched)
        m_ui.setValue(QStringLiteral("lastSchedule"), w.toMap());
    Download* last = nullptr;
    for (const QString& u : urls)
        last = m_mgr->add(u, QDir::toNativeSeparators(dir), urls.size() == 1 ? fileName.trimmed() : QString(), start || sched,
                          {}, {}, {}, sched ? &w : nullptr);
    rememberFolder(dir);
    if (last)
        select(last->id());
    return {};
}

void AppController::select(qint64 id)
{
    Download* d = byId(id);
    if (m_selected == d)
        return;
    m_selected = d;
    emit selectedChanged();
}

void AppController::pause(qint64 id) { m_mgr->pause(byId(id)); }
void AppController::resume(qint64 id) { m_mgr->resume(byId(id)); }

void AppController::togglePause(qint64 id)
{
    Download* d = byId(id);
    if (!d)
        return;
    if (d->completed() && d->seeding())
        m_mgr->stopSeeding(d);
    else if (d->running() || d->queued())
        m_mgr->pause(d);
    else
        m_mgr->resume(d);
}

void AppController::restart(qint64 id) { m_mgr->restart(byId(id)); }
void AppController::remove(qint64 id, bool deleteFiles) { m_mgr->remove(byId(id), deleteFiles); }
void AppController::setPriority(qint64 id, int priority) { m_mgr->setPriority(byId(id), priority); }
void AppController::pauseAll() { m_mgr->pauseAll(); }
void AppController::resumeAll() { m_mgr->resumeAll(); }

void AppController::openFile(qint64 id)
{
    Download* d = byId(id);
    if (d && d->completed() && QFileInfo::exists(d->filePath()))
        QDesktopServices::openUrl(QUrl::fromLocalFile(d->filePath()));
}

void AppController::openFolder(qint64 id)
{
    Download* d = byId(id);
    if (!d)
        return;
    QString path = d->filePath();
    if (!path.isEmpty() && !d->completed())
        path += FileNamer::kPartSuffix;
#ifdef Q_OS_WIN
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,") + QDir::toNativeSeparators(path)});
        return;
    }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(d->destDir()));
}

void AppController::copyLink(qint64 id)
{
    if (Download* d = byId(id)) {
        m_lastClip = d->url();
        QGuiApplication::clipboard()->setText(d->url());
    }
}

void AppController::quit() { QCoreApplication::quit(); }

void AppController::notifyHiddenToTray()
{
    if (m_trayHintShown)
        return;
    m_trayHintShown = true;
    emit hiddenToTray();
}

// ------------------------------------------------------------------ traffic modes

QVariantMap AppController::tumSettings(int mode) const
{
    const auto l = m_mgr->limits(mode);
    return {{QStringLiteral("maxDownloads"), l.maxDownloads},
            {QStringLiteral("maxConnections"), l.maxConnections},
            {QStringLiteral("speedLimit"), l.speedLimitKBps}};
}

void AppController::setTumSettings(int mode, int maxDownloads, int maxConnections, int speedLimitKBps)
{
    m_mgr->setLimits(mode, {maxDownloads, maxConnections, speedLimitKBps});
    emit trafficModeChanged();
}

QString AppController::tumDescription(int mode) const
{
    const auto l = m_mgr->limits(mode);
    const QString speed = l.speedLimitKBps > 0 ? fmtSpeed(double(l.speedLimitKBps) * 1024) : tr("unlimited speed");
    return tr("%1 download(s) at a time, %2 connections each, %3").arg(l.maxDownloads).arg(l.maxConnections).arg(speed);
}

// ------------------------------------------------------------------ stats

void AppController::scheduleRecount() { m_recountTimer.start(); }

void AppController::recount()
{
    QVariantMap c;
    int all = 0, active = 0, completed = 0;
    QHash<QString, int> cats;
    for (Download* d : m_mgr->downloads()) {
        ++all;
        if (d->completed())
            ++completed;
        else
            ++active;
        ++cats[d->category()];
    }
    c[QStringLiteral("all")] = all;
    c[QStringLiteral("active")] = active;
    c[QStringLiteral("completed")] = completed;
    for (const QString& k : kCategories)
        c[k] = cats.value(k);
    m_counts = c;
    emit countsChanged();
    updateStats();
}

void AppController::updateStats()
{
    double speed = 0, up = 0;
    int active = 0;
    for (Download* d : m_mgr->downloads()) {
        if (d->running()) {
            speed += d->speed();
            ++active;
        }
        if (d->running() || d->seeding())
            up += d->uploadSpeed();
    }
    m_totalSpeed = speed;
    m_totalUpload = up;
    m_activeCount = active;
    emit statsChanged();
}

// ------------------------------------------------------------------ helpers

QString AppController::clipboardUrls() const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QStringList out;
    for (const QString& t : QGuiApplication::clipboard()->text().split(ws, Qt::SkipEmptyParts))
        if (m_mgr->canHandle(t))
            out << t;
    return out.join(QLatin1Char('\n'));
}

void AppController::onClipboardChanged()
{
    const QString t = QGuiApplication::clipboard()->text().trimmed();
    if (t == m_lastClip)
        return;
    m_lastClip = t;
    if (!monitorClipboard() || t.isEmpty() || t.size() > 2048 || t.contains(QLatin1Char('\n')))
        return;
    if (!m_mgr->canHandle(t))
        return;
    const QString ext = QFileInfo(QUrl(t).path()).suffix().toLower();
    if (kCatchExtensions.contains(ext))
        emit addDialogRequested(t);
}

QString AppController::fmtSize(qint64 bytes) const
{
    if (bytes < 0)
        return QStringLiteral("?");
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    if (i == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(QString::number(v, 'f', v < 10 ? 2 : (v < 100 ? 1 : 0)), QString::fromLatin1(units[i]));
}

QString AppController::fmtSpeed(double bps) const { return fmtSize(qint64(bps)) + QStringLiteral("/s"); }

QString AppController::fmtEta(qint64 s) const
{
    if (s < 0)
        return QStringLiteral("—");
    if (s < 60)
        return tr("%1 s").arg(s);
    if (s < 3600)
        return tr("%1 min %2 s").arg(s / 60).arg(s % 60);
    if (s < 86400)
        return tr("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
    return tr("%1 d %2 h").arg(s / 86400).arg((s % 86400) / 3600);
}

QString AppController::fmtDate(const QDateTime& dt) const
{
    return dt.isValid() ? QLocale().toString(dt, QLocale::ShortFormat) : QString();
}

QUrl AppController::toUrl(const QString& localPath) const { return QUrl::fromLocalFile(localPath); }
QString AppController::urlToLocal(const QUrl& url) const { return QDir::toNativeSeparators(url.toLocalFile()); }

void AppController::handleExternalArgs(const QStringList& args)
{
    QStringList urls;
    bool silent = false;
    for (const QString& a : args) {
        if (a == QLatin1String("--silent") || a == QLatin1String("-s"))
            silent = true;
        else if (!a.startsWith(QLatin1Char('-')) && m_mgr->canHandle(a))
            urls << a;
    }
    if (urls.isEmpty()) {
        emit showWindowRequested();
        return;
    }
    if (silent)
        addDownloads(urls.join(QLatin1Char('\n')), defaultFolder(), {}, true);
    else
        emit addDialogRequested(urls.join(QLatin1Char('\n')));
}

// ------------------------------------------------------------------ links / torrents

int AppController::countLinks(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    return int(BatchLinks::expandAll(text.split(ws, Qt::SkipEmptyParts)).size());
}

bool AppController::isTorrentLink(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList l = text.split(ws, Qt::SkipEmptyParts);
    return !l.isEmpty() && m_mgr->moduleFor(l.first()) == QLatin1String("downloadsbt");
}

void AppController::stopSeeding(qint64 id) { m_mgr->stopSeeding(byId(id)); }
void AppController::setFileWanted(qint64 id, int index, bool wanted) { m_mgr->setFileWanted(byId(id), index, wanted); }

// ------------------------------------------------------------------ scheduling

QVariantList AppController::trafficRules() const
{
    QVariantList out;
    for (const TrafficRule& r : m_mgr->trafficRules())
        out << r.toMap();
    return out;
}

void AppController::setTrafficRules(const QVariantList& rules)
{
    QList<TrafficRule> list;
    for (const QVariant& v : rules)
        list << TrafficRule::fromMap(v.toMap());
    m_mgr->setTrafficRules(list);
}

QVariantMap AppController::downloadSchedule(qint64 id) const
{
    Download* d = byId(id);
    QVariantMap m = d && d->scheduled() ? d->rec.schedule.toMap() : defaultSchedule();
    m[QStringLiteral("enabled")] = d && d->scheduled();
    return m;
}

void AppController::setDownloadSchedule(qint64 id, bool enabled, const QVariantMap& window)
{
    const ScheduleWindow w = ScheduleWindow::fromMap(window);
    if (enabled)
        m_ui.setValue(QStringLiteral("lastSchedule"), w.toMap());
    m_mgr->setSchedule(byId(id), enabled, w);
}

QVariantMap AppController::defaultSchedule() const
{
    // Last used window, else FDM's classic "night" window 01:00-07:00 every day.
    QVariantMap m = m_ui.value(QStringLiteral("lastSchedule")).toMap();
    if (m.isEmpty())
        m = ScheduleWindow{0x7F, 60, 7 * 60}.toMap();
    m[QStringLiteral("enabled")] = false;
    return m;
}

QString AppController::scheduleText(const QVariantMap& window) const { return ScheduleWindow::fromMap(window).text(); }

void AppController::performWhenDone(int action)
{
    setWhenDone(0); // one-shot, like FDM
    if (qEnvironmentVariableIsSet("FLUXDM_DEV_OPEN") || qEnvironmentVariableIsSet("FLUXDM_DEV_DEMO")) {
        QCoreApplication::quit(); // dev/screenshot runs must never sleep or shut the machine down
        return;
    }
#ifdef Q_OS_WIN
    switch (action) {
    case 1: SetSuspendState(FALSE, FALSE, FALSE); return;
    case 2: SetSuspendState(TRUE, FALSE, FALSE); return;
    case 3:
        m_mgr->shutdown();
        QProcess::startDetached(QStringLiteral("shutdown.exe"), {QStringLiteral("/s"), QStringLiteral("/t"), QStringLiteral("0")});
        QCoreApplication::quit();
        return;
    default: break;
    }
#endif
    if (action == 4 || action == 3)
        QCoreApplication::quit();
}

// ------------------------------------------------------------------ BitTorrent settings

int AppController::uploadLimitKBps() const { return int(m_mgr->uploadLimitKBps()); }
void AppController::setUploadLimitKBps(int kbps) { m_mgr->setUploadLimitKBps(kbps); }
double AppController::seedRatio() const { return m_mgr->seedRatio(); }
void AppController::setSeedRatio(double r) { m_mgr->setSeedRatio(r); }

bool AppController::torrentAssociated() const
{
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    return reg.value(QStringLiteral("magnet/shell/open/command/Default")).toString().contains(exe, Qt::CaseInsensitive);
#else
    return false;
#endif
}

void AppController::setTorrentAssociated(bool on)
{
#ifdef Q_OS_WIN
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"), QSettings::NativeFormat);
    const QString cmd = QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QStringLiteral("%1"));
    if (on) {
        reg.setValue(QStringLiteral("magnet/Default"), QStringLiteral("URL:Magnet link"));
        reg.setValue(QStringLiteral("magnet/URL Protocol"), QString());
        reg.setValue(QStringLiteral("magnet/shell/open/command/Default"), cmd);
        reg.setValue(QStringLiteral(".torrent/Default"), QStringLiteral("FluxDM.torrent"));
        reg.setValue(QStringLiteral("FluxDM.torrent/Default"), QStringLiteral("BitTorrent file"));
        reg.setValue(QStringLiteral("FluxDM.torrent/shell/open/command/Default"), cmd);
    } else if (torrentAssociated()) {
        reg.remove(QStringLiteral("magnet"));
        reg.remove(QStringLiteral("FluxDM.torrent"));
        if (reg.value(QStringLiteral(".torrent/Default")).toString() == QLatin1String("FluxDM.torrent"))
            reg.remove(QStringLiteral(".torrent"));
    }
    reg.sync();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#else
    Q_UNUSED(on);
#endif
}