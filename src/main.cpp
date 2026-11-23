#include <QApplication>
#include <QDir>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>
#include <QSettings>
#include <QTranslator>
#include <QMessageBox>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

#include <curl/curl.h>

#include "core/AppLog.h"
#include "core/Database.h"
#include "core/DownloadManager.h"
#include "core/Instance.h"
#include "dev/DevDemo.h"
#include "engines/demo/DemoEngine.h"
#include "engines/dash/DashEngine.h"
#include "engines/sites/SiteEngine.h"
#include "engines/hls/HlsEngine.h"
#include "core/InstanceServer.h"
#include "ui/AppController.h"
#include "ui/AppIcon.h"
#include "ui/TrayIcon.h"
#ifdef FLUXDM_HAS_TORRENT
#include "engines/bt/BtEngine.h"
#endif

// FLUXDM_DATA_DIR selects a separate profile (dev/screenshots): own database,
// settings and single-instance channel, so it never touches the real one.
static QString profileDir() { return Instance::profileDir(); }
static QString instanceName() { return Instance::channelName(); }

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
    // UI zoom (FDM: settings.ini "zoom", percent) must be known before the application exists.
    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        const int zoom = QSettings(QDir(profileDir()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat)
                             .value(QStringLiteral("zoom"), 100).toInt();
        if (zoom != 100 && zoom >= 50 && zoom <= 300)
            qputenv("QT_SCALE_FACTOR", QByteArray::number(zoom / 100.0));
    }
    QApplication app(argc, argv);

    QStringList args = app.arguments().mid(1);
    if (args.removeAll(QStringLiteral("--restarted")) > 0) {
        // Started by "Restart now": wait for the old instance to let go of the channel.
        for (int i = 0; i < 50; ++i) {
            QLocalSocket probe;
            probe.connectToServer(instanceName());
            if (!probe.waitForConnected(100))
                break;
            probe.disconnectFromServer();
            QThread::msleep(100);
        }
    } else if (!args.contains(QStringLiteral("--uninstall-cleanup")) && !args.contains(QStringLiteral("--install-setup"))
               && forwardToRunningInstance(args)) {
        return 0;
    }

    // Language: the setting, else Windows' language; English when there is no translation.
    QTranslator translator;
    {
        const QString lang = QSettings(QDir(profileDir()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat)
                                 .value(QStringLiteral("language")).toString();
        const QLocale locale = lang.isEmpty() ? QLocale::system() : QLocale(lang);
        if (lang != QLatin1String("en") && translator.load(locale, QStringLiteral("fluxdm"), QStringLiteral("_"), QStringLiteral(":/i18n"))) {
            app.installTranslator(&translator);
            QLocale::setDefault(locale);
        }
    }

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    {
        QFont f(QStringLiteral("Segoe UI"));
        f.setPointSizeF(9.5);
        app.setFont(f); // explicit, so offscreen renders (dev screenshots) match the desktop
    }
    app.setWindowIcon(AppIcon::make());
    app.setQuitOnLastWindowClosed(false); // the tray keeps us alive
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const QString dataDir = profileDir();
    QDir().mkpath(dataDir);
    AppLog::init(QDir(dataDir).filePath(QStringLiteral("logs")));

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
#ifdef FLUXDM_HAS_TORRENT
        int btPort = db.setting(QStringLiteral("BtListenPort"), 0).toInt();
        if (btPort <= 0) { // stable random port, like FDM
            btPort = QRandomGenerator::global()->bounded(20000, 60000);
            db.setSetting(QStringLiteral("BtListenPort"), btPort);
        }
        // Dev profiles (screenshots) stay off the network: loopback only, no DHT.
        const bool offline = qEnvironmentVariableIsSet("FLUXDM_DATA_DIR") && !qEnvironmentVariableIsSet("FLUXDM_ONLINE");
        mgr.addEngine(new BtEngine(QDir(dataDir).filePath(QStringLiteral("bt")), offline ? 0 : btPort, offline));
#endif
        mgr.addEngine(new HlsEngine); // .m3u8 links: takes precedence over plain HTTP
        mgr.addEngine(new DashEngine); // .mpd links (FDM: downloadsfragments)
        mgr.addEngine(new SiteEngine); // YouTube & co. through yt-dlp (FDM: downloadsms)
        const bool demo = qEnvironmentVariableIsSet("FLUXDM_DEV_DEMO");
        if (demo)
            mgr.addEngine(new DemoEngine);
        mgr.load();
        if (demo)
            DevDemo::seed(mgr, QDir(dataDir).filePath(QStringLiteral("downloads")));
        AppController ctl(&mgr, dataDir);
        // Run by the uninstaller: undo what this copy registered for this Windows account.
        if (args.contains(QStringLiteral("--uninstall-cleanup"))) {
            ctl.uninstallCleanup();
            mgr.shutdown();
            AppLog::shutdown();
            return 0;
        }
        // Run by the installer for the options ticked there: "--install-setup assoc browser autostart".
        if (const qsizetype at = args.indexOf(QStringLiteral("--install-setup")); at >= 0) {
            const QStringList opts = args.mid(at + 1);
            if (opts.contains(QStringLiteral("assoc")))
                ctl.setTorrentAssociated(true);
            if (opts.contains(QStringLiteral("browser")))
                ctl.setBrowserIntegrated(true);
            if (opts.contains(QStringLiteral("autostart")))
                ctl.setStartWithWindows(true);
            mgr.shutdown();
            AppLog::shutdown();
            return 0;
        }
        AppController::s_instance = &ctl;
        TrayIcon tray(&ctl);

        InstanceServer server;
        server.setRequestHandler([&ctl](const QVariantMap& r) { return ctl.handleBrowserRequest(r); });
        server.setArgsHandler([&ctl](const QStringList& args) { ctl.handleExternalArgs(args); });

        QQmlApplicationEngine engine;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                         [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
        engine.loadFromModule("FluxDM", "Main");
        // Listen only once the UI exists, so a request that launched us (browser) isn't lost.
        if (!server.listen(instanceName()))
            qWarning() << "Cannot open the single-instance channel" << instanceName();

        // Developer aid: FLUXDM_DEV_GRAB=<png> saves a render of the window and exits.
        const QString grabPath = qEnvironmentVariable("FLUXDM_DEV_GRAB");
        if (!grabPath.isEmpty())
            QTimer::singleShot(qEnvironmentVariableIntValue("FLUXDM_DEV_GRAB_DELAY") > 0 ? qEnvironmentVariableIntValue("FLUXDM_DEV_GRAB_DELAY") : 3000,
                               &app, [&engine, grabPath] {
                // The main window, or the most recently opened one (separate download windows).
                QQuickWindow* w = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow*>(engine.rootObjects().first());
                if (qEnvironmentVariable("FLUXDM_DEV_OPEN") == QLatin1String("window"))
                    for (QWindow* tw : QGuiApplication::topLevelWindows())
                        if (auto* q = qobject_cast<QQuickWindow*>(tw); q && q != w && q->isVisible())
                            w = q;
                if (w)
                    w->grabWindow().save(grabPath);
                QCoreApplication::quit();
            });

        if (!args.isEmpty())
            QTimer::singleShot(300, &ctl, [&ctl, args] { ctl.handleExternalArgs(args); });

        QObject::connect(&app, &QCoreApplication::aboutToQuit, &mgr, [&mgr] { mgr.shutdown(); });
        rc = app.exec();
    }
    curl_global_cleanup();
    AppLog::shutdown(); // a clean exit: next start won't report a crash
    return rc;
}
