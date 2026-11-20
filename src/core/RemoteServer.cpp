#include "core/RemoteServer.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <memory>

#include "core/Download.h"
#include "core/DownloadManager.h"

static constexpr qint64 kMaxRequest = 64 * 1024;
static constexpr int kMaxFailures = 5;
static constexpr qint64 kLockMs = 60 * 1000;
static constexpr int kMaxGlobalFailures = 20;           // wrong codes from all addresses ...
static constexpr qint64 kGlobalWindowMs = 10 * 60 * 1000; // ... within 10 minutes
static constexpr qint64 kGlobalLockMs = 5 * 60 * 1000;    // pause all sign-ins this long
static constexpr qint64 kTokenIdleMs = 12LL * 3600 * 1000; // unused tokens expire
static constexpr int kMaxTokens = 64;
static constexpr int kMaxConnections = 32;

// Same time whatever the input, so the code can't be guessed digit by digit from timings.
static bool sameCode(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

RemoteServer::RemoteServer(DownloadManager* mgr, QObject* parent)
    : QObject(parent), m_mgr(mgr), m_code(newAccessCode())
{
    connect(&m_server, &QTcpServer::newConnection, this, &RemoteServer::onConnection);
}

bool RemoteServer::start(quint16 port, const QHostAddress& address)
{
    stop();
    return m_server.listen(address, port);
}

void RemoteServer::stop()
{
    if (m_server.isListening())
        m_server.close();
}

void RemoteServer::setAccessCode(const QString& code)
{
    m_code = code.trimmed();
    m_tokens.clear(); // everyone signs in again
}

bool RemoteServer::isAllowedHost(const QByteArray& hostHeader)
{
    QString host = QString::fromLatin1(hostHeader).trimmed().toLower();
    if (host.startsWith(QLatin1Char('['))) { // [::1]:8080
        const int close = int(host.indexOf(QLatin1Char(']')));
        return close > 1 && !QHostAddress(host.mid(1, close - 1)).isNull();
    }
    const int colon = int(host.lastIndexOf(QLatin1Char(':')));
    if (colon >= 0)
        host = host.left(colon);
    if (host.isEmpty())
        return false;
    if (!QHostAddress(host).isNull() || host == QLatin1String("localhost"))
        return true;
    const QString me = QSysInfo::machineHostName().toLower();
    return !me.isEmpty() && (host == me || host.startsWith(me + QLatin1Char('.')));
}

bool RemoteServer::tokenValid(const QByteArray& token)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_tokens.begin(); it != m_tokens.end();)
        it = now - it.value() > kTokenIdleMs ? m_tokens.erase(it) : std::next(it);
    auto it = m_tokens.find(token);
    if (token.isEmpty() || it == m_tokens.end())
        return false;
    it.value() = now;
    return true;
}

void RemoteServer::noteFailure(const QString& ip, qint64 now)
{
    for (auto it = m_failures.begin(); it != m_failures.end();) // forget old, unlocked entries
        it = (it->lockedUntil < now && now - it->last > kLockMs) ? m_failures.erase(it) : std::next(it);
    Failures& f = m_failures[ip];
    f.last = now;
    if (++f.count >= kMaxFailures) {
        f.count = 0;
        f.lockedUntil = now + kLockMs;
    }
    m_recentFailures.append(now);
    while (!m_recentFailures.isEmpty() && now - m_recentFailures.first() > kGlobalWindowMs)
        m_recentFailures.removeFirst();
    if (m_recentFailures.size() >= kMaxGlobalFailures) {
        m_globalLockUntil = now + kGlobalLockMs;
        m_recentFailures.clear();
    }
}

QString RemoteServer::newAccessCode()
{
    return QStringLiteral("%1").arg(QRandomGenerator::system()->bounded(1000000), 6, 10, QLatin1Char('0'));
}

bool RemoteServer::isLocalNetwork(const QHostAddress& in)
{
    bool v4ok = false;
    const QHostAddress a = in.protocol() == QAbstractSocket::IPv6Protocol && in.toIPv4Address(&v4ok) && v4ok
                               ? QHostAddress(in.toIPv4Address()) : in; // ::ffff:192.168.1.5
    if (a.isLoopback())
        return true;
    if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        return a.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) || a.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12)
            || a.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16) || a.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16);
    }
    return a.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10) || a.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7);
}

