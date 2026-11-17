#include "engines/bt/BtEngine.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>

namespace lt = libtorrent;

namespace {
QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }

QByteArray toBytes(const std::vector<char>& v) { return QByteArray(v.data(), qsizetype(v.size())); }

constexpr lt::resume_data_flags_t kResumeFlags = lt::torrent_handle::save_info_dict;
} // namespace

BtEngine::BtEngine(const QString& stateDir, int listenPort, QObject* parent)
    : DownloadEngine(parent), m_stateDir(stateDir)
{
    QDir().mkpath(stateDir);

    lt::session_params params;
    QFile state(QDir(stateDir).filePath(QStringLiteral("session.dat")));
    if (state.open(QIODevice::ReadOnly)) {
        const QByteArray buf = state.readAll();
        try {
            params = lt::read_session_params(lt::span<char const>(buf.constData(), buf.size()), lt::session_handle::save_dht_state);
        } catch (...) {
        }
    }
    lt::settings_pack& sp = params.settings;
    const QString port = QString::number(listenPort);
    sp.set_str(lt::settings_pack::listen_interfaces, ss(QStringLiteral("0.0.0.0:%1,[::]:%1").arg(port)));
    sp.set_int(lt::settings_pack::alert_mask,
               lt::alert_category::status | lt::alert_category::error | lt::alert_category::storage);
    sp.set_bool(lt::settings_pack::enable_dht, true);
    sp.set_bool(lt::settings_pack::enable_lsd, true);
    sp.set_bool(lt::settings_pack::enable_upnp, true);
    sp.set_bool(lt::settings_pack::enable_natpmp, true);
    sp.set_str(lt::settings_pack::user_agent, "FluxDM/" FLUXDM_VERSION);
    sp.set_str(lt::settings_pack::peer_fingerprint, lt::generate_fingerprint("FX", 0, 1, 0));
    sp.set_str(lt::settings_pack::dht_bootstrap_nodes,
               "dht.libtorrent.org:25401,router.bittorrent.com:6881,router.utorrent.com:6881,dht.transmissionbt.com:6881");
    // The download manager does the queueing (traffic modes, priorities, schedules).
    sp.set_int(lt::settings_pack::active_downloads, -1);
    sp.set_int(lt::settings_pack::active_seeds, -1);
    sp.set_int(lt::settings_pack::active_limit, -1);
    m_ses = std::make_unique<lt::session>(std::move(params));

    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &BtEngine::poll);
    m_timer.start();
}

BtEngine::~BtEngine() { shutdown(); }

int BtEngine::listenPort() const { return m_ses ? m_ses->listen_port() : 0; }

bool BtEngine::canHandle(const QUrl& url) const
{
    const QString s = url.scheme().toLower();
    if (s == QLatin1String("magnet"))
        return url.toString().contains(QLatin1String("xt=urn:bt"), Qt::CaseInsensitive);
    if (url.isLocalFile())
        return url.toLocalFile().endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive) && QFileInfo::exists(url.toLocalFile());
    if (s == QLatin1String("http") || s == QLatin1String("https"))
        return url.path().endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive);
    return false;
}

// ------------------------------------------------------------------ start / stop

