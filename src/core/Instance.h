#pragma once
#include <QHash>
#include <QStandardPaths>
#include <QString>

// Profile directory and single-instance channel name, shared by the app and
// the browser native-messaging host so both find each other.
namespace Instance {

// FLUXDM_DATA_DIR selects a separate profile (dev/screenshots/tests).
inline QString profileDir()
{
    const QString custom = qEnvironmentVariable("FLUXDM_DATA_DIR");
    if (!custom.isEmpty())
        return custom;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/FluxDM/FluxDM");
}

inline QString channelName()
{
    QString name = QStringLiteral("FluxDM-") + qEnvironmentVariable("USERNAME", QStringLiteral("user"));
    if (qEnvironmentVariableIsSet("FLUXDM_DATA_DIR"))
        name += QLatin1Char('-') + QString::number(qHash(profileDir()), 16);
    return name;
}

inline const char* kNativeHostName = "com.fluxdm.nativehost";

} // namespace Instance
