#pragma once
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantList>

// A value that goes into one request header (referrer, cookies, user agent) or a yt-dlp header
// argument. These come from the browser extension and the remote page; a CR or LF in them would
// start a new header, so control characters are dropped.
inline QString headerSafe(const QString& v)
{
    QString out;
    out.reserve(v.size());
    for (const QChar c : v)
        if (c.unicode() >= 0x20 && c.unicode() != 0x7f)
            out += c;
    return out.trimmed();
}

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
    QString user;          // server login (HTTP auth / FTP); empty = none or in the URL
    QString password;
    QString ffmpeg;        // for engines that combine tracks (DASH); empty = not available
    QStringList mirrors;   // other addresses of the same file (spread connections, fall back)
    QByteArray postData;   // send as a POST (a form the browser submitted), empty = GET
    bool trustCertificate = false; // the user accepted this server's untrusted certificate
    QStringList siteTool;  // yt-dlp command (program + prefix args) for site downloads; empty = missing
    int existingFile = 0;  // FileNamer::ExistingFile: keep both / overwrite / ask
    QByteArray resumeData; // engine-specific opaque blob
    int maxConnections = 8;
    bool seedOnly = false; // BitTorrent: data is complete, just seed
    bool pickFiles = false;  // BitTorrent: wait for the user to choose files once they are known
    bool sequential = false; // BitTorrent: download pieces in order (playable while downloading)
    qint64 speedLimit = 0; // per-download cap in bytes/s, 0 = none
    int ipVersion = 0;     // 0 any, 4 IPv4 only, 6 IPv6 only
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
