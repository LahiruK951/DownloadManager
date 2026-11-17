#pragma once
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

class AppController;

class TrayIcon : public QObject {
    Q_OBJECT
public:
    explicit TrayIcon(AppController* app, QObject* parent = nullptr);

private:
    void updateTooltip();
    AppController* m_app;
    QSystemTrayIcon m_tray;
    QMenu m_menu;
    QString m_lastFile;
};
