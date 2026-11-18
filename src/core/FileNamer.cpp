#include "core/FileNamer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>

namespace FileNamer {

static QString unquote(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"'))) {
        v = v.mid(1, v.size() - 2);
        v.replace(QStringLiteral("\\\""), QStringLiteral("\""));
    }
    return v;
}

QString fromContentDisposition(const QByteArray& header)
{
    if (header.isEmpty())
        return {};
    const QString h = QString::fromUtf8(header);

    // RFC 5987: filename*=UTF-8''na%C3%AFve.txt  (preferred when present)
    static const QRegularExpression extRe(QStringLiteral(R"(filename\*\s*=\s*([^;]+))"),
                                          QRegularExpression::CaseInsensitiveOption);
    auto m = extRe.match(h);
    if (m.hasMatch()) {
        QString v = unquote(m.captured(1));
        const int p = v.indexOf(QStringLiteral("''"));
        if (p >= 0) {
            const QString charset = v.left(p).toLower();
            const QByteArray raw = QByteArray::fromPercentEncoding(v.mid(p + 2).toLatin1());
            v = (charset.isEmpty() || charset.contains(QStringLiteral("utf-8")))
                    ? QString::fromUtf8(raw) : QString::fromLatin1(raw);
        }
        if (!v.isEmpty())
            return v;
    }

    static const QRegularExpression re(QStringLiteral(R"((?:^|;)\s*filename\s*=\s*("(?:[^"\\]|\\.)*"|[^;]+))"),
                                       QRegularExpression::CaseInsensitiveOption);
    m = re.match(h);
    if (m.hasMatch()) {
        QString v = unquote(m.captured(1));
        if (v.contains(QLatin1Char('%'))) {
            const QString dec = QUrl::fromPercentEncoding(v.toUtf8());
            if (!dec.isEmpty())
                v = dec;
        }
        // Some servers form-encode spaces as '+' ("Movie+-+Part+1.mp4"). Decode only when it is
        // unambiguous: no real spaces, several '+', and no "++" (keeps "a+b.txt", "C++.pdf").
        if (!v.contains(QLatin1Char(' ')) && v.count(QLatin1Char('+')) >= 2 && !v.contains(QLatin1String("++")))
            v.replace(QLatin1Char('+'), QLatin1Char(' '));
        return v;
    }
    return {};
}

QString fromUrl(const QUrl& url)
{
    QString path = url.path(QUrl::FullyDecoded);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

QString sanitize(QString name)
{
    static const QRegularExpression bad(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    name.replace(bad, QStringLiteral("_"));
    name = name.trimmed();
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    if (name.isEmpty())
        name = QStringLiteral("download");

    static const QRegularExpression reserved(QStringLiteral(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$)"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(QFileInfo(name).completeBaseName()).hasMatch())
        name.prepend(QLatin1Char('_'));

    if (name.size() > 200) {
        const QString ext = QFileInfo(name).suffix();
        const int keep = 200 - (ext.isEmpty() ? 0 : ext.size() + 1);
        name = name.left(keep) + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    }
    return name;
}

QString uniquePath(const QString& dir, const QString& name, const QString& partSuffix)
{
    const QDir d(dir);
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString ext = fi.suffix();
    for (int i = 0;; ++i) {
        const QString cand = i == 0 ? name
            : QStringLiteral("%1 (%2)%3").arg(base).arg(i).arg(ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
        const QString p = d.filePath(cand);
        if (!QFile::exists(p) && !QFile::exists(p + partSuffix))
            return p;
    }
}

QString category(const QString& fileName)
{
    static const QHash<QString, QString> map = [] {
        QHash<QString, QString> m;
        auto add = [&m](const char* cat, std::initializer_list<const char*> exts) {
            for (auto e : exts) m.insert(QString::fromLatin1(e), QString::fromLatin1(cat));
        };
        add("Video", {"mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "mpg", "mpeg", "3gp", "ts", "m2ts"});
        add("Music", {"mp3", "flac", "wav", "aac", "ogg", "m4a", "wma", "opus", "alac", "aiff"});
        add("Archive", {"zip", "rar", "7z", "tar", "gz", "bz2", "xz", "zst", "iso", "img", "cab", "tgz"});
        add("Docs", {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "rtf", "odt", "ods", "epub", "csv", "md"});
        add("Programs", {"exe", "msi", "msix", "apk", "dmg", "pkg", "deb", "rpm", "appimage", "bat", "jar"});
        add("Pictures", {"jpg", "jpeg", "png", "gif", "webp", "bmp", "svg", "tif", "tiff", "heic", "ico"});
        return m;
    }();
    return map.value(QFileInfo(fileName).suffix().toLower(), QStringLiteral("Other"));
}

bool looksLikeTorrentFile(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.isFile() || fi.size() < 32 || fi.size() > 32 * 1024 * 1024)
        return false;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = f.read(256 * 1024);
    // A torrent is a bencoded dictionary with an "info" dictionary (and usually "announce").
    return head.startsWith('d') && (head.contains("4:infod") || head.contains("8:announce"));
}

} // namespace FileNamer
