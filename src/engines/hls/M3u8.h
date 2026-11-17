#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

// Minimal HLS (RFC 8216) playlist model: enough for VOD downloading.
struct M3u8Variant {
    QUrl url;
    qint64 bandwidth = 0;
    QString resolution; // "1920x1080"
};

struct M3u8Key {
    QString method;     // "NONE", "AES-128", "SAMPLE-AES"
    QUrl uri;
    QByteArray iv;      // 16 bytes when given explicitly, else empty
};

struct M3u8Segment {
    QUrl url;
    double duration = 0;
    qint64 sequence = 0; // media sequence number (default IV for AES-128)
    int keyIndex = -1;   // into M3u8Playlist::keys, -1 = clear
};

struct M3u8Playlist {
    bool valid = false;
    bool master = false;
    QList<M3u8Variant> variants;
    QList<M3u8Segment> segments;
    QList<M3u8Key> keys;
    QUrl initSection;    // #EXT-X-MAP (fMP4)
    bool endList = false;
    double totalDuration = 0;

    // Highest-bandwidth variant (FDM picks the best quality by default).
    const M3u8Variant* best() const;
};

namespace M3u8 {
M3u8Playlist parse(const QByteArray& text, const QUrl& base);
// AES-128 IV derived from the media sequence number when no IV attribute is present.
QByteArray sequenceIv(qint64 sequence);
} // namespace M3u8
