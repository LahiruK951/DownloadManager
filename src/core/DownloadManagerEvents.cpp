// DownloadManager: what engines report (progress, finish, failure) and automatic retries.
#include "core/DownloadManager.h"

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
#include "core/Metalink.h"
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

// ------------------------------------------------------------------ engine callbacks

void DownloadManager::onMeta(qint64 id, const QString& fileName, const QString& filePath, qint64 total, bool resumable)
{
    Download* d = find(id);
    if (!d || !(d->running() || d->seeding()))
        return;
    d->rec.filePath = filePath;
    d->rec.total = total;
    d->rec.resumable = resumable ? 1 : 0;
    d->setTitle(fileName);
    // The real name is known now (Content-Disposition, torrent name): add matching tags.
    QList<int> tags = d->rec.tags;
    for (int t : tagsFor(fileName))
        if (!tags.contains(t))
            tags << t;
    d->setTags(tags);
    if (d->rec.state == Download::Starting) {
        d->rec.state = Download::Downloading;
        emit d->stateChanged();
    }
    d->notify();
    persist(d);
}

void DownloadManager::onStats(const TaskStats& st)
{
    if (Download* d = find(st.id)) {
        const QString cat = d->rec.contentCategory;
        const qint64 downBefore = d->rec.received, upBefore = d->uploaded();
        d->applyStats(st);
        // Traffic statistics: only growth counts (a restart from zero is not negative traffic).
        countTraffic(d->rec.received - downBefore, d->uploaded() - upBefore, st.speed + st.uploadSpeed);
        if (d->rec.contentCategory != cat)
            persist(d); // keep it when the torrent is not loaded (finished, or before metadata)
    }
    statsChanged();
}

void DownloadManager::onResumeData(qint64 id, const QByteArray& data)
{
    Download* d = find(id);
    if (!d || !(d->running() || d->seeding()) || data.isEmpty())
        return;
    d->rec.resumeData = data;
    m_db->saveProgress(id, d->rec.received, data);
}

void DownloadManager::onFinished(qint64 id, const QString& filePath)
{
    Download* d = find(id);
    if (!d || !d->running())
        return;
    d->rec.filePath = filePath;
    d->setTitle(QFileInfo(filePath).fileName());
    if (d->rec.total < 0)
        d->rec.total = d->rec.received;
    countTraffic(d->rec.total - d->rec.received, 0, d->speed()); // the part since the last progress report
    d->rec.received = d->rec.total;
    d->rec.finished = QDateTime::currentDateTime();
    d->autoRetryCount = 0;
    d->autoRetryAt = {};
    if (!d->isTorrent())
        d->rec.resumeData.clear(); // torrents keep it to seed after restart
    d->setFileMissing(false);
    d->setState(Download::Completed);
    if (d->isTorrent())
        d->setSeeding(true);
    persist(d);
    emit downloadFinished(d);
    statsChanged();
    schedule();
    if (d->rec.moduleUid == QLatin1String("downloadsm3u") && filePath.endsWith(QLatin1String(".ts"), Qt::CaseInsensitive)
        && convertStreams())
        convertToMp4(d);
    if (d->rec.moduleUid == QLatin1String("downloadswww") && FileNamer::looksLikeTorrentFile(filePath))
        openDownloadedTorrent(d);
    else if (d->rec.moduleUid == QLatin1String("downloadswww") && Metalink::looksLike(filePath))
        openDownloadedMetalink(d);
    else if (!d->isTorrent() && antivirusCheck() && QFileInfo(filePath).isFile())
        checkWithAntivirus(d);
    if (!d->isTorrent() && !d->rec.expectedHash.isEmpty() && QFileInfo(filePath).isFile())
        verifyChecksum(d);
    if (extractArchives() && canExtract(d))
        extract(d);
}

bool DownloadManager::antivirusCheck() const { return m_db->setting(QStringLiteral("AntivirusCheck"), true).toBool(); }
void DownloadManager::setAntivirusCheck(bool on) { m_db->setSetting(QStringLiteral("AntivirusCheck"), on); }

