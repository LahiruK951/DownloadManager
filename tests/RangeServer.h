#pragma once
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>

// Minimal throttled HTTP/1.1 server with optional Range support, for engine tests.
class RangeServer : public QObject {
    Q_OBJECT
public:
    QByteArray data;
    bool rangeSupport = true;
    int chunk = 64 * 1024;   // bytes per tick per connection
    int intervalMs = 2;      // tick interval
    int failStatus = 0;      // e.g. 404 to make every request fail
    QByteArray contentDisposition;

    int requests = 0;
    int rangedRequests = 0;
    qint64 bytesSent = 0;
    QList<qint64> rangeStarts;

    bool start()
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* s = m_server.nextPendingConnection())
                handle(s);
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString url(const QString& path = QStringLiteral("/file.bin")) const
    {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }

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
            ++requests;

            if (failStatus) {
                sock->write("HTTP/1.1 " + QByteArray::number(failStatus) + " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                sock->disconnectFromHost();
                return;
            }

            const qint64 size = data.size();
            qint64 b = 0, e = size - 1;
            bool ranged = false;
            for (const QByteArray& line : buf->split('\n')) {
                const QByteArray l = line.trimmed();
                if (rangeSupport && l.toLower().startsWith("range: bytes=")) {
                    const QByteArray spec = l.mid(13);
                    const int dash = spec.indexOf('-');
                    b = spec.left(dash).toLongLong();
                    const QByteArray endPart = spec.mid(dash + 1);
                    if (!endPart.isEmpty())
                        e = qMin(endPart.toLongLong(), size - 1);
                    ranged = true;
                }
            }
            QByteArray hdr;
            if (ranged) {
                ++rangedRequests;
                rangeStarts.append(b);
                hdr = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(b) + '-'
                    + QByteArray::number(e) + '/' + QByteArray::number(size) + "\r\n";
            } else {
                hdr = "HTTP/1.1 200 OK\r\n";
            }
            if (rangeSupport)
                hdr += "Accept-Ranges: bytes\r\n";
            if (!contentDisposition.isEmpty())
                hdr += "Content-Disposition: " + contentDisposition + "\r\n";
            hdr += "Content-Length: " + QByteArray::number(e - b + 1) + "\r\nConnection: close\r\n\r\n";
            sock->write(hdr);

            auto pos = std::make_shared<qint64>(b);
            auto* timer = new QTimer(sock);
            timer->setTimerType(Qt::PreciseTimer);
            timer->setInterval(intervalMs);
            connect(timer, &QTimer::timeout, sock, [this, sock, timer, pos, e] {
                if (sock->state() != QAbstractSocket::ConnectedState) {
                    timer->stop();
                    return;
                }
                if (sock->bytesToWrite() > 4 * chunk)
                    return;
                const qint64 n = qMin<qint64>(chunk, e - *pos + 1);
                if (n <= 0) {
                    timer->stop();
                    sock->disconnectFromHost();
                    return;
                }
                sock->write(data.constData() + *pos, n);
                *pos += n;
                bytesSent += n;
            });
            timer->start();
        });
    }

    QTcpServer m_server;
};
