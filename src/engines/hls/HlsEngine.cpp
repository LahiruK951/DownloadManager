#include "engines/hls/HlsEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxyFactory>
#include <QNetworkReply>

#include "core/Aes128.h"
#include "core/FileNamer.h"

namespace {
constexpr int kMaxRetries = 5;
constexpr int kReadyWindow = 24; // segments buffered in memory ahead of the writer
}

HlsEngine::HlsEngine(QObject* parent)
    : DownloadEngine(parent)
{
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    m_nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &HlsEngine::emitStats);
    m_timer.start();
}

HlsEngine::~HlsEngine() { shutdown(); }

bool HlsEngine::canHandle(const QUrl& url) const
{
    const QString s = url.scheme().toLower();
    return (s == QLatin1String("http") || s == QLatin1String("https"))
        && url.path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

QString HlsEngine::outputBaseName(const QUrl& playlistUrl)
{
    // ".../my-show/master.m3u8" -> "my-show": generic playlist names say nothing about the video.
    static const QStringList generic = {QStringLiteral("master"), QStringLiteral("index"), QStringLiteral("playlist"),
                                        QStringLiteral("prog_index"), QStringLiteral("chunklist"), QStringLiteral("manifest"),
                                        QStringLiteral("main"), QStringLiteral("video")};
    const QStringList parts = playlistUrl.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return QStringLiteral("video");
    QString base = QFileInfo(parts.last()).completeBaseName();
    if (generic.contains(base.toLower()) && parts.size() >= 2)
        base = parts[parts.size() - 2];
    return base.isEmpty() ? QStringLiteral("video") : base;
}

HlsEngine::Task* HlsEngine::task(qint64 id)
{
    auto it = m_tasks.find(id);
    return it == m_tasks.end() ? nullptr : it->second.get();
}

QNetworkRequest HlsEngine::request(const Task& t, const QUrl& url) const
{
    QNetworkRequest r(url);
    if (!t.p.userAgent.isEmpty())
        r.setHeader(QNetworkRequest::UserAgentHeader, t.p.userAgent);
    if (!t.p.referrer.isEmpty())
        r.setRawHeader("Referer", t.p.referrer.toUtf8());
    if (!t.p.cookies.isEmpty())
        r.setRawHeader("Cookie", t.p.cookies.toUtf8());
    r.setTransferTimeout(60000);
    return r;
}

QNetworkReply* HlsEngine::get(Task& t, const QUrl& url)
{
    QNetworkReply* reply = m_nam.get(request(t, url));
    t.replies.append(reply);
    return reply;
}

// ------------------------------------------------------------------ lifecycle

void HlsEngine::start(const TaskParams& params)
{
    if (task(params.id)) {
        release(*task(params.id));
        m_tasks.erase(params.id);
    }
    auto t = std::make_unique<Task>();
    t->id = params.id;
    t->p = params;
    t->pace.start();
    Task& ref = *t;
    m_tasks.emplace(params.id, std::move(t));

    const QJsonObject rd = QJsonDocument::fromJson(params.resumeData).object();
    if (rd.value(QStringLiteral("kind")).toString() == QLatin1String("hls") && QFile::exists(rd.value(QStringLiteral("partPath")).toString())) {
        ref.resumed = true;
        ref.mediaUrl = QUrl(rd.value(QStringLiteral("media")).toString());
        ref.nextToRequest = ref.nextToWrite = rd.value(QStringLiteral("done")).toInt();
        ref.written = rd.value(QStringLiteral("written")).toInteger();
        ref.fileName = rd.value(QStringLiteral("fileName")).toString();
        ref.finalPath = rd.value(QStringLiteral("finalPath")).toString();
        ref.partPath = rd.value(QStringLiteral("partPath")).toString();
        ref.initWritten = true;
        emit log(params.id, tr("Resuming at segment %1").arg(ref.nextToWrite + 1));
        fetchPlaylist(params.id, ref.mediaUrl, true);
        return;
    }
    emit log(params.id, tr("Loading playlist %1").arg(params.url));
    fetchPlaylist(params.id, QUrl(params.url), false);
}

void HlsEngine::fetchPlaylist(qint64 id, const QUrl& url, bool isMedia)
{
    Task* t = task(id);
    if (!t)
        return;
    QNetworkReply* reply = get(*t, url);
    connect(reply, &QNetworkReply::finished, this, [this, id, reply, isMedia] {
        reply->deleteLater();
        Task* t = task(id);
        if (!t)
            return;
        t->replies.removeAll(reply);
        if (reply->error() != QNetworkReply::NoError) {
            fail(*t, tr("Cannot load the playlist: %1").arg(reply->errorString()));
            return;
        }
        const QUrl finalUrl = reply->url();
        const M3u8Playlist pl = M3u8::parse(reply->readAll(), finalUrl);
        if (!pl.valid) {
            fail(*t, tr("Not an HLS playlist"));
            return;
        }
        if (pl.master) {
            const M3u8Variant* best = pl.best();
            if (!best || isMedia) {
                fail(*t, tr("The playlist has no playable streams"));
                return;
            }
            emit log(id, tr("%1 quality levels; using %2 (%3 kbit/s)")
                             .arg(pl.variants.size())
                             .arg(best->resolution.isEmpty() ? tr("best") : best->resolution)
                             .arg(best->bandwidth / 1000));
            t->mediaUrl = best->url;
            fetchPlaylist(id, best->url, true);
            return;
        }
        t->mediaUrl = finalUrl;
        t->pl = pl;
        onMediaPlaylist(*t);
    });
}

void HlsEngine::onMediaPlaylist(Task& t)
{
    if (!t.pl.endList) {
        fail(t, tr("Live streams are not supported yet"));
        return;
    }
    if (t.pl.segments.isEmpty()) {
        fail(t, tr("The playlist has no segments"));
        return;
    }
    for (const M3u8Key& k : t.pl.keys)
        if (k.method != QLatin1String("AES-128")) {
            fail(t, tr("Encryption %1 is not supported (DRM-protected stream)").arg(k.method));
            return;
        }

    if (!t.resumed) {
        QString name = t.p.fileName;
        if (name.isEmpty())
            name = outputBaseName(QUrl(t.p.url)) + (t.pl.initSection.isValid() ? QStringLiteral(".mp4") : QStringLiteral(".ts"));
        name = FileNamer::sanitize(name);
        QDir().mkpath(t.p.destDir);
        t.finalPath = FileNamer::uniquePath(t.p.destDir, name);
        t.fileName = QFileInfo(t.finalPath).fileName();
        t.partPath = t.finalPath + FileNamer::kPartSuffix;
    }
    t.file.setFileName(t.partPath);
    if (!t.file.open(t.resumed ? QIODevice::ReadWrite : (QIODevice::WriteOnly | QIODevice::Truncate))) {
        fail(t, tr("Cannot create file: %1").arg(t.file.errorString()));
        return;
    }
    if (t.resumed) {
        if (t.nextToWrite > t.pl.segments.size()) {
            fail(t, tr("The stream changed since the download was paused"));
            return;
        }
        t.file.resize(t.written); // drop anything written after the last saved point
        t.file.seek(t.written);
    }
    emit log(t.id, tr("%1 segments, %2 min%3")
                       .arg(t.pl.segments.size())
                       .arg(qRound(t.pl.totalDuration / 60.0))
                       .arg(t.pl.keys.isEmpty() ? QString() : tr(", AES-128 encrypted")));
    emit meta(t.id, t.fileName, t.finalPath, -1, true);

    if (t.pl.initSection.isValid() && !t.initWritten) {
        const qint64 id = t.id;
        QNetworkReply* reply = get(t, t.pl.initSection);
        connect(reply, &QNetworkReply::finished, this, [this, id, reply] {
            reply->deleteLater();
            Task* t = task(id);
            if (!t)
                return;
            t->replies.removeAll(reply);
            if (reply->error() != QNetworkReply::NoError) {
                fail(*t, tr("Cannot load the stream header: %1").arg(reply->errorString()));
                return;
            }
            const QByteArray data = reply->readAll();
            t->file.write(data);
            t->written += data.size();
            t->initWritten = true;
            pump(id);
        });
        return;
    }
    t.initWritten = true;
    pump(t.id);
}

// ------------------------------------------------------------------ segments

void HlsEngine::pump(qint64 id)
{
    Task* t = task(id);
    if (!t || !t->initWritten)
        return;
    const int count = int(t->pl.segments.size());
    const int conc = qMin(m_maxConn, 6);
    while (t->inFlight.size() < conc && t->nextToRequest < count && t->ready.size() < kReadyWindow) {
        // Traffic-mode speed limit: pace requests so the average stays under the cap.
        if (m_limit > 0 || t->p.speedLimit > 0) {
            qint64 perTask = m_limit > 0 ? qMax<qint64>(16 * 1024, m_limit / qMax<qint64>(1, qint64(m_tasks.size()))) : LLONG_MAX;
            if (t->p.speedLimit > 0)
                perTask = qMin(perTask, t->p.speedLimit);
            const double allowed = double(perTask) * (double(t->pace.elapsed()) / 1000.0 + 1.0);
            if (double(t->pacedBytes) > allowed) {
                QTimer::singleShot(200, this, [this, id] { pump(id); });
                return;
            }
        }
        const int index = t->nextToRequest;
        const M3u8Segment& seg = t->pl.segments[index];
        if (seg.keyIndex >= 0 && !t->keys.contains(seg.keyIndex)) {
            if (!t->keysLoading.contains(seg.keyIndex)) {
                t->keysLoading.insert(seg.keyIndex);
                const int k = seg.keyIndex;
                QNetworkReply* reply = get(*t, t->pl.keys[k].uri);
                connect(reply, &QNetworkReply::finished, this, [this, id, reply, k] {
                    reply->deleteLater();
                    Task* t = task(id);
                    if (!t)
                        return;
                    t->replies.removeAll(reply);
                    const QByteArray key = reply->readAll();
                    if (reply->error() != QNetworkReply::NoError || key.size() != 16) {
                        fail(*t, tr("Cannot load the decryption key"));
                        return;
                    }
                    t->keys.insert(k, key);
                    t->keysLoading.remove(k);
                    pump(id);
                });
            }
            return; // wait for the key
        }
        ++t->nextToRequest;
        t->inFlight.insert(index);
        QNetworkReply* reply = get(*t, seg.url);
        connect(reply, &QNetworkReply::finished, this, [this, id, index, reply] { onSegment(id, index, reply); });
    }
}

void HlsEngine::onSegment(qint64 id, int index, QNetworkReply* reply)
{
    reply->deleteLater();
    Task* t = task(id);
    if (!t)
        return;
    t->replies.removeAll(reply);
    t->inFlight.remove(index);

    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (++t->retries[index] > kMaxRetries || (status >= 400 && status < 500 && status != 408 && status != 429)) {
            fail(*t, tr("Segment %1: %2").arg(index + 1).arg(reply->errorString()));
            return;
        }
        emit log(id, tr("Segment %1: %2 (retry %3)").arg(index + 1).arg(reply->errorString()).arg(t->retries[index]));
        // re-request this segment after a short backoff
        QTimer::singleShot(1000 * t->retries[index], this, [this, id, index] {
            Task* t = task(id);
            if (!t)
                return;
            t->inFlight.insert(index);
            QNetworkReply* r = get(*t, t->pl.segments[index].url);
            connect(r, &QNetworkReply::finished, this, [this, id, index, r] { onSegment(id, index, r); });
        });
        return;
    }

    QByteArray data = reply->readAll();
    t->tickBytes += data.size();
    t->pacedBytes += data.size();
    const M3u8Segment& seg = t->pl.segments[index];
    if (seg.keyIndex >= 0) {
        const M3u8Key& key = t->pl.keys[seg.keyIndex];
        bool ok = false;
        data = Aes128::decrypt(t->keys.value(seg.keyIndex), key.iv.isEmpty() ? M3u8::sequenceIv(seg.sequence) : key.iv, data, &ok);
        if (!ok) {
            fail(*t, tr("Segment %1 could not be decrypted").arg(index + 1));
            return;
        }
    }
    t->ready.insert(index, data);
    flush(*t);
    if (task(id))
        pump(id);
}

