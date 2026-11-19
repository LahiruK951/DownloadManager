#include "core/MediaConvert.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>

#include "core/FileNamer.h"

MediaConvert::MediaConvert(const QString& ffmpeg, const QString& input, const ConvertOptions& opt, QObject* parent)
    : QObject(parent), m_ffmpeg(ffmpeg), m_input(input), m_opt(opt)
{
    const QFileInfo fi(input);
    const QString dir = opt.outputDir.isEmpty() ? fi.absolutePath() : opt.outputDir;
    QDir().mkpath(dir);
    m_output = FileNamer::uniquePath(dir, fi.completeBaseName() + QLatin1Char('.') + opt.format);
}

MediaConvert::~MediaConvert()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(3000);
        QFile::remove(m_output);
    }
}

QSet<QString> MediaConvert::encoders(const QString& ffmpeg)
{
    static QMutex lock;
    static QHash<QString, QSet<QString>> cache;
    QMutexLocker guard(&lock);
    if (cache.contains(ffmpeg))
        return cache.value(ffmpeg);
    QProcess p;
    p.start(ffmpeg, {QStringLiteral("-hide_banner"), QStringLiteral("-encoders")});
    QSet<QString> found;
    if (p.waitForFinished(10000)) {
        // lines look like " A..... libmp3lame           libmp3lame MP3 ..."
        static const QRegularExpression line(QStringLiteral(R"(^\s*[VAS][\w.]{5}\s+(\S+))"));
        for (const QByteArray& l : p.readAllStandardOutput().split('\n')) {
            const auto m = line.match(QString::fromLatin1(l));
            if (m.hasMatch())
                found.insert(m.captured(1));
        }
    }
    cache.insert(ffmpeg, found);
    return found;
}

QStringList MediaConvert::mp3Args(const QString& in, const QString& out, const ConvertOptions& o, const QSet<QString>& enc)
{
    QStringList a{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-i"), in,
                  QStringLiteral("-vn"), QStringLiteral("-map"), QStringLiteral("0:a:0")};
    if (enc.contains(QStringLiteral("libmp3lame")) || enc.isEmpty()) {
        a << QStringLiteral("-c:a") << QStringLiteral("libmp3lame");
        if (o.vbr)
            a << QStringLiteral("-q:a") << QString::number(qBound(0, o.vbrQuality, 9));
        else
            a << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(qBound(32, o.bitrateKbps, 320));
    } else { // Media Foundation's encoder: constant bitrate only
        a << QStringLiteral("-c:a") << QStringLiteral("mp3_mf") << QStringLiteral("-b:a")
          << QStringLiteral("%1k").arg(o.vbr ? 192 : qBound(32, o.bitrateKbps, 320));
    }
    a << QStringLiteral("-progress") << QStringLiteral("pipe:1") << QStringLiteral("-nostats") << out;
    return a;
}

QStringList MediaConvert::mp4Plan(const QSet<QString>& enc)
{
    QStringList plan{QStringLiteral("copy")};
    if (enc.contains(QStringLiteral("libx264")))
        plan << QStringLiteral("libx264");
    else if (enc.contains(QStringLiteral("h264_mf")))
        plan << QStringLiteral("h264_mf");
    plan << QStringLiteral("mpeg4");
    return plan;
}

QStringList MediaConvert::mp4Args(const QString& in, const QString& out, const QString& codec)
{
    QStringList a{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-i"), in,
                  QStringLiteral("-map"), QStringLiteral("0:v:0?"), QStringLiteral("-map"), QStringLiteral("0:a:0?")};
    if (codec == QLatin1String("copy")) {
        a << QStringLiteral("-c") << QStringLiteral("copy");
    } else {
        a << QStringLiteral("-c:v") << codec;
        if (codec == QLatin1String("libx264"))
            a << QStringLiteral("-preset") << QStringLiteral("veryfast") << QStringLiteral("-crf") << QStringLiteral("22");
        else if (codec == QLatin1String("h264_mf"))
            a << QStringLiteral("-b:v") << QStringLiteral("4M");
        else
            a << QStringLiteral("-q:v") << QStringLiteral("4");
        a << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-c:a") << QStringLiteral("aac")
          << QStringLiteral("-b:a") << QStringLiteral("160k");
    }
    a << QStringLiteral("-movflags") << QStringLiteral("+faststart") << QStringLiteral("-progress") << QStringLiteral("pipe:1")
      << QStringLiteral("-nostats") << out;
    return a;
}

