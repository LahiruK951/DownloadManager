#pragma once
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkReply;

// "Check for updates": reads a manifest {version, url, sha256, signature, notes} (written by
// scripts\package.ps1 -Installer as latest.json), downloads the installer and only accepts it
// if its SHA-256 matches. When a release public key is built in, the manifest must also carry a
// valid Ed25519 signature of version + SHA-256 (ReleaseSignature). HTTPS only (plain http just
// for localhost, i.e. tests). Each download goes to its own new temporary folder.
class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(const QString& currentVersion, QObject* parent = nullptr);

    void check(const QUrl& manifestUrl);
    void download(); // the installer of the last check that found an update
    QString version() const { return m_version; }
    QString notes() const { return m_notes; }

    static int compareVersions(const QString& a, const QString& b); // <0, 0, >0 ("1.10" > "1.9")
    static bool allowedUrl(const QUrl& url);
    // Starts the installer quietly; it closes FluxDM, updates and starts it again.
    static bool install(const QString& installerPath);
    // The key manifests must be signed with; "" = signatures not required. Default: built in.
    void setPublicKey(const QByteArray& base64) { m_publicKey = base64; }

signals:
    void checked(bool updateAvailable, const QString& version, const QString& notes);
    void progress(qint64 received, qint64 total);
    void ready(const QString& installerPath);
    void failed(const QString& error);

private:
    QNetworkAccessManager m_nam;
    QString m_current, m_version, m_notes;
    QByteArray m_sha;
    QByteArray m_publicKey = FLUXDM_UPDATE_PUBLIC_KEY;
    QUrl m_installer;
    QPointer<QNetworkReply> m_reply;
};
