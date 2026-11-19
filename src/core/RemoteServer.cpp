#include "core/RemoteServer.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <memory>

#include "core/Download.h"
#include "core/DownloadManager.h"

static constexpr qint64 kMaxRequest = 64 * 1024;
static constexpr int kMaxFailures = 5;
static constexpr qint64 kLockMs = 60 * 1000;

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
        if (!isLocalNetwork(s->peerAddress())) { // never answer the internet
            s->abort();
            s->deleteLater();
            continue;
        }
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
            const qint64 len = r.headers.value("content-length").toLongLong();
            if (buf->size() < end + 4 + len)
                return; // wait for the body
            r.body = buf->mid(end + 4, len);
            disconnect(s, &QTcpSocket::readyRead, this, nullptr);
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
    if (r.method == "GET" && (r.path == "/" || r.path.startsWith("/?"))) {
        QFile page(QStringLiteral(":/remote/index.html"));
        page.open(QIODevice::ReadOnly);
        reply(s, 200, "text/html; charset=utf-8", page.readAll(),
              "Content-Security-Policy: default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'\r\n");
        return;
    }
    if (r.method == "POST" && r.path == "/api/login") {
        auto& f = m_failures[ip];
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (f.second > now) {
            reply(s, 429, "application/json", R"({"error":"Too many wrong codes. Try again in a minute."})");
            return;
        }
        if (m_code.isEmpty() || parseJson(r.body).value(QStringLiteral("code")).toString().trimmed() != m_code) {
            if (++f.first >= kMaxFailures) {
                f = {0, now + kLockMs};
            }
            reply(s, 403, "application/json", R"({"error":"Wrong access code"})");
            return;
        }
        m_failures.remove(ip);
        QByteArray token(32, Qt::Uninitialized);
        QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(token.data()), token.size() / 4);
        const QByteArray hex = token.toHex();
        m_tokens.insert(hex);
        reply(s, 200, "application/json", QJsonDocument(QJsonObject{{QStringLiteral("token"), QString::fromLatin1(hex)}}).toJson(QJsonDocument::Compact));
        return;
    }
    if (!r.path.startsWith("/api/")) {
        reply(s, 404, "text/plain", "Not found");
        return;
    }
    if (!m_tokens.contains(r.headers.value("x-fluxdm-token"))) {
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
        const QString err = url.isEmpty() ? QStringLiteral("Enter a link") : (m_add ? m_add(url) : QStringLiteral("Not available"));
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
