#include "ui/TrayIcon.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>

#include "ui/AppController.h"
#include "ui/AppIcon.h"

TrayIcon::TrayIcon(AppController* app, QObject* parent)
    : QObject(parent), m_app(app)
{
    m_tray.setIcon(AppIcon::make());
    m_tray.setToolTip(QStringLiteral("FluxDM"));

    m_menu.addAction(tr("Show FluxDM"), app, &AppController::requestShow);
    m_menu.addAction(tr("Add download…"), app, [app] { emit app->addDialogRequested(app->clipboardUrls()); });
    m_menu.addSeparator();
    m_menu.addAction(tr("Resume all"), app, &AppController::resumeAll);
    m_menu.addAction(tr("Pause all"), app, &AppController::pauseAll);
    m_menu.addSeparator();
    m_menu.addAction(tr("Exit"), qApp, &QCoreApplication::quit);
    m_tray.setContextMenu(&m_menu);

    connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
            m_app->requestShow();
    });
    connect(&m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
        if (!m_lastFile.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastFile));
        else
            m_app->requestShow();
    });
    connect(app, &AppController::downloadCompleted, this, [this](const QString& title, const QString& path) {
        if (!m_app->notifyOnComplete())
            return;
        m_lastFile = path;
        m_tray.showMessage(tr("Download complete"), title, QSystemTrayIcon::Information, 5000);
    });
    connect(app, &AppController::hiddenToTray, this, [this] {
        m_lastFile.clear();
        m_tray.showMessage(QStringLiteral("FluxDM"), tr("FluxDM keeps running in the notification area."),
                           QSystemTrayIcon::Information, 3000);
    });
    connect(app, &AppController::statsChanged, this, &TrayIcon::updateTooltip);
    m_tray.show();
}

void TrayIcon::updateTooltip()
{
    if (m_app->activeCount() == 0)
        m_tray.setToolTip(QStringLiteral("FluxDM"));
    else
        m_tray.setToolTip(tr("FluxDM — %1 active, %2").arg(m_app->activeCount()).arg(m_app->fmtSpeed(m_app->totalSpeed())));
}
