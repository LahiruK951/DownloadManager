#pragma once
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QSet>
#include <QTcpServer>
#include <functional>

class DownloadManager;
class QTcpSocket;

// Remote control over the local network (FDM: remote control from a phone). A small HTTP
// server with one mobile-friendly page and a JSON API. Off unless started.
// Security: only private/loopback client addresses; an access code to sign in, which yields
// a random token sent as a header (not a cookie, so other web pages can't use it);
// repeated wrong codes lock that address out for a while.
class RemoteServer : public QObject {
    Q_OBJECT
public:
    explicit RemoteServer(DownloadManager* mgr, QObject* parent = nullptr);

    bool start(quint16 port, const QHostAddress& address = QHostAddress::Any); // port 0 = any free port
    void stop();
    bool isRunning() const { return m_server.isListening(); }
    quint16 port() const { return m_server.serverPort(); }
    void setAccessCode(const QString& code); // signing in again is required after a change
    QString accessCode() const { return m_code; }
    // Adding a link needs app context (default folder, browser metadata): provided by the UI layer.
    void setAddHandler(std::function<QString(const QString& url)> fn) { m_add = std::move(fn); }

    static QString newAccessCode(); // 6 digits
    static bool isLocalNetwork(const QHostAddress& a);
    static QStringList lanAddresses(); // to show "http://192.168.x.y:port"

private:
    struct Request {
        QByteArray method, path, body;
        QHash<QByteArray, QByteArray> headers; // lower-case names
    };
    void onConnection();
    void handle(QTcpSocket* s, const Request& r);
    void reply(QTcpSocket* s, int code, const QByteArray& type, const QByteArray& body, const QByteArray& extra = {});
    QByteArray downloadsJson() const;

    DownloadManager* m_mgr;
    QTcpServer m_server;
    QString m_code;
    QSet<QByteArray> m_tokens;
    QHash<QString, QPair<int, qint64>> m_failures; // address -> (count, locked until ms)
    std::function<QString(const QString&)> m_add;
};
