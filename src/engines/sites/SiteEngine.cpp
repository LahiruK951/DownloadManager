#include "engines/sites/SiteEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "core/FileNamer.h"

SiteEngine::SiteEngine(QObject* parent)
    : DownloadEngine(parent)
{
}

SiteEngine::~SiteEngine()
{
    for (auto& [id, t] : m_tasks)
        if (t->proc) {
            t->proc->disconnect(this);
            t->proc->kill();
            t->proc->waitForFinished(2000);
        }
}

bool SiteEngine::isSupportedSite(const QUrl& url)
{
    const QString s = url.scheme().toLower();
    if (s != QLatin1String("http") && s != QLatin1String("https"))
        return false;
    // Pages, not files: a direct .mp4 on these hosts still goes to the normal HTTP module.
    static const QRegularExpression file(QStringLiteral(R"(\.(mp4|webm|mkv|mp3|m4a|zip|exe|iso|m3u8|mpd)$)"),
                                         QRegularExpression::CaseInsensitiveOption);
    if (file.match(url.path()).hasMatch())
        return false;
    static const QStringList sites = {
        QStringLiteral("youtube.com"), QStringLiteral("youtu.be"), QStringLiteral("youtube-nocookie.com"),
        QStringLiteral("vimeo.com"), QStringLiteral("dailymotion.com"), QStringLiteral("twitch.tv"),
        QStringLiteral("soundcloud.com"), QStringLiteral("bandcamp.com"), QStringLiteral("mixcloud.com"),
        QStringLiteral("x.com"), QStringLiteral("twitter.com"), QStringLiteral("facebook.com"), QStringLiteral("fb.watch"),
        QStringLiteral("instagram.com"), QStringLiteral("tiktok.com"), QStringLiteral("reddit.com"),
        QStringLiteral("bilibili.com"), QStringLiteral("ted.com"), QStringLiteral("rumble.com"),
        QStringLiteral("streamable.com"), QStringLiteral("vk.com"), QStringLiteral("nicovideo.jp")};
    const QString host = url.host().toLower();
    for (const QString& site : sites)
        if (host == site || host.endsWith(QLatin1Char('.') + site))
            return true;
    return false;
}

bool SiteEngine::canHandle(const QUrl& url) const { return isSupportedSite(url); }

QStringList SiteEngine::arguments(const TaskParams& p, int connections, qint64 speedLimit)
{
    const QString dir = QDir::toNativeSeparators(p.destDir);
    QStringList a{QStringLiteral("--newline"), QStringLiteral("--no-colors"), QStringLiteral("--no-playlist"),
                  QStringLiteral("--no-simulate"), QStringLiteral("--continue"), QStringLiteral("--no-mtime"),
                  // machine-readable progress: done total estimate speed
                  QStringLiteral("--progress-template"),
                  QStringLiteral("download:FLUXDM-PROGRESS %(progress.downloaded_bytes)s %(progress.total_bytes)s "
                                 "%(progress.total_bytes_estimate)s %(progress.speed)s"),
                  QStringLiteral("--print"), QStringLiteral("after_move:FLUXDM-FILE %(filepath)s"),
                  QStringLiteral("-N"), QString::number(qBound(1, connections, 8)),
                  QStringLiteral("-P"), dir};
    // Best quality: video + audio merged (needs ffmpeg), else the best single file.
    if (!p.ffmpeg.isEmpty())
        a << QStringLiteral("-f") << QStringLiteral("bv*+ba/b") << QStringLiteral("--merge-output-format") << QStringLiteral("mp4/mkv")
          << QStringLiteral("--ffmpeg-location") << QDir::toNativeSeparators(p.ffmpeg);
    else
        a << QStringLiteral("-f") << QStringLiteral("b");
    a << QStringLiteral("-o") << (p.fileName.isEmpty() ? QStringLiteral("%(title).150B [%(id)s].%(ext)s")
                                                       : FileNamer::sanitize(p.fileName));
    if (speedLimit > 0)
        a << QStringLiteral("--limit-rate") << QString::number(speedLimit);
    if (!p.proxy.isEmpty())
        a << QStringLiteral("--proxy") << (p.proxy == QLatin1String("none") ? QString() : p.proxy);
    const QString ua = headerSafe(p.userAgent), ref = headerSafe(p.referrer), cookies = headerSafe(p.cookies);
    if (!ua.isEmpty())
        a << QStringLiteral("--user-agent") << ua;
    if (!ref.isEmpty())
        a << QStringLiteral("--referer") << ref;
    if (!cookies.isEmpty())
        a << QStringLiteral("--add-headers") << QStringLiteral("Cookie:") + cookies;
    if (!p.user.isEmpty())
        a << QStringLiteral("-u") << p.user << QStringLiteral("-p") << p.password;
    if (p.ipVersion == 4 || p.ipVersion == 6)
        a << (p.ipVersion == 4 ? QStringLiteral("-4") : QStringLiteral("-6"));
    a << QStringLiteral("--") << p.url;
    return a;
}

