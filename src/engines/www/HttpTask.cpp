#include "engines/www/HttpTask.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>

#include "core/FileNamer.h"
#include "engines/www/CurlWorker.h"

namespace {
constexpr qint64 kMinSplit = 512 * 1024; // never create a segment smaller than this
constexpr int kMaxRetries = 5;

qint64 parseContentRangeTotal(const QByteArray& v)
{
    // "bytes 0-1023/4096"  or  "bytes */4096"
    const int slash = v.lastIndexOf('/');
    if (slash < 0)
        return -1;
    bool ok = false;
    const qint64 t = v.mid(slash + 1).trimmed().toLongLong(&ok);
    return ok ? t : -1;
}
} // namespace

HttpTask::HttpTask(CurlWorker* worker, const TaskParams& params)
    : m_w(worker), m_p(params)
{
    setMaxConnections(m_p.maxConnections);
    setSpeedLimit(m_p.speedLimit);
}

HttpTask::~HttpTask()
{
    for (auto& s : m_segs)
        releaseHandle(s.get());
    m_file.close();
}

void HttpTask::log(const QString& line) { emit m_w->log(m_p.id, line); }

bool HttpTask::isHttp() const
{
    return m_p.url.startsWith(QLatin1String("http"), Qt::CaseInsensitive);
}

// ------------------------------------------------------------------ lifecycle

void HttpTask::start()
{
    if (!m_p.resumeData.isEmpty() && restore(m_p.resumeData)) {
        QString err;
        if (QFile::exists(m_partPath) && m_file.open(m_partPath, false, &err) && m_file.preallocate(m_total)) {
            m_metaKnown = true;
            m_resumable = true;
            log(QStringLiteral("Resuming at %1 of %2 bytes").arg(received()).arg(m_total));
            emit m_w->meta(m_p.id, m_fileName, m_finalPath, m_total, true);
            return; // tick() starts the unfinished segments
        }
        log(QStringLiteral("Partial file is missing; starting over"));
        m_file.close();
        m_segs.clear();
    }
    resetAll();
}

void HttpTask::resetAll()
{
    for (auto& s : m_segs)
        releaseHandle(s.get());
    m_segs.clear();
    m_file.close();
    if (!m_partPath.isEmpty())
        QFile::remove(m_partPath);
    m_metaKnown = false;
    m_resumable = false;
    m_changedOnServer = false;
    m_total = -1;

    auto s = std::make_unique<Segment>();
    s->task = this;
    s->probe = true;
    m_segs.push_back(std::move(s));
    log(QStringLiteral("Connecting to %1").arg(m_p.url));
    startSegment(m_segs.back().get());
}

void HttpTask::stop()
{
    for (auto& s : m_segs)
        releaseHandle(s.get());
    m_file.close();
    m_dead = true;
}

void HttpTask::complete()
{
    for (auto& s : m_segs)
        releaseHandle(s.get());
    m_file.close();

    QString target = m_finalPath;
    if (QFile::exists(target)) // someone created it meanwhile
        target = FileNamer::uniquePath(QFileInfo(target).absolutePath(), QFileInfo(target).fileName());
    if (!QFile::rename(m_partPath, target)) {
        fail(QStringLiteral("Cannot rename the downloaded file to %1").arg(target));
        return;
    }
    m_finalPath = target;
    log(QStringLiteral("Download complete: %1").arg(QDir::toNativeSeparators(target)));
    m_dead = true;
    emit m_w->finished(m_p.id, target);
}

void HttpTask::fail(const QString& error)
{
    for (auto& s : m_segs)
        releaseHandle(s.get());
    m_file.close();
    log(QStringLiteral("Error: %1").arg(error));
    m_dead = true;
    emit m_w->failed(m_p.id, error, resumeData());
}

// ------------------------------------------------------------------ segments

