#pragma once
#include <QLocalServer>
#include <QStringList>
#include <QVariantMap>
#include <functional>

// The single-instance channel: a second launch forwards its command line here (a JSON array),
// and the browser's native host sends requests (a JSON object, answered with {"ok","error"}).
// One line per message. Only this Windows user can connect, and a message without a line end
// within kMaxLine bytes (or kIdleMs) is dropped.
class InstanceServer : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kMaxLine = 2 * 1024 * 1024;
    static constexpr int kIdleMs = 10000;

    explicit InstanceServer(QObject* parent = nullptr);
    bool listen(const QString& name);
    QString serverName() const { return m_server.serverName(); }
    QLocalServer::SocketOptions socketOptions() const { return m_server.socketOptions(); }

    // Browser request -> error text ("" = ok).
    void setRequestHandler(std::function<QString(const QVariantMap&)> fn) { m_onRequest = std::move(fn); }
    void setArgsHandler(std::function<void(const QStringList&)> fn) { m_onArgs = std::move(fn); }

private:
    void onConnection();
    QLocalServer m_server;
    std::function<QString(const QVariantMap&)> m_onRequest;
    std::function<void(const QStringList&)> m_onArgs;
};