QStringList RemoteServer::lanAddresses()
{
    QStringList out;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && isLocalNetwork(a))
            out << a.toString();
    return out;
}

void RemoteServer::onConnection()
{
    while (QTcpSocket* s = m_server.nextPendingConnection()) {
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        if (!isLocalNetwork(s->peerAddress()) || m_open >= kMaxConnections) { // never answer the internet
            s->abort();
            s->deleteLater();
            continue;
        }
        ++m_open;
        connect(s, &QObject::destroyed, this, [this] { --m_open; });
        // A client that never finishes its request doesn't get to keep the connection.
        auto* idle = new QTimer(s);
        idle->setSingleShot(true);
        connect(idle, &QTimer::timeout, s, [s] { s->abort(); s->deleteLater(); });
        idle->start(m_idleMs);
        auto buf = std::make_shared<QByteArray>();
        connect(s, &QTcpSocket::readyRead, this, [this, s, buf] {
            buf->append(s->readAll());
            if (buf->size() > kMaxRequest) {
                reply(s, 413, "text/plain", "Too large");
                return;
            }
            const int end = int(buf->indexOf("\r\n\r\n"));
            if (end < 0)
                return;
            Request r;
            const QList<QByteArray> lines = buf->left(end).split('\n');
            const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
            r.method = first.value(0);
            r.path = first.value(1);
            for (int i = 1; i < lines.size(); ++i) {
                const int c = int(lines[i].indexOf(':'));
                if (c > 0)
                    r.headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
            }
            bool lenOk = true;
            const QByteArray lenHeader = r.headers.value("content-length", "0");
            const qint64 len = lenHeader.toLongLong(&lenOk);
            if (!lenOk || len < 0 || len > kMaxRequest) {
                disconnect(s, &QTcpSocket::readyRead, this, nullptr);
                reply(s, len > kMaxRequest ? 413 : 400, "text/plain", "Bad request");
                return;
            }
            if (buf->size() < end + 4 + len)
                return; // wait for the body
            r.body = buf->mid(end + 4, len);
            disconnect(s, &QTcpSocket::readyRead, this, nullptr);
            if (auto* t = s->findChild<QTimer*>())
                t->stop();
            handle(s, r);
        });
    }
}

void RemoteServer::reply(QTcpSocket* s, int code, const QByteArray& type, const QByteArray& body, const QByteArray& extra)
{
    static const QHash<int, QByteArray> text = {{200, "OK"}, {400, "Bad Request"}, {401, "Unauthorized"}, {403, "Forbidden"},
                                                {404, "Not Found"}, {413, "Payload Too Large"}, {429, "Too Many Requests"}};
    QByteArray h = "HTTP/1.1 " + QByteArray::number(code) + ' ' + text.value(code, "Error") + "\r\n";
    h += "Content-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
    h += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n";
    h += "Referrer-Policy: no-referrer\r\nConnection: close\r\n" + extra + "\r\n";
    s->write(h + body);
    s->disconnectFromHost();
}

static QJsonObject parseJson(const QByteArray& body) { return QJsonDocument::fromJson(body).object(); }

QByteArray RemoteServer::downloadsJson() const
{
    QJsonArray list;
    for (Download* d : m_mgr->downloads()) {
        list.append(QJsonObject{{QStringLiteral("id"), d->id()},
                                {QStringLiteral("title"), d->title()},
                                {QStringLiteral("state"), d->seeding() ? QStringLiteral("Seeding") : d->stateText()},
                                {QStringLiteral("running"), d->running()},
                                {QStringLiteral("completed"), d->completed()},
                                {QStringLiteral("paused"), d->paused() || d->failed()},
                                {QStringLiteral("progress"), d->progress()},
                                {QStringLiteral("received"), d->received()},
                                {QStringLiteral("total"), d->total()},
                                {QStringLiteral("speed"), d->speed()},
                                {QStringLiteral("eta"), d->eta()},
                                {QStringLiteral("error"), d->failed() ? d->error() : QString()}});
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("downloads"), list},
                                     {QStringLiteral("mode"), m_mgr->trafficMode()}}).toJson(QJsonDocument::Compact);
}

