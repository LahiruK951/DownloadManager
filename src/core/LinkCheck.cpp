#include "core/LinkCheck.h"

#include <algorithm>

#include <QFileInfo>
#include <QLocale>
#include <QUrlQuery>

namespace LinkCheck {

static bool isPageExtension(const QString& ext)
{
    static const QStringList page = {QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("shtml"),
                                     QStringLiteral("xhtml"), QStringLiteral("php"), QStringLiteral("asp"),
                                     QStringLiteral("aspx"), QStringLiteral("jsp"), QStringLiteral("cgi"),
                                     QStringLiteral("pl")};
    return page.contains(ext.toLower());
}

bool isUnexpectedWebPage(const QByteArray& contentType, const QByteArray& contentDisposition,
                         const QStringList& expectedNames)
{
    const QByteArray ct = contentType.trimmed().toLower();
    if (!ct.startsWith("text/html") && !ct.startsWith("application/xhtml"))
        return false;
    if (contentDisposition.toLower().contains("attachment"))
        return false; // the server explicitly offers it as a file
    for (const QString& name : expectedNames) {
        const QString ext = QFileInfo(name).suffix();
        // An extension made of letters/digits, e.g. "zip", "mp4", "tar.gz" -> "gz".
        if (!ext.isEmpty() && ext.size() <= 8 && !isPageExtension(ext)
            && std::all_of(ext.begin(), ext.end(), [](QChar c) { return c.isLetterOrNumber(); }))
            return true;
    }
    return false;
}

QDateTime linkExpiry(const QUrl& url)
{
    const QUrlQuery q(url);
    for (const auto& item : q.queryItems()) {
        const QString key = item.first.toLower();
        if (key != QLatin1String("expire") && key != QLatin1String("expires") && key != QLatin1String("exp"))
            continue;
        bool ok = false;
        const qint64 v = item.second.toLongLong(&ok);
        // Plausible Unix time (2001..2100); durations like "3600" are not expiry times.
        if (ok && v > 1'000'000'000LL && v < 4'102'444'800LL)
            return QDateTime::fromSecsSinceEpoch(v);
    }
    return {};
}

static QString expiredPart(const QUrl& url, const QDateTime& now)
{
    const QDateTime exp = linkExpiry(url);
    if (!exp.isValid() || exp > now)
        return {};
    return QStringLiteral("the link expired on %1. Get a new link from the website")
        .arg(QLocale().toString(exp.toLocalTime(), QLocale::ShortFormat));
}

QString httpErrorText(long code, const QUrl& url, const QDateTime& now)
{
    const QString prefix = QStringLiteral("HTTP error %1").arg(code);
    if (code == 401 || code == 403 || code == 404 || code == 410) {
        const QString expired = expiredPart(url, now);
        if (!expired.isEmpty())
            return prefix + QStringLiteral(": ") + expired;
    }
    switch (code) {
    case 401: return prefix + QStringLiteral(": the server requires a user name and password");
    case 403: return prefix + QStringLiteral(": access denied. The link may have expired, or it only works from the browser that opened it");
    case 404: return prefix + QStringLiteral(": the file was not found on the server");
    case 410: return prefix + QStringLiteral(": the link has expired or the file was removed");
    case 416: return prefix + QStringLiteral(": the server refused to continue the file. The link has probably expired: "
                                             "use Change address to give FluxDM a fresh link; the downloaded part is kept");
    case 429: return prefix + QStringLiteral(": the server is limiting requests; trying again later");
    default: break;
    }
    if (code >= 500)
        return prefix + QStringLiteral(": the server had a problem; trying again later");
    return prefix;
}

QString browserCheckText(long code)
{
    return QStringLiteral("HTTP error %1: this site only lets a web browser download (browser check). "
                          "Click the link in your browser with FluxDM's browser integration turned on, "
                          "or save the file in your browser and open it with FluxDM").arg(code);
}

QString webPageText(const QUrl& url, const QDateTime& now)
{
    const QString expired = expiredPart(url, now);
    return QStringLiteral("The server sent a web page instead of the file: ")
        + (expired.isEmpty()
               ? QStringLiteral("the link may have expired, or the site wants it downloaded from your browser "
                                "(sign-in, captcha or a countdown page)")
               : expired);
}

} // namespace LinkCheck
