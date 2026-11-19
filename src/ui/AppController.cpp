#include "ui/AppController.h"

#include <QClipboard>
#include <cmath>
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QSet>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <powrprof.h>
#include <mmsystem.h>
#include <shlobj.h>
#endif

#include "core/AppLog.h"
#include "core/BatchLinks.h"
#include "core/BrowserHost.h"
#include "core/Instance.h"
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
    m_proxy->setSortOrder(m_ui.value(QStringLiteral("sortKey"), QStringLiteral("added")).toString(),
                          m_ui.value(QStringLiteral("sortDescending"), true).toBool());

    // Watch folder for .torrent files: added without asking (FDM's behaviour).
    m_torrentWatcher = new TorrentFolderWatcher(QDir(dataDir).filePath(QStringLiteral("torrents")), this);
    connect(m_torrentWatcher, &TorrentFolderWatcher::torrentAdded, this, [this](const QString& copy, const QString& name) {
        // Silent (FDM: ForceSilentDownloadsFromTorrentFolder): all files, no "choose files" question.
        if (Download* d = m_mgr->add(copy, QDir::toNativeSeparators(defaultFolder()), {}, false)) {
            d->rec.pickFiles = false;
            d->addLog(tr("Added from the watched folder: %1").arg(name));
            m_mgr->resume(d);
            emit notice(tr("Added %1 from the watched folder").arg(name));
        }
    });
    m_torrentWatcher->setFolder(torrentWatchFolder());
    connect(m_mgr, &DownloadManager::conversionFinished, this, [this](Download*, const QString& out, const QString& err) {
        emit notice(err.isEmpty() ? tr("Converted: %1").arg(QFileInfo(out).fileName()) : tr("Conversion failed: %1").arg(err));
    });
    // Re-sort the list when the queue changes (only matters when sorted by queue order).
    connect(m_mgr, &DownloadManager::queueReordered, this, [this] { m_proxy->invalidate(); });
    connect(m_mgr, &DownloadManager::tagsChanged, this, [this] {
        // a deleted tag can't stay the active filter
        if (filter().startsWith(QLatin1String("tag:")) && m_mgr->tag(filter().mid(4).toInt()).id == 0)
            setFilter(QStringLiteral("all"));
        emit tagsChanged();
        scheduleRecount();
    });
    m_recountTimer.setSingleShot(true);
    m_recountTimer.setInterval(150);
    connect(&m_recountTimer, &QTimer::timeout, this, &AppController::recount);

    connect(m_mgr, &DownloadManager::downloadAdded, this, [this](Download* d) {
        watch(d);
        scheduleRecount();
    });
    connect(m_mgr, &DownloadManager::downloadRemoved, this, [this](Download* d) {
        if (m_sel.removeAll(d->id()) > 0)
            selectionUpdated();
        if (m_selected == d) {
            m_selected = m_sel.isEmpty() ? nullptr : byId(m_sel.last());
            emit selectedChanged();
        }
        scheduleRecount();
    });
    connect(m_mgr, &DownloadManager::downloadFinished, this, [this](Download* d) {
        emit downloadCompleted(d->title(), d->filePath());
#ifdef Q_OS_WIN
        if (soundOnComplete() && !qEnvironmentVariableIsSet("FLUXDM_DEV_DEMO"))
            PlaySoundW(L"SystemNotification", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
#endif
    });
    connect(m_mgr, &DownloadManager::statsUpdated, this, &AppController::updateStats);
    connect(m_mgr, &DownloadManager::trafficModeChanged, this, &AppController::trafficModeChanged);
    connect(m_mgr, &DownloadManager::allDownloadsFinished, this, [this] {
        if (m_whenDone != 0)
            emit whenDoneCountdown(m_whenDone);
    });
    for (Download* d : m_mgr->downloads())
        watch(d);
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

QString AppController::sortKey() const { return m_proxy->sortKey(); }
bool AppController::sortDescending() const { return m_proxy->sortDescending(); }

void AppController::setSort(const QString& key, bool descending)
{
    m_proxy->setSortOrder(key, descending);
    m_ui.setValue(QStringLiteral("sortKey"), m_proxy->sortKey());
    m_ui.setValue(QStringLiteral("sortDescending"), descending);
    emit sortChanged();
}

// The torrent's largest wanted audio/video file: {path, size, done}, empty if none.
QVariantMap AppController::playableFile(const Download* d) const
{
    if (!d || !d->isTorrent())
        return {};
    QVariantMap best;
    const bool folder = d->files().size() > 1 || QFileInfo(d->filePath()).isDir();
    for (const QVariant& v : d->files()) {
        const QVariantMap f = v.toMap();
        const QString name = f.value(QStringLiteral("name")).toString();
        const QString cat = FileNamer::category(name);
        if ((cat != QLatin1String("Video") && cat != QLatin1String("Music")) || !f.value(QStringLiteral("wanted"), true).toBool())
            continue;
        if (f.value(QStringLiteral("size")).toLongLong() <= best.value(QStringLiteral("size"), -1).toLongLong())
            continue;
        best = {{QStringLiteral("path"), folder ? QDir(d->filePath()).filePath(name) : d->filePath()},
                {QStringLiteral("size"), f.value(QStringLiteral("size"))},
                {QStringLiteral("done"), f.value(QStringLiteral("done"))}};
    }
    return best;
}

bool AppController::canPlayWhileDownloading(qint64 id) const
{
    const Download* d = byId(id);
    return d && d->isTorrent() && d->running() && !playableFile(d).isEmpty();
}

void AppController::playWhileDownloading(qint64 id)
{
    Download* d = byId(id);
    if (!canPlayWhileDownloading(id))
        return;
    m_mgr->setSequential(d, true); // the player needs the file from its start
    if (!m_playWhenReady.contains(id))
        m_playWhenReady << id;
    emit notice(tr("Getting the start of the file first; it opens in your player in a moment"));
    updateStats();
}

QVariantMap AppController::proxySettings() const { return m_mgr->proxySettings().toMap(); }

QString AppController::setProxySettings(int mode, const QString& type, const QString& host, int port,
                                        const QString& user, const QString& password, bool keepPassword)
{
    ProxySettings ps = m_mgr->proxySettings();
    ps.mode = qBound(0, mode, 2);
    if (ps.mode == ProxySettings::Manual) {
        if (host.trimmed().isEmpty())
            return tr("Enter the proxy's address.");
        if (port <= 0 || port > 65535)
            return tr("Enter a port between 1 and 65535.");
        ps.type = type == QLatin1String("socks5") ? QStringLiteral("socks5") : QStringLiteral("http");
        ps.host = host.trimmed();
        ps.port = port;
        ps.user = user.trimmed();
        if (!keepPassword || !password.isEmpty())
            ps.password = ps.user.isEmpty() ? QString() : password;
    }
    m_mgr->setProxySettings(ps);
    emit settingsChanged();
    return {};
}

int AppController::maxConnectionsPerServer() const { return m_mgr->maxConnectionsPerServer(); }
void AppController::setMaxConnectionsPerServer(int n) { m_mgr->setMaxConnectionsPerServer(n); }

QVariantMap AppController::convertDefaults(const QString& format) const
{
    const QString p = format == QLatin1String("mp4") ? QStringLiteral("mp4Converter") : QStringLiteral("mp3Converter");
    return {{QStringLiteral("vbr"), !m_ui.value(p + QStringLiteral("ConstantBitrateEnabled"), true).toBool()},
            {QStringLiteral("bitrate"), m_ui.value(p + QStringLiteral("ConstantBitrate"), 192).toInt()},
            {QStringLiteral("quality"), m_ui.value(p + QStringLiteral("VariableQuality"), 2).toInt()},
            {QStringLiteral("folder"), m_ui.value(p + QStringLiteral("DestinationDir")).toString()}};
}

bool AppController::canConvert(qint64 id, const QString& format) const
{
    const QString f = DownloadManager::convertibleFile(byId(id));
    if (f.isEmpty())
        return false;
    if (format == QLatin1String("mp4"))
        return FileNamer::category(f) == QLatin1String("Video") && QFileInfo(f).suffix().compare(QLatin1String("mp4"), Qt::CaseInsensitive) != 0;
    return QFileInfo(f).suffix().compare(QLatin1String("mp3"), Qt::CaseInsensitive) != 0;
}

QString AppController::convertFile(qint64 id, const QString& format, const QVariantMap& options)
{
    ConvertOptions o;
    o.format = format == QLatin1String("mp4") ? QStringLiteral("mp4") : QStringLiteral("mp3");
    o.vbr = options.value(QStringLiteral("vbr")).toBool();
    o.bitrateKbps = options.value(QStringLiteral("bitrate"), 192).toInt();
    o.vbrQuality = options.value(QStringLiteral("quality"), 2).toInt();
    o.outputDir = options.value(QStringLiteral("folder")).toString().trimmed();
    // FDM's settings keys, so the next conversion starts from the same choices.
    const QString p = o.format == QLatin1String("mp4") ? QStringLiteral("mp4Converter") : QStringLiteral("mp3Converter");
    m_ui.setValue(p + QStringLiteral("ConstantBitrateEnabled"), !o.vbr);
    m_ui.setValue(p + QStringLiteral("ConstantBitrate"), o.bitrateKbps);
    m_ui.setValue(p + QStringLiteral("VariableQuality"), o.vbrQuality);
    m_ui.setValue(p + QStringLiteral("DestinationDir"), o.outputDir);
    const QString err = m_mgr->convertFile(byId(id), o);
    if (!err.isEmpty())
        emit notice(err);
    return err;
}

QVariantList AppController::tags() const
{
    QVariantList l;
    for (const Tag& t : m_mgr->tags())
        l << t.toMap();
    return l;
}

bool AppController::tagNameTaken(const QString& name, int exceptId) const
{
    for (const Tag& t : m_mgr->tags())
        if (t.id != exceptId && t.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

int AppController::saveTag(int id, const QString& name, const QString& color, const QString& extensions, const QString& folder)
{
    if (name.trimmed().isEmpty() || tagNameTaken(name, id))
        return 0;
    Tag t = id > 0 ? m_mgr->tag(id) : Tag();
    t.id = id > 0 ? t.id : 0;
    t.name = name.trimmed();
    t.color = color;
    t.extensions = Tag::parseExtensions(extensions);
    t.folder = folder.trimmed().isEmpty() ? QString() : QDir::toNativeSeparators(folder.trimmed());
    return m_mgr->saveTag(t);
}

void AppController::removeTag(int id) { m_mgr->removeTag(id); }
void AppController::setTag(qint64 downloadId, int tagId, bool on) { m_mgr->setTag(byId(downloadId), tagId, on); }

void AppController::setSelectionTag(int tagId, bool on)
{
    for (qint64 id : m_sel)
        m_mgr->setTag(byId(id), tagId, on);
}

int AppController::selectionTagState(int tagId) const
{
    int with = 0;
    for (qint64 id : m_sel)
        if (const Download* d = byId(id); d && d->hasTag(tagId))
            ++with;
    return with == 0 ? 0 : (with == m_sel.size() ? 2 : 1);
}

QString AppController::torrentWatchFolder() const { return m_ui.value(QStringLiteral("torrentWatchFolder")).toString(); }

void AppController::setTorrentWatchFolder(const QString& folder)
{
    const QString f = folder.trimmed().isEmpty() ? QString() : QDir::toNativeSeparators(folder.trimmed());
    if (f == torrentWatchFolder())
        return;
    m_ui.setValue(QStringLiteral("torrentWatchFolder"), f);
    m_torrentWatcher->setFolder(f);
    emit settingsChanged();
}

bool AppController::chooseTorrentFiles() const { return m_mgr->chooseTorrentFiles(); }

void AppController::setChooseTorrentFiles(bool on)
{
    m_mgr->setChooseTorrentFiles(on);
    emit settingsChanged();
}

void AppController::chooseFiles(qint64 id, const QVariantList& wantedIndexes)
{
    QList<int> wanted;
    for (const QVariant& v : wantedIndexes)
        wanted << v.toInt();
    m_mgr->chooseFiles(byId(id), wanted);
}

void AppController::setSequential(qint64 id, bool on) { m_mgr->setSequential(byId(id), on); }
QVariantMap AppController::torrentDetails(qint64 id) const { return m_mgr->details(byId(id)); }

bool AppController::previousSessionCrashed() const { return AppLog::previousSessionCrashed(); }

QString AppController::problemReport() const
{
    return AppLog::report(200) + QStringLiteral("\nDownloads: %1 in the list\n").arg(m_mgr->downloads().size());
}

void AppController::copyProblemReport()
{
    m_lastClip = problemReport();
    QGuiApplication::clipboard()->setText(m_lastClip);
    emit notice(tr("Report copied"));
}

QString AppController::saveProblemReport(const QUrl& file)
{
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return tr("Cannot write %1").arg(QDir::toNativeSeparators(path));
    f.write(problemReport().toUtf8());
    emit notice(tr("Saved %1").arg(QFileInfo(path).fileName()));
    return {};
}

void AppController::openLogsFolder() const { QDesktopServices::openUrl(QUrl::fromLocalFile(AppLog::logDir())); }

int AppController::zoom() const { return m_ui.value(QStringLiteral("zoom"), 100).toInt(); }

void AppController::setZoom(int percent)
{
    percent = qBound(50, percent, 300);
    if (percent == zoom())
        return;
    m_ui.setValue(QStringLiteral("zoom"), percent);
    m_ui.sync();
    emit settingsChanged();
}

int AppController::zoomInEffect() const
{
    const double f = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1")).toDouble();
    return f > 0 ? int(std::lround(f * 100)) : 100;
}

void AppController::restartApp()
{
    m_ui.sync();
    QStringList args{QStringLiteral("--restarted")};
    qunsetenv("QT_SCALE_FACTOR"); // the new process reads the zoom setting itself
    if (QProcess::startDetached(QCoreApplication::applicationFilePath(), args))
        QCoreApplication::quit();
}

bool AppController::preventSleep() const { return m_ui.value(QStringLiteral("preventSleep"), true).toBool(); }

void AppController::setPreventSleep(bool on)
{
    if (preventSleep() == on)
        return;
    m_ui.setValue(QStringLiteral("preventSleep"), on);
    m_sleepGuard.setActive(on && m_activeCount > 0);
    emit settingsChanged();
}
UI_SETTING(soundOnComplete, setSoundOnComplete, bool, "soundOnComplete", true, toBool)
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
                                    const QVariantMap& schedule, int existingFile)
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
    for (const QString& u : urls) {
        const QVariantMap meta = m_browserMeta.take(u); // cookies etc. captured by the browser extension
        QString name = urls.size() == 1 ? fileName.trimmed() : QString();
        last = m_mgr->add(u, QDir::toNativeSeparators(dir), name, start || sched,
                          meta.value(QStringLiteral("referrer")).toString(), meta.value(QStringLiteral("cookies")).toString(),
                          meta.value(QStringLiteral("userAgent")).toString(), sched ? &w : nullptr);
        if (last && existingFile >= 0)
            m_mgr->setExistingFileReaction(last, existingFile);
    }
    rememberFolder(dir);
    if (last)
        select(last->id());
    return {};
}

void AppController::selectionUpdated()
{
    ++m_selRev;
    emit selectionChanged();
}

void AppController::select(qint64 id)
{
    Download* d = byId(id);
    m_sel.clear();
    if (d)
        m_sel.append(id);
    selectionUpdated();
    if (m_selected == d)
        return;
    m_selected = d;
    emit selectedChanged();
}

void AppController::toggleSelect(qint64 id)
{
    Download* d = byId(id);
    if (!d)
        return;
    if (m_sel.contains(id)) {
        m_sel.removeAll(id);
        if (m_selected == d) {
            m_selected = m_sel.isEmpty() ? nullptr : byId(m_sel.last());
            emit selectedChanged();
        }
    } else {
        m_sel.append(id);
        m_selected = d;
        emit selectedChanged();
    }
    selectionUpdated();
}

void AppController::selectRange(qint64 id)
{
    const int to = m_proxy->rowOf(id);
    const int from = m_selected ? m_proxy->rowOf(m_selected->id()) : -1;
    if (to < 0)
        return;
    if (from < 0) {
        select(id);
        return;
    }
    m_sel.clear();
    for (int r = qMin(from, to); r <= qMax(from, to); ++r)
        m_sel.append(m_proxy->idAt(r));
    selectionUpdated(); // the current item stays where the range started
}

void AppController::selectAll()
{
    m_sel.clear();
    for (int r = 0; r < m_proxy->rowCount(); ++r)
        m_sel.append(m_proxy->idAt(r));
    if (!m_selected && !m_sel.isEmpty()) {
        m_selected = byId(m_sel.first());
        emit selectedChanged();
    }
    selectionUpdated();
}

void AppController::clearSelection()
{
    m_sel.clear();
    selectionUpdated();
    if (m_selected) {
        m_selected = nullptr;
        emit selectedChanged();
    }
}

QVariantList AppController::selectedIds() const
{
    QVariantList out;
    for (qint64 id : m_sel)
        out << id;
    return out;
}

void AppController::pauseSelected()
{
    for (qint64 id : m_sel)
        if (Download* d = byId(id); d && (d->running() || d->queued()))
            m_mgr->pause(d);
}

void AppController::resumeSelected()
{
    for (qint64 id : m_sel)
        m_mgr->resume(byId(id));
}

void AppController::removeSelected(bool deleteFiles)
{
    const QList<qint64> ids = m_sel; // remove() prunes m_sel through downloadRemoved
    for (qint64 id : ids)
        m_mgr->remove(byId(id), deleteFiles);
}

void AppController::copySelectedLinks()
{
    QStringList links;
    for (qint64 id : m_sel)
        if (Download* d = byId(id))
            links << d->sourceUrl();
    m_lastClip = links.join(QLatin1Char('\n'));
    QGuiApplication::clipboard()->setText(m_lastClip);
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
void AppController::setDownloadSpeedLimit(qint64 id, int kbps) { m_mgr->setSpeedLimit(byId(id), kbps); }
void AppController::pauseAll() { m_mgr->pauseAll(); }
void AppController::resumeAll() { m_mgr->resumeAll(); }

// What "Open" would act on, or an error explaining why it can't.
static QString openablePath(Download* d, QString* error)
{
    if (!d)
        return {};
    if (!d->completed()) {
        *error = QCoreApplication::translate("AppController", "The download hasn't finished yet");
        return {};
    }
    if (d->converting() || d->moving()) {
        *error = QCoreApplication::translate("AppController", "Wait until the file is ready");
        return {};
    }
    const QString path = d->filePath();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        d->setFileMissing(true);
        *error = QCoreApplication::translate("AppController", "The file was moved or deleted");
        return {};
    }
    return path;
}

QString AppController::openFile(qint64 id)
{
    const QString err = doOpen(id);
    if (!err.isEmpty())
        emit notice(err);
    return err;
}

QString AppController::openWith(qint64 id)
{
    const QString err = doOpenWith(id);
    if (!err.isEmpty())
        emit notice(err);
    return err;
}

QString AppController::doOpen(qint64 id)
{
    QString error;
    const QString path = openablePath(byId(id), &error);
    if (path.isEmpty())
        return error;
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        return {};
    // No program is associated with this type: let the user pick one.
    return QFileInfo(path).isFile() ? doOpenWith(id) : tr("Cannot open %1").arg(path);
}

QString AppController::doOpenWith(qint64 id)
{
    QString error;
    const QString path = openablePath(byId(id), &error);
    if (path.isEmpty())
        return error;
    if (!QFileInfo(path).isFile())
        return tr("“Open with” works on files, not folders");
#ifdef Q_OS_WIN
    // OpenAs_RunDLL takes the rest of the command line as the path: pass it unquoted.
    QProcess p;
    p.setProgram(QStringLiteral("rundll32.exe"));
    p.setNativeArguments(QStringLiteral("shell32.dll,OpenAs_RunDLL ") + QDir::toNativeSeparators(path));
    return p.startDetached() ? QString() : tr("Cannot show the “Open with” dialog");
#else
    return QDesktopServices::openUrl(QUrl::fromLocalFile(path)) ? QString() : tr("Cannot open %1").arg(path);
#endif
}

static bool isWebLink(const QString& s)
{
    return s.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) || s.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

// The referrer is the page the link was on (browser integration, HLS sniffing). For a torrent
// fetched from the web it is the .torrent link itself, which is not a page.
bool AppController::hasPage(qint64 id) const
{
    const Download* d = byId(id);
    return d && !d->isTorrent() && isWebLink(d->referrer()) && d->referrer() != d->url();
}

void AppController::openPage(qint64 id)
{
    if (hasPage(id))
        QDesktopServices::openUrl(QUrl(byId(id)->referrer()));
}

void AppController::copyPath(qint64 id)
{
    if (Download* d = byId(id)) {
        m_lastClip = d->filePath().isEmpty() ? d->destDir() : d->filePath();
        QGuiApplication::clipboard()->setText(m_lastClip);
    }
}

void AppController::watch(Download* d)
{
    connect(d, &Download::stateChanged, this, &AppController::scheduleRecount);
    connect(d, &Download::categoryChanged, this, &AppController::scheduleRecount);
    connect(d, &Download::tagsChanged, this, &AppController::scheduleRecount);
    connect(d, &Download::stateChanged, this, [this, d] {
        if (d->needsLogin())
            emit loginRequired(d);
        else if (d->fileExists())
            emit fileConflict(d);
        else if (d->awaitingFiles())
            emit filesRequired(d);
    });
}

QString AppController::loginServer(qint64 id) const
{
    const Download* d = byId(id);
    return d ? DownloadManager::loginServer(d->url()) : QString();
}

QString AppController::loginUser(qint64 id) const
{
    const Download* d = byId(id);
    QString user, password;
    if (d)
        m_mgr->loginFor(d->url(), &user, &password);
    return user;
}

void AppController::signIn(qint64 id, const QString& user, const QString& password, bool remember)
{
    Download* d = byId(id);
    if (!d || user.trimmed().isEmpty())
        return;
    m_mgr->setLogin(d->url(), user.trimmed(), password, remember);
    d->addLog(tr("Signing in as %1").arg(user.trimmed()));
    m_mgr->resume(d);
    emit settingsChanged(); // saved-login count
}

int AppController::existingFileDefault() const { return m_mgr->existingFileDefault(); }

void AppController::setExistingFileDefault(int reaction)
{
    m_mgr->setExistingFileDefault(reaction);
    emit settingsChanged();
}

bool AppController::fileExistsIn(const QString& folder, const QString& name) const
{
    const QString n = name.trimmed();
    return !n.isEmpty() && QFileInfo::exists(QDir(folder.trimmed().isEmpty() ? defaultFolder() : folder.trimmed()).filePath(n));
}

void AppController::resolveExisting(qint64 id, int reaction)
{
    Download* d = byId(id);
    if (!d || !d->fileExists() || (reaction != FileNamer::KeepBoth && reaction != FileNamer::Overwrite))
        return;
    m_mgr->setExistingFileReaction(d, reaction);
    d->addLog(reaction == FileNamer::Overwrite ? tr("Replacing the existing file") : tr("Keeping both files"));
    m_mgr->resume(d);
}

int AppController::savedLoginCount() const { return m_mgr->savedLoginCount(); }

void AppController::forgetLogins()
{
    m_mgr->forgetLogins();
    emit settingsChanged();
}

QString AppController::rename(qint64 id, const QString& newName) { return m_mgr->rename(byId(id), newName); }
bool AppController::canRename(qint64 id) const { return DownloadManager::canRename(byId(id)); }

QString AppController::openSelected()
{
    constexpr int kMax = 10; // don't launch a wall of players by accident
    int opened = 0, skipped = 0;
    for (qint64 id : m_sel) {
        Download* d = byId(id);
        if (!d || !d->completed() || opened >= kMax) {
            ++skipped;
            continue;
        }
        if (doOpen(id).isEmpty())
            ++opened;
        else
            ++skipped;
    }
    const QString msg = opened == 0 ? tr("None of the selected downloads can be opened yet")
        : skipped ? tr("Opened %1, skipped %2 (not finished, missing, or over %3)").arg(opened).arg(skipped).arg(kMax)
        : QString();
    if (!msg.isEmpty())
        emit notice(msg);
    return msg;
}

void AppController::moveInQueue(qint64 id, const QString& where)
{
    if (Download* d = byId(id))
        m_mgr->moveInQueue({d}, where);
}

void AppController::moveSelectionInQueue(const QString& where)
{
    QList<Download*> ds;
    for (qint64 id : m_sel)
        if (Download* d = byId(id))
            ds << d;
    m_mgr->moveInQueue(ds, where);
}

void AppController::setPrioritySelected(int priority)
{
    for (qint64 id : m_sel)
        m_mgr->setPriority(byId(id), priority);
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
    if (Download* d = byId(id)) { // a torrent from a downloaded .torrent: its web link, not the temp file
        m_lastClip = d->sourceUrl();
        QGuiApplication::clipboard()->setText(m_lastClip);
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
        for (int t : d->rec.tags)
            ++cats[QStringLiteral("tag:%1").arg(t)];
    }
    c[QStringLiteral("all")] = all;
    c[QStringLiteral("active")] = active;
    c[QStringLiteral("completed")] = completed;
    for (const QString& k : kCategories)
        c[k] = cats.value(k);
    for (const Tag& t : m_mgr->tags())
        c[QStringLiteral("tag:%1").arg(t.id)] = cats.value(QStringLiteral("tag:%1").arg(t.id));
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
    // "Play while downloading": open the file once its first part is on disk.
    for (int i = int(m_playWhenReady.size()) - 1; i >= 0; --i) {
        Download* d = byId(m_playWhenReady[i]);
        if (!d || (!d->running() && !d->completed())) {
            m_playWhenReady.removeAt(i);
            continue;
        }
        const QVariantMap f = playableFile(d);
        const qint64 size = f.value(QStringLiteral("size")).toLongLong(), done = f.value(QStringLiteral("done")).toLongLong();
        if (d->completed() || (size > 0 && done >= qMin<qint64>(size, qMax<qint64>(8LL << 20, size / 50)))) {
            m_playWhenReady.removeAt(i);
            QDesktopServices::openUrl(QUrl::fromLocalFile(f.value(QStringLiteral("path")).toString()));
        }
    }
    // Downloading (not just seeding) keeps the computer awake; released as soon as nothing runs.
    m_sleepGuard.setActive(active > 0 && preventSleep());
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
    if (!dt.isValid())
        return {};
    const QLocale loc;
    const QString time = loc.toString(dt.time(), QLocale::ShortFormat);
    const qint64 days = dt.date().daysTo(QDate::currentDate());
    if (days == 0)
        return tr("Today, %1").arg(time);
    if (days == 1)
        return tr("Yesterday, %1").arg(time);
    if (days > 1 && days < 7)
        return loc.dayName(dt.date().dayOfWeek(), QLocale::LongFormat) + QStringLiteral(", ") + time;
    return loc.toString(dt.date(), QLocale::ShortFormat);
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
        addDownloads(urls.join(QLatin1Char('\n')), folderFor(urls.first()), {}, true);
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

// ------------------------------------------------------------------ browser integration

bool AppController::browserAddSilently() const { return m_ui.value(QStringLiteral("browserAddSilently"), false).toBool(); }
void AppController::setBrowserAddSilently(bool v)
{
    if (v == browserAddSilently())
        return;
    m_ui.setValue(QStringLiteral("browserAddSilently"), v);
    emit settingsChanged();
}

bool AppController::startHidden() const { return QCoreApplication::arguments().contains(QStringLiteral("--tray")); }

QString AppController::suggestedName(const QString& url) const
{
    return m_browserMeta.value(url.trimmed()).value(QStringLiteral("fileName")).toString();
}

// Best guess of the saved name before the server is asked: the browser's name, else the link's.
// The server may still name it differently (Content-Disposition).
QString AppController::expectedName(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList links = text.split(ws, Qt::SkipEmptyParts);
    if (links.size() != 1)
        return {};
    const QString browser = suggestedName(links.first());
    return browser.isEmpty() ? FileNamer::sanitize(FileNamer::fromUrl(QUrl(links.first()))) : browser;
}

QString AppController::handleBrowserRequest(const QVariantMap& r)
{
    const QString cmd = r.value(QStringLiteral("cmd")).toString();
    if (cmd == QLatin1String("ping"))
        return {};
    if (cmd == QLatin1String("show")) {
        emit showWindowRequested();
        return {};
    }
    if (cmd != QLatin1String("download"))
        return tr("Unknown command");
    const QString url = r.value(QStringLiteral("url")).toString().trimmed();
    if (!m_mgr->canHandle(url))
        return tr("Unsupported link");
    QVariantMap meta;
    for (const char* k : {"referrer", "cookies", "userAgent", "fileName"})
        meta.insert(QString::fromLatin1(k), r.value(QString::fromLatin1(k)));
    meta[QStringLiteral("fileName")] = FileNamer::sanitize(QFileInfo(meta.value(QStringLiteral("fileName")).toString()).fileName());
    if (meta.value(QStringLiteral("fileName")).toString() == QLatin1String("download"))
        meta.remove(QStringLiteral("fileName"));
    m_browserMeta.insert(url, meta);

    if (browserAddSilently() || r.value(QStringLiteral("silent")).toBool()) {
        const QString err = addDownloads(url, folderFor(url), suggestedName(url), true);
        return err;
    }
    emit addDialogRequested(url);
    return {};
}

QString AppController::extensionDir() const
{
    return QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("browser/chromium")));
}