void HttpTask::startSegment(Segment* s)
{
    CURL* h = curl_easy_init();
    s->h = h;
    s->validated = false;
    s->reachedEnd = false;
    s->paused = false;
    s->fatal.clear();
    s->contentDisposition.clear();
    s->contentRange.clear();
    s->etag.clear();
    s->lastModified.clear();
    s->errbuf[0] = 0;
    s->retryAt = 0;

    if (!m_resumable && m_metaKnown)
        s->done = 0; // cannot resume: restart this (only) segment from the beginning

    const QByteArray url = (m_effUrl.isEmpty() ? m_p.url : m_effUrl).toUtf8();
    curl_easy_setopt(h, CURLOPT_URL, url.constData());
    curl_easy_setopt(h, CURLOPT_PRIVATE, s);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &HttpTask::writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, &HttpTask::headerCb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, s->errbuf);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(h, CURLOPT_BUFFERSIZE, 131072L);
    curl_easy_setopt(h, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_REVOKE_BEST_EFFORT));

    const QByteArray ua = m_p.userAgent.toUtf8();
    if (!ua.isEmpty())
        curl_easy_setopt(h, CURLOPT_USERAGENT, ua.constData());
    const QByteArray ref = m_p.referrer.toUtf8();
    if (!ref.isEmpty())
        curl_easy_setopt(h, CURLOPT_REFERER, ref.constData());
    const QByteArray cookies = m_p.cookies.toUtf8();
    if (!cookies.isEmpty())
        curl_easy_setopt(h, CURLOPT_COOKIE, cookies.constData());
    const QByteArray proxy = m_p.proxy.toUtf8();
    if (!proxy.isEmpty())
        curl_easy_setopt(h, CURLOPT_PROXY, proxy.constData());

    // The probe asks for "0-" so a 206 reply tells us ranges are supported
    // without spending an extra request.
    QByteArray range = QByteArray::number(s->cur()) + '-';
    if (m_metaKnown && m_resumable && s->end >= 0)
        range += QByteArray::number(s->end);
    curl_easy_setopt(h, CURLOPT_RANGE, range.constData());

    curl_multi_add_handle(m_w->multi(), h);
}

void HttpTask::releaseHandle(Segment* s)
{
    if (!s->h)
        return;
    curl_multi_remove_handle(m_w->multi(), s->h);
    curl_easy_cleanup(s->h);
    s->h = nullptr;
    s->paused = false;
}

int HttpTask::activeCount() const
{
    return int(std::count_if(m_segs.begin(), m_segs.end(), [](const auto& s) { return s->h != nullptr; }));
}

qint64 HttpTask::received() const
{
    qint64 r = 0;
    for (const auto& s : m_segs)
        r += s->done;
    return r;
}

bool HttpTask::allComplete() const
{
    if (!m_metaKnown)
        return false;
    if (m_total == 0)
        return true;
    return std::all_of(m_segs.begin(), m_segs.end(), [](const auto& s) { return s->complete(); });
}

// ------------------------------------------------------------------ callbacks

size_t HttpTask::headerCb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* s = static_cast<Segment*>(userdata);
    const size_t len = size * nmemb;
    const QByteArray line = QByteArray(ptr, qsizetype(len)).trimmed();
    if (line.startsWith("HTTP/")) { // new response (e.g. after a redirect)
        s->contentDisposition.clear();
        s->contentRange.clear();
        s->etag.clear();
        s->lastModified.clear();
        return len;
    }
    const int c = line.indexOf(':');
    if (c > 0) {
        const QByteArray key = line.left(c).trimmed().toLower();
        const QByteArray val = line.mid(c + 1).trimmed();
        if (key == "content-disposition")
            s->contentDisposition = val;
        else if (key == "content-range")
            s->contentRange = val;
        else if (key == "etag")
            s->etag = val;
        else if (key == "last-modified")
            s->lastModified = val;
    }
    return len;
}

