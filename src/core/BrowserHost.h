#pragma once
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

// Native messaging host manifests (the JSON a browser reads to find fluxdm-nativehost.exe).
// Chromium browsers allow callers by origin, Firefox by add-on ID; otherwise they're the same.
namespace BrowserHost {

inline QJsonObject chromiumManifest(const QString& name, const QString& hostExe, const QString& extensionId)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("description"), QStringLiteral("FluxDM download manager")},
            {QStringLiteral("path"), hostExe},
            {QStringLiteral("type"), QStringLiteral("stdio")},
            {QStringLiteral("allowed_origins"), QJsonArray{QStringLiteral("chrome-extension://%1/").arg(extensionId)}}};
}

inline QJsonObject firefoxManifest(const QString& name, const QString& hostExe, const QString& addonId)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("description"), QStringLiteral("FluxDM download manager")},
            {QStringLiteral("path"), hostExe},
            {QStringLiteral("type"), QStringLiteral("stdio")},
            {QStringLiteral("allowed_extensions"), QJsonArray{addonId}}};
}

// The Firefox add-on ID from its manifest.json (browser_specific_settings.gecko.id), "" if none.
inline QString geckoId(const QString& manifestPath)
{
    QFile f(manifestPath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("browser_specific_settings")).toObject()
        .value(QStringLiteral("gecko")).toObject().value(QStringLiteral("id")).toString();
}

} // namespace BrowserHost