void AppController::openExtensionDir() const { QDesktopServices::openUrl(QUrl::fromLocalFile(extensionDir())); }

QString AppController::firefoxExtensionDir() const
{
    return QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("browser/firefox")));
}

void AppController::openFirefoxExtensionDir() const { QDesktopServices::openUrl(QUrl::fromLocalFile(firefoxExtensionDir())); }

#ifdef Q_OS_WIN
static const char* kBrowserKeys[] = {
    "HKEY_CURRENT_USER\\Software\\Google\\Chrome\\NativeMessagingHosts",
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Edge\\NativeMessagingHosts",
    "HKEY_CURRENT_USER\\Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts",
};
static const char* kFirefoxKey = "HKEY_CURRENT_USER\\Software\\Mozilla\\NativeMessagingHosts";
#endif

bool AppController::browserIntegrated() const
{
#ifdef Q_OS_WIN
    QSettings reg(QString::fromLatin1(kBrowserKeys[0]), QSettings::NativeFormat);
    const QString manifest = reg.value(QString::fromLatin1(Instance::kNativeHostName) + QStringLiteral("/Default")).toString();
    return !manifest.isEmpty() && QFile::exists(manifest);
#else
    return false;
#endif
}

QString AppController::setBrowserIntegrated(bool on)
{
#ifdef Q_OS_WIN
    const QString hostName = QString::fromLatin1(Instance::kNativeHostName);
    const QString manifestPath = QDir(Instance::profileDir()).filePath(QStringLiteral("nativehost.json"));
    if (on) {
        const QString host = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("fluxdm-nativehost.exe"));
        if (!QFile::exists(host))
            return tr("fluxdm-nativehost.exe is missing next to FluxDM.exe");
        // The extension ID is fixed by the public key in its manifest (see browser/chromium/manifest.json).
        QFile idFile(QDir(extensionDir()).filePath(QStringLiteral("extension-id.txt")));
        QString extId = idFile.open(QIODevice::ReadOnly) ? QString::fromLatin1(idFile.readAll()).trimmed() : QString();
        if (extId.isEmpty())
            return tr("Browser extension files are missing (%1)").arg(extensionDir());
        const QJsonObject manifest = BrowserHost::chromiumManifest(hostName, QDir::toNativeSeparators(host), extId);
        QFile f(manifestPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return tr("Cannot write %1").arg(manifestPath);
        f.write(QJsonDocument(manifest).toJson());
        f.close();
        for (const char* key : kBrowserKeys) {
            QSettings reg(QString::fromLatin1(key), QSettings::NativeFormat);
            reg.setValue(hostName + QStringLiteral("/Default"), QDir::toNativeSeparators(manifestPath));
        }
        // Firefox: its own manifest (allowed by add-on ID) under the Mozilla key.
        const QString geckoId = BrowserHost::geckoId(QDir(firefoxExtensionDir()).filePath(QStringLiteral("manifest.json")));
        if (!geckoId.isEmpty()) {
            const QString ffPath = QDir(Instance::profileDir()).filePath(QStringLiteral("nativehost-firefox.json"));
            QFile ff(ffPath);
            if (ff.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                ff.write(QJsonDocument(BrowserHost::firefoxManifest(hostName, QDir::toNativeSeparators(host), geckoId)).toJson());
                ff.close();
                QSettings reg(QString::fromLatin1(kFirefoxKey), QSettings::NativeFormat);
                reg.setValue(hostName + QStringLiteral("/Default"), QDir::toNativeSeparators(ffPath));
            }
        }
    } else {
        for (const char* key : kBrowserKeys) {
            QSettings reg(QString::fromLatin1(key), QSettings::NativeFormat);
            reg.remove(hostName);
        }
        QSettings ffReg(QString::fromLatin1(kFirefoxKey), QSettings::NativeFormat);
        ffReg.remove(hostName);
        QFile::remove(manifestPath);
        QFile::remove(QDir(Instance::profileDir()).filePath(QStringLiteral("nativehost-firefox.json")));
    }
    emit settingsChanged();
    return {};
#else
    Q_UNUSED(on);
    return tr("Not supported on this platform");
#endif
}
int AppController::autoRetryLimit() const { return m_mgr->autoRetryLimit(); }
void AppController::setAutoRetryLimit(int n) { m_mgr->setAutoRetryLimit(n); }
// ------------------------------------------------------------------ folders by file type (FDM: tag default folders)

