#pragma once
#include <QDateTime>
#include <QDir>
#include <QObject>
#include <QStringList>
#include <QVariantList>

#include "core/DownloadRecord.h"
#include "core/Types.h"

// A download as seen by the UI: persistent record + live statistics.
class Download : public QObject {
    Q_OBJECT
    Q_PROPERTY(qint64 id READ id CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)
    Q_PROPERTY(QString referrer READ referrer NOTIFY changed)
    Q_PROPERTY(QString sourceUrl READ sourceUrl NOTIFY changed) // what to show as "Address"
    Q_PROPERTY(QString filePath READ filePath NOTIFY changed)
    Q_PROPERTY(QString destDir READ destDir NOTIFY changed)
    Q_PROPERTY(qint64 total READ total NOTIFY changed)
    Q_PROPERTY(qint64 received READ received NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(double speed READ speed NOTIFY changed)
    Q_PROPERTY(qint64 eta READ eta NOTIFY changed)
    Q_PROPERTY(int connections READ connections NOTIFY changed)
    Q_PROPERTY(int state READ state NOTIFY changed)
    Q_PROPERTY(QString stateText READ stateText NOTIFY changed)
    Q_PROPERTY(bool queued READ queued NOTIFY changed)
    Q_PROPERTY(bool starting READ starting NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(bool failed READ failed NOTIFY changed)
    Q_PROPERTY(bool completed READ completed NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(int resumable READ resumable NOTIFY changed)
    Q_PROPERTY(QString category READ category NOTIFY categoryChanged)
    Q_PROPERTY(QDateTime created READ created CONSTANT)
    Q_PROPERTY(QDateTime finishedAt READ finishedAt NOTIFY changed)
    Q_PROPERTY(QVariantList segments READ segments NOTIFY changed)
    Q_PROPERTY(QStringList log READ log NOTIFY logChanged)
    Q_PROPERTY(int priority READ priority NOTIFY changed)
    // scheduling
    Q_PROPERTY(bool scheduled READ scheduled NOTIFY changed)
    Q_PROPERTY(QString scheduleText READ scheduleText NOTIFY changed)
    Q_PROPERTY(bool waitingForSchedule READ waitingForSchedule NOTIFY changed)
    // BitTorrent
    Q_PROPERTY(bool isTorrent READ isTorrent CONSTANT)
    Q_PROPERTY(bool seeding READ seeding NOTIFY changed)
    Q_PROPERTY(double uploadSpeed READ uploadSpeed NOTIFY changed)
    Q_PROPERTY(qint64 uploaded READ uploaded NOTIFY changed)
    Q_PROPERTY(double ratio READ ratio NOTIFY changed)
    Q_PROPERTY(int seeds READ seeds NOTIFY changed)
    Q_PROPERTY(int peers READ peers NOTIFY changed)
    Q_PROPERTY(QVariantList files READ files NOTIFY filesChanged)
    Q_PROPERTY(qint64 retryIn READ retryIn NOTIFY changed) // seconds until the automatic retry, -1 = none
    Q_PROPERTY(int autoRetries READ autoRetries NOTIFY changed)
    Q_PROPERTY(bool fileMissing READ fileMissing NOTIFY changed)
    Q_PROPERTY(int speedLimit READ speedLimit NOTIFY changed) // KB/s, 0 = none
    Q_PROPERTY(bool moving READ moving NOTIFY changed)
    Q_PROPERTY(bool converting READ converting NOTIFY changed)
    Q_PROPERTY(int hashStatus READ hashStatus NOTIFY changed)        // HashStatus
    Q_PROPERTY(QString expectedHash READ expectedHash NOTIFY changed)
    Q_PROPERTY(QString hashText READ hashText NOTIFY changed)        // "SHA-256 matches" / what was computed
    Q_PROPERTY(double convertProgress READ convertProgress NOTIFY changed) // 0..1, -1 unknown
    Q_PROPERTY(bool needsLogin READ needsLogin NOTIFY changed) // failed asking for a user name/password
    Q_PROPERTY(bool awaitingFiles READ awaitingFiles NOTIFY changed) // torrent: waiting for the user to pick files
    Q_PROPERTY(QVariantList pendingFiles READ pendingFiles NOTIFY changed) // the files to pick from
    Q_PROPERTY(bool sequential READ sequential NOTIFY changed)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY tagsChanged) // user tag ids
    Q_PROPERTY(qint64 queueOrder READ queueOrder NOTIFY changed)
    Q_PROPERTY(bool certificateProblem READ certificateProblem NOTIFY changed) // stopped: untrusted TLS certificate
    Q_PROPERTY(bool fileExists READ fileExists NOTIFY changed) // stopped: "ask" and the file is already there

public:
    enum State { Queued = 0, Starting, Downloading, Paused, Error, Completed };
    Q_ENUM(State)

    explicit Download(const DownloadRecord& rec, QObject* parent = nullptr);

    DownloadRecord rec;
    int pendingStops = 0; // engine stop requests not yet acknowledged
    int autoRetryCount = 0;   // automatic retries used since the last success/manual action
    QDateTime autoRetryAt;    // when the next automatic retry happens (invalid = none)

    qint64 id() const { return rec.id; }
    QString title() const { return rec.title; }
    QString url() const { return rec.url; }
    QString referrer() const { return rec.referrer; }
    // A torrent started from a downloaded .torrent file has a local path as its URL;
    // the web link it came from (kept as referrer) is more useful to show.
    QString sourceUrl() const
    {
        const bool local = !rec.url.contains(QLatin1String("://")) && !rec.url.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive);
        return local && !rec.referrer.isEmpty() ? rec.referrer : (local ? QDir::toNativeSeparators(rec.url) : rec.url);
    }
    QString filePath() const { return QDir::toNativeSeparators(rec.filePath); }
    QString destDir() const { return QDir::toNativeSeparators(rec.destDir); }
    qint64 total() const { return rec.total; }
    qint64 received() const { return rec.received; }
    double progress() const;
    double speed() const { return m_speed; }
    qint64 eta() const;
    int connections() const { return m_connections; }
    int state() const { return rec.state; }
    QString stateText() const;
    bool queued() const { return rec.state == Queued; }
    bool starting() const { return rec.state == Starting; }
    bool running() const { return rec.state == Starting || rec.state == Downloading; }
    bool paused() const { return rec.state == Paused; }
    bool failed() const { return rec.state == Error; }
    bool completed() const { return rec.state == Completed; }
    QString error() const { return rec.lastError; }
    int resumable() const { return rec.resumable; }
    QString category() const;
    QDateTime created() const { return rec.created; }
    QDateTime finishedAt() const { return rec.finished; }
    QVariantList segments() const;
    QStringList log() const { return m_log; }
    int priority() const { return rec.priority; }

    bool scheduled() const { return rec.scheduled; }
    QString scheduleText() const { return rec.scheduled ? rec.schedule.text() : QString(); }
    bool inScheduleWindow(const QDateTime& now) const { return !rec.scheduled || rec.schedule.contains(now); }
    bool waitingForSchedule() const { return queued() && !inScheduleWindow(QDateTime::currentDateTime()); }

    bool isTorrent() const { return rec.moduleUid == QLatin1String("downloadsbt"); }
    bool seeding() const { return m_seeding; }
    double uploadSpeed() const { return m_uploadSpeed; }
    qint64 uploaded() const { return m_uploaded; }
    double ratio() const;
    int seeds() const { return m_seeds; }
    int peers() const { return m_peers; }
    QVariantList files() const { return m_files; }
    qint64 retryIn() const { return failed() && autoRetryAt.isValid() ? qMax<qint64>(0, QDateTime::currentDateTime().secsTo(autoRetryAt)) : -1; }
    int autoRetries() const { return autoRetryCount; }
    bool fileMissing() const { return m_missing; }
    int speedLimit() const { return rec.speedLimitKBps; }
    bool moving() const { return m_moving; }
    bool converting() const { return m_converting; }
    bool awaitingFiles() const { return !m_pendingFiles.isEmpty(); }
    QVariantList pendingFiles() const { return m_pendingFiles; }
    void setPendingFiles(const QVariantList& files)
    {
        if (files.isEmpty() && m_pendingFiles.isEmpty())
            return;
        m_pendingFiles = files;
        emit changed();
        emit stateChanged(); // lets the UI ask right away
    }
    bool sequential() const { return rec.sequential; }
    QVariantList tags() const
    {
        QVariantList l;
        for (int t : rec.tags)
            l << t;
        return l;
    }
    bool hasTag(int id) const { return rec.tags.contains(id); }
    qint64 queueOrder() const { return rec.queueOrder ? rec.queueOrder : rec.id; }
    void setTags(const QList<int>& ids)
    {
        if (ids == rec.tags)
            return;
        rec.tags = ids;
        emit changed();
        emit tagsChanged();
    }
    bool certificateProblem() const { return failed() && rec.lastError.startsWith(QLatin1String("Untrusted certificate")); }
    bool fileExists() const { return failed() && rec.lastError.startsWith(QLatin1String("File already exists")); }
    bool needsLogin() const
    {
        return failed() && (rec.lastError.startsWith(QLatin1String("HTTP error 401")) || rec.lastError.startsWith(QLatin1String("Login ")));
    }
    enum HashStatus { HashNone = 0, HashChecking = 1, HashMatch = 2, HashMismatch = 3, HashFailed = 4 };
    Q_ENUM(HashStatus)
    int hashStatus() const { return rec.hashStatus; }
    QString expectedHash() const { return rec.expectedHash; }
    QString hashText() const // the details are kept for this session; after a restart only the verdict
    {
        if (!m_hashText.isEmpty())
            return m_hashText;
        return rec.hashStatus == HashMatch ? tr("Checksum matches") : rec.hashStatus == HashMismatch ? tr("Checksum does not match") : QString();
    }
    void setHashResult(int status, const QString& text) { rec.hashStatus = status; m_hashText = text; emit changed(); }
    void setConverting(bool c) { if (m_converting != c) { m_converting = c; m_convertProgress = -1; emit changed(); } }
    double convertProgress() const { return m_convertProgress; }
    void setConvertProgress(double p) { m_convertProgress = p; emit changed(); }
    void setMoving(bool m) { if (m_moving != m) { m_moving = m; emit changed(); } }
    void setFileMissing(bool m) { if (m_missing != m) { m_missing = m; emit changed(); } }

    void setState(State s, const QString& error = {});
    void setTitle(const QString& title);
    void applyStats(const TaskStats& st);
    void setSegmentsFromResumeData();
    void clearLive();
    void setSeeding(bool on);
    void addLog(const QString& line);
    void notify() { emit changed(); }

signals:
    void changed();
    void logChanged();
    void stateChanged();
    void titleChanged();
    void filesChanged();
    void categoryChanged(); // a multi-file download's content category became known
    void tagsChanged();

private:
    double m_speed = 0;
    int m_connections = 0;
    QList<qint64> m_segs; // triples begin,end,done
    QStringList m_log;
    bool m_seeding = false;
    double m_uploadSpeed = 0;
    qint64 m_uploaded = 0;
    int m_seeds = 0;
    int m_peers = 0;
    QVariantList m_files;
    bool m_missing = false;
    bool m_moving = false;
    bool m_converting = false;
    QVariantList m_pendingFiles;
    double m_convertProgress = -1;
    QString m_hashText;
};
