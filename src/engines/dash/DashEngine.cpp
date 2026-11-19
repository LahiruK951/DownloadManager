#include "engines/dash/DashEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "core/FileNamer.h"
#include "core/LinkCheck.h"
#include "engines/hls/HlsEngine.h"

static constexpr int kMaxRetries = 5;

DashEngine::DashEngine(QObject* parent)
    : DownloadEngine(parent)
{
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &DashEngine::emitStats);
    m_timer.start();
}

DashEngine::~DashEngine()
{
    for (auto& [id, t] : m_tasks)
        release(*t);
}

bool DashEngine::canHandle(const QUrl& url) const
{
    const QString s = url.scheme().toLower();
    return (s == QLatin1String("http") || s == QLatin1String("https"))
        && url.path().endsWith(QLatin1String(".mpd"), Qt::CaseInsensitive);
}

DashEngine::Task* DashEngine::task(qint64 id)
{
    auto it = m_tasks.find(id);
    return it == m_tasks.end() ? nullptr : it->second.get();
}

QNetworkRequest DashEngine::request(const Task& t, const MpdSegment& s) const
{
    QNetworkRequest r(s.url);
    if (!t.p.userAgent.isEmpty())
        r.setHeader(QNetworkRequest::UserAgentHeader, t.p.userAgent);
    if (!t.p.referrer.isEmpty())
        r.setRawHeader("Referer", t.p.referrer.toUtf8());
    if (!t.p.cookies.isEmpty())
        r.setRawHeader("Cookie", t.p.cookies.toUtf8());
    if (s.rangeStart >= 0)
        r.setRawHeader("Range", "bytes=" + QByteArray::number(s.rangeStart) + '-'
                                    + (s.rangeEnd >= 0 ? QByteArray::number(s.rangeEnd) : QByteArray()));
    r.setTransferTimeout(60000);
    return r;
}

// ------------------------------------------------------------------ lifecycle

void DashEngine::start(const TaskParams& params)
{
    if (Task* old = task(params.id)) {
        release(*old);
        m_tasks.erase(params.id);
    }
    auto t = std::make_unique<Task>();
    t->id = params.id;
    t->p = params;
    t->limit = params.speedLimit;
    t->mpdUrl = QUrl(params.url);
    t->window.start();
    const QJsonObject rd = QJsonDocument::fromJson(params.resumeData).object();
    if (rd.value(QStringLiteral("kind")).toString() == QLatin1String("dash"))
        t->resume = rd;
    const qint64 id = params.id;
    m_tasks.emplace(id, std::move(t));

    emit log(id, tr("Loading DASH manifest %1").arg(params.url));
    MpdSegment m;
    m.url = QUrl(params.url);
    QNetworkReply* reply = m_nam.get(request(*task(id), m));
    task(id)->replies.append(reply);
    connect(reply, &QNetworkReply::finished, this, [this, id, reply] { onManifest(id, reply); });
}