QString AppController::categoryFolder(const QString& category) const
{
    return m_ui.value(QStringLiteral("categoryFolders/") + category).toString();
}

void AppController::setCategoryFolder(const QString& category, const QString& folder)
{
    const QString f = QDir::toNativeSeparators(folder.trimmed());
    if (f.isEmpty())
        m_ui.remove(QStringLiteral("categoryFolders/") + category);
    else
        m_ui.setValue(QStringLiteral("categoryFolders/") + category, f);
    emit settingsChanged();
}

QString AppController::categoryOf(const QString& links) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList l = BatchLinks::expandAll(links.split(ws, Qt::SkipEmptyParts), 1);
    if (l.isEmpty())
        return {};
    const QString url = l.first();
    QString name = suggestedName(url);
    if (name.isEmpty())
        name = FileNamer::fromUrl(QUrl(url));
    return FileNamer::category(name);
}

QString AppController::folderFor(const QString& links) const
{
    // A user tag with a folder for this file type wins over the folder by category.
    const QString tagFolder = m_mgr->tagFolderFor(expectedName(links));
    if (!tagFolder.isEmpty())
        return tagFolder;
    const QString folder = categoryFolder(categoryOf(links));
    return folder.isEmpty() ? defaultFolder() : folder;
}
// ------------------------------------------------------------------ import / export (FDM: .fdd)