// Like a browser: the antivirus scans the new file and Windows marks it as coming from the internet.
// Runs on its own thread (a scan of a large file takes a while).
void DownloadManager::checkWithAntivirus(Download* d)
{
    const QString path = d->rec.filePath, url = d->rec.url, ref = d->rec.referrer;
    auto result = std::make_shared<std::pair<int, QString>>(AttachmentCheck::Unavailable, QString());
    QThread* th = QThread::create([path, url, ref, result] { result->first = AttachmentCheck::check(path, url, ref, &result->second); });
    QPointer<Download> guard(d);
    connect(th, &QThread::finished, this, [this, th, guard, path, result] {
        th->deleteLater();
        if (!guard || guard->rec.filePath != path)
            return;
        if (result->first == AttachmentCheck::Blocked) {
            const QString msg = tr("Blocked by your antivirus: %1").arg(result->second);
            guard->addLog(msg);
            guard->setFileMissing(!QFileInfo::exists(path));
            guard->setState(Download::Error, msg);
            persist(guard);
            emit antivirusChecked(guard, true, msg);
        } else if (result->first == AttachmentCheck::Clean) {
            guard->addLog(tr("Checked by your antivirus"));
            emit antivirusChecked(guard, false, {});
        } else {
            guard->addLog(tr("Not checked by an antivirus (%1)").arg(result->second));
        }
    });
    th->start();
}

void DownloadManager::setExpectedChecksum(Download* d, const QString& text)
{
    if (!d)
        return;
    d->rec.expectedHash = Checksum::parse(text).hex.isEmpty() ? QString() : text.trimmed();
    d->setHashResult(Download::HashNone, {});
    persist(d);
}

QString DownloadManager::verifyChecksum(Download* d, const QString& expectedText)
{
    if (!d || !d->completed() || d->isTorrent())
        return tr("Only finished downloads can be checked");
    if (d->hashStatus() == Download::HashChecking)
        return tr("Already checking");
    const QString text = expectedText.trimmed().isEmpty() ? d->rec.expectedHash : expectedText.trimmed();
    const Checksum::Expected want = Checksum::parse(text);
    if (!want.valid())
        return tr("Paste an MD5, SHA-1, SHA-256 or SHA-512 checksum");
    const QString path = d->rec.filePath;
    if (!QFileInfo(path).isFile())
        return tr("The file is missing");
    d->rec.expectedHash = text;
    d->setHashResult(Download::HashChecking, tr("Computing %1…").arg(want.name));
    persist(d);
    auto result = std::make_shared<std::pair<QString, QString>>(); // digest, error
    QThread* th = QThread::create([path, want, result] { result->first = Checksum::ofFile(path, want.algo, &result->second); });
    QPointer<Download> guard(d);
    connect(th, &QThread::finished, this, [this, th, guard, path, want, result] {
        th->deleteLater();
        if (!guard)
            return;
        if (result->first.isEmpty()) {
            guard->setHashResult(Download::HashFailed, tr("Could not read the file: %1").arg(result->second));
        } else if (result->first == want.hex) {
            guard->setHashResult(Download::HashMatch, tr("%1 matches").arg(want.name));
            guard->addLog(tr("%1 checksum matches").arg(want.name));
            emit checksumChecked(guard, true, guard->hashText());
        } else {
            guard->setHashResult(Download::HashMismatch, tr("%1 does not match: the file is %2").arg(want.name, result->first));
            guard->addLog(tr("%1 checksum does NOT match (expected %2, file is %3)").arg(want.name, want.hex, result->first));
            emit checksumChecked(guard, false, guard->hashText());
        }
        persist(guard);
    });
    th->start();
    return {};
}

void DownloadManager::openDownloadedTorrent(Download* d)
{
    // Links like ".../torrent/download/<hash>" deliver a .torrent file without saying so in the URL.
    // Like FDM, start it as a torrent instead of leaving the small .torrent file in the list.
    QString path = d->rec.filePath;
    if (!path.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive)) {
        const QString renamed = FileNamer::uniquePath(QFileInfo(path).absolutePath(), QFileInfo(path).fileName() + QStringLiteral(".torrent"));
        if (QFile::rename(path, renamed))
            path = renamed;
    }
    if (!canHandle(path)) {
        d->addLog(tr("This is a .torrent file, but BitTorrent support is not available"));
        return;
    }
    Download* t = add(path, d->rec.destDir, {}, true, d->rec.url);
    if (!t)
        return;
    t->addLog(tr("Started from the torrent file downloaded from %1").arg(d->rec.url));
    const qint64 oldId = d->id();
    QTimer::singleShot(0, this, [this, oldId] { remove(find(oldId), false); });
}

void DownloadManager::openDownloadedMetalink(Download* d)
{
    // Like a .torrent: the small Metalink file turns into the downloads it describes.
    const QList<Download*> added = addFromMetalink(d->rec.filePath, d->rec.destDir, true, d->rec.url);
    if (added.isEmpty()) {
        d->addLog(tr("This looks like a Metalink file, but it lists nothing FluxDM can download"));
        return;
    }
    const qint64 oldId = d->id();
    QTimer::singleShot(0, this, [this, oldId] { remove(find(oldId), false); });
}