void DashEngine::onManifest(qint64 id, QNetworkReply* reply)
{
    reply->deleteLater();
    Task* tp = task(id);
    if (!tp || tp->stopping)
        return;
    Task& t = *tp;
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        fail(t, code >= 400 ? LinkCheck::httpErrorText(code, t.mpdUrl) : tr("Cannot load the manifest: %1").arg(reply->errorString()));
        return;
    }
    const MpdManifest m = Mpd::parse(reply->readAll(), reply->url());
    if (!m.valid) {
        fail(t, m.error);
        return;
    }

    // The same representations as before when resuming, else the best of each kind.
    QList<const MpdRepresentation*> chosen;
    const QJsonArray saved = t.resume.value(QStringLiteral("tracks")).toArray();
    if (!saved.isEmpty()) {
        for (const QJsonValue& v : saved)
            if (const MpdRepresentation* r = m.byId(v.toObject().value(QStringLiteral("rep")).toString()))
                chosen << r;
        if (chosen.size() != saved.size()) {
            emit log(id, tr("The stream changed since the last time; starting over"));
            chosen.clear();
            t.resume = {};
        }
    }
    if (chosen.isEmpty()) {
        if (const MpdRepresentation* v = m.best(QStringLiteral("video")))
            chosen << v;
        if (const MpdRepresentation* a = m.best(QStringLiteral("audio")))
            chosen << a;
    }

    qint64 bandwidth = 0;
    for (int i = 0; i < chosen.size(); ++i) {
        auto trk = std::make_unique<Track>();
        trk->rep = *chosen[i];
        if (!trk->rep.init.url.isEmpty())
            trk->items << trk->rep.init;
        trk->items << trk->rep.segments;
        bandwidth += trk->rep.bandwidth;
        t.tracks.push_back(std::move(trk));
    }
    if (m.duration > 0 && bandwidth > 0)
        t.estimate = qint64(double(bandwidth) * m.duration / 8.0);

    for (const auto& trk : t.tracks)
        emit log(id, trk->rep.kind == QLatin1String("video")
                         ? QObject::tr("Video: %1x%2, %3 kbps, %4 (%5 segments)").arg(trk->rep.width).arg(trk->rep.height)
                               .arg(trk->rep.bandwidth / 1000).arg(trk->rep.codecs).arg(trk->items.size())
                         : QObject::tr("Audio: %1 kbps, %2 (%3 segments)").arg(trk->rep.bandwidth / 1000).arg(trk->rep.codecs).arg(trk->items.size()));

    if (!t.resume.isEmpty()) {
        t.fileName = t.resume.value(QStringLiteral("fileName")).toString();
        t.finalPath = t.resume.value(QStringLiteral("finalPath")).toString();
        for (size_t i = 0; i < t.tracks.size(); ++i) {
            const QJsonObject o = saved[int(i)].toObject();
            Track& trk = *t.tracks[i];
            trk.partPath = o.value(QStringLiteral("part")).toString();
            trk.nextToRequest = trk.nextToWrite = o.value(QStringLiteral("done")).toInt();
            trk.written = o.value(QStringLiteral("written")).toInteger();
            if (!QFile::exists(trk.partPath) || QFileInfo(trk.partPath).size() < trk.written) {
                trk.nextToRequest = trk.nextToWrite = 0; // part file gone: this track from the start
                trk.written = 0;
            }
        }
        emit log(id, tr("Resuming"));
    } else {
        const bool webm = std::any_of(t.tracks.begin(), t.tracks.end(), [](const auto& trk) { return trk->rep.mimeType.contains(QLatin1String("webm")); });
        const bool audioOnly = t.tracks.size() == 1 && t.tracks[0]->rep.kind == QLatin1String("audio");
        QString name = t.p.fileName;
        if (name.isEmpty())
            name = HlsEngine::outputBaseName(t.mpdUrl) + (audioOnly ? (webm ? QStringLiteral(".webm") : QStringLiteral(".m4a"))
                                                                   : (webm ? QStringLiteral(".mkv") : QStringLiteral(".mp4")));
        name = FileNamer::sanitize(name);
        QDir().mkpath(t.p.destDir);
        bool exists = false;
        t.finalPath = FileNamer::resolveTarget(t.p.destDir, name, t.p.existingFile, &exists);
        if (exists) {
            fail(t, QStringLiteral("File already exists: %1").arg(name));
            return;
        }
        t.fileName = QFileInfo(t.finalPath).fileName();
        for (const auto& trk : t.tracks)
            trk->partPath = t.finalPath + QLatin1Char('.') + trk->rep.kind + FileNamer::kPartSuffix;
    }
    for (const auto& trk : t.tracks) {
        trk->file.setFileName(trk->partPath);
        if (!trk->file.open(trk->written > 0 ? QIODevice::ReadWrite : (QIODevice::WriteOnly | QIODevice::Truncate))) {
            fail(t, tr("Cannot create file: %1").arg(trk->file.errorString()));
            return;
        }
        trk->file.resize(trk->written); // drop anything written after the last saved point
        trk->file.seek(trk->written);
    }
    emit meta(id, t.fileName, t.finalPath, t.estimate, true);
    pump(id);
}

void DashEngine::pump(qint64 id)
{
    Task* tp = task(id);
    if (!tp || tp->stopping || tp->mux)
        return;
    Task& t = *tp;
    if (std::all_of(t.tracks.begin(), t.tracks.end(), [](const auto& trk) { return trk->done(); })) {
        finishTracks(t);
        return;
    }
    const qint64 cap = t.limit > 0 ? (m_limit > 0 ? qMin(t.limit, m_limit) : t.limit) : m_limit;
    if (cap > 0 && t.window.elapsed() < 1000 && t.windowBytes >= cap)
        return; // over the speed limit for this second; emitStats() pumps again
    int inFlight = 0;
    for (const auto& trk : t.tracks)
        inFlight += int(trk->inFlight.size());
    while (inFlight < m_maxConn) {
        // The track that is furthest behind (relative to its length) goes next, so both finish together.
        Track* next = nullptr;
        int nextIndex = -1;
        double best = 2;
        for (size_t i = 0; i < t.tracks.size(); ++i) {
            Track& trk = *t.tracks[i];
            if (trk.nextToRequest >= trk.items.size() || trk.ready.size() > 16) // don't buffer too far ahead
                continue;
            const double pos = double(trk.nextToRequest) / double(qMax<qsizetype>(1, trk.items.size()));
            if (pos < best) {
                best = pos;
                next = &trk;
                nextIndex = int(i);
            }
        }
        if (!next)
            break;
        const int index = next->nextToRequest++;
        next->inFlight.insert(index);
        ++inFlight;
        QNetworkReply* reply = m_nam.get(request(t, next->items[index]));
        t.replies.append(reply);
        connect(reply, &QNetworkReply::finished, this, [this, id, nextIndex, index, reply] { onSegment(id, nextIndex, index, reply); });
    }
}

