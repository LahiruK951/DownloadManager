#include "core/DownloadManager.h"
// Construction, loading, adding/removing and settings that belong to the manager. Other parts: DownloadManager*.cpp.

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxyFactory>
#include <QDir>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QJsonObject>
#include <QPointer>
#include <thread>
#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include "core/FileMover.h"
#include <algorithm>

#include "core/AppLog.h"
#include "core/AttachmentCheck.h"
#include "core/Checksum.h"
#include "core/DiskSpace.h"
#include <QThread>
#include "core/Database.h"
#include "core/Secret.h"
#include "core/DownloadEngine.h"
#include "core/FileNamer.h"
#include "engines/www/WwwEngine.h"

namespace {
QString systemProxyFor(const QString& url)
{
    if (!url.startsWith(QLatin1String("http"), Qt::CaseInsensitive) && !url.startsWith(QLatin1String("ftp"), Qt::CaseInsensitive))
        return {};
    const auto proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(QUrl(url)));
    for (const auto& p : proxies) {
        if (p.type() == QNetworkProxy::HttpProxy)
            return QStringLiteral("http://%1:%2").arg(p.hostName()).arg(p.port());
        if (p.type() == QNetworkProxy::Socks5Proxy)
            return QStringLiteral("socks5h://%1:%2").arg(p.hostName()).arg(p.port());
    }
    return {};
}
} // namespace

DownloadManager::DownloadManager(Database* db, QObject* parent)
    : QObject(parent), m_db(db)
{
    m_statsTimer.setSingleShot(true);
    m_statsTimer.setInterval(100);
    connect(&m_statsTimer, &QTimer::timeout, this, &DownloadManager::statsUpdated);
    m_db->setErrorHandler([this](const QString& err) {
        static const qint64 kQuietMs = 60 * 1000;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_lastStorageError && now - m_lastStorageError < kQuietMs)
            return;
        m_lastStorageError = now;
        emit storageError(err);
    });
    m_manualMode = qBound(0, m_db->setting(QStringLiteral("TumMode"), int(TrafficMode::High)).toInt(), 2);
    m_effectiveMode = m_manualMode;
    const QJsonArray rules = QJsonDocument::fromJson(m_db->setting(QStringLiteral("TumSchedule")).toString().toUtf8()).array();
    for (const auto& v : rules)
        m_rules.append(TrafficRule::fromMap(v.toObject().toVariantMap()));

    addEngine(new WwwEngine(this));

    // Schedules are minute-granular; a 15 s tick keeps transitions prompt.
    m_clock.setInterval(15000);
    connect(&m_clock, &QTimer::timeout, this, [this] {
        evaluateTrafficRules();
        runAutoRetries();
        recheckDiskSpace();
        if (++m_tick % 4 == 0) { // once a minute
            checkMissingFiles();
            flushUsage();
            removeOldFinished();
        }
        schedule();
    });
    m_clock.start();
    evaluateTrafficRules();
    applyLimits();
}

DownloadManager::~DownloadManager()
{
    if (!m_shuttingDown)
        shutdown();
    m_db->setErrorHandler({}); // the database may outlive this manager
}

void DownloadManager::addEngine(DownloadEngine* e)
{
    e->setParent(this);
    // Specialised engines (e.g. BitTorrent for .torrent URLs) take precedence over the generic one.
    m_engines.prepend(e);
    connect(e, &DownloadEngine::meta, this, &DownloadManager::onMeta);
    connect(e, &DownloadEngine::stats, this, &DownloadManager::onStats);
    connect(e, &DownloadEngine::resumeData, this, &DownloadManager::onResumeData);
    connect(e, &DownloadEngine::finished, this, &DownloadManager::onFinished);
    connect(e, &DownloadEngine::failed, this, &DownloadManager::onFailed);
    connect(e, &DownloadEngine::stopped, this, &DownloadManager::onStopped);
    connect(e, &DownloadEngine::log, this, &DownloadManager::onLog);
    connect(e, &DownloadEngine::filesAwaiting, this, [this](qint64 id, const QVariantList& files) {
        if (Download* d = find(id))
            d->setPendingFiles(files);
    });
    applyLimits();
}

QString DownloadManager::defaultUserAgent()
{
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) FluxDM/" FLUXDM_VERSION);
}