void DownloadManager::onFailed(qint64 id, const QString& error, const QByteArray& resumeData)
{
    Download* d = find(id);
    if (!d || !d->running())
        return;
    if (!resumeData.isEmpty())
        d->rec.resumeData = resumeData;
    d->setSegmentsFromResumeData();
    d->setPendingFiles({});
    QString why = error;
    if (!DiskSpace::isWaiting(error) && !DiskSpace::volumeAvailable(d->rec.destDir)) {
        d->addLog(error);
        why = DiskSpace::unavailableText(d->rec.destDir); // the drive went away mid-download
    } else if (DiskSpace::looksLikeFullDisk(error) && !DiskSpace::isShortage(error)) {
        const qint64 free = DiskSpace::freeBytes(d->rec.destDir);
        const qint64 need = d->rec.total > 0 ? qMax<qint64>(0, d->rec.total - d->rec.received) : 0;
        if (free >= 0 && free < need + DiskSpace::kMargin) {
            d->addLog(error);
            why = DiskSpace::shortageText(need, free, d->rec.destDir);
        }
    }
    d->setState(Download::Error, why);
    if (DiskSpace::isWaiting(why)) {
        d->addLog(DiskSpace::isShortage(why) ? tr("Waiting for free disk space; the download continues by itself")
                                             : tr("The download continues when the drive is back"));
        persist(d);
        statsChanged();
        schedule();
        return;
    }
    // FDM-style automatic retries for transient failures: 30 s, 1, 2, 5, 10 min...
    if (isRetriable(error) && d->autoRetryCount < autoRetryLimit()) {
        static const int delays[] = {30, 60, 120, 300, 600};
        const int delay = delays[qMin(d->autoRetryCount, 4)];
        d->autoRetryAt = QDateTime::currentDateTime().addSecs(delay);
        d->addLog(tr("Will retry automatically in %1 s (attempt %2 of %3)").arg(delay).arg(d->autoRetryCount + 1).arg(autoRetryLimit()));
        d->notify();
    }
    persist(d);
    statsChanged();
    schedule();
}

void DownloadManager::onStopped(qint64 id, const QByteArray& resumeData)
{
    Download* d = find(id);
    if (!d)
        return;
    if (d->pendingStops > 0)
        --d->pendingStops;
    d->setPendingFiles({}); // the engine let go; it asks again on the next start
    if (d->completed()) { // seeding ended (ratio reached or stopped by the user)
        d->setSeeding(false);
        d->rec.resumeData.clear(); // don't resume seeding on the next start
        persist(d);
        statsChanged();
        return;
    }
    // If the download was already restarted, this stale data belongs to the old task.
    if (!d->running() && d->pendingStops == 0 && !resumeData.isEmpty() && d->rec.state != Download::Queued) {
        d->rec.resumeData = resumeData;
        d->setSegmentsFromResumeData();
        d->notify();
        m_db->saveProgress(id, d->rec.received, resumeData);
    } else if (d->rec.state == Download::Queued && !resumeData.isEmpty() && !d->rec.resumeData.isEmpty()) {
        // Re-queued by a traffic-mode/schedule switch: keep the freshest resume point.
        d->rec.resumeData = resumeData;
        m_db->saveProgress(id, d->rec.received, resumeData);
    }
    statsChanged();
}

void DownloadManager::onLog(qint64 id, const QString& line)
{
    if (Download* d = find(id))
        d->addLog(line);
}

// ------------------------------------------------------------------ automatic retries

bool DownloadManager::chooseTorrentFiles() const { return m_db->setting(QStringLiteral("BtChooseFiles"), true).toBool(); }
void DownloadManager::setChooseTorrentFiles(bool on) { m_db->setSetting(QStringLiteral("BtChooseFiles"), on); }

void DownloadManager::chooseFiles(Download* d, const QList<int>& wanted)
{
    if (!d || !d->awaitingFiles() || wanted.isEmpty())
        return;
    d->rec.pickFiles = false;
    d->setPendingFiles({});
    persist(d);
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid))
        e->chooseFiles(d->id(), wanted);
}

void DownloadManager::setSequential(Download* d, bool on)
{
    if (!d || d->rec.sequential == on)
        return;
    d->rec.sequential = on;
    d->addLog(on ? tr("Downloading in order") : tr("Downloading rarest pieces first"));
    d->notify();
    persist(d);
    if (DownloadEngine* e = engineByUid(d->rec.moduleUid))
        e->setSequential(d->id(), on);
}

