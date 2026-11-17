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
    Q_PROPERTY(QString category READ category NOTIFY changed)
    Q_PROPERTY(QDateTime created READ created CONSTANT)
    Q_PROPERTY(QDateTime finishedAt READ finishedAt NOTIFY changed)
    Q_PROPERTY(QVariantList segments READ segments NOTIFY changed)
    Q_PROPERTY(QStringList log READ log NOTIFY logChanged)
    Q_PROPERTY(int priority READ priority NOTIFY changed)

public:
    enum State { Queued = 0, Starting, Downloading, Paused, Error, Completed };
    Q_ENUM(State)

    explicit Download(const DownloadRecord& rec, QObject* parent = nullptr);

    DownloadRecord rec;
    int pendingStops = 0; // engine stop requests not yet acknowledged

    qint64 id() const { return rec.id; }
    QString title() const { return rec.title; }
    QString url() const { return rec.url; }
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

    void setState(State s, const QString& error = {});
    void setTitle(const QString& title);
    void applyStats(const TaskStats& st);
    void setSegmentsFromResumeData();
    void clearLive();
    void addLog(const QString& line);
    void notify() { emit changed(); }

signals:
    void changed();
    void logChanged();
    void stateChanged();
    void titleChanged();

private:
    double m_speed = 0;
    int m_connections = 0;
    QList<qint64> m_segs; // triples begin,end,done
    QStringList m_log;
};