QString AppController::exportDownloads(const QUrl& file) const
{
    QJsonArray items;
    for (Download* d : m_mgr->downloads()) {
        QJsonObject o{{QStringLiteral("url"), d->rec.url},
                      {QStringLiteral("folder"), d->destDir()},
                      {QStringLiteral("title"), d->title()},
                      {QStringLiteral("completed"), d->completed()},
                      {QStringLiteral("added"), d->rec.created.toString(Qt::ISODate)}};
        if (!d->rec.preferredName.isEmpty())
            o[QStringLiteral("fileName")] = d->rec.preferredName;
        if (!d->rec.referrer.isEmpty())
            o[QStringLiteral("referrer")] = d->rec.referrer;
        items.append(o);
    }
    const QJsonObject doc{{QStringLiteral("format"), QStringLiteral("fluxdm-downloads")},
                          {QStringLiteral("version"), 1},
                          {QStringLiteral("downloads"), items}};
    QFile f(file.toLocalFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return tr("Cannot write %1").arg(QDir::toNativeSeparators(f.fileName()));
    f.write(QJsonDocument(doc).toJson());
    return tr("Exported %n download(s).", nullptr, int(items.size()));
}

QString AppController::importDownloads(const QUrl& file)
{
    QFile f(file.toLocalFile());
    if (!f.open(QIODevice::ReadOnly))
        return tr("Cannot open %1").arg(QDir::toNativeSeparators(f.fileName()));
    const QByteArray data = f.readAll();

    struct Item { QString url, folder, fileName, referrer; };
    QList<Item> items;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isObject() && doc.object().value(QStringLiteral("format")).toString() == QLatin1String("fluxdm-downloads")) {
        for (const auto& v : doc.object().value(QStringLiteral("downloads")).toArray()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("completed")).toBool())
                continue; // nothing to do for finished ones
            items.append({o.value(QStringLiteral("url")).toString(), o.value(QStringLiteral("folder")).toString(),
                          o.value(QStringLiteral("fileName")).toString(), o.value(QStringLiteral("referrer")).toString()});
        }
    } else { // plain list of links, one per line
        static const QRegularExpression ws(QStringLiteral("\\s+"));
        for (const QString& l : BatchLinks::expandAll(QString::fromUtf8(data).split(ws, Qt::SkipEmptyParts)))
            items.append({l, {}, {}, {}});
    }

    int added = 0, skipped = 0;
    QSet<QString> known;
    for (Download* d : m_mgr->downloads())
        known.insert(d->rec.url);
    for (const Item& it : items) {
        if (!m_mgr->canHandle(it.url) || known.contains(it.url)) {
            ++skipped;
            continue;
        }
        const QString folder = it.folder.isEmpty() ? folderFor(it.url) : it.folder;
        if (m_mgr->add(it.url, folder, it.fileName, false, it.referrer)) { // imported paused, like FDM
            known.insert(it.url);
            ++added;
        }
    }
    if (added == 0)
        return tr("No new downloads found in the file.");
    return skipped > 0 ? tr("Imported %1 download(s) (paused); skipped %2.").arg(added).arg(skipped)
                       : tr("Imported %1 download(s) (paused).").arg(added);
}
QString AppController::linkModule(const QString& text) const
{
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList l = text.split(ws, Qt::SkipEmptyParts);
    return l.isEmpty() ? QString() : m_mgr->moduleFor(l.first());
}
QString AppController::moveSelected(const QUrl& folder)
{
    const QString dir = folder.toLocalFile();
    QStringList errors;
    for (qint64 id : m_sel)
        if (Download* d = byId(id)) {
            const QString err = m_mgr->moveTo(d, dir);
            if (!err.isEmpty())
                errors << d->title() + QStringLiteral(": ") + err;
        }
    return errors.isEmpty() ? QString() : errors.join(QLatin1Char('\n'));
}

bool AppController::canMove(qint64 id) const { return DownloadManager::canMove(byId(id)); }
bool AppController::convertStreams() const { return m_mgr->convertStreams(); }
void AppController::setConvertStreams(bool on) { m_mgr->setConvertStreams(on); }
QString AppController::ffmpegSetting() const { return m_mgr->ffmpegSetting(); }
void AppController::setFfmpegSetting(const QString& path) { m_mgr->setFfmpegSetting(path); }
QString AppController::ffmpegFound() const { return QDir::toNativeSeparators(m_mgr->ffmpegPath()); }
QString AppController::ytdlpSetting() const { return m_mgr->ytdlpSetting(); }
void AppController::setYtdlpSetting(const QString& path) { m_mgr->setYtdlpSetting(path); }
QString AppController::ytdlpFound() const { return QDir::toNativeSeparators(m_mgr->ytdlpPath()); }