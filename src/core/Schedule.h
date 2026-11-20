#pragma once
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// A weekly time window (FDM: downloadsScheduleRecords days/fromTime/toTime).
// days: bit 0 = Monday ... bit 6 = Sunday. Times are minutes after midnight.
// from == to means the whole day; from > to crosses midnight (e.g. 23:00-07:00).
struct ScheduleWindow {
    int days = 0x7F;
    int fromMin = 0;
    int toMin = 0;

    static bool dayOn(int days, const QDate& d) { return days & (1 << (d.dayOfWeek() - 1)); }

    bool contains(const QDateTime& t) const
    {
        const int m = t.time().hour() * 60 + t.time().minute();
        const QDate today = t.date();
        if (fromMin == toMin)
            return dayOn(days, today);
        if (fromMin < toMin)
            return dayOn(days, today) && m >= fromMin && m < toMin;
        return (dayOn(days, today) && m >= fromMin) || (dayOn(days, today.addDays(-1)) && m < toMin);
    }

    static QString hhmm(int min) { return QStringLiteral("%1:%2").arg(min / 60, 2, 10, QLatin1Char('0')).arg(min % 60, 2, 10, QLatin1Char('0')); }

    QString text() const
    {
        // Translatable (context "Schedule"); QT_TRANSLATE_NOOP lets lupdate find the day names.
        static const char* names[] = {QT_TRANSLATE_NOOP("Schedule", "Mon"), QT_TRANSLATE_NOOP("Schedule", "Tue"),
                                      QT_TRANSLATE_NOOP("Schedule", "Wed"), QT_TRANSLATE_NOOP("Schedule", "Thu"),
                                      QT_TRANSLATE_NOOP("Schedule", "Fri"), QT_TRANSLATE_NOOP("Schedule", "Sat"),
                                      QT_TRANSLATE_NOOP("Schedule", "Sun")};
        QString d;
        if ((days & 0x7F) == 0x7F)
            d = QCoreApplication::translate("Schedule", "Every day");
        else if ((days & 0x7F) == 0x1F)
            d = QCoreApplication::translate("Schedule", "Weekdays");
        else if ((days & 0x7F) == 0x60)
            d = QCoreApplication::translate("Schedule", "Weekends");
        else {
            QStringList l;
            for (int i = 0; i < 7; ++i)
                if (days & (1 << i))
                    l << QCoreApplication::translate("Schedule", names[i]);
            d = l.join(QLatin1Char(' '));
        }
        if (fromMin == toMin)
            return d;
        return d + QLatin1Char(' ') + hhmm(fromMin) + QStringLiteral("–") + hhmm(toMin);
    }

    QVariantMap toMap() const
    {
        return {{QStringLiteral("days"), days}, {QStringLiteral("from"), fromMin}, {QStringLiteral("to"), toMin}};
    }
    static ScheduleWindow fromMap(const QVariantMap& m)
    {
        ScheduleWindow w;
        w.days = m.value(QStringLiteral("days"), 0x7F).toInt() & 0x7F;
        w.fromMin = qBound(0, m.value(QStringLiteral("from"), 0).toInt(), 24 * 60 - 1);
        w.toMin = qBound(0, m.value(QStringLiteral("to"), 0).toInt(), 24 * 60 - 1);
        return w;
    }
};

// "Use <mode> during <window>" — e.g. snail (Low) mode on weekdays 09:00–18:00.
struct TrafficRule {
    bool enabled = true;
    ScheduleWindow window;
    int mode = 0;

    QVariantMap toMap() const
    {
        QVariantMap m = window.toMap();
        m[QStringLiteral("enabled")] = enabled;
        m[QStringLiteral("mode")] = mode;
        return m;
    }
    static TrafficRule fromMap(const QVariantMap& m)
    {
        TrafficRule r;
        r.window = ScheduleWindow::fromMap(m);
        r.enabled = m.value(QStringLiteral("enabled"), true).toBool();
        r.mode = qBound(0, m.value(QStringLiteral("mode"), 0).toInt(), 2);
        return r;
    }
};
