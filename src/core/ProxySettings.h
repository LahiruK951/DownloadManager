#pragma once
#include <QNetworkProxy>
#include <QString>
#include <QUrl>
#include <QVariantMap>

// How downloads reach the internet (FDM: Settings > Network > Proxy).
struct ProxySettings {
    enum Mode { System = 0, None = 1, Manual = 2 };
    int mode = System;
    QString type = QStringLiteral("http"); // "http" | "socks5"
    QString host;
    int port = 8080;
    QString user;
    QString password;

    bool manualValid() const { return mode == Manual && !host.trimmed().isEmpty() && port > 0 && port < 65536; }

    // libcurl CURLOPT_PROXY value: "" = leave curl's default (system/env), "none" = force direct,
    // else "http://user:pass@host:port" / "socks5h://..." (SOCKS: names resolved by the proxy).
    QString curlProxy() const
    {
        if (mode == None)
            return QStringLiteral("none");
        if (!manualValid())
            return {};
        QUrl u;
        u.setScheme(type == QLatin1String("socks5") ? QStringLiteral("socks5h") : QStringLiteral("http"));
        u.setHost(host.trimmed());
        u.setPort(port);
        if (!user.isEmpty()) {
            u.setUserName(user);
            u.setPassword(password);
        }
        return u.toString(QUrl::FullyEncoded);
    }

    // For Qt's network stack (HLS, DASH, .torrent files).
    QNetworkProxy qtProxy() const
    {
        if (mode == None)
            return QNetworkProxy(QNetworkProxy::NoProxy);
        if (!manualValid())
            return QNetworkProxy(QNetworkProxy::DefaultProxy);
        return QNetworkProxy(type == QLatin1String("socks5") ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                             host.trimmed(), quint16(port), user, password);
    }

    QVariantMap toMap() const // without the password
    {
        return {{QStringLiteral("mode"), mode}, {QStringLiteral("type"), type}, {QStringLiteral("host"), host},
                {QStringLiteral("port"), port}, {QStringLiteral("user"), user},
                {QStringLiteral("hasPassword"), !password.isEmpty()}};
    }
};