void DashEngine::onSegment(qint64 id, int trackIndex, int index, QNetworkReply* reply)
{
    reply->deleteLater();
    Task* tp = task(id);
    if (!tp || tp->stopping || trackIndex >= int(tp->tracks.size()))
        return;
    Task& t = *tp;
    t.replies.removeAll(reply);
    Track& trk = *t.tracks[size_t(trackIndex)];
    trk.inFlight.remove(index);
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        if (code >= 400 && code < 500 && code != 408 && code != 429) {
            fail(t, LinkCheck::httpErrorText(code, reply->url()));
            return;
        }
        const int n = ++trk.retries[index];
        if (n > kMaxRetries) {
            fail(t, code >= 400 ? LinkCheck::httpErrorText(code, reply->url()) : reply->errorString());
            return;
        }
        emit log(id, tr("Segment %1 of the %2: %3 (retry %4/%5)").arg(index + 1).arg(trk.rep.kind, reply->errorString()).arg(n).arg(kMaxRetries));
        trk.inFlight.insert(index); // reserved until the retry starts
        QTimer::singleShot(1000 << (n - 1), this, [this, id, trackIndex, index] {
            Task* tp2 = task(id);
            if (!tp2 || tp2->stopping)
                return;
            Track& tr2 = *tp2->tracks[size_t(trackIndex)];
            QNetworkReply* again = m_nam.get(request(*tp2, tr2.items[index]));
            tp2->replies.append(again);
            connect(again, &QNetworkReply::finished, this, [this, id, trackIndex, index, again] { onSegment(id, trackIndex, index, again); });
        });
        return;
    }
    const QByteArray data = reply->readAll();
    t.tickBytes += data.size();
    t.windowBytes += data.size();
    trk.ready.insert(index, data);
    while (trk.ready.contains(trk.nextToWrite)) {
        const QByteArray chunk = trk.ready.take(trk.nextToWrite);
        if (trk.file.write(chunk) != chunk.size()) {
            fail(t, tr("Disk write error: %1").arg(trk.file.errorString()));
            return;
        }
        trk.written += chunk.size();
        ++trk.nextToWrite;
    }
    pump(id);
}

void DashEngine::finishTracks(Task& t)
{
    for (const auto& trk : t.tracks)
        trk->file.close();
    const qint64 id = t.id;
    auto done = [this, id](const QString& path) {
        Task* tp = task(id);
        if (!tp)
            return;
        const qint64 size = QFileInfo(path).size();
        emit stats(TaskStats{id, size, size, 0, 0, {0, size - 1, size}});
        emit log(id, tr("Download complete: %1").arg(QDir::toNativeSeparators(path)));
        release(*tp);
        m_tasks.erase(id);
        emit finished(id, path);
    };

    if (t.tracks.size() == 1) { // video only or audio only: the part file is the result
        const QString target = FileNamer::completionTarget(t.finalPath, t.p.existingFile);
        if (!QFile::rename(t.tracks[0]->partPath, target)) {
            fail(t, tr("Cannot rename the downloaded file to %1").arg(target));
            return;
        }
        done(target);
        return;
    }

    const QString video = t.tracks[0]->partPath, audio = t.tracks[1]->partPath;
    auto keepSeparate = [this, id, video, audio, done](const QString& why) {
        Task* tp = task(id);
        if (!tp)
            return;
        // FDM shows one file; without ffmpeg the best we can do is two playable files.
        const QString v = FileNamer::completionTarget(tp->finalPath, tp->p.existingFile);
        const QFileInfo fi(v);
        const QString a = FileNamer::uniquePath(fi.absolutePath(), fi.completeBaseName() + QStringLiteral(".m4a"));
        if (!QFile::rename(video, v) || !QFile::rename(audio, a)) {
            fail(*tp, tr("Cannot rename the downloaded files"));
            return;
        }
        emit log(id, tr("%1 Video and audio were saved separately: %2").arg(why, QFileInfo(a).fileName()));
        done(v);
    };
    if (t.p.ffmpeg.isEmpty()) {
        keepSeparate(tr("ffmpeg was not found, so the tracks couldn't be combined."));
        return;
    }
    const QString target = FileNamer::completionTarget(t.finalPath, t.p.existingFile);
    const QString tmp = target + QStringLiteral(".mux") + FileNamer::kPartSuffix + QLatin1Char('.') + QFileInfo(target).suffix();
    emit log(id, tr("Combining video and audio…"));
    auto* proc = new QProcess(this);
    t.mux = proc;
    connect(proc, &QProcess::finished, this, [this, id, proc, tmp, target, video, audio, done, keepSeparate](int code, QProcess::ExitStatus st) {
        proc->deleteLater();
        if (!task(id))
            return;
        if (st == QProcess::NormalExit && code == 0 && QFileInfo(tmp).size() > 0 && QFile::rename(tmp, target)) {
            QFile::remove(video);
            QFile::remove(audio);
            done(target);
            return;
        }
        QFile::remove(tmp);
        const QString err = QString::fromLocal8Bit(proc->readAllStandardError()).trimmed().section(QLatin1Char('\n'), -1);
        keepSeparate(tr("Combining failed (%1).").arg(err.isEmpty() ? tr("exit code %1").arg(code) : err));
    });
    QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-nostdin"),
                     QStringLiteral("-y"), QStringLiteral("-i"), video, QStringLiteral("-i"), audio,
                     QStringLiteral("-map"), QStringLiteral("0:v:0"), QStringLiteral("-map"), QStringLiteral("1:a:0"),
                     QStringLiteral("-c"), QStringLiteral("copy")};
    if (target.endsWith(QLatin1String(".mp4"), Qt::CaseInsensitive))
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    proc->start(t.p.ffmpeg, args << tmp);
}

