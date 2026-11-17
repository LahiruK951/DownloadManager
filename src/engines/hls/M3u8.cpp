#include "engines/hls/M3u8.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

namespace {
// Parses an attribute list: KEY=VALUE,KEY="quoted, value",...
QHash<QString, QString> attributes(const QString& s)
{
    QHash<QString, QString> out;
    int i = 0;
    const int n = int(s.size());
    while (i < n) {
        const int eq = int(s.indexOf(QLatin1Char('='), i));
        if (eq < 0)
            break;
        const QString key = s.mid(i, eq - i).trimmed().toUpper();
        int j = eq + 1;
        QString value;
        if (j < n && s[j] == QLatin1Char('"')) {
            const int close = int(s.indexOf(QLatin1Char('"'), j + 1));
            value = s.mid(j + 1, (close < 0 ? n : close) - j - 1);
            j = close < 0 ? n : close + 1;
        } else {
            const int comma = int(s.indexOf(QLatin1Char(','), j));
            value = s.mid(j, (comma < 0 ? n : comma) - j).trimmed();
            j = comma < 0 ? n : comma;
        }
        out.insert(key, value);
        i = j;
        while (i < n && (s[i] == QLatin1Char(',') || s[i] == QLatin1Char(' ')))
            ++i;
    }
    return out;
}

QByteArray hexIv(QString v)
{
    if (v.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        v = v.mid(2);
    QByteArray iv = QByteArray::fromHex(v.toLatin1());
    if (iv.size() > 16)
        iv = iv.right(16);
    while (iv.size() < 16)
        iv.prepend('\0');
    return iv;
}
} // namespace

const M3u8Variant* M3u8Playlist::best() const
{
    const M3u8Variant* b = nullptr;
    for (const M3u8Variant& v : variants)
        if (!b || v.bandwidth > b->bandwidth)
            b = &v;
    return b;
}

QByteArray M3u8::sequenceIv(qint64 sequence)
{
    QByteArray iv(16, '\0');
    for (int i = 0; i < 8; ++i)
        iv[15 - i] = char((quint64(sequence) >> (8 * i)) & 0xff);
    return iv;
}

M3u8Playlist M3u8::parse(const QByteArray& text, const QUrl& base)
{
    M3u8Playlist pl;
    const QStringList lines = QString::fromUtf8(text).split(QRegularExpression(QStringLiteral("\r?\n")));
    if (lines.isEmpty() || !lines.first().trimmed().startsWith(QLatin1String("#EXTM3U")))
        return pl;
    pl.valid = true;

    qint64 sequence = 0;
    double pendingDuration = -1;
    int currentKey = -1;
    M3u8Variant pendingVariant;
    bool expectVariantUri = false;

    for (int li = 1; li < lines.size(); ++li) {
        const QString line = lines[li].trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF:"))) {
            const auto a = attributes(line.mid(18));
            pendingVariant = M3u8Variant{};
            pendingVariant.bandwidth = a.value(QStringLiteral("BANDWIDTH")).toLongLong();
            pendingVariant.resolution = a.value(QStringLiteral("RESOLUTION"));
            expectVariantUri = true;
            pl.master = true;
        } else if (line.startsWith(QLatin1String("#EXT-X-MEDIA-SEQUENCE:"))) {
            sequence = line.mid(22).trimmed().toLongLong();
        } else if (line.startsWith(QLatin1String("#EXTINF:"))) {
            const QString v = line.mid(8);
            pendingDuration = v.left(v.indexOf(QLatin1Char(','))).toDouble();
        } else if (line.startsWith(QLatin1String("#EXT-X-KEY:"))) {
            const auto a = attributes(line.mid(11));
            M3u8Key key;
            key.method = a.value(QStringLiteral("METHOD")).toUpper();
            if (key.method == QLatin1String("NONE") || key.method.isEmpty()) {
                currentKey = -1;
                continue;
            }
            key.uri = base.resolved(QUrl(a.value(QStringLiteral("URI"))));
            if (a.contains(QStringLiteral("IV")))
                key.iv = hexIv(a.value(QStringLiteral("IV")));
            pl.keys.append(key);
            currentKey = int(pl.keys.size()) - 1;
        } else if (line.startsWith(QLatin1String("#EXT-X-MAP:"))) {
            pl.initSection = base.resolved(QUrl(attributes(line.mid(11)).value(QStringLiteral("URI"))));
        } else if (line.startsWith(QLatin1String("#EXT-X-ENDLIST"))) {
            pl.endList = true;
        } else if (!line.startsWith(QLatin1Char('#'))) { // a URI line
            const QUrl url = base.resolved(QUrl(line));
            if (expectVariantUri) {
                pendingVariant.url = url;
                pl.variants.append(pendingVariant);
                expectVariantUri = false;
            } else {
                M3u8Segment seg;
                seg.url = url;
                seg.duration = qMax(0.0, pendingDuration);
                seg.sequence = sequence++;
                seg.keyIndex = currentKey;
                pl.totalDuration += seg.duration;
                pl.segments.append(seg);
                pendingDuration = -1;
            }
        }
    }
    return pl;
}