void BtEngine::start(const TaskParams& p)
{
    if (m_down)
        return;
    removeTask(p.id); // a restart of a known task
    Task& t = m_tasks[p.id];
    t.id = p.id;
    t.destDir = p.destDir;
    t.seedOnly = p.seedOnly;
    t.lastResume = p.resumeData;

    lt::error_code ec;
    if (!p.resumeData.isEmpty()) {
        lt::add_torrent_params atp = lt::read_resume_data(lt::span<char const>(p.resumeData.constData(), p.resumeData.size()), ec);
        if (!ec) {
            if (t.seedOnly && atp.ti && !QFileInfo::exists(QDir(QString::fromStdString(atp.save_path)).filePath(qs(atp.ti->name())))) {
                emit log(p.id, tr("Files are gone; not seeding"));
                m_tasks.erase(p.id);
                emit stopped(p.id, {});
                return;
            }
            emit log(p.id, t.seedOnly ? tr("Resuming seeding") : tr("Resuming torrent"));
            addTorrent(t, std::move(atp));
            return;
        }
        emit log(p.id, tr("Resume data unusable (%1); starting over").arg(qs(ec.message())));
    }
    if (t.seedOnly) { // nothing to seed from
        m_tasks.erase(p.id);
        emit stopped(p.id, {});
        return;
    }

    const QString url = p.url.trimmed();
    if (url.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
        lt::add_torrent_params atp = lt::parse_magnet_uri(ss(url), ec);
        if (ec)
            return failTask(t, tr("Invalid magnet link: %1").arg(qs(ec.message())));
        emit log(p.id, tr("Looking for peers to fetch the torrent information…"));
        addTorrent(t, std::move(atp));
        return;
    }
    if (url.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
        QNetworkRequest req{QUrl(url)};
        req.setHeader(QNetworkRequest::UserAgentHeader, p.userAgent);
        if (!p.referrer.isEmpty())
            req.setRawHeader("Referer", p.referrer.toUtf8());
        if (!p.cookies.isEmpty())
            req.setRawHeader("Cookie", p.cookies.toUtf8());
        emit log(p.id, tr("Downloading .torrent file %1").arg(url));
        QNetworkReply* reply = m_nam.get(req);
        t.fetch = reply;
        const qint64 id = p.id;
        connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
            reply->deleteLater();
            auto it = m_tasks.find(id);
            if (it == m_tasks.end() || it->second.fetch != reply)
                return;
            Task& task = it->second;
            task.fetch = nullptr;
            if (reply->error() != QNetworkReply::NoError)
                return failTask(task, tr("Cannot download the .torrent file: %1").arg(reply->errorString()));
            const QByteArray buf = reply->readAll();
            try {
                addTorrent(task, lt::load_torrent_buffer(lt::span<char const>(buf.constData(), buf.size())));
            } catch (const std::exception& e) {
                failTask(task, tr("Not a valid .torrent file: %1").arg(QString::fromLocal8Bit(e.what())));
            }
        });
        return;
    }
    // local .torrent file
    const QString path = url.startsWith(QLatin1String("file:"), Qt::CaseInsensitive) ? QUrl(url).toLocalFile() : url;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return failTask(t, tr("Cannot open %1").arg(QDir::toNativeSeparators(path)));
    const QByteArray buf = f.readAll();
    try {
        addTorrent(t, lt::load_torrent_buffer(lt::span<char const>(buf.constData(), buf.size())));
    } catch (const std::exception& e) {
        failTask(t, tr("Not a valid .torrent file: %1").arg(QString::fromLocal8Bit(e.what())));
    }
}

void BtEngine::addTorrent(Task& t, lt::add_torrent_params atp)
{
    if (atp.save_path.empty())
        atp.save_path = ss(QDir::toNativeSeparators(t.destDir));
    atp.flags &= ~(lt::torrent_flags::auto_managed | lt::torrent_flags::paused | lt::torrent_flags::stop_when_ready);
    if (t.seedOnly)
        atp.flags |= lt::torrent_flags::upload_mode; // never download more in seed-only mode
    lt::error_code ec;
    t.h = m_ses->add_torrent(std::move(atp), ec);
    if (ec) {
        const QString msg = ec == lt::errors::duplicate_torrent ? tr("This torrent is already being downloaded")
                                                                 : qs(ec.message());
        failTask(t, msg);
        return;
    }
    if (t.h.torrent_file())
        emitMeta(t);
}

void BtEngine::stop(qint64 id)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end()) {
        emit stopped(id, {});
        return;
    }
    Task& t = it->second;
    if (t.fetch) {
        t.fetch->abort();
        m_tasks.erase(it);
        emit stopped(id, {});
        return;
    }
    if (!t.h.is_valid()) {
        m_tasks.erase(it);
        emit stopped(id, t.lastResume);
        return;
    }
    t.stopping = true;
    t.h.pause();
    t.h.save_resume_data(kResumeFlags); // stopped() is emitted when the data arrives
}

void BtEngine::removeTask(qint64 id, bool removeFromSession)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end())
        return;
    if (it->second.fetch)
        it->second.fetch->abort();
    if (removeFromSession && it->second.h.is_valid())
        m_ses->remove_torrent(it->second.h);
    m_tasks.erase(it);
}

void BtEngine::failTask(Task& t, const QString& error)
{
    const qint64 id = t.id;
    const QByteArray resume = t.lastResume;
    emit log(id, tr("Error: %1").arg(error));
    removeTask(id);
    emit failed(id, error, resume);
}

BtEngine::Task* BtEngine::byHandle(const lt::torrent_handle& h)
{
    for (auto& [id, t] : m_tasks)
        if (t.h == h)
            return &t;
    return nullptr;
}

QString BtEngine::finalPath(const Task& t) const
{
    const auto ti = t.h.torrent_file();
    const lt::torrent_status st = t.h.status(lt::torrent_handle::query_save_path | lt::torrent_handle::query_name);
    const QString name = ti ? qs(ti->name()) : qs(st.name);
    return QDir(qs(st.save_path)).filePath(name);
}

void BtEngine::emitMeta(Task& t)
{
    if (t.metaEmitted)
        return;
    const auto ti = t.h.torrent_file();
    if (!ti)
        return;
    t.metaEmitted = true;
    emit log(t.id, tr("Torrent: %1 — %2 file(s), %3 pieces of %4 KB")
                       .arg(qs(ti->name()))
                       .arg(ti->num_files())
                       .arg(ti->num_pieces())
                       .arg(ti->piece_length() / 1024));
    emit meta(t.id, qs(ti->name()), finalPath(t), ti->total_size(), true);
    t.h.save_resume_data(kResumeFlags); // persist the info dict right away (magnet → no refetch)
}

