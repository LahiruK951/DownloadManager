#pragma once
#include <QList>
#include <QString>
#include <QUrl>

// Minimal MPEG-DASH (ISO/IEC 23009-1) manifest model: enough to download on-demand videos.
struct MpdSegment {
    QUrl url;
    qint64 rangeStart = -1; // byte range inside `url` (SegmentList@mediaRange, SegmentBase); -1 = whole
    qint64 rangeEnd = -1;   // inclusive
};

struct MpdRepresentation {
    QString id;
    QString kind;      // "video" | "audio" | "" (text, ...)
    QString mimeType;  // "video/mp4", "audio/webm", ...
    QString codecs;
    qint64 bandwidth = 0;
    int width = 0, height = 0;
    QString lang;
    MpdSegment init;   // url empty when the media segments are self-initialising
    QList<MpdSegment> segments;
};

struct MpdManifest {
    bool valid = false;
    QString error;      // why it can't be downloaded (live, DRM, unsupported addressing)
    double duration = 0; // seconds
    QList<MpdRepresentation> representations;

    // Highest bandwidth of a kind (FDM takes the best quality by default).
    const MpdRepresentation* best(const QString& kind) const;
    const MpdRepresentation* byId(const QString& id) const;
};

namespace Mpd {
MpdManifest parse(const QByteArray& xml, const QUrl& manifestUrl);
double parseDuration(const QString& iso8601); // "PT1H2M3.5S" -> 3723.5, -1 if invalid
// $RepresentationID$, $Number%05d$, $Time$, $Bandwidth$, $$
QString expandTemplate(const QString& tpl, const QString& repId, qint64 number, qint64 time, qint64 bandwidth);
} // namespace Mpd
