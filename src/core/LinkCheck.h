#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QUrl>

// Recognizes links that stopped working: an HTML page served where a file was expected
// (expired file-host links, sign-in walls, countdown/captcha pages) and signed links whose
// expiry time has passed. Used to turn vague failures into messages a user can act on.
namespace LinkCheck {

// True when the server answered with a web page although the link (or the name the user
// gave) points at a file such as .zip or .mp4. Pages the user really asked for (.html, .php,
// names without an extension) and attachments (Content-Disposition) are not flagged.
bool isUnexpectedWebPage(const QByteArray& contentType, const QByteArray& contentDisposition,
                         const QStringList& expectedNames);

// Expiry time of a signed link (?expire=, ?expires=, ?exp= with a Unix time), invalid if none.
QDateTime linkExpiry(const QUrl& url);

// "HTTP error 403: …" with a hint for the codes that usually mean a dead link. Keeps the
// "HTTP error <code>" prefix that the auto-retry rules match on.
QString httpErrorText(long code, const QUrl& url, const QDateTime& now = QDateTime::currentDateTime());

// "HTTP error <code>: …" for a site that only lets a real web browser through (Cloudflare check).
QString browserCheckText(long code);

// Full message for the web-page case, mentioning the expiry time when the link carries one.
QString webPageText(const QUrl& url, const QDateTime& now = QDateTime::currentDateTime());

} // namespace LinkCheck