size_t HttpTask::writeCb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* s = static_cast<Segment*>(userdata);
    HttpTask* t = s->task;
    const size_t n = size * nmemb;

    if (!s->validated) {
        if (!t->onFirstData(s))
            return 0; // abort; reason is in s->fatal
        s->validated = true;
    }
    if (t->m_w->throttled() || t->taskThrottled()) {
        s->paused = true;
        return CURL_WRITEFUNC_PAUSE; // curl re-delivers this chunk after unpause
    }

    qint64 take = qint64(n);
    if (s->end >= 0) {
        const qint64 rem = s->remaining();
        if (rem <= 0) {
            s->reachedEnd = true;
            return 0;
        }
        take = qMin(take, rem);
    }
    if (!t->m_file.write(s->cur(), ptr, take)) {
        t->m_writeError = true;
        return 0;
    }
    s->done += take;
    t->m_tickBytes += take;
    t->m_dirty = true;
    t->m_w->consume(take);
    if (t->m_limit > 0)
        t->m_bucket -= double(take);
    if (take < qint64(n)) { // range was shortened by a split: our part is done
        s->reachedEnd = true;
        return 0;
    }
    return n;
}

bool HttpTask::onFirstData(Segment* s)
{
    long code = 0;
    curl_easy_getinfo(s->h, CURLINFO_RESPONSE_CODE, &code);
    if (!m_metaKnown)
        return initMeta(s, code);
    if (!isHttp() || !m_resumable)
        return true;
    if (code != 206) {
        s->fatal = QStringLiteral("Server ignored the range request (HTTP %1)").arg(code);
        return false;
    }
    const qint64 tot = parseContentRangeTotal(s->contentRange);
    if (tot > 0 && tot != m_total) {
        s->fatal = QStringLiteral("File size changed on the server");
        m_changedOnServer = true;
        return false;
    }
    return true;
}

