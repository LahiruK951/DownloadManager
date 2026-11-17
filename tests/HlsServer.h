#pragma once
#include <QHash>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>

// Tiny HTTP server serving fixed bodies by path (playlists, segments, keys).
class HlsServer : public QObject {
    Q_OBJECT
public:
    QHash<QString, QByteArray> files;
    QHash<QString, int> hits;
    int delayMs = 0; // per response, to make stop/resume testable

    bool start()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_server.nextPendingConnection())
                handle(s);
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString url(const QString& path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path); }

private:
    void handle(QTcpSocket* sock)
    {
        auto buf = std::make_shared<QByteArray>();
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QTcpSocket::readyRead, sock, [this, sock, buf] {
            buf->append(sock->readAll());
            if (buf->indexOf("\r\n\r\n") < 0)
                return;
            disconnect(sock, &QTcpSocket::readyRead, nullptr, nullptr);
            const QList<QByteArray> first = buf->left(buf->indexOf("\r\n")).split(' ');
            const QString path = first.size() > 1 ? QString::fromLatin1(first[1]) : QString();
            hits[path]++;
            auto respond = [this, sock, path] {
                if (sock->state() != QAbstractSocket::ConnectedState)
                    return;
                if (!files.contains(path)) {
                    sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    const QByteArray body = files.value(path);
                    sock->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size())
                                + "\r\nConnection: close\r\n\r\n" + body);
                }
                sock->disconnectFromHost();
            };
            if (delayMs > 0)
                QTimer::singleShot(delayMs, sock, respond);
            else
                respond();
        });
    }

    QTcpServer m_server;
};
