#include "engines/dash/Mpd.h"

#include <QRegularExpression>
#include <QXmlStreamReader>
#include <array>
#include <cmath>

const MpdRepresentation* MpdManifest::best(const QString& kind) const
{
    const MpdRepresentation* b = nullptr;
    for (const MpdRepresentation& r : representations)
        if (r.kind == kind && (!b || r.bandwidth > b->bandwidth))
            b = &r;
    return b;
}

const MpdRepresentation* MpdManifest::byId(const QString& id) const
{
    for (const MpdRepresentation& r : representations)
        if (r.id == id)
            return &r;
    return nullptr;
}

namespace Mpd {

double parseDuration(const QString& s)
{
    static const QRegularExpression re(QStringLiteral(R"(^P(?:(\d+(?:\.\d+)?)D)?(?:T(?:(\d+(?:\.\d+)?)H)?(?:(\d+(?:\.\d+)?)M)?(?:(\d+(?:\.\d+)?)S)?)?$)"));
    const auto m = re.match(s.trimmed());
    if (!m.hasMatch() || s.trimmed() == QLatin1String("P") || s.trimmed() == QLatin1String("PT"))
        return -1;
    return m.captured(1).toDouble() * 86400 + m.captured(2).toDouble() * 3600 + m.captured(3).toDouble() * 60
         + m.captured(4).toDouble();
}

QString expandTemplate(const QString& tpl, const QString& repId, qint64 number, qint64 time, qint64 bandwidth)
{
    static const QRegularExpression id(QStringLiteral(R"(\$(RepresentationID|Number|Time|Bandwidth)(?:%0(\d+)d)?\$)"));
    QString out;
    int pos = 0;
    auto it = id.globalMatch(tpl);
    while (it.hasNext()) {
        const auto m = it.next();
        out += QString(tpl.mid(pos, m.capturedStart() - pos)).replace(QLatin1String("$$"), QLatin1String("$"));
        const QString name = m.captured(1);
        const int width = m.captured(2).toInt();
        if (name == QLatin1String("RepresentationID"))
            out += repId;
        else {
            const qint64 v = name == QLatin1String("Number") ? number : name == QLatin1String("Time") ? time : bandwidth;
            out += QStringLiteral("%1").arg(v, width, 10, QLatin1Char('0'));
        }
        pos = int(m.capturedEnd());
    }
    out += QString(tpl.mid(pos)).replace(QLatin1String("$$"), QLatin1String("$"));
    return out;
}

namespace {

// Attributes that are inherited from AdaptationSet down to Representation.
struct Tmpl {
    bool present = false;
    QString media, init;
    qint64 startNumber = 1, timescale = 1, duration = 0;
    QList<std::array<qint64, 3>> timeline; // t (-1 = continue), d, r
};
struct SegList {
    bool present = false;
    MpdSegment init;
    QList<QPair<QString, QString>> items; // media, mediaRange
    QString initSource, initRange;
    qint64 timescale = 1, duration = 0;
};
struct Ctx {
    QUrl base;
    Tmpl tmpl;
    SegList list;
    QString indexRange, initRange; // SegmentBase
    bool segmentBase = false;
    QString mime, codecs, contentType, lang;
};

QPair<qint64, qint64> parseRange(const QString& r)
{
    const int dash = r.indexOf(QLatin1Char('-'));
    if (dash <= 0)
        return {-1, -1};
    return {r.left(dash).toLongLong(), r.mid(dash + 1).toLongLong()};
}

QString kindOf(const Ctx& c)
{
    const QString ct = c.contentType.toLower(), mime = c.mime.toLower();
    if (ct == QLatin1String("video") || mime.startsWith(QLatin1String("video/")))
        return QStringLiteral("video");
    if (ct == QLatin1String("audio") || mime.startsWith(QLatin1String("audio/")))
        return QStringLiteral("audio");
    return {};
}

void readTemplate(QXmlStreamReader& x, Tmpl& t)
{
    const auto a = x.attributes();
    t.present = true;
    if (a.hasAttribute(QLatin1String("media")))
        t.media = a.value(QLatin1String("media")).toString();
    if (a.hasAttribute(QLatin1String("initialization")))
        t.init = a.value(QLatin1String("initialization")).toString();
    if (a.hasAttribute(QLatin1String("startNumber")))
        t.startNumber = a.value(QLatin1String("startNumber")).toLongLong();
    if (a.hasAttribute(QLatin1String("timescale")))
        t.timescale = qMax<qint64>(1, a.value(QLatin1String("timescale")).toLongLong());
    if (a.hasAttribute(QLatin1String("duration")))
        t.duration = a.value(QLatin1String("duration")).toLongLong();
    while (x.readNextStartElement()) {
        if (x.name() == QLatin1String("SegmentTimeline")) {
            t.timeline.clear();
            while (x.readNextStartElement()) {
                if (x.name() == QLatin1String("S")) {
                    const auto s = x.attributes();
                    t.timeline.append({s.hasAttribute(QLatin1String("t")) ? s.value(QLatin1String("t")).toLongLong() : -1,
                                       s.value(QLatin1String("d")).toLongLong(),
                                       s.hasAttribute(QLatin1String("r")) ? s.value(QLatin1String("r")).toLongLong() : 0});
                }
                x.skipCurrentElement();
            }
        } else {
            x.skipCurrentElement();
        }
    }
}

void readList(QXmlStreamReader& x, SegList& l)
{
    l.present = true;
    l.items.clear();
    const auto a = x.attributes();
    if (a.hasAttribute(QLatin1String("timescale")))
        l.timescale = qMax<qint64>(1, a.value(QLatin1String("timescale")).toLongLong());
    if (a.hasAttribute(QLatin1String("duration")))
        l.duration = a.value(QLatin1String("duration")).toLongLong();
    while (x.readNextStartElement()) {
        const auto s = x.attributes();
        if (x.name() == QLatin1String("Initialization")) {
            l.initSource = s.value(QLatin1String("sourceURL")).toString();
            l.initRange = s.value(QLatin1String("range")).toString();
        } else if (x.name() == QLatin1String("SegmentURL")) {
            l.items.append({s.value(QLatin1String("media")).toString(), s.value(QLatin1String("mediaRange")).toString()});
        }
        x.skipCurrentElement();
    }
}

QUrl childBase(const Ctx& c, const QString& text) { return c.base.resolved(QUrl(text.trimmed())); }

// Builds the segment list of one representation from the inherited context.
QString buildSegments(MpdRepresentation& r, const Ctx& c, double periodDuration)
{
    if (c.tmpl.present && !c.tmpl.media.isEmpty()) {
        const Tmpl& t = c.tmpl;
        if (!t.init.isEmpty())
            r.init.url = c.base.resolved(QUrl(expandTemplate(t.init, r.id, 0, 0, r.bandwidth)));
        qint64 number = t.startNumber;
        if (!t.timeline.isEmpty()) {
            qint64 time = 0;
            const qint64 end = periodDuration > 0 ? qint64(std::llround(periodDuration * double(t.timescale))) : -1;
            for (int i = 0; i < t.timeline.size(); ++i) {
                const auto& s = t.timeline[i];
                if (s[0] >= 0)
                    time = s[0];
                qint64 repeat = s[2];
                if (repeat < 0) { // until the next S@t or the end of the period
                    const qint64 until = i + 1 < t.timeline.size() && t.timeline[i + 1][0] >= 0 ? t.timeline[i + 1][0] : end;
                    repeat = until > time && s[1] > 0 ? (until - time + s[1] - 1) / s[1] - 1 : 0;
                }
                for (qint64 k = 0; k <= repeat; ++k) {
                    r.segments.append({c.base.resolved(QUrl(expandTemplate(t.media, r.id, number, time, r.bandwidth)))});
                    ++number;
                    time += s[1];
                }
            }
        } else if (t.duration > 0) {
            if (periodDuration <= 0)
                return QStringLiteral("The stream has no length (live stream?)");
            const double segSecs = double(t.duration) / double(t.timescale);
            const qint64 count = qint64(std::ceil(periodDuration / segSecs - 1e-6));
            for (qint64 k = 0; k < count; ++k)
                r.segments.append({c.base.resolved(QUrl(expandTemplate(t.media, r.id, number + k, (number + k - t.startNumber) * t.duration, r.bandwidth)))});
        } else {
            return QStringLiteral("Unsupported segment template");
        }
        return {};
    }
    if (c.list.present) {
        if (!c.list.initSource.isEmpty() || !c.list.initRange.isEmpty()) {
            r.init.url = c.list.initSource.isEmpty() ? c.base : c.base.resolved(QUrl(c.list.initSource));
            const auto rg = parseRange(c.list.initRange);
            r.init.rangeStart = rg.first;
            r.init.rangeEnd = rg.second;
        }
        for (const auto& it : c.list.items) {
            MpdSegment s;
            s.url = it.first.isEmpty() ? c.base : c.base.resolved(QUrl(it.first));
            const auto rg = parseRange(it.second);
            s.rangeStart = rg.first;
            s.rangeEnd = rg.second;
            r.segments.append(s);
        }
        return r.segments.isEmpty() ? QStringLiteral("Empty segment list") : QString();
    }
    // SegmentBase or a bare BaseURL: the whole representation is one file.
    if (c.base.isValid() && !c.base.path().endsWith(QLatin1Char('/'))) {
        r.segments.append({c.base});
        return {};
    }
    return QStringLiteral("No segments for representation %1").arg(r.id);
}

} // namespace

MpdManifest parse(const QByteArray& xml, const QUrl& manifestUrl)
{
    MpdManifest m;
    QXmlStreamReader x(xml);
    if (!x.readNextStartElement() || x.name() != QLatin1String("MPD")) {
        m.error = QStringLiteral("Not a DASH manifest");
        return m;
    }
    const auto mpdAttr = x.attributes();
    if (mpdAttr.value(QLatin1String("type")).toString() == QLatin1String("dynamic")) {
        m.error = QStringLiteral("Live DASH streams aren't supported, only videos on demand");
        return m;
    }
    m.duration = parseDuration(mpdAttr.value(QLatin1String("mediaPresentationDuration")).toString());
    if (m.duration < 0)
        m.duration = 0;

    Ctx mpd;
    mpd.base = manifestUrl;
    bool drm = false;
    bool periodSeen = false;
    QString buildError;
    while (x.readNextStartElement()) {
        if (x.name() == QLatin1String("BaseURL")) {
            mpd.base = childBase(mpd, x.readElementText());
        } else if (x.name() == QLatin1String("Period") && !periodSeen) { // the first period: the programme
            periodSeen = true;
            Ctx period = mpd;
            const double pd = parseDuration(x.attributes().value(QLatin1String("duration")).toString());
            const double periodDuration = pd > 0 ? pd : m.duration;
            while (x.readNextStartElement()) {
                if (x.name() == QLatin1String("BaseURL")) {
                    period.base = childBase(period, x.readElementText());
                } else if (x.name() == QLatin1String("SegmentTemplate")) {
                    readTemplate(x, period.tmpl);
                } else if (x.name() == QLatin1String("AdaptationSet")) {
                    Ctx set = period;
                    const auto a = x.attributes();
                    set.mime = a.value(QLatin1String("mimeType")).toString();
                    set.codecs = a.value(QLatin1String("codecs")).toString();
                    set.contentType = a.value(QLatin1String("contentType")).toString();
                    set.lang = a.value(QLatin1String("lang")).toString();
                    QList<QPair<MpdRepresentation, Ctx>> reps;
                    while (x.readNextStartElement()) {
                        const QStringView n = x.name();
                        if (n == QLatin1String("BaseURL")) {
                            set.base = childBase(set, x.readElementText());
                        } else if (n == QLatin1String("ContentProtection")) {
                            drm = true;
                            x.skipCurrentElement();
                        } else if (n == QLatin1String("SegmentTemplate")) {
                            readTemplate(x, set.tmpl);
                        } else if (n == QLatin1String("SegmentList")) {
                            readList(x, set.list);
                        } else if (n == QLatin1String("Representation")) {
                            Ctx rc = set;
                            MpdRepresentation r;
                            const auto ra = x.attributes();
                            r.id = ra.value(QLatin1String("id")).toString();
                            r.bandwidth = ra.value(QLatin1String("bandwidth")).toLongLong();
                            r.width = ra.value(QLatin1String("width")).toInt();
                            r.height = ra.value(QLatin1String("height")).toInt();
                            if (ra.hasAttribute(QLatin1String("mimeType")))
                                rc.mime = ra.value(QLatin1String("mimeType")).toString();
                            if (ra.hasAttribute(QLatin1String("codecs")))
                                rc.codecs = ra.value(QLatin1String("codecs")).toString();
                            while (x.readNextStartElement()) {
                                const QStringView rn = x.name();
                                if (rn == QLatin1String("BaseURL"))
                                    rc.base = childBase(rc, x.readElementText());
                                else if (rn == QLatin1String("SegmentTemplate"))
                                    readTemplate(x, rc.tmpl);
                                else if (rn == QLatin1String("SegmentList"))
                                    readList(x, rc.list);
                                else if (rn == QLatin1String("ContentProtection")) {
                                    drm = true;
                                    x.skipCurrentElement();
                                } else {
                                    if (rn == QLatin1String("SegmentBase"))
                                        rc.segmentBase = true;
                                    x.skipCurrentElement();
                                }
                            }
                            reps.append({r, rc});
                        } else {
                            x.skipCurrentElement();
                        }
                    }
                    // Segment info declared after the Representations still applies to them.
                    for (auto& [r, rc] : reps) {
                        if (!rc.tmpl.present && set.tmpl.present)
                            rc.tmpl = set.tmpl;
                        if (!rc.list.present && set.list.present)
                            rc.list = set.list;
                        r.mimeType = rc.mime;
                        r.codecs = rc.codecs;
                        r.lang = set.lang;
                        r.kind = kindOf(rc);
                        if (r.kind.isEmpty())
                            continue;
                        const QString err = buildSegments(r, rc, periodDuration);
                        if (!err.isEmpty()) {
                            buildError = err;
                            continue;
                        }
                        m.representations.append(r);
                    }
                } else {
                    x.skipCurrentElement();
                }
            }
        } else {
            x.skipCurrentElement();
        }
    }
    if (x.hasError() && x.error() != QXmlStreamReader::PrematureEndOfDocumentError) {
        m.error = QStringLiteral("Invalid DASH manifest: %1").arg(x.errorString());
        return m;
    }
    if (drm) {
        m.error = QStringLiteral("This stream is DRM-protected and can't be downloaded");
        return m;
    }
    if (m.representations.isEmpty()) {
        m.error = buildError.isEmpty() ? QStringLiteral("No video or audio in this DASH manifest") : buildError;
        return m;
    }
    m.valid = true;
    return m;
}

} // namespace Mpd
