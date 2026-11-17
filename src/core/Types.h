#pragma once
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>

// Parameters handed from the download manager (GUI thread) to an engine.
struct TaskParams {
    qint64 id = 0;
    QString url;
    QString destDir;
    QString fileName;      // user-preferred name; empty = derive from server/URL
    QString referrer;
    QString cookies;
    QString userAgent;
    QString proxy;         // "http://host:port" or empty
    QByteArray resumeData; // engine-specific opaque blob (JSON for downloadswww)
    int maxConnections = 8;
};

// Periodic progress snapshot posted from an engine to the GUI thread.
struct TaskStats {
    qint64 id = 0;
    qint64 total = -1;
    qint64 received = 0;
    double speed = 0;       // bytes/s (smoothed)
    int connections = 0;
    QList<qint64> segments; // flattened triples: begin, end(inclusive), done
};

Q_DECLARE_METATYPE(TaskParams)
Q_DECLARE_METATYPE(TaskStats)
