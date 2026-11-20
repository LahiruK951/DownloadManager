#include "engines/www/CurlWorker.h"

#include <QMutexLocker>
#include <algorithm>
#include <vector>

#include "engines/www/HttpTask.h"

CurlWorker::CurlWorker(QObject* parent)
    : QThread(parent)
{
    qRegisterMetaType<TaskStats>();
    qRegisterMetaType<TaskParams>();
    m_multi = curl_multi_init();
    if (!m_multi)
        qFatal("curl_multi_init failed"); // nothing can be downloaded without it; the crash handler records it
    // One TCP connection per segment: never multiplex ranges onto one HTTP/2 connection.
    curl_multi_setopt(m_multi, CURLMOPT_PIPELINING, long(CURLPIPE_NOTHING));
    m_clock.start();
}

CurlWorker::~CurlWorker()
{
    if (isRunning())
        shutdownAndWait();
    curl_multi_cleanup(m_multi);
}

void CurlWorker::post(std::function<void()> fn)
{
    {
        QMutexLocker l(&m_mx);
        m_cmds.push_back(std::move(fn));
    }
    curl_multi_wakeup(m_multi);
}

void CurlWorker::startTask(const TaskParams& params)
{
    post([this, params] {
        auto it = m_tasks.find(params.id);
        if (it != m_tasks.end()) { // restart of a still-known task: drop the old one first
            it->second->stop();
            m_tasks.erase(it);
        }
        TaskParams p = params;
        p.maxConnections = std::min(p.maxConnections, connectionCap());
        auto t = std::make_unique<HttpTask>(this, p);
        HttpTask* raw = t.get();
        m_tasks.emplace(p.id, std::move(t));
        raw->start();
    });
}

void CurlWorker::stopTask(qint64 id)
{
    post([this, id] {
        auto it = m_tasks.find(id);
        if (it == m_tasks.end()) {
            emit stopped(id, {});
            return;
        }
        it->second->stop();
        const QByteArray rd = it->second->resumeData();
        m_tasks.erase(it);
        emit stopped(id, rd);
    });
}

void CurlWorker::setSpeedLimit(qint64 bytesPerSec)
{
    post([this, bytesPerSec] {
        m_limit = std::max<qint64>(0, bytesPerSec);
        m_bucket = 0;
    });
}

void CurlWorker::setTaskSpeedLimit(qint64 id, qint64 bytesPerSec)
{
    post([this, id, bytesPerSec] {
        auto it = m_tasks.find(id);
        if (it != m_tasks.end())
            it->second->setSpeedLimit(bytesPerSec);
    });
}

void CurlWorker::setMaxConnections(int perDownload)
{
    post([this, perDownload] {
        m_maxConn = std::max(1, perDownload);
        for (auto& [id, t] : m_tasks)
            t->setMaxConnections(connectionCap());
    });
}

void CurlWorker::setMaxConnectionsPerServer(int n)
{
    post([this, n] {
        m_perServer = std::max(0, n);
        // curl queues extra transfers to the same host until a connection frees up.
        curl_multi_setopt(m_multi, CURLMOPT_MAX_HOST_CONNECTIONS, long(m_perServer));
        for (auto& [id, t] : m_tasks)
            t->setMaxConnections(connectionCap());
    });
}

QHash<qint64, QByteArray> CurlWorker::shutdownAndWait()
{
    if (!isRunning())
        return m_final;
    m_quit = true;
    curl_multi_wakeup(m_multi);
    wait();
    return m_final;
}

void CurlWorker::unpauseSome()
{
    std::vector<Segment*> paused;
    for (auto& [id, t] : m_tasks) {
        if (t->taskThrottled())
            continue; // this download is over its own cap; its turn comes after the next refill
        for (auto& s : t->segments())
            if (s->h && s->paused)
                paused.push_back(s.get());
    }
    const size_t n = paused.size();
    for (size_t i = 0; i < n && !throttled(); ++i) {
        Segment* s = paused[(m_rr + i) % n]; // round-robin for fairness
        if (s->task->taskThrottled())
            continue;
        s->paused = false;
        curl_easy_pause(s->h, CURLPAUSE_CONT);
    }
    ++m_rr;
}

void CurlWorker::run()
{
    qint64 lastRefill = nowMs();
    qint64 lastStats = lastRefill;
    qint64 lastResume = lastRefill;

    while (true) {
        std::deque<std::function<void()>> cmds;
        {
            QMutexLocker l(&m_mx);
            cmds.swap(m_cmds);
        }
        for (auto& c : cmds)
            c();
        if (m_quit)
            break;

        const qint64 now = nowMs();
        for (auto& [id, t] : m_tasks)
            t->refillBucket(now - lastRefill);
        if (m_limit > 0) {
            m_bucket += double(m_limit) * double(now - lastRefill) / 1000.0;
            m_bucket = std::min(m_bucket, std::max(double(m_limit) / 4.0, 16384.0));
            unpauseSome();
        } else {
            unpauseSome(); // releases anything paused before the limit was lifted
        }
        lastRefill = now;

        int running = 0;
        curl_multi_perform(m_multi, &running);

        int left = 0;
        while (CURLMsg* msg = curl_multi_info_read(m_multi, &left)) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            char* priv = nullptr;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
            auto* s = reinterpret_cast<Segment*>(priv);
            const CURLcode rc = msg->data.result;
            if (s && s->task && !s->task->isDead())
                s->task->onDone(s, rc, now);
        }

        for (auto& [id, t] : m_tasks)
            t->tick(now);

        if (now - lastStats >= 500) {
            const double dt = double(now - lastStats) / 1000.0;
            for (auto& [id, t] : m_tasks)
                if (!t->isDead())
                    emit stats(t->stats(dt));
            lastStats = now;
        }
        if (now - lastResume >= 2000) {
            for (auto& [id, t] : m_tasks)
                if (!t->isDead() && t->takeDirty())
                    emit resumeData(id, t->resumeData());
            lastResume = now;
        }

        for (auto it = m_tasks.begin(); it != m_tasks.end();)
            it = it->second->isDead() ? m_tasks.erase(it) : std::next(it);

        const bool limited = m_limit > 0 || std::any_of(m_tasks.begin(), m_tasks.end(), [](const auto& kv) { return kv.second->hasSpeedLimit(); });
        curl_multi_poll(m_multi, nullptr, 0, limited ? 20 : 100, nullptr);
    }

    for (auto& [id, t] : m_tasks) {
        t->stop();
        m_final.insert(id, t->resumeData());
    }
    m_tasks.clear();
}