QVariantMap DownloadManager::details(Download* d) const
{
    DownloadEngine* e = d ? engineByUid(d->rec.moduleUid) : nullptr;
    return e ? e->details(d->id()) : QVariantMap();
}

int DownloadManager::existingFileDefault() const
{
    return qBound(0, m_db->setting(QStringLiteral("ExistingFileReaction"), int(FileNamer::Ask)).toInt(), 2);
}

void DownloadManager::setExistingFileDefault(int reaction)
{
    m_db->setSetting(QStringLiteral("ExistingFileReaction"), qBound(0, reaction, 2));
}

void DownloadManager::setExistingFileReaction(Download* d, int reaction)
{
    if (!d)
        return;
    d->rec.existingFileReaction = qBound(0, reaction, 2);
    persist(d);
}

int DownloadManager::autoRetryLimit() const { return qBound(0, m_db->setting(QStringLiteral("AutoRetryCount"), 5).toInt(), 50); }

void DownloadManager::setAutoRetryLimit(int n) { m_db->setSetting(QStringLiteral("AutoRetryCount"), qBound(0, n, 50)); }

bool DownloadManager::isRetriable(const QString& error)
{
    // Permanent problems: retrying cannot help.
    static const QRegularExpression permanent(QStringLiteral(
        R"(HTTP error 4(0[0-7]|09|1\d|2[0-8]|3\d|4\d|5\d)\b|Disk write|Cannot create|Cannot allocate|Cannot rename|Unsupported|)"
        R"(already being downloaded|Untrusted certificate|web page instead|browser check|File already exists|Login required|Login failed|Invalid magnet|Not a valid|No engine|keeps changing|Cannot open)"),
        QRegularExpression::CaseInsensitiveOption);
    return !error.isEmpty() && !permanent.match(error).hasMatch();
}

void DownloadManager::runAutoRetries()
{
    const QDateTime now = QDateTime::currentDateTime();
    for (Download* d : m_list) {
        if (d->failed() && d->autoRetryAt.isValid()) {
            if (d->autoRetryAt <= now) {
                d->autoRetryAt = {};
                ++d->autoRetryCount;
                d->addLog(tr("Retrying automatically"));
                d->setState(Download::Queued);
                persist(d);
            } else {
                d->notify(); // refresh the countdown
            }
        }
    }
}
void DownloadManager::recheckDiskSpace()
{
    bool any = false;
    for (Download* d : m_list) {
        if (!d->failed() || !DiskSpace::isWaiting(d->rec.lastError))
            continue;
        if (DiskSpace::isUnavailable(d->rec.lastError)) {
            if (!DiskSpace::volumeAvailable(d->rec.destDir))
                continue;
            d->addLog(tr("The drive is back; continuing"));
            d->setState(Download::Queued);
            persist(d);
            any = true;
            continue;
        }
        const qint64 free = DiskSpace::freeBytes(d->rec.destDir);
        const qint64 need = (d->rec.total > 0 ? qMax<qint64>(0, d->rec.total - d->rec.received) : 0) + DiskSpace::kMargin;
        if (free >= 0 && free < need)
            continue;
        d->addLog(tr("Enough free disk space again; continuing"));
        d->setState(Download::Queued);
        persist(d);
        any = true;
    }
    if (any)
        schedule();
}

void DownloadManager::checkMissingFiles()
{
    // FDM marks completed downloads whose file was moved or deleted. Looking at many files (some on
    // a network share that may hang) happens on a worker thread, never in the UI.
    if (m_checkingFiles)
        return;
    QList<QPair<qint64, QString>> items;
    for (Download* d : m_list)
        if (d->completed() && !d->rec.filePath.isEmpty())
            items.append({d->id(), d->rec.filePath});
        else
            d->setFileMissing(false);
    if (items.isEmpty())
        return;
    m_checkingFiles = true;
    auto missing = std::make_shared<QSet<qint64>>();
    QThread* th = QThread::create([items, missing] {
        for (const auto& [id, path] : items)
            if (!QFileInfo::exists(path))
                missing->insert(id);
    });
    connect(th, &QThread::finished, this, [this, th, items, missing] {
        th->deleteLater();
        m_checkingFiles = false;
        for (const auto& [id, path] : items)
            if (Download* d = find(id); d && d->completed() && d->rec.filePath == path)
                d->setFileMissing(missing->contains(id));
    });
    th->start();
}
