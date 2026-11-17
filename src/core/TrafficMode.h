#pragma once
#include <QString>

// FDM's "Traffic Usage Mode" (TUM): Low / Medium / High presets.
namespace TrafficMode {
enum Mode { Low = 0, Medium = 1, High = 2 };

struct Limits {
    int maxDownloads = 5;
    int maxConnections = 8;   // per download
    int speedLimitKBps = 0;   // 0 = unlimited
};

inline QString prefix(int mode) {
    switch (mode) {
    case Low: return QStringLiteral("Low");
    case Medium: return QStringLiteral("Medium");
    default: return QStringLiteral("High");
    }
}

inline Limits defaults(int mode) {
    switch (mode) {
    case Low: return {1, 4, 512};
    case Medium: return {3, 8, 0};
    default: return {6, 16, 0};
    }
}
} // namespace TrafficMode