SiteEngine::Line SiteEngine::parse(const QString& raw)
{
    Line l;
    const QString line = raw.trimmed();
    if (line.startsWith(QLatin1String("FLUXDM-PROGRESS "))) {
        const QStringList f = line.mid(16).split(QLatin1Char(' '));
        l.kind = Line::Progress;
        auto num = [&](int i) { bool ok = false; const double v = f.value(i).toDouble(&ok); return ok ? v : -1.0; };
        l.done = qint64(qMax(0.0, num(0)));
        l.total = num(1) > 0 ? qint64(num(1)) : (num(2) > 0 ? qint64(num(2)) : -1);
        l.speed = qMax(0.0, num(3));
    } else if (line.startsWith(QLatin1String("FLUXDM-FILE "))) {
        l.kind = Line::Final;
        l.path = line.mid(12);
    } else if (line.startsWith(QLatin1String("[download] Destination: "))) {
        l.kind = Line::Destination;
        l.path = line.mid(24);
    } else if (line.startsWith(QLatin1String("[Merger] Merging formats into \""))) {
        l.kind = Line::Destination; // the name the merged file will have
        l.path = line.mid(31).chopped(1);
    } else if (line.startsWith(QLatin1String("[download] ")) && line.endsWith(QLatin1String(" has already been downloaded"))) {
        l.kind = Line::AlreadyThere;
        l.path = line.mid(11).chopped(28);
    } else if (line.startsWith(QLatin1String("ERROR: "))) {
        l.kind = Line::Error;
        l.text = line.mid(7);
        // "[youtube] abc: Video unavailable" -> "Video unavailable"
        static const QRegularExpression prefix(QStringLiteral(R"(^\[[^\]]+\]\s*[^:]+:\s*)"));
        l.text.remove(prefix);
    }
    return l;
}

void SiteEngine::start(const TaskParams& params)
{
    if (auto it = m_tasks.find(params.id); it != m_tasks.end()) {
        if (it->second->proc) {
            it->second->proc->disconnect(this);
            it->second->proc->kill();
        }
        m_tasks.erase(it);
    }
    if (params.siteTool.isEmpty()) {
        emit failed(params.id, tr("Downloading from this site needs yt-dlp: put yt-dlp.exe next to FluxDM, on PATH, "
                                  "or set it in Settings > Downloads"), {});
        return;
    }
    auto t = std::make_unique<Task>();
    t->id = params.id;
    t->p = params;
    QDir().mkpath(params.destDir);
    auto* proc = new QProcess(this);
    t->proc = proc;
    const qint64 id = params.id;
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, id, proc] {
        if (auto it = m_tasks.find(id); it != m_tasks.end())
            onOutput(*it->second, proc->readAllStandardOutput());
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, id, proc] {
        if (auto it = m_tasks.find(id); it != m_tasks.end())
            onOutput(*it->second, proc->readAllStandardError());
    });
    connect(proc, &QProcess::finished, this, [this, id](int code, QProcess::ExitStatus st) { onFinished(id, code, st); });
    connect(proc, &QProcess::errorOccurred, this, [this, id](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            onFinished(id, -1, QProcess::CrashExit);
    });
    // siteTool: program, then any prefix arguments (tests run a script through Python).
    QStringList cmd = params.siteTool;
    const QString program = cmd.takeFirst();
    const QStringList args = cmd + arguments(params, m_maxConn, params.speedLimit > 0 ? params.speedLimit : m_limit);
    emit log(id, tr("Asking yt-dlp for %1").arg(params.url));
    m_tasks.emplace(id, std::move(t));
    proc->start(program, args);
}