// ------------------------------------------------------------------ polling

void BtEngine::poll()
{
    if (m_down)
        return;
    handleAlerts();
    ++m_tick;
    emitStats(m_tick % 4 == 0); // pieces/files every 2 s
    if (m_tick % 20 == 0)      // every 10 s
        for (auto& [id, t] : m_tasks)
            if (t.h.is_valid() && !t.stopping && t.h.need_save_resume_data())
                t.h.save_resume_data(kResumeFlags);
}

void BtEngine::handleAlerts()
{
    std::vector<lt::alert*> alerts;
    m_ses->pop_alerts(&alerts);
    for (lt::alert* a : alerts) {
        if (auto* ra = lt::alert_cast<lt::save_resume_data_alert>(a)) {
            Task* t = byHandle(ra->handle);
            if (!t)
                continue;
            t->lastResume = toBytes(lt::write_resume_data_buf(ra->params));
            if (t->stopping) {
                const qint64 id = t->id;
                const QByteArray rd = t->lastResume;
                removeTask(id);
                emit stopped(id, rd);
            } else {
                emit resumeData(t->id, t->lastResume);
            }
        } else if (auto* fa = lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
            Task* t = byHandle(fa->handle);
            if (t && t->stopping) {
                const qint64 id = t->id;
                const QByteArray rd = t->lastResume;
                removeTask(id);
                emit stopped(id, rd);
            }
        } else if (auto* ma = lt::alert_cast<lt::metadata_received_alert>(a)) {
            if (Task* t = byHandle(ma->handle))
                emitMeta(*t);
        } else if (auto* fin = lt::alert_cast<lt::torrent_finished_alert>(a)) {
            Task* t = byHandle(fin->handle);
            if (!t || t->seedOnly || t->finishedEmitted || !t->metaEmitted)
                continue;
            // Pieces pass the hash check while still in libtorrent's store buffer:
            // report completion only once everything is flushed to disk.
            t->finishedEmitted = true;
            t->h.flush_cache();
        } else if (auto* cf = lt::alert_cast<lt::cache_flushed_alert>(a)) {
            Task* t = byHandle(cf->handle);
            if (!t || !t->finishedEmitted || t->finishReported)
                continue;
            t->finishReported = true;
            emit log(t->id, tr("Download complete; seeding"));
            t->h.save_resume_data(kResumeFlags);
            emit finished(t->id, finalPath(*t));
        } else if (auto* ea = lt::alert_cast<lt::torrent_error_alert>(a)) {
            if (Task* t = byHandle(ea->handle))
                failTask(*t, qs(ea->error.message()));
        } else if (auto* fe = lt::alert_cast<lt::file_error_alert>(a)) {
            if (Task* t = byHandle(fe->handle))
                failTask(*t, tr("%1: %2").arg(QString::fromUtf8(fe->filename()), qs(fe->error.message())));
        }
    }
}