void HlsEngine::flush(Task& t)
{
    while (t.ready.contains(t.nextToWrite)) {
        const QByteArray data = t.ready.take(t.nextToWrite);
        if (t.file.write(data) != data.size()) {
            fail(t, tr("Disk write error: %1").arg(t.file.errorString()));
            return;
        }
        t.written += data.size();
        ++t.nextToWrite;
        t.dirty = true;
    }
    if (t.nextToWrite == t.pl.segments.size())
        finish(t);
}

void HlsEngine::finish(Task& t)
{
    const qint64 id = t.id;
    t.file.close();
    QString target = t.finalPath;
    if (QFile::exists(target))
        target = FileNamer::uniquePath(QFileInfo(target).absolutePath(), QFileInfo(target).fileName());
    if (!QFile::rename(t.partPath, target)) {
        fail(t, tr("Cannot rename the downloaded file to %1").arg(target));
        return;
    }
    emit stats(TaskStats{id, t.written, t.written, 0, 0, {0, t.written - 1, t.written}});
    emit log(id, tr("Download complete: %1").arg(QDir::toNativeSeparators(target)));
    release(t);
    m_tasks.erase(id);
    emit finished(id, target);
}

void HlsEngine::fail(Task& t, const QString& error)
{
    const qint64 id = t.id;
    const QByteArray rd = makeResumeData(t);
    emit log(id, tr("Error: %1").arg(error));
    release(t);
    m_tasks.erase(id);
    emit failed(id, error, rd);
}