void SiteEngine::onOutput(Task& t, const QByteArray& chunk)
{
    t.outBuf += chunk;
    int nl;
    while ((nl = int(t.outBuf.indexOf('\n'))) >= 0) {
        const QString line = QString::fromUtf8(t.outBuf.left(nl)).trimmed();
        t.outBuf.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        const Line l = parse(line);
        switch (l.kind) {
        case Line::Progress:
            t.curDone = l.done;
            t.curTotal = l.total;
            t.speed = l.speed;
            {
                TaskStats s;
                s.id = t.id;
                s.received = t.previousStreams + t.curDone;
                s.total = t.curTotal > 0 ? t.previousStreams + t.curTotal : -1;
                s.speed = t.speed;
                s.connections = 1;
                emit stats(s);
            }
            break;
        case Line::Destination:
        case Line::AlreadyThere:
            if (!t.currentFile.isEmpty() && l.path != t.currentFile && l.kind == Line::Destination && t.curTotal > 0)
                t.previousStreams += t.curTotal; // the video stream is done; now the audio one
            t.curDone = 0;
            t.currentFile = l.path;
            if (!t.metaSent || l.path.endsWith(QLatin1String(".mp4")) || l.path.endsWith(QLatin1String(".mkv"))) {
                t.metaSent = true;
                // Format-specific intermediate names ("x.f137.mp4") become the final name after merging.
                QString name = QFileInfo(l.path).fileName();
                name.remove(QRegularExpression(QStringLiteral(R"(\.f\d+(?=\.\w+$))")));
                emit meta(t.id, name, QDir(t.p.destDir).filePath(name), -1, true);
            }
            emit log(t.id, line);
            break;
        case Line::Final:
            t.finalFile = l.path;
            break;
        case Line::Error:
            t.lastError = l.text;
            emit log(t.id, line);
            break;
        case Line::None:
            if (line.startsWith(QLatin1Char('[')) && !line.startsWith(QLatin1String("[download]")))
                emit log(t.id, line); // [youtube] Extracting..., [Merger] ..., [info] ...
            break;
        }
    }
}

void SiteEngine::onFinished(qint64 id, int code, QProcess::ExitStatus st)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end())
        return;
    std::unique_ptr<Task> t = std::move(it->second);
    m_tasks.erase(it);
    if (t->proc)
        t->proc->deleteLater();
    if (t->stopping) {
        emit stopped(id, QJsonDocument(QJsonObject{{QStringLiteral("kind"), QStringLiteral("site")}}).toJson(QJsonDocument::Compact));
        return;
    }
    const QString path = !t->finalFile.isEmpty() ? t->finalFile : t->currentFile;
    if (st == QProcess::NormalExit && code == 0 && !path.isEmpty() && QFileInfo::exists(path)) {
        const qint64 size = QFileInfo(path).size();
        emit meta(id, QFileInfo(path).fileName(), path, size, true);
        emit stats(TaskStats{id, size, size, 0, 0, {}});
        emit log(id, tr("Download complete: %1").arg(QDir::toNativeSeparators(path)));
        emit finished(id, path);
        return;
    }
    QString err = t->lastError;
    if (err.isEmpty())
        err = code == -1 ? tr("Cannot start yt-dlp (%1)").arg(t->p.siteTool.value(0)) : tr("yt-dlp stopped with code %1").arg(code);
    emit failed(id, err, QJsonDocument(QJsonObject{{QStringLiteral("kind"), QStringLiteral("site")}}).toJson(QJsonDocument::Compact));
}

void SiteEngine::stop(qint64 id)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) {
        emit stopped(id, {});
        return;
    }
    it->second->stopping = true;
    if (it->second->proc)
        it->second->proc->kill(); // yt-dlp keeps its .part files; --continue picks them up next time
    else
        onFinished(id, 0, QProcess::NormalExit);
}

QHash<qint64, QByteArray> SiteEngine::shutdown()
{
    QHash<qint64, QByteArray> out;
    for (auto& [id, t] : m_tasks) {
        out.insert(id, QByteArrayLiteral("{\"kind\":\"site\"}"));
        if (t->proc) {
            t->proc->disconnect(this);
            t->proc->kill();
            t->proc->waitForFinished(3000);
        }
    }
    m_tasks.clear();
    return out;
}