QString DownloadManager::userAgent() const { return m_db->setting(QStringLiteral("UserAgent")).toString(); }
void DownloadManager::setUserAgent(const QString& ua) { m_db->setSetting(QStringLiteral("UserAgent"), headerSafe(ua)); }
int DownloadManager::ipVersion() const
{
    const int v = m_db->setting(QStringLiteral("IpVersion"), 0).toInt();
    return v == 4 || v == 6 ? v : 0;
}
void DownloadManager::setIpVersion(int v) { m_db->setSetting(QStringLiteral("IpVersion"), v == 4 || v == 6 ? v : 0); }
int DownloadManager::autoRemoveDays() const { return qBound(0, m_db->setting(QStringLiteral("AutoRemoveDays"), 0).toInt(), 3650); }
void DownloadManager::setAutoRemoveDays(int days)
{
    m_db->setSetting(QStringLiteral("AutoRemoveDays"), qBound(0, days, 3650));
    removeOldFinished();
}

void DownloadManager::countTraffic(qint64 down, qint64 up, double speed)
{
    // Reports come every 500 ms; a bigger jump is progress restored from earlier (a resumed
    // stream reporting what it already had), not traffic.
    const qint64 plausible = qint64(qMax(0.0, speed) * 3) + 8LL * 1024 * 1024;
    if (down > plausible)
        down = 0;
    if (up > plausible)
        up = 0;
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    if (today != m_pendingDay) {
        flushUsage();
        m_pendingDay = today;
    }
    m_pendingDown += qMax<qint64>(0, down);
    m_pendingUp += qMax<qint64>(0, up);
}

void DownloadManager::flushUsage()
{
    if ((m_pendingDown > 0 || m_pendingUp > 0) && !m_pendingDay.isEmpty()
        && m_db->addUsage(m_pendingDay, m_pendingDown, m_pendingUp)) {
        m_pendingDown = 0;
        m_pendingUp = 0;
    }
}

QVariantMap DownloadManager::usage() const
{
    const QDate today = QDate::currentDate();
    auto [dayDown, dayUp] = m_db->usageSince(today.toString(Qt::ISODate));
    auto [monDown, monUp] = m_db->usageSince(QDate(today.year(), today.month(), 1).toString(Qt::ISODate));
    auto [allDown, allUp] = m_db->usageSince(QString());
    if (m_pendingDay == today.toString(Qt::ISODate)) {
        dayDown += m_pendingDown; dayUp += m_pendingUp;
    }
    monDown += m_pendingDown; monUp += m_pendingUp;
    allDown += m_pendingDown; allUp += m_pendingUp;
    return {{QStringLiteral("todayDown"), dayDown}, {QStringLiteral("todayUp"), dayUp},
            {QStringLiteral("monthDown"), monDown}, {QStringLiteral("monthUp"), monUp},
            {QStringLiteral("totalDown"), allDown}, {QStringLiteral("totalUp"), allUp}};
}

int DownloadManager::btListenPort() const { return m_db->setting(QStringLiteral("BtListenPort"), 0).toInt(); }
void DownloadManager::setBtListenPort(int port) { m_db->setSetting(QStringLiteral("BtListenPort"), qBound(1024, port, 65535)); }

int DownloadManager::removeOldFinished()
{
    const int days = autoRemoveDays();
    if (days <= 0)
        return 0;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    QList<Download*> old;
    for (Download* d : m_list)
        if (d->completed() && !d->seeding() && !d->converting() && !d->moving() && d->rec.finished.isValid() && d->rec.finished < cutoff)
            old << d;
    for (Download* d : old) {
        AppLog::write(QStringLiteral("Removed from the list after %1 days: %2").arg(days).arg(d->title()));
        remove(d, false);
    }
    return int(old.size());
}