double MediaConvert::parseClock(const QByteArray& t)
{
    const QList<QByteArray> p = t.trimmed().split(':');
    if (p.size() != 3)
        return -1;
    bool a = false, b = false, c = false;
    const double v = p[0].toDouble(&a) * 3600 + p[1].toDouble(&b) * 60 + p[2].toDouble(&c);
    return a && b && c ? v : -1;
}

void MediaConvert::start()
{
    const QSet<QString> enc = encoders(m_ffmpeg);
    if (m_opt.format == QLatin1String("mp4")) {
        m_plan = mp4Plan(enc);
        run(mp4Args(m_input, m_output, m_plan.takeFirst()));
    } else {
        run(mp3Args(m_input, m_output, m_opt, enc));
    }
}

void MediaConvert::run(const QStringList& args)
{
    m_err.clear();
    auto* p = new QProcess(this);
    m_proc = p;
    connect(p, &QProcess::readyReadStandardOutput, this, &MediaConvert::onOutput);
    connect(p, &QProcess::readyReadStandardError, this, [this, p] {
        m_err += p->readAllStandardError();
        if (m_duration < 0) {
            static const QRegularExpression dur(QStringLiteral(R"(Duration:\s*(\d+:\d+:\d+(?:\.\d+)?))"));
            const auto m = dur.match(QString::fromLatin1(m_err));
            if (m.hasMatch())
                m_duration = parseClock(m.captured(1).toLatin1());
        }
    });
    connect(p, &QProcess::finished, this, &MediaConvert::onFinished);
    connect(p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && !m_done) {
            m_done = true;
            emit finished({}, tr("Cannot start ffmpeg (%1)").arg(m_ffmpeg));
        }
    });
    emit progress(m_duration > 0 ? 0 : -1);
    p->start(m_ffmpeg, args);
}

void MediaConvert::onOutput()
{
    // "-progress pipe:1" prints key=value blocks; out_time_us is microseconds written so far.
    const QByteArray chunk = m_proc->readAllStandardOutput();
    for (const QByteArray& line : chunk.split('\n')) {
        if (line.startsWith("out_time_us=") || line.startsWith("out_time_ms=")) { // both are µs in ffmpeg
            const double secs = line.mid(line.indexOf('=') + 1).trimmed().toDouble() / 1e6;
            if (m_duration > 0 && secs >= 0)
                emit progress(qBound(0.0, secs / m_duration, 1.0));
        }
    }
}

void MediaConvert::onFinished(int code, QProcess::ExitStatus status)
{
    if (m_proc)
        m_proc->deleteLater();
    m_proc = nullptr;
    if (m_done)
        return;
    const bool ok = status == QProcess::NormalExit && code == 0 && QFileInfo(m_output).size() > 0;
    if (!ok && !m_plan.isEmpty()) {
        // The streams don't fit MP4 as they are (or that encoder failed here): next attempt.
        QFile::remove(m_output);
        run(mp4Args(m_input, m_output, m_plan.takeFirst()));
        return;
    }
    m_done = true;
    if (ok) {
        emit progress(1.0);
        emit finished(m_output, {});
        return;
    }
    QFile::remove(m_output);
    QString err;
    for (const QByteArray& l : m_err.split('\n')) // last meaningful line of ffmpeg's log
        if (!l.trimmed().isEmpty())
            err = QString::fromLocal8Bit(l).trimmed();
    emit finished({}, err.isEmpty() ? tr("ffmpeg stopped with code %1").arg(code) : err);
}
