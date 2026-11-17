#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTimer>

#include <curl/curl.h>

#include "core/Database.h"
#include "core/DownloadManager.h"
#include "ui/AppController.h"
#include "ui/AppIcon.h"
#include "ui/TrayIcon.h"

static QString instanceName()
{
    return QStringLiteral("FluxDM-") + qEnvironmentVariable("USERNAME", QStringLiteral("user"));
}

// Forward our arguments to an already running instance. Returns true if one was found.
static bool forwardToRunningInstance(const QStringList& args)
{
    QLocalSocket s;
    s.connectToServer(instanceName());
    if (!s.waitForConnected(300))
        return false;
    s.write(QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact) + '\n');
    s.waitForBytesWritten(1000);
    s.disconnectFromServer();
    return true;
}

int main(int argc, char* argv[])
{
    QApplication::setOrganizationName(QStringLiteral("FluxDM"));
    QApplication::setApplicationName(QStringLiteral("FluxDM"));
    QApplication::setApplicationVersion(QStringLiteral(FLUXDM_VERSION));
    QApplication app(argc, argv);

    const QStringList args = app.arguments().mid(1);
    if (forwardToRunningInstance(args))
        return 0;

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    app.setWindowIcon(AppIcon::make());
    app.setQuitOnLastWindowClosed(false); // the tray keeps us alive
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDir);

    Database db;
    QString err;
    if (!db.open(dataDir, &err)) {
        QMessageBox::critical(nullptr, QStringLiteral("FluxDM"), QStringLiteral("Cannot open the database:\n%1").arg(err));
        return 1;
    }
    db.backup();

    int rc = 0;
    {
        DownloadManager mgr(&db);
        mgr.load();
        AppController ctl(&mgr, dataDir);
        AppController::s_instance = &ctl;
        TrayIcon tray(&ctl);

        QLocalServer server;
        QLocalServer::removeServer(instanceName());
        server.listen(instanceName());
        QObject::connect(&server, &QLocalServer::newConnection, &server, [&server, &ctl] {
            while (QLocalSocket* c = server.nextPendingConnection()) {
                QObject::connect(c, &QLocalSocket::readyRead, c, [c, &ctl] {
                    if (!c->canReadLine())
                        return;
                    QStringList list;
                    for (const auto& v : QJsonDocument::fromJson(c->readLine()).array())
                        list << v.toString();
                    ctl.handleExternalArgs(list);
                    c->disconnectFromServer();
                });
                QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
            }
        });

        QQmlApplicationEngine engine;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                         [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
        engine.loadFromModule("FluxDM", "Main");

        if (!args.isEmpty())
            QTimer::singleShot(300, &ctl, [&ctl, args] { ctl.handleExternalArgs(args); });

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &mgr, [&mgr] { mgr.shutdown(); });
        rc = app.exec();
    }
    curl_global_cleanup();
    return rc;
}
