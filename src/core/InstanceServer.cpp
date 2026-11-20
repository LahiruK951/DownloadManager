#include "core/InstanceServer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>

InstanceServer::InstanceServer(QObject* parent)
    : QObject(parent)
{
    m_server.setSocketOptions(QLocalServer::UserAccessOption); // not other users on this computer
    connect(&m_server, &QLocalServer::newConnection, this, &InstanceServer::onConnection);
}

bool InstanceServer::listen(const QString& name)
{
    QLocalServer::removeServer(name);
    return m_server.listen(name);
}

void InstanceServer::onConnection()
{
    while (QLocalSocket* c = m_server.nextPendingConnection()) {
        connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
        auto* idle = new QTimer(c);
        idle->setSingleShot(true);
        connect(idle, &QTimer::timeout, c, [c] { c->abort(); });
        idle->start(kIdleMs);
        connect(c, &QLocalSocket::readyRead, c, [this, c, idle] {
            if (!c->canReadLine()) {
                if (c->bytesAvailable() > kMaxLine)
                    c->abort(); // no line end in sight: not one of ours
                return;
            }
            idle->stop();
            const QJsonDocument doc = QJsonDocument::fromJson(c->readLine(kMaxLine));
            if (doc.isObject()) { // browser integration: {"cmd":"download", ...}
                const QString reply = m_onRequest ? m_onRequest(doc.object().toVariantMap()) : QStringLiteral("Not ready");
                c->write(QJsonDocument(QJsonObject{{QStringLiteral("ok"), reply.isEmpty()}, {QStringLiteral("error"), reply}})
                             .toJson(QJsonDocument::Compact) + '\n');
                c->flush();
            } else if (doc.isArray()) { // a second instance forwarding its command line
                QStringList list;
                for (const auto& v : doc.array())
                    list << v.toString();
                if (m_onArgs)
                    m_onArgs(list);
            }
            c->disconnectFromServer();
        });
    }
}