void RemoteServer::handle(QTcpSocket* s, const Request& r)
{
    const QString ip = s->peerAddress().toString();
    if (!isAllowedHost(r.headers.value("host"))) { // a web page that rebinds its own name to this computer
        reply(s, 403, "text/plain", "Open FluxDM by the address shown in its settings");
        return;
    }
    if (r.method == "GET" && (r.path == "/" || r.path.startsWith("/?"))) {
        QFile page(QStringLiteral(":/remote/index.html"));
        if (!page.open(QIODevice::ReadOnly)) {
            reply(s, 404, "text/plain", "Not found");
            return;
        }
        reply(s, 200, "text/html; charset=utf-8", page.readAll(),
              "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'\r\n");
        return;
    }
    if (r.method == "POST" && r.path == "/api/login") {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_globalLockUntil > now || m_failures.value(ip).lockedUntil > now) {
            reply(s, 429, "application/json", R"({"error":"Too many wrong codes. Try again in a few minutes."})");
            return;
        }
        const QByteArray given = parseJson(r.body).value(QStringLiteral("code")).toString().trimmed().toUtf8();
        if (m_code.isEmpty() || !sameCode(given, m_code.toUtf8())) {
            noteFailure(ip, now);
            reply(s, 403, "application/json", R"({"error":"Wrong access code"})");
            return;
        }
        m_failures.remove(ip);
        QByteArray token(32, Qt::Uninitialized);
        QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(token.data()), token.size() / 4);
        const QByteArray hex = token.toHex();
        tokenValid({}); // drops expired tokens
        while (m_tokens.size() >= kMaxTokens) { // the least recently used goes
            auto oldest = std::min_element(m_tokens.begin(), m_tokens.end());
            m_tokens.erase(oldest);
        }
        m_tokens.insert(hex, now);
        reply(s, 200, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("token"), QString::fromLatin1(hex)}}).toJson(QJsonDocument::Compact));
        return;
    }
    if (!r.path.startsWith("/api/")) {
        reply(s, 404, "text/plain", "Not found");
        return;
    }
    if (!tokenValid(r.headers.value("x-fluxdm-token"))) {
        reply(s, 401, "application/json", R"({"error":"Sign in first"})");
        return;
    }
    const QJsonObject body = parseJson(r.body);
    auto ok = [&](const QByteArray& extra = {}) { reply(s, 200, "application/json", extra.isEmpty() ? QByteArray(R"({"ok":true})") : extra); };
    auto target = [&]() { return m_mgr->find(qint64(body.value(QStringLiteral("id")).toDouble())); };
    if (r.method == "GET" && r.path == "/api/downloads") {
        ok(downloadsJson());
    } else if (r.method != "POST") {
        reply(s, 400, "application/json", R"({"error":"Use POST"})");
    } else if (r.path == "/api/add") {
        const QString url = body.value(QStringLiteral("url")).toString().trimmed();
        // Web and magnet links only: never a path to a file on this computer.
        static const QStringList schemes = {QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("ftp"),
                                            QStringLiteral("ftps"), QStringLiteral("magnet")};
        const bool web = schemes.contains(QUrl(url).scheme().toLower());
        const QString err = url.isEmpty() ? QStringLiteral("Enter a link")
                            : !web        ? QStringLiteral("Only web and magnet links can be added remotely")
                                          : (m_add ? m_add(url) : QStringLiteral("Not available"));
        if (err.isEmpty())
            ok();
        else
            reply(s, 400, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("error"), err}}).toJson(QJsonDocument::Compact));
    } else if (r.path == "/api/pause") {
        m_mgr->pause(target());
        ok();
    } else if (r.path == "/api/resume") {
        m_mgr->resume(target());
        ok();
    } else if (r.path == "/api/pauseAll") {
        m_mgr->pauseAll();
        ok();
    } else if (r.path == "/api/resumeAll") {
        m_mgr->resumeAll();
        ok();
    } else if (r.path == "/api/mode") {
        m_mgr->setTrafficMode(qBound(0, body.value(QStringLiteral("mode")).toInt(), 2));
        ok();
    } else if (r.path == "/api/logout") {
        m_tokens.remove(r.headers.value("x-fluxdm-token"));
        ok();
    } else {
        reply(s, 404, "application/json", R"({"error":"Unknown command"})");
    }
}
