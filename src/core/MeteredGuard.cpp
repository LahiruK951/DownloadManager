#include "core/MeteredGuard.h"

#include <QNetworkInformation>

#include "core/Download.h"
#include "core/DownloadManager.h"

MeteredGuard::MeteredGuard(DownloadManager* mgr, QObject* parent)
    : QObject(parent), m_mgr(mgr)
{
}

static QList<Download*> activeDownloads(DownloadManager* mgr)
{
    QList<Download*> out;
    for (Download* d : mgr->downloads())
        if (d->running() || d->queued())
            out << d;
    return out;
}

void MeteredGuard::setMetered(bool metered)
{
    if (metered == m_metered)
        return;
    m_metered = metered;
    if (!metered) {
        m_allowed = false;
        int n = 0;
        for (qint64 id : std::as_const(m_pausedByUs))
            if (Download* d = m_mgr->find(id); d && d->paused()) {
                m_mgr->resume(d);
                ++n;
            }
        m_pausedByUs.clear();
        if (n)
            emit resumed(n);
        return;
    }
    if (m_policy == Ignore || m_allowed)
        return;
    const int active = int(activeDownloads(m_mgr).size());
    if (active == 0)
        return;
    if (m_policy == Pause)
        pauseNow();
    else
        emit askUser(active);
}

void MeteredGuard::pauseNow()
{
    int n = 0;
    for (Download* d : activeDownloads(m_mgr)) {
        m_mgr->pause(d);
        if (!m_pausedByUs.contains(d->id()))
            m_pausedByUs << d->id();
        ++n;
    }
    if (n)
        emit paused(n);
}

void MeteredGuard::keepDownloading() { m_allowed = true; }

bool MeteredGuard::watchSystem(MeteredGuard* guard)
{
    if (!QNetworkInformation::loadBackendByFeatures(QNetworkInformation::Feature::Metered))
        return false;
    QNetworkInformation* info = QNetworkInformation::instance();
    QObject::connect(info, &QNetworkInformation::isMeteredChanged, guard, &MeteredGuard::setMetered);
    guard->setMetered(info->isMetered());
    return true;
}
