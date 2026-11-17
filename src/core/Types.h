#pragma once
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QVariantList>

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
    QByteArray resumeData; // engine-specific opaque blob
    int maxConnections = 8;
    bool seedOnly = false; // BitTorrent: data is complete, just seed
};

// Periodic progress snapshot posted from an engine to the GUI thread.
struct TaskStats {
    qint64 id = 0;
    qint64 total = -1;
    qint64 received = 0;
    double speed = 0;       // bytes/s (smoothed)
    int connections = 0;
    QList<qint64> segments; // flattened triples: begin, end(inclusive), done
    // BitTorrent extras
    double uploadSpeed = 0;
    qint64 uploaded = 0;
    int seeds = 0;
    int peers = 0;
    bool seeding = false;
    QVariantList files;     // [{name, size, done, wanted}] (only when it changed)
};

Q_DECLARE_METATYPE(TaskParams)
Q_DECLARE_METATYPE(TaskStats)
