#pragma once
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>

// What the MP3/MP4 converter should produce (FDM: mp3Converter* / mp4Converter* settings).
struct ConvertOptions {
    QString format = QStringLiteral("mp3"); // "mp3" | "mp4"
    bool vbr = false;                        // MP3: variable bitrate (LAME -V) instead of constant
    int bitrateKbps = 192;                   // MP3 constant bitrate
    int vbrQuality = 2;                      // MP3 VBR: 0 (best) .. 9 (smallest)
    QString outputDir;                       // "" = next to the original
};

// Runs ffmpeg in the background to convert one finished media file; the original is kept.
// MP4 first tries a lossless remux (seconds), then re-encodes with the best H.264 encoder this
// ffmpeg has (libx264 > h264_mf), then MPEG-4 as a last resort. MP3: libmp3lame > mp3_mf.
class MediaConvert : public QObject {
    Q_OBJECT
public:
    MediaConvert(const QString& ffmpeg, const QString& input, const ConvertOptions& opt, QObject* parent = nullptr);
    ~MediaConvert() override; // stops ffmpeg and removes a half-written output

    QString outputPath() const { return m_output; }
    void start();

    // Pure helpers (tested directly).
    static QStringList mp3Args(const QString& in, const QString& out, const ConvertOptions& o, const QSet<QString>& encoders);
    static QStringList mp4Plan(const QSet<QString>& encoders); // video codecs to try: "copy", ...
    static QStringList mp4Args(const QString& in, const QString& out, const QString& videoCodec);
    static double parseClock(const QByteArray& hhmmss); // "00:01:02.50" -> 62.5, -1 if invalid
    static QSet<QString> encoders(const QString& ffmpeg); // cached per ffmpeg path

signals:
    void progress(double fraction); // 0..1, or -1 while the duration is unknown
    void finished(const QString& output, const QString& error); // error empty on success

private:
    void run(const QStringList& args);
    void onOutput();
    void onFinished(int code, QProcess::ExitStatus status);

    QString m_ffmpeg, m_input, m_output;
    ConvertOptions m_opt;
    QPointer<QProcess> m_proc;
    QByteArray m_err;
    double m_duration = -1;
    QStringList m_plan; // MP4 attempts left
    bool m_done = false;
};