void DashEngine::fail(Task& t, const QString& error)
{
    const qint64 id = t.id;
    const QByteArray rd = makeResumeData(t);
    emit log(id, tr("Error: %1").arg(error));
    release(t);
    m_tasks.erase(id);
    emit failed(id, error, rd);
}

void DashEngine::release(Task& t)
{
    t.stopping = true;
    for (const auto& r : t.replies)
        if (r) {
            r->disconnect(this);
            r->abort();
            r->deleteLater();
        }
    t.replies.clear();
    if (t.mux) {
        t.mux->disconnect(this);
        t.mux->kill();
        t.mux->waitForFinished(2000);
    }
    for (const auto& trk : t.tracks) {
        trk->file.flush();
        trk->file.close();
    }
}

QByteArray DashEngine::makeResumeData(const Task& t) const
{
    if (t.tracks.empty() || t.finalPath.isEmpty())
        return {};
    QJsonArray tracks;
    for (const auto& trk : t.tracks)
        tracks.append(QJsonObject{{QStringLiteral("rep"), trk->rep.id}, {QStringLiteral("part"), trk->partPath},
                                  {QStringLiteral("done"), trk->nextToWrite}, {QStringLiteral("written"), trk->written}});
    return QJsonDocument(QJsonObject{{QStringLiteral("kind"), QStringLiteral("dash")}, {QStringLiteral("tracks"), tracks},
                                     {QStringLiteral("fileName"), t.fileName}, {QStringLiteral("finalPath"), t.finalPath},
                                     {QStringLiteral("partPath"), t.tracks[0]->partPath}})
        .toJson(QJsonDocument::Compact);
}

qint64 DashEngine::received(const Task& t) const
{
    qint64 r = 0;
    for (const auto& trk : t.tracks) {
        r += trk->written;
        for (const QByteArray& b : trk->ready)
            r += b.size();
    }
    return r;
}

void DashEngine::emitStats()
{
    ++m_tick;
    std::vector<qint64> ids;
    for (auto& [id, t] : m_tasks) {
        if (t->tracks.empty() || t->stopping)
            continue;
        const double inst = double(t->tickBytes) * 2.0; // 500 ms ticks
        t->tickBytes = 0;
        t->speed = t->speed <= 0 ? inst : t->speed * 0.7 + inst * 0.3;
        if (t->window.elapsed() >= 1000) {
            t->window.restart();
            t->windowBytes = 0;
            ids.push_back(id);
        }
        TaskStats s;
        s.id = id;
        s.received = received(*t);
        // The estimate from the manifest can be off; never show more than 100 %.
        s.total = t->estimate > 0 ? qMax(t->estimate, s.received) : -1;
        s.speed = t->speed;
        int conns = 0;
        for (const auto& trk : t->tracks)
            conns += int(trk->inFlight.size());
        s.connections = conns;
        emit stats(s);
        if (s.received > 0 && m_tick % 4 == 0)
            emit resumeData(id, makeResumeData(*t));
    }
    for (qint64 id : ids)
        pump(id); // a new speed-limit window opened
}

void DashEngine::stop(qint64 id)
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

QHash<qint64, QByteArray> DashEngine::shutdown()
{
    QHash<qint64, QByteArray> out;
    for (auto& [id, t] : m_tasks) {
        out.insert(id, makeResumeData(*t));
        release(*t);
    }
    m_tasks.clear();
    return out;
}

void DashEngine::setTaskSpeedLimit(qint64 id, qint64 bytesPerSec)
{
    if (Task* t = task(id))
        t->limit = bytesPerSec;
}
