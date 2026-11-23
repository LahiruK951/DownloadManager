#pragma once
#include <curl/curl.h>

#include <QByteArray>
#include <QSet>
#include <QStringList>
#include <QString>
#include <memory>
#include <vector>

#include "core/Types.h"
#include "engines/www/FileWriter.h"

class CurlWorker;
class HttpTask;

// One byte range of the file served by (at most) one connection.
struct Segment {
    qint64 begin = 0;
    qint64 end = -1;   // inclusive; -1 = unknown (size not known yet)
    qint64 done = 0;   // bytes written starting at `begin`

    CURL* h = nullptr;
    int retries = 0;
    qint64 retryAt = 0; // worker clock ms; 0 = start as soon as a slot is free
    bool probe = false;
    bool validated = false;
    int source = 0;    // 0 = the original address, n = mirror n
    bool reachedEnd = false;
    bool paused = false;
    bool browserCheck = false; // the reply is a Cloudflare "are you a browser" challenge
    bool linkFailure = false;  // resuming: the link answered without the requested part
    bool webPage = false;      // ... and sent a web page instead
    QString usedUrl;           // the address this connection asked
    QString fatal;

    QByteArray contentDisposition, contentRange, etag, lastModified, contentType;
    char errbuf[CURL_ERROR_SIZE] = {};
    HttpTask* task = nullptr;

    qint64 cur() const { return begin + done; }
    qint64 remaining() const { return end < 0 ? -1 : end - cur() + 1; }
    bool complete() const { return end >= 0 && remaining() <= 0; }
};

// A single HTTP/HTTPS/FTP download split into dynamically-balanced segments.
// Lives entirely on the CurlWorker thread.
class HttpTask {
public:
    HttpTask(CurlWorker* worker, const TaskParams& params);
    ~HttpTask();

    qint64 id() const { return m_p.id; }
    void start();
    void stop();
    void tick(qint64 nowMs);
    void onDone(Segment* s, CURLcode rc, qint64 nowMs);
    TaskStats stats(double dtSec);
    QByteArray resumeData() const;
    bool isDead() const { return m_dead; }
    bool takeDirty() { const bool d = m_dirty; m_dirty = false; return d; }
    // Periodic save: resume data is taken first, the file is flushed on a helper thread, and only
    // then is the resume data reported, so saved progress never covers bytes that aren't on the
    // disk yet, and a slow disk doesn't stall the other downloads.
    void saveResumeDataDurably();
    void setMaxConnections(int n) { m_p.maxConnections = n < 1 ? 1 : n; }
    // Per-download speed cap (token bucket refilled by the worker loop).
    void setSpeedLimit(qint64 bps) { m_limit = bps < 0 ? 0 : bps; m_bucket = 0; }
    bool taskThrottled() const { return m_limit > 0 && m_bucket <= 0; }
    bool hasSpeedLimit() const { return m_limit > 0; }
    void refillBucket(qint64 dtMs)
    {
        if (m_limit <= 0)
            return;
        m_bucket += double(m_limit) * double(dtMs) / 1000.0;
        const double cap = qMax(double(m_limit) / 4.0, 16384.0);
        if (m_bucket > cap)
            m_bucket = cap;
    }
    std::vector<std::unique_ptr<Segment>>& segments() { return m_segs; }

    static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t headerCb(char* ptr, size_t size, size_t nmemb, void* userdata);

private:
    bool restore(const QByteArray& data);
    bool isHttp() const;
    bool onFirstData(Segment* s);
    bool initMeta(Segment* s, long code);
    void adoptLink(Segment* s);
    void startSegment(Segment* s);
    void releaseHandle(Segment* s);
    int activeCount() const;
    bool allComplete() const;
    qint64 received() const;
    void resetAll();
    void complete();
    void fail(const QString& error);
    void log(const QString& line);

    CurlWorker* m_w;
    TaskParams m_p;
    std::vector<std::unique_ptr<Segment>> m_segs;
    FileWriter m_file;

    QString m_effUrl;
    QString m_fileName, m_finalPath, m_partPath;
    QByteArray m_etag, m_lastModified;
    qint64 m_total = -1;
    bool m_metaKnown = false;
    bool m_resumable = false;
    bool m_writeError = false;
    bool m_fatalTask = false;
    QStringList m_sources;      // original + mirrors
    QSet<int> m_badSources;     // mirrors that failed or serve something else
    int m_nextSource = 0;
    int pickSource();
    bool m_changedOnServer = false;
    // The saved (redirected) link stopped working, so the original address is being asked for a
    // fresh one. Cleared when the fresh link delivers; allows one refresh per expiry.
    bool m_linkRefreshed = false;
    // The user gave a new address: continue only if it serves a file of the same size.
    bool m_verifyAddress = false;
    QString currentUrl() const { return m_effUrl.isEmpty() ? m_p.url : m_effUrl; }
    bool m_dead = false;
    bool m_dirty = false;
    int m_restarts = 0;

    qint64 m_tickBytes = 0;
    qint64 m_limit = 0;
    double m_bucket = 0;
    double m_speed = 0;
    bool m_speedInit = false;
};
