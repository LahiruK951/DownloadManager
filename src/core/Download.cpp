#include "core/Download.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTime>

#include "core/FileNamer.h"

Download::Download(const DownloadRecord& r, QObject* parent)
    : QObject(parent), rec(r)
{
    setSegmentsFromResumeData();
}

double Download::progress() const
{
    if (rec.state == Completed)
        return 1.0;
    if (rec.total <= 0)
        return -1.0;
    return qBound(0.0, double(rec.received) / double(rec.total), 1.0);
}

qint64 Download::eta() const
{
    if (!running() || m_speed < 1 || rec.total <= 0)
        return -1;
    return qint64(double(rec.total - rec.received) / m_speed);
}

QString Download::stateText() const
{
    switch (rec.state) {
    case Queued: return waitingForSchedule() ? tr("Scheduled") : tr("Queued");
    case Starting: return tr("Connecting");
    case Downloading: return tr("Downloading");
    case Paused: return tr("Paused");
    case Error: return tr("Error");
    case Completed: return tr("Completed");
    }
    return {};
}

QString Download::category() const
{
    return rec.contentCategory.isEmpty() ? FileNamer::category(rec.title) : rec.contentCategory;
}

QVariantList Download::segments() const
{
    QVariantList out;
    if (rec.state == Completed) {
        out.append(QVariant(QVariantList{0.0, 1.0}));
        return out;
    }
    if (rec.total <= 0)
        return out;
    const double t = double(rec.total);
    for (qsizetype i = 0; i + 2 < m_segs.size(); i += 3) {
        const qint64 b = m_segs[i], d = m_segs[i + 2];
        if (d > 0)
            out.append(QVariant(QVariantList{double(b) / t, double(b + d) / t}));
    }
    return out;
}

void Download::setState(State s, const QString& error)
{
    const bool changedState = rec.state != s;
    rec.state = s;
    if (s == Error)
        rec.lastError = error;
    else if (s != Completed)
        rec.lastError.clear();
    if (!running())
        clearLive();
    emit changed();
    if (changedState)
        emit stateChanged();
}

void Download::setTitle(const QString& title)
{
    if (rec.title == title)
        return;
    rec.title = title;
    emit changed();
    emit titleChanged();
    emit categoryChanged();
}

double Download::ratio() const
{
    const qint64 base = rec.total > 0 ? rec.total : rec.received;
    return base > 0 ? double(m_uploaded) / double(base) : 0.0;
}

void Download::setSeeding(bool on)
{
    if (m_seeding == on)
        return;
    m_seeding = on;
    if (!on) {
        m_uploadSpeed = 0;
        m_peers = m_seeds = 0;
    }
    emit changed();
}

void Download::applyStats(const TaskStats& st)
{
    if (!st.files.isEmpty()) {
        m_files = st.files;
        emit filesChanged();
        const QString cat = m_files.size() > 1 ? FileNamer::categoryOfFiles(m_files) : QString();
        if (cat != rec.contentCategory) {
            rec.contentCategory = cat;
            emit changed();
            emit categoryChanged();
        }
    }
    m_uploadSpeed = st.uploadSpeed;
    m_uploaded = qMax(m_uploaded, st.uploaded);
    m_seeds = st.seeds;
    m_peers = st.peers;
    if (completed()) { // still seeding after completion
        m_seeding = st.seeding;
        emit changed();
        return;
    }
    if (!running())
        return;
    if (st.total >= 0)
        rec.total = st.total;
    rec.received = st.received;
    m_speed = st.speed;
    m_connections = st.connections;
    m_segs = st.segments;
    if (rec.state == Starting && st.received > 0) {
        rec.state = Downloading;
        emit stateChanged();
    }
    emit changed();
}

void Download::setSegmentsFromResumeData()
{
    m_segs.clear();
    const QJsonObject o = QJsonDocument::fromJson(rec.resumeData).object();
    for (const auto& v : o.value(QStringLiteral("segments")).toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() == 3)
            m_segs << a[0].toInteger() << a[1].toInteger() << a[2].toInteger();
    }
}

void Download::clearLive()
{
    m_speed = 0;
    m_connections = 0;
}

void Download::addLog(const QString& line)
{
    m_log.append(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")) + QStringLiteral("  ") + line);
    if (m_log.size() > 500)
        m_log.removeFirst();
    emit logChanged();
}