void DownloadManager::load()
{
    m_nextId = m_db->maxId();
    m_tags = m_db->loadTags();
    applyQtProxy(proxySettings());
    for (const DownloadRecord& r : m_db->loadDownloads()) {
        auto* d = new Download(r, this);
        if (d->rec.state == Download::Starting || d->rec.state == Download::Downloading)
            d->rec.state = Download::Queued; // resume what was running when we quit
        if (d->rec.hashStatus == Download::HashChecking)
            d->rec.hashStatus = Download::HashNone; // interrupted by quitting
        m_list.append(d);
        m_byId.insert(d->id(), d);
    }
    checkMissingFiles();
    removeOldFinished();
    schedule();
    // Completed torrents keep seeding across restarts until the ratio is reached.
    for (Download* d : m_list)
        if (d->completed() && d->isTorrent() && !d->rec.resumeData.isEmpty())
            if (DownloadEngine* e = engineByUid(d->rec.moduleUid)) {
                TaskParams p;
                p.id = d->id();
                p.url = d->rec.url;
                p.destDir = d->rec.destDir;
                p.resumeData = d->rec.resumeData;
                p.seedOnly = true;
                d->setSeeding(true);
                e->start(p);
            }
}

DownloadEngine* DownloadManager::engineFor(const QString& url) const
{
    const QString t = url.trimmed();
    const QUrl u = t.contains(QLatin1String("://")) || t.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)
                       ? QUrl(t, QUrl::TolerantMode) : QUrl::fromLocalFile(t);
    for (DownloadEngine* e : m_engines)
        if (e->canHandle(u))
            return e;
    return nullptr;
}

DownloadEngine* DownloadManager::engineByUid(const QString& uid) const
{
    for (DownloadEngine* e : m_engines)
        if (e->moduleUid() == uid)
            return e;
    return nullptr;
}

bool DownloadManager::canHandle(const QString& url) const { return engineFor(url) != nullptr; }

QString DownloadManager::moduleFor(const QString& url) const
{
    DownloadEngine* e = engineFor(url);
    return e ? e->moduleUid() : QString();
}

Download* DownloadManager::find(qint64 id) const { return m_byId.value(id); }

Download* DownloadManager::findByUrl(const QString& url) const
{
    const auto key = [](const QString& u) { return QUrl(u.trimmed()).adjusted(QUrl::RemoveFragment).toString(); };
    const QString k = key(url);
    if (k.isEmpty())
        return nullptr;
    for (Download* d : m_list)
        if (key(d->rec.url) == k)
            return d;
    return nullptr;
}

Download* DownloadManager::add(const QString& url, const QString& destDir, const QString& fileName, bool start,
                               const QString& referrer, const QString& cookies, const QString& userAgent,
                               const ScheduleWindow* sched)
{
    DownloadEngine* e = engineFor(url);
    if (!e)
        return nullptr;
    DownloadRecord r;
    r.id = ++m_nextId;
    r.moduleUid = e->moduleUid();
    r.url = url.trimmed();
    r.destDir = destDir;
    r.preferredName = fileName.trimmed();
    if (!r.preferredName.isEmpty())
        r.title = r.preferredName;
    else if (r.url.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive))
        r.title = QUrlQuery(QUrl(r.url)).queryItemValue(QStringLiteral("dn"), QUrl::FullyDecoded);
    else
        r.title = FileNamer::fromUrl(QUrl(r.url));
    if (r.title.isEmpty())
        r.title = r.url.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive) ? tr("Magnet link") : QUrl(r.url).host();
    r.referrer = headerSafe(referrer);
    r.cookies = headerSafe(cookies);
    r.existingFileReaction = existingFileDefault();
    r.pickFiles = r.moduleUid == QLatin1String("downloadsbt") && chooseTorrentFiles();
    r.tags = tagsFor(r.title);
    qint64 last = 0;
    for (Download* x : m_list)
        last = qMax(last, x->queueOrder());
    r.queueOrder = qMax(last + 1, r.id); // the end of the queue
    r.userAgent = headerSafe(userAgent);
    r.created = QDateTime::currentDateTime();
    r.state = start ? Download::Queued : Download::Paused;
    if (sched) {
        r.scheduled = true;
        r.schedule = *sched;
    }

    auto* d = new Download(r, this);
    m_list.prepend(d);
    m_byId.insert(d->id(), d);
    persist(d);
    d->addLog(tr("Added %1").arg(r.url));
    if (r.scheduled)
        d->addLog(tr("Scheduled: %1").arg(r.schedule.text()));
    emit downloadAdded(d);
    schedule();
    return d;
}

void DownloadManager::persist(Download* d) { m_db->save(d->rec); }