void BtEngine::emitStats(bool full)
{
    std::vector<qint64> seedDone;
    for (auto& [id, t] : m_tasks) {
        if (!t.h.is_valid() || t.stopping)
            continue;
        const lt::torrent_status st = t.h.status(full ? lt::torrent_handle::query_pieces : lt::status_flags_t{});
        TaskStats s;
        s.id = id;
        s.total = st.has_metadata ? st.total_wanted : -1;
        s.received = st.total_wanted_done;
        s.speed = st.download_payload_rate;
        s.uploadSpeed = st.upload_payload_rate;
        s.uploaded = st.all_time_upload;
        s.seeds = st.num_seeds;
        s.peers = st.num_peers;
        s.connections = st.num_peers;
        s.seeding = st.is_seeding || st.is_finished;

        const auto ti = st.has_metadata ? t.h.torrent_file() : nullptr;
        if (full && ti) {
            // piece bitmap → runs of downloaded bytes for the segment bar
            const qint64 plen = ti->piece_length();
            const qint64 total = ti->total_size();
            const int n = st.pieces.size();
            for (int i = 0; i < n;) {
                if (!st.pieces.get_bit(lt::piece_index_t(i))) {
                    ++i;
                    continue;
                }
                int j = i;
                while (j < n && st.pieces.get_bit(lt::piece_index_t(j)))
                    ++j;
                const qint64 b = i * plen, e = qMin<qint64>(qint64(j) * plen, total) - 1;
                s.segments << b << e << (e - b + 1);
                i = j;
            }
            // per-file progress (only sent when it changed)
            const lt::file_storage& fs = ti->layout();
            const std::vector<std::int64_t> prog = t.h.file_progress(lt::torrent_handle::piece_granularity);
            const std::vector<lt::download_priority_t> prio = t.h.get_file_priorities();
            const QString root = qs(ti->name()) + QLatin1Char('/');
            QVariantList files;
            for (int i = 0; i < fs.num_files(); ++i) {
                const lt::file_index_t fi(i);
                if (fs.pad_file_at(fi))
                    continue;
                QString name = QDir::fromNativeSeparators(qs(fs.file_path(fi)));
                if (fs.num_files() > 1 && name.startsWith(root))
                    name = name.mid(root.size());
                files << QVariantMap{{QStringLiteral("index"), i},
                                     {QStringLiteral("name"), name},
                                     {QStringLiteral("size"), qint64(fs.file_size(fi))},
                                     {QStringLiteral("done"), qint64(i < int(prog.size()) ? prog[i] : 0)},
                                     {QStringLiteral("wanted"), i < int(prio.size()) ? prio[i] != lt::dont_download : true}};
            }
            if (files != t.lastFiles) {
                t.lastFiles = files;
                s.files = files;
            }
        }
        emit stats(s);

        // Seeding policy: stop at the configured share ratio (0 = don't seed at all).
        if ((t.finishReported || t.seedOnly) && s.seeding) {
            const qint64 base = qMax<qint64>(1, st.total_wanted);
            if (m_ratio <= 0 || double(st.all_time_upload) >= m_ratio * double(base))
                seedDone.push_back(id);
        }
    }
    for (qint64 id : seedDone) {
        emit log(id, m_ratio <= 0 ? tr("Seeding disabled; torrent stopped") : tr("Share ratio %1 reached; seeding stopped").arg(m_ratio));
        removeTask(id);
        emit stopped(id, {});
    }
}

// ------------------------------------------------------------------ settings

void BtEngine::setSpeedLimit(qint64 bytesPerSec)
{
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::download_rate_limit, int(qMin<qint64>(bytesPerSec, INT_MAX)));
    m_ses->apply_settings(std::move(sp));
}

void BtEngine::setUploadLimit(qint64 bytesPerSec)
{
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::upload_rate_limit, int(qMin<qint64>(bytesPerSec, INT_MAX)));
    m_ses->apply_settings(std::move(sp));
}

void BtEngine::setMaxConnections(int perDownload)
{
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::connections_limit, qMax(50, perDownload * 25));
    m_ses->apply_settings(std::move(sp));
}

void BtEngine::setSeedRatio(double ratio) { m_ratio = ratio; }

void BtEngine::addPeer(qint64 id, const QString& host, int port)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end() || !it->second.h.is_valid())
        return;
    lt::error_code ec;
    const lt::address addr = lt::make_address(ss(host), ec);
    if (!ec)
        it->second.h.connect_peer(lt::tcp::endpoint(addr, std::uint16_t(port)));
}

void BtEngine::setFileWanted(qint64 id, int index, bool wanted)
{
    auto it = m_tasks.find(id);
    if (it == m_tasks.end() || !it->second.h.is_valid())
        return;
    it->second.h.file_priority(lt::file_index_t(index), wanted ? lt::default_priority : lt::dont_download);
    it->second.lastFiles.clear(); // force a refresh
}

QHash<qint64, QByteArray> BtEngine::shutdown()
{
    QHash<qint64, QByteArray> out;
    if (m_down)
        return out;
    m_down = true;
    m_timer.stop();

    int outstanding = 0;
    for (auto& [id, t] : m_tasks) {
        if (t.fetch)
            t.fetch->abort();
        if (t.h.is_valid()) {
            t.h.pause();
            t.h.save_resume_data(kResumeFlags);
            ++outstanding;
        }
        out.insert(id, t.lastResume);
    }
    QElapsedTimer timer;
    timer.start();
    while (outstanding > 0 && timer.elapsed() < 5000) {
        if (!m_ses->wait_for_alert(lt::milliseconds(250)))
            continue;
        std::vector<lt::alert*> alerts;
        m_ses->pop_alerts(&alerts);
        for (lt::alert* a : alerts) {
            if (auto* ra = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                if (Task* t = byHandle(ra->handle))
                    out.insert(t->id, toBytes(lt::write_resume_data_buf(ra->params)));
                --outstanding;
            } else if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
                --outstanding;
            }
        }
    }
    // Seed-only tasks keep whatever they had; finished seeding ones are not resumed.
    QFile state(QDir(m_stateDir).filePath(QStringLiteral("session.dat")));
    if (state.open(QIODevice::WriteOnly))
        state.write(toBytes(lt::write_session_params_buf(m_ses->session_state(lt::session_handle::save_dht_state))));
    m_tasks.clear();
    m_ses.reset();
    return out;
}
