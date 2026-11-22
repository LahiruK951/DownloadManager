#include "ui/AppController.h"
// Construction, traffic modes, stats and helpers. Settings, actions and integration: AppController*.cpp.

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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <powrprof.h>
#include <mmsystem.h>
#include <shlobj.h>
#endif

#include "core/AppLog.h"
#include "core/BatchLinks.h"
#include "core/Secret.h"
#include "core/Checksum.h"
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
    m_languageAtStart = language();
    m_metered = new MeteredGuard(m_mgr, this);
    m_metered->setPolicy(meteredPolicy());
    connect(m_metered, &MeteredGuard::askUser, this, &AppController::meteredQuestion);
    connect(m_metered, &MeteredGuard::paused, this, [this](int n) {
        emit notice(tr("Metered connection: paused %1 download(s); they continue on an unmetered connection").arg(n));
    });
    connect(m_metered, &MeteredGuard::resumed, this, [this](int n) {
        emit notice(tr("No longer metered: resumed %1 download(s)").arg(n));
    });
    if (!qEnvironmentVariableIsSet("FLUXDM_DEV_DEMO")) // screenshots stay independent of this PC's network
        QTimer::singleShot(0, this, [this] { MeteredGuard::watchSystem(m_metered); });
    m_updater = new Updater(QStringLiteral(FLUXDM_VERSION), this);
    connect(m_updater, &Updater::checked, this, [this](bool available, const QString& version, const QString&) {
        m_ui.setValue(QStringLiteral("updateLastCheck"), QDateTime::currentDateTime());
        setUpdate(available ? 2 : 0, available ? tr("FluxDM %1 is available.").arg(version)
                                               : tr("FluxDM is up to date (%1).").arg(QStringLiteral(FLUXDM_VERSION)));
    });
    connect(m_updater, &Updater::progress, this, [this](qint64 got, qint64 total) {
        if (total > 0)
            setUpdate(3, tr("Downloading the update… %1%").arg(got * 100 / total));
    });
    connect(m_updater, &Updater::ready, this, [this](const QString& path) {
        m_updateFile = path;
        setUpdate(4, tr("FluxDM %1 is ready to install.").arg(m_updater->version()));
    });
    connect(m_updater, &Updater::failed, this, [this](const QString& err) { setUpdate(5, err); });
    // A daily look, a little after start, when an update address is set.
    if (autoCheckUpdates() && !updateUrl().isEmpty()
        && m_ui.value(QStringLiteral("updateLastCheck")).toDateTime().secsTo(QDateTime::currentDateTime()) > 86400)
        QTimer::singleShot(15000, this, [this] { checkForUpdates(); });
    // Remote control: links added from a phone go through the same path as the Add dialog.
    m_remote = new RemoteServer(m_mgr, this);
    m_remote->setAddHandler([this](const QString& url) { return addSilently(url, QString(), QString()); });
    // The access code is kept protected (DPAPI); older versions stored it as plain text.
    QString code = Secret::unprotect(QByteArray::fromBase64(m_ui.value(QStringLiteral("remoteCodeProtected")).toByteArray()));
    if (code.isEmpty())
        code = m_ui.value(QStringLiteral("remoteCode")).toString();
    if (code.isEmpty())
        code = RemoteServer::newAccessCode();
    m_ui.setValue(QStringLiteral("remoteCodeProtected"), Secret::protect(code).toBase64());
    m_ui.remove(QStringLiteral("remoteCode"));
    m_remote->setAccessCode(code);
    applyRemote();
    connect(m_mgr, &DownloadManager::antivirusChecked, this, [this](Download* d, bool blocked, const QString& msg) {
        if (blocked)
            emit notice(QStringLiteral("%1: %2").arg(d->title(), msg));
    });
    m_speedSampler.setInterval(1000);
    connect(&m_speedSampler, &QTimer::timeout, this, [this] {
        m_speedHistory.append(m_totalSpeed);
        while (m_speedHistory.size() > 120)
            m_speedHistory.removeFirst();
        emit speedHistoryChanged();
    });
    m_speedSampler.start();
    connect(m_mgr, &DownloadManager::extracted, this, [this](Download* d, const QString& folder, const QString& err) {
        emit notice(err.isEmpty() ? tr("Extracted %1").arg(d->title()) : tr("Couldn't extract %1: %2").arg(d->title(), err));
        Q_UNUSED(folder)
    });
    connect(m_mgr, &DownloadManager::torrentCreated, this, [this](const QString& path, const QString& err) {
        emit notice(err.isEmpty() ? tr("Created %1 and started sharing it").arg(QFileInfo(path).fileName())
                                  : tr("Couldn't create the torrent: %1").arg(err));
    });
    connect(m_mgr, &DownloadManager::checksumChecked, this, [this](Download* d, bool match, const QString&) {
        if (!match)
            emit notice(tr("Checksum mismatch: %1 may be damaged or not the file that was published").arg(d->title()));
    });
    connect(m_mgr, &DownloadManager::storageError, this, [this](const QString& err) {
        emit notice(tr("Can't save download progress: %1. Check free space on the drive with FluxDM's data.").arg(err));
    });
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
            // A double-clicked .torrent in a folder with spaces: as an encoded file URL, so the space
            // doesn't split it into two links in the Add dialog.
            urls << (QFileInfo(a).isAbsolute() && QFileInfo::exists(a) ? QUrl::fromLocalFile(a).toString(QUrl::FullyEncoded) : a);
    }
    if (urls.isEmpty()) {
        emit showWindowRequested();
        return;
    }
    if (silent)
        addSilently(urls.join(QLatin1Char('\n')), folderFor(urls.first()), {});
    else
        emit addDialogRequested(urls.join(QLatin1Char('\n')));
}