bool HttpTask::initMeta(Segment* s, long code)
{
    curl_off_t cl = -1;
    curl_easy_getinfo(s->h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    char* eff = nullptr;
    curl_easy_getinfo(s->h, CURLINFO_EFFECTIVE_URL, &eff);
    if (eff)
        m_effUrl = QString::fromUtf8(eff);

    if (isHttp()) {
        if (code == 206) {
            m_total = parseContentRangeTotal(s->contentRange);
            m_resumable = m_total > 0;
        } else {
            m_total = cl >= 0 ? qint64(cl) : -1;
            m_resumable = false;
        }
    } else { // FTP: SIZE + REST
        m_total = cl > 0 ? qint64(cl) : -1;
        m_resumable = m_total > 0;
    }
    m_etag = s->etag;
    m_lastModified = s->lastModified;

    QString name = m_p.fileName;
    if (name.isEmpty())
        name = FileNamer::fromContentDisposition(s->contentDisposition);
    if (name.isEmpty())
        name = FileNamer::fromUrl(QUrl(m_effUrl));
    if (name.isEmpty())
        name = FileNamer::fromUrl(QUrl(m_p.url));
    name = FileNamer::sanitize(name);

    QDir().mkpath(m_p.destDir);
    m_finalPath = FileNamer::uniquePath(m_p.destDir, name);
    m_fileName = QFileInfo(m_finalPath).fileName();
    m_partPath = m_finalPath + FileNamer::kPartSuffix;

    QString err;
    if (!m_file.open(m_partPath, true, &err)) {
        s->fatal = QStringLiteral("Cannot create file: %1").arg(err);
        m_fatalTask = true;
        return false;
    }
    if (m_total > 0 && !m_file.preallocate(m_total)) {
        s->fatal = QStringLiteral("Cannot allocate %1 bytes: %2").arg(m_total).arg(m_file.lastError());
        m_fatalTask = true;
        return false;
    }
    s->end = m_total > 0 ? m_total - 1 : -1;
    s->probe = false;
    m_metaKnown = true;
    m_dirty = true;

    log(QStringLiteral("HTTP %1, size %2, %3")
            .arg(code)
            .arg(m_total >= 0 ? QString::number(m_total) : QStringLiteral("unknown"))
            .arg(m_resumable ? QStringLiteral("resume supported") : QStringLiteral("resume NOT supported")));
    if (m_effUrl != m_p.url)
        log(QStringLiteral("Redirected to %1").arg(m_effUrl));
    emit m_w->meta(m_p.id, m_fileName, m_finalPath, m_total, m_resumable);
    return true;
}

// ------------------------------------------------------------------ scheduling

void HttpTask::onDone(Segment* s, CURLcode rc, qint64 nowMs)
{
    long code = 0;
    curl_easy_getinfo(s->h, CURLINFO_RESPONSE_CODE, &code);
    const QString curlErr = s->errbuf[0] ? QString::fromUtf8(s->errbuf).trimmed()
                                         : QString::fromUtf8(curl_easy_strerror(rc));
    // Empty body: the write callback never ran, so collect metadata now.
    if (rc == CURLE_OK && !m_metaKnown && !initMeta(s, code)) {
        releaseHandle(s);
        fail(s->fatal);
        return;
    }
    releaseHandle(s);
    if (m_dead)
        return;

    if (m_writeError) {
        fail(QStringLiteral("Disk write error: %1").arg(m_file.lastError()));
        return;
    }
    if (m_fatalTask) {
        fail(s->fatal);
        return;
    }

    bool ok = s->reachedEnd || rc == CURLE_OK;
    if (ok) {
        if (s->end < 0) { // size was unknown: the transfer ending cleanly means done
            m_total = s->done;
            s->end = s->done - 1;
        } else if (!s->complete()) {
            ok = false; // connection closed before our range was delivered
        }
    }
    if (ok) {
        s->retries = 0;
        if (allComplete())
            complete();
        return;
    }

    if (m_changedOnServer) {
        if (++m_restarts > 2) {
            fail(QStringLiteral("The file keeps changing on the server"));
            return;
        }
        log(QStringLiteral("File changed on the server; restarting from scratch"));
        resetAll();
        return;
    }

    const QString err = !s->fatal.isEmpty() ? s->fatal
                        : code >= 400       ? QStringLiteral("HTTP error %1").arg(code)
                                            : curlErr;
    // Permanent client errors: don't hammer the server.
    if (code >= 400 && code < 500 && code != 408 && code != 429) {
        fail(err);
        return;
    }

    const int idx = int(std::find_if(m_segs.begin(), m_segs.end(), [s](const auto& p) { return p.get() == s; }) - m_segs.begin());
    if (++s->retries <= kMaxRetries) {
        const qint64 delay = 1000LL << (s->retries - 1);
        s->retryAt = nowMs + delay;
        log(QStringLiteral("Connection %1: %2 (retry %3/%4 in %5 s)").arg(idx + 1).arg(err).arg(s->retries).arg(kMaxRetries).arg(delay / 1000));
        return;
    }
    if (activeCount() > 0) { // other connections still work: keep trying slowly
        s->retries = 0;
        s->retryAt = nowMs + 30000;
        log(QStringLiteral("Connection %1: %2 (will retry in 30 s)").arg(idx + 1).arg(err));
        return;
    }
    fail(err);
}

void HttpTask::tick(qint64 nowMs)
{
    if (m_dead)
        return;
    int active = activeCount();
    const int maxConn = m_resumable ? m_p.maxConnections : 1;

    // 1) (re)start idle unfinished segments: resumed ones and retries whose time has come
    for (auto& up : m_segs) {
        Segment* s = up.get();
        if (active >= maxConn)
            break;
        if (s->h || s->complete() || nowMs < s->retryAt)
            continue;
        if (!m_metaKnown && !s->probe)
            continue;
        startSegment(s);
        ++active;
    }

    // 2) dynamic segmentation: split the largest remaining range in half
    if (!m_metaKnown || !m_resumable)
        return;
    while (active < maxConn) {
        Segment* best = nullptr;
        for (auto& up : m_segs) {
            Segment* s = up.get();
            if (s->h && (!best || s->remaining() > best->remaining()))
                best = s;
        }
        if (!best || best->remaining() < 2 * kMinSplit)
            break;
        const qint64 upper = best->remaining() / 2;
        auto ns = std::make_unique<Segment>();
        ns->task = this;
        ns->begin = best->cur() + (best->remaining() - upper);
        ns->end = best->end;
        best->end = ns->begin - 1;
        Segment* raw = ns.get();
        m_segs.push_back(std::move(ns));
        startSegment(raw);
        ++active;
        m_dirty = true;
    }
}

// ------------------------------------------------------------------ state

TaskStats HttpTask::stats(double dtSec)
{
    TaskStats st;
    st.id = m_p.id;
    st.total = m_total;
    st.received = received();
    const double inst = dtSec > 0 ? double(m_tickBytes) / dtSec : 0;
    m_speed = m_speedInit ? 0.6 * m_speed + 0.4 * inst : inst;
    m_speedInit = true;
    m_tickBytes = 0;
    st.speed = m_speed;
    st.connections = activeCount();
    st.segments.reserve(qsizetype(m_segs.size() * 3));
    for (const auto& s : m_segs)
        st.segments << s->begin << s->end << s->done;
    return st;
}

QByteArray HttpTask::resumeData() const
{
    if (!m_metaKnown || !m_resumable)
        return {};
    QJsonObject o;
    o[QStringLiteral("v")] = 1;
    o[QStringLiteral("resumable")] = true;
    o[QStringLiteral("total")] = m_total;
    o[QStringLiteral("url")] = m_effUrl;
    o[QStringLiteral("fileName")] = m_fileName;
    o[QStringLiteral("finalPath")] = m_finalPath;
    o[QStringLiteral("partPath")] = m_partPath;
    o[QStringLiteral("etag")] = QString::fromLatin1(m_etag);
    o[QStringLiteral("lastModified")] = QString::fromLatin1(m_lastModified);
    QJsonArray segs;
    for (const auto& s : m_segs)
        segs.append(QJsonArray{s->begin, s->end, s->done});
    o[QStringLiteral("segments")] = segs;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool HttpTask::restore(const QByteArray& data)
{
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    if (o.value(QStringLiteral("v")).toInt() != 1 || !o.value(QStringLiteral("resumable")).toBool())
        return false;
    m_total = o.value(QStringLiteral("total")).toInteger(-1);
    m_effUrl = o.value(QStringLiteral("url")).toString();
    m_fileName = o.value(QStringLiteral("fileName")).toString();
    m_finalPath = o.value(QStringLiteral("finalPath")).toString();
    m_partPath = o.value(QStringLiteral("partPath")).toString();
    m_etag = o.value(QStringLiteral("etag")).toString().toLatin1();
    m_lastModified = o.value(QStringLiteral("lastModified")).toString().toLatin1();

    std::vector<std::unique_ptr<Segment>> segs;
    for (const auto& v : o.value(QStringLiteral("segments")).toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() != 3)
            return false;
        auto s = std::make_unique<Segment>();
        s->task = this;
        s->begin = a[0].toInteger();
        s->end = a[1].toInteger();
        s->done = a[2].toInteger();
        if (s->end < s->begin - 1 || s->done < 0 || s->done > s->end - s->begin + 1)
            return false;
        segs.push_back(std::move(s));
    }
    if (m_total <= 0 || segs.empty() || m_partPath.isEmpty())
        return false;

    // Merge adjacent completed segments so the list doesn't grow forever.
    std::sort(segs.begin(), segs.end(), [](const auto& a, const auto& b) { return a->begin < b->begin; });
    for (auto& s : segs) {
        if (!m_segs.empty() && m_segs.back()->complete() && s->complete() && m_segs.back()->end + 1 == s->begin) {
            m_segs.back()->end = s->end;
            m_segs.back()->done = m_segs.back()->end - m_segs.back()->begin + 1;
        } else {
            m_segs.push_back(std::move(s));
        }
    }
    return true;
}