void HlsEngine::release(Task& t)
{
    const auto replies = t.replies;
    t.replies.clear();
    for (const QPointer<QNetworkReply>& r : replies)
        if (r) {
            r->disconnect(this);
            r->abort();
            r->deleteLater();
        }
    if (t.file.isOpen()) {
        t.file.flush();
        t.file.close();
    }
}

// ------------------------------------------------------------------ state

QByteArray HlsEngine::makeResumeData(const Task& t) const
{
    if (t.partPath.isEmpty() || !t.mediaUrl.isValid())
        return {};
    return QJsonDocument(QJsonObject{{QStringLiteral("v"), 1},
                                     {QStringLiteral("kind"), QStringLiteral("hls")},
                                     {QStringLiteral("media"), t.mediaUrl.toString()},
                                     {QStringLiteral("done"), t.nextToWrite},
                                     {QStringLiteral("written"), t.written},
                                     {QStringLiteral("fileName"), t.fileName},
                                     {QStringLiteral("finalPath"), t.finalPath},
                                     {QStringLiteral("partPath"), t.partPath}})
        .toJson(QJsonDocument::Compact);
}

void HlsEngine::emitStats()
{
    ++m_tick;
    for (auto& [id, up] : m_tasks) {
        Task& t = *up;
        if (t.pl.segments.isEmpty())
            continue;
        const double inst = double(t.tickBytes) / 0.5;
        t.speed = t.speedInit ? 0.6 * t.speed + 0.4 * inst : inst;
        t.speedInit = true;
        t.tickBytes = 0;

        qint64 buffered = 0;
        for (const QByteArray& b : t.ready)
            buffered += b.size();
        const int count = int(t.pl.segments.size());
        const int got = t.nextToWrite + int(t.ready.size());
        TaskStats s;
        s.id = id;
        s.received = t.written + buffered;
        // Size is unknown up front: extrapolate from the segments received so far.
        s.total = got > 0 ? qMax(s.received, qint64(double(s.received) / got * count)) : -1;
        s.speed = t.speed;
        s.connections = int(t.inFlight.size());
        if (s.total > 0) {
            const double per = double(s.total) / count;
            s.segments << 0 << qint64(per * count) - 1 << qint64(per * t.nextToWrite);
            for (auto it = t.ready.begin(); it != t.ready.end(); ++it)
                s.segments << qint64(per * it.key()) << qint64(per * (it.key() + 1)) - 1 << qint64(per);
        }
        emit stats(s);
        if (m_tick % 4 == 0 && t.dirty) {
            t.dirty = false;
            emit resumeData(id, makeResumeData(t));
        }
    }
}

void HlsEngine::stop(qint64 id)
{
    Task* t = task(id);
    if (!t) {
        emit stopped(id, {});
        return;
    }
    const QByteArray rd = makeResumeData(*t);
    release(*t);
    m_tasks.erase(id);
    emit stopped(id, rd);
}

QHash<qint64, QByteArray> HlsEngine::shutdown()
{
    QHash<qint64, QByteArray> out;
    for (auto& [id, t] : m_tasks) {
        out.insert(id, makeResumeData(*t));
        release(*t);
    }
    m_tasks.clear();
    m_timer.stop();
    return out;
}

void HlsEngine::setTaskSpeedLimit(qint64 id, qint64 bytesPerSec)
{
    if (Task* t = task(id)) {
        t->p.speedLimit = bytesPerSec;
        t->pacedBytes = 0;
        t->pace.restart();
        pump(id);
    }
}