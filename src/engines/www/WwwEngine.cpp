#include "engines/www/WwwEngine.h"

#include "engines/www/CurlWorker.h"

WwwEngine::WwwEngine(QObject* parent)
    : DownloadEngine(parent), m_worker(new CurlWorker(this))
{
    connect(m_worker, &CurlWorker::meta, this, &DownloadEngine::meta);
    connect(m_worker, &CurlWorker::stats, this, &DownloadEngine::stats);
    connect(m_worker, &CurlWorker::resumeData, this, &DownloadEngine::resumeData);
    connect(m_worker, &CurlWorker::finished, this, &DownloadEngine::finished);
    connect(m_worker, &CurlWorker::failed, this, &DownloadEngine::failed);
    connect(m_worker, &CurlWorker::stopped, this, &DownloadEngine::stopped);
    connect(m_worker, &CurlWorker::log, this, &DownloadEngine::log);
    m_worker->start();
}

WwwEngine::~WwwEngine()
{
    m_worker->shutdownAndWait();
}

bool WwwEngine::canHandle(const QUrl& url) const
{
    const QString s = url.scheme().toLower();
    return url.isValid() && !url.host().isEmpty()
        && (s == QLatin1String("http") || s == QLatin1String("https") || s == QLatin1String("ftp") || s == QLatin1String("ftps"));
}

void WwwEngine::start(const TaskParams& params) { m_worker->startTask(params); }
void WwwEngine::stop(qint64 id) { m_worker->stopTask(id); }
void WwwEngine::setSpeedLimit(qint64 bytesPerSec) { m_worker->setSpeedLimit(bytesPerSec); }
void WwwEngine::setMaxConnections(int perDownload) { m_worker->setMaxConnections(perDownload); }
void WwwEngine::setTaskSpeedLimit(qint64 id, qint64 bytesPerSec) { m_worker->setTaskSpeedLimit(id, bytesPerSec); }
QHash<qint64, QByteArray> WwwEngine::shutdown() { return m_worker->shutdownAndWait(); }
