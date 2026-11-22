#include "core/Metalink.h"

#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>

namespace Metalink {

bool looksLike(const QString& path)
{
    const QFileInfo fi(path);
    const QString suffix = fi.suffix().toLower();
    if (suffix == QLatin1String("meta4") || suffix == QLatin1String("metalink"))
        return true;
    if (!fi.isFile() || fi.size() > 4 * 1024 * 1024)
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(4096);
    return head.contains("<metalink") && (head.contains("urn:ietf:params:xml:ns:metalink") || head.contains("metalinker.org"));
}

namespace {
int hashRank(const QString& type) // stronger is better
{
    const QString t = type.toLower().remove(QLatin1Char('-'));
    if (t == QLatin1String("sha512")) return 4;
    if (t == QLatin1String("sha256")) return 3;
    if (t == QLatin1String("sha1")) return 2;
    if (t == QLatin1String("md5")) return 1;
    return 0;
}

bool usableUrl(const QString& u)
{
    const QString s = QUrl(u).scheme().toLower();
    return s == QLatin1String("http") || s == QLatin1String("https") || s == QLatin1String("ftp");
}
} // namespace

QList<File> parse(const QByteArray& xml, QString* error)
{
    QList<File> out;
    QXmlStreamReader r(xml);
    File cur;
    bool inFile = false;
    int bestHash = 0;
    QList<QPair<int, QString>> urls; // (order key, url): lower first
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            const QStringView n = r.name();
            if (n == QLatin1String("file")) {
                cur = File();
                cur.name = QFileInfo(r.attributes().value(QLatin1String("name")).toString()).fileName();
                inFile = true;
                bestHash = 0;
                urls.clear();
            } else if (inFile && n == QLatin1String("size")) {
                cur.size = r.readElementText().trimmed().toLongLong();
            } else if (inFile && n == QLatin1String("hash")) {
                const QString type = r.attributes().value(QLatin1String("type")).toString();
                const QString hex = r.readElementText().trimmed();
                if (hashRank(type) > bestHash && !hex.isEmpty()) { // piece hashes have no "type" here
                    bestHash = hashRank(type);
                    cur.hash = type.toLower() + QLatin1Char(':') + hex.toLower();
                }
            } else if (inFile && n == QLatin1String("url")) {
                const auto a = r.attributes();
                // v4: priority 1 = best; v3: preference 100 = best.
                int key = 500;
                if (a.hasAttribute(QLatin1String("priority")))
                    key = a.value(QLatin1String("priority")).toInt();
                else if (a.hasAttribute(QLatin1String("preference")))
                    key = 1000 - a.value(QLatin1String("preference")).toInt();
                const QString u = r.readElementText().trimmed();
                if (usableUrl(u))
                    urls.append({key, u});
            }
        } else if (r.isEndElement() && r.name() == QLatin1String("file") && inFile) {
            inFile = false;
            std::stable_sort(urls.begin(), urls.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            for (const auto& [k, u] : urls)
                if (!cur.urls.contains(u))
                    cur.urls << u;
            if (!cur.name.isEmpty() && !cur.urls.isEmpty())
                out << cur;
        }
    }
    if (r.hasError() && error)
        *error = r.errorString();
    return out;
}

} // namespace Metalink
