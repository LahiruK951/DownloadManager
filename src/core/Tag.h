#pragma once
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// A user tag (FDM: tables tags + downloadsTagsDetails). Downloads whose file extension is in
// `extensions` get the tag automatically; `folder`, when set, is where those downloads go.
struct Tag {
    int id = 0;
    QString name;
    QString color;         // "#rrggbb"
    QStringList extensions; // lower case, without the dot
    QString folder;        // "" = no special folder

    bool matches(const QString& fileName) const
    {
        const QString ext = QFileInfo(fileName).suffix().toLower();
        return !ext.isEmpty() && extensions.contains(ext);
    }
    QVariantMap toMap() const
    {
        return {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("color"), color},
                {QStringLiteral("extensions"), extensions.join(QStringLiteral(", "))}, {QStringLiteral("folder"), folder}};
    }
    // "ISO, .img;  zip" -> {"iso", "img", "zip"}
    static QStringList parseExtensions(const QString& text)
    {
        QStringList out;
        for (QString e : text.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts)) {
            e = e.toLower();
            while (e.startsWith(QLatin1Char('.')) || e.startsWith(QLatin1Char('*')))
                e.remove(0, 1);
            if (!e.isEmpty() && !out.contains(e))
                out << e;
        }
        return out;
    }
};
