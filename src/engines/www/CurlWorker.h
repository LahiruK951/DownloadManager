#pragma once
#include <curl/curl.h>

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QThread>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>

#include "core/Types.h"

class HttpTask;

// Single thread driving all HTTP/FTP transfers through one curl_multi handle.
// Public methods are thread-safe; they enqueue commands for the worker loop.
class CurlWorker : public QThread {
    Q_OBJECT
public:
    explicit CurlWorker(QObject* parent = nullptr);
    ~CurlWorker() override;

    void startTask(const TaskParams& params);
    void stopTask(qint64 id);
    void setSpeedLimit(qint64 bytesPerSec);
    void setMaxConnections(int perDownload);
    // Across all downloads to one host (FDM: MaxConnectionsPerServer); 0 = no limit.
    void setMaxConnectionsPerServer(int n);
    void setTaskSpeedLimit(qint64 id, qint64 bytesPerSec);
    QHash<qint64, QByteArray> shutdownAndWait();

    // --- worker-thread only (used by HttpTask)
    CURLM* multi() const { return m_multi; }
    bool throttled() const { return m_limit > 0 && m_bucket <= 0; }
    void consume(qint64 n) { if (m_limit > 0) m_bucket -= double(n); }

signals:
    void meta(qint64 id, const QString& fileName, const QString& filePath, qint64 total, bool resumable);
    void stats(const TaskStats& stats);
    void resumeData(qint64 id, const QByteArray& data);
    void finished(qint64 id, const QString& filePath);
    void failed(qint64 id, const QString& error, const QByteArray& resumeData);
    void stopped(qint64 id, const QByteArray& resumeData);
    void log(qint64 id, const QString& line);

protected:
    void run() override;

private:
    void post(std::function<void()> fn);
    void unpauseSome();
    qint64 nowMs() const { return m_clock.elapsed(); }

    CURLM* m_multi = nullptr;
    QMutex m_mx;
    std::deque<std::function<void()>> m_cmds;
    std::atomic_bool m_quit{false};

    std::map<qint64, std::unique_ptr<HttpTask>> m_tasks;
    qint64 m_limit = 0;   // bytes/s, 0 = unlimited
    double m_bucket = 0;  // token bucket
    int m_maxConn = 8;
    int m_perServer = 0;
    int connectionCap() const { return m_perServer > 0 ? qMin(m_maxConn, m_perServer) : m_maxConn; }
    size_t m_rr = 0;
    QElapsedTimer m_clock;
    QHash<qint64, QByteArray> m_final;
};
