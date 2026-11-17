#pragma once
#include <QRegularExpression>
#include <QStringList>

// FDM "batch download": expands one numeric or letter range per link,
//   https://x.com/img[1-12].jpg   https://x.com/part[001-120].bin   https://x.com/[a-f].txt
namespace BatchLinks {
inline QStringList expand(const QString& link, int maxCount = 1000)
{
    static const QRegularExpression re(QStringLiteral(R"(\[(\d+)-(\d+)\]|\[([a-zA-Z])-([a-zA-Z])\])"));
    const auto m = re.match(link);
    if (!m.hasMatch())
        return {link};
    const QString pre = link.left(m.capturedStart());
    const QString post = link.mid(m.capturedEnd());
    QStringList out;
    if (!m.captured(1).isEmpty()) {
        const QString a = m.captured(1), b = m.captured(2);
        const qint64 from = a.toLongLong(), to = b.toLongLong();
        const int width = (a.startsWith(QLatin1Char('0')) && a.size() > 1) ? int(a.size()) : 0;
        const int step = from <= to ? 1 : -1;
        for (qint64 i = from;; i += step) {
            out << pre + QStringLiteral("%1").arg(i, width, 10, QLatin1Char('0')) + post;
            if (i == to || out.size() >= maxCount)
                break;
        }
    } else {
        const QChar a = m.captured(3).at(0), b = m.captured(4).at(0);
        const int step = a <= b ? 1 : -1;
        for (int c = a.unicode();; c += step) {
            out << pre + QChar(c) + post;
            if (c == b.unicode() || out.size() >= maxCount)
                break;
        }
    }
    return out;
}

inline QStringList expandAll(const QStringList& links, int maxCount = 1000)
{
    QStringList out;
    for (const QString& l : links) {
        out << expand(l, maxCount - int(out.size()));
        if (out.size() >= maxCount)
            break;
    }
    return out;
}
} // namespace BatchLinks
