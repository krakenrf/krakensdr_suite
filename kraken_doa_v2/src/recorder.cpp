// ============================================
// src/recorder.cpp - 📼 IQ & audio recorder: settings, IQ recordings (SigMF),
// the recordings folder, downloads / uploads
// ============================================
// See include/recorder.hpp and SIGMF.md. The audio recordings are in
// recorder_audio.cpp.
//
// IQ: the producer (the data receiver thread for the full band, the
// decimation thread for a VFO's band) copies each block into a queue; ONE
// writer thread per recording appends every antenna to its own .sigmf-data
// file, hashes it (SHA-512), keeps the page cache in check
// (sync_file_range + POSIX_FADV_DONTNEED every 8 MB: the dirty pages of a
// 24 MB/s stream would otherwise pile up and stall in bursts) and rewrites
// the metadata whenever a new capture segment began (frequency, gain,
// calibration burst, a gap). The queue may hold mem_budget() bytes: a disk
// slower than the data is buffered there (the start is refused when that
// can't last the whole recording - measured with "Test write speed" or
// during an earlier recording); when it is full the recording STOPS (a
// coherent recording with holes in it is worth less than a shorter one).

#include "recorder.hpp"
#include "recorder_internal.hpp"
#include "sigmf.hpp"

#include "channel_manager.hpp"
#include "config.hpp"
#include "decimator_manager.hpp"
#include "globals.hpp"
#include "playback.hpp"
#include "settings_store.hpp"
#include "signal_processing/music_processor.hpp"
#include "station_info.hpp"
#include "utils/disk_util.hpp"
#include "utils/fs_compat.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"
#include "utils/platform.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace rec {

std::atomic<int> g_iq_mode{0};
std::atomic<int> g_band_vfo{-1};

namespace {

constexpr const char* DEFAULT_DIR = "recordings";
constexpr double FS = 2.4e6;
constexpr uint64_t FAT_MAX = 0xFFFFFFFFull - (1 << 20);   // a FAT32 file stays below 4 GiB
constexpr uint64_t SYNC_STEP = 8u << 20;                   // writeback per file every 8 MB
constexpr int64_t NO_DATA_MS = 5000;
constexpr double BAND_SCALE = 16384.0;                     // ci16 counts per receiver full scale
constexpr int SPEEDTEST_MB = 128;
const int DURATIONS[] = {10, 30, 60, 120, 300, 600, 1800, 3600, 7200, 21600, 43200, 86400};

using clk = std::chrono::steady_clock;

int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now().time_since_epoch()).count();
}

// recording names: letters, digits, '_', '-', '.' (no leading '.'), <= 80.
// Windows also refuses a trailing '.' (another spelling of the name without
// it - the "is it being recorded" checks below compare names) and the device
// names ("nul", "com1.sigmf" ...); fs_portable_name is always true on Linux
bool valid_name(const std::string& n) {
    if (n.empty() || n.size() > 80 || n[0] == '.' || n[0] == '-') return false;
    for (char c : n)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    return n.find("..") == std::string::npos && fs_portable_name(n);
}
std::string clean_name(const std::string& in) {
    std::string o;
    for (char c : in) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') o += c;
        else if (c == ' ' || c == '.') o += '_';
        if (o.size() >= 64) break;
    }
    while (!o.empty() && (o[0] == '_' || o[0] == '-')) o.erase(0, 1);
    // Windows: "con", "aux", "com1" ... are devices, not folders
    if (!o.empty() && !fs_portable_name(o)) o = "_" + o;
    return o;
}

bool ends_with(const std::string& s, const std::string& e) {
    return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

// the files a recording consists of - the only ones served, deleted or
// packed (the folder setting can point anywhere: a download must never hand
// out settings, keys or anything else that happens to lie there)
bool recording_file(const std::string& n) {
    return ends_with(n, ".sigmf-meta") || ends_with(n, ".sigmf-data") || ends_with(n, ".sigmf-collection") ||
           ends_with(n, ".wav") || ends_with(n, ".opus");
}

std::string fmt(double v, int prec) {
    char b[64];
    snprintf(b, sizeof b, "%.*f", prec, is_finite_value(v) ? v : 0.0);
    return b;
}

bool write_all(int fd, const uint8_t* p, size_t n) {
    while (n) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

// whole file, atomically (tmp + rename)
bool write_file(const std::string& path, const std::string& data) {
    const size_t sl = path.rfind('/');
    const std::string tmp = path.substr(0, sl + 1) + "." + path.substr(sl + 1) + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const bool ok = write_all(fd, reinterpret_cast<const uint8_t*>(data.data()), data.size()) && fdatasync(fd) == 0;
    ::close(fd);
    // fs_replace: Windows' rename() refuses an existing target (the metadata
    // is rewritten every 2 s - the second rewrite stopped every recording)
    if (!ok || fs_replace(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

uint64_t mem_available() {
#ifdef _WIN32
    // no /proc/meminfo: GlobalMemoryStatusEx's available physical memory
    // (without it the memory buffer fell to its 64 MB floor)
    return fs_mem_available();
#else
    std::ifstream f("/proc/meminfo");
    std::string k;
    uint64_t v;
    std::string unit;
    while (f >> k >> v >> unit)
        if (k == "MemAvailable:") return v * 1024;
    return 0;
#endif
}

// --- settings ---------------------------------------------------------------------
struct Settings {
    std::string dir = DEFAULT_DIR;
    std::string mode = "iq";        // iq | audio
    std::string iq = "full";        // full | band
    int duration_s = 30;            // 0 = no time limit
    int band_vfo = -1;              // -1 = the VFO you listen to (the first 🔊)
    std::string aformat = "wav";    // wav | opus
    int arate = 48000;              // WAV
    int akbps = 32;                 // Opus
    bool askip = false;             // audio: leave out what the squelch closed
    std::string avfos = "all";      // all | id,id,...
    bool location = true;           // the station's position in the metadata
    std::string name, notes;        // the next recording (not persisted)
};

// --- IQ recording -------------------------------------------------------------------
struct CapInfo {
    double freq[MAX_CHANNELS]{};
    float gain[MAX_CHANNELS]{};
    uint32_t phase = 0;
    bool noise = false;
    int64_t t_ms = 0;
    double heading = -1;
};

struct Chunk {
    std::unique_ptr<uint8_t[]> buf;   // channel-major
    uint32_t n = 0;                   // samples per channel
    bool cap = false;                 // a capture segment starts here
    CapInfo ci;
    uint64_t gap = 0;                 // samples not recorded just before it
};

struct Seg {
    uint64_t start = 0, global = 0;
    CapInfo ci;
};

struct IqSession {
    // fixed at the start
    bool band = false;
    int M = 0;
    int bpc = 2;                      // bytes per complex sample (cu8 2, ci16 4)
    double rate = FS;
    int vfo = -1, decim = 1;
    double band_offset_hz = 0, band_bw_hz = 0, tuner_hz0 = 0;
    std::string name, folder;
    uint64_t limit = 0;               // samples, 0 = none
    uint64_t reserve = 0, budget = 0;
    bool fat = false;
    int64_t start_ms = 0;
    bool location = true;
    std::string notes, station_id, hw;
    std::vector<std::string> array_cmds;
    std::string vfos_snapshot;
    std::vector<std::array<double, 3>> geometry;
    bool st_valid = false;
    double st_lat = 0, st_lon = 0, st_hdg = -1;
    std::string st_src;
    bool kerberos = false, wb_variant = false;
    // producer (one thread)
    CapInfo last;
    bool have_last = false;
    uint64_t taken = 0, gap_pending = 0, last_seq = 0;
    bool have_seq = false;
    // queue
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Chunk> q;
    uint64_t queued = 0;
    bool stop_req = false;
    std::string reason;
    std::atomic<bool> shutdown{false};
    bool halt = false;               // writer: a fatal problem - what is queued is dropped
    // writer
    std::vector<int> fd;
    std::vector<std::unique_ptr<sigmf::Sha512>> sha;
    std::vector<uint64_t> fbytes, synced, prev_synced;
    std::vector<Seg> segs;
    uint64_t written = 0, gaps = 0;
    std::vector<std::array<double, 4>> track;
    std::string error;
    // read by the status (any thread)
    std::atomic<uint64_t> a_taken{0}, a_written{0}, a_bytes{0}, a_queued{0}, a_peak{0};
    std::atomic<double> in_bps{0}, out_bps{0};
    std::atomic<bool> finishing{false};

    void request_stop(const std::string& why) {
        {
            std::lock_guard<std::mutex> lk(qmu);
            if (stop_req) return;
            stop_req = true;
            reason = why;
        }
        qcv.notify_all();
    }
    bool stopping() {
        std::lock_guard<std::mutex> lk(qmu);
        return stop_req;
    }
    // producer: queue a chunk; false = stopped (or the buffer is full: stops)
    bool push(Chunk&& c, uint64_t bytes) {
        bool full = false;
        {
            std::lock_guard<std::mutex> lk(qmu);
            if (stop_req) return false;
            if (queued + bytes > budget) {
                stop_req = true;
                reason = "the disk is too slow - the memory buffer is full";
                full = true;
            } else {
                q.push_back(std::move(c));
                queued += bytes;
                a_queued.store(queued, std::memory_order_relaxed);
                if (queued > a_peak.load(std::memory_order_relaxed)) a_peak.store(queued, std::memory_order_relaxed);
            }
        }
        qcv.notify_all();
        return !full;
    }
};

// --- state --------------------------------------------------------------------------
std::mutex g_mu;   // everything below
Settings g_set;
std::shared_ptr<IqSession> g_iq;     // reset by its (detached) writer thread when it has finished
std::condition_variable g_done_cv;
struct Last {
    std::string kind, name, reason, error;
    uint64_t bytes = 0;
    double seconds = 0;
    int64_t t_ms = 0;
};
Last g_last;
std::string g_error;                      // the last refusal / failure
std::atomic<uint64_t> g_ver{1}, g_list_ver{1};
// measured write speeds per folder (speed test, or a recording that backed up)
struct Speed {
    double mbps = 0;
    int64_t t_ms = 0;
    bool measured_live = false;
};
std::map<std::string, Speed> g_speed;
struct SpeedTest {
    bool running = false;
    std::string dir, error;
    int pct = 0;
};
SpeedTest g_test;

void bump() { g_ver.fetch_add(1); }
void bump_list() {
    g_list_ver.fetch_add(1);
    g_ver.fetch_add(1);
}

std::string dir_abs_locked() { return diskutil::absolute(g_set.dir); }

uint64_t reserve_for(uint64_t total) {
    // keep max(512 MiB, 2 %) free, at most 4 GiB
    return std::min<uint64_t>(4ull << 30, std::max<uint64_t>(512ull << 20, total / 50));
}

// --- metadata -----------------------------------------------------------------------
std::string extensions_json() {
    return std::string("[{\"name\":\"spatial\",\"version\":\"") + sigmf::SPATIAL_VERSION +
           "\",\"optional\":true},{\"name\":\"krakensdr\",\"version\":\"" + sigmf::KRAKEN_VERSION +
           "\",\"optional\":true}]";
}

std::string seg_json(const IqSession& s, const Seg& g, int k) {
    std::ostringstream o;
    o << "{\"core:sample_start\":" << g.start << ",\"core:global_index\":" << g.global
      << ",\"core:frequency\":" << fmt(g.ci.freq[k], 0) << ",\"core:datetime\":\"" << sigmf::iso_utc(g.ci.t_ms) << "\"";
    if (s.location && g.ci.heading >= 0) o << ",\"spatial:aperture_azimuth\":" << fmt(g.ci.heading, 1);
    if (g.ci.gain[k] > -900) o << ",\"krakensdr:gain_db\":" << fmt(g.ci.gain[k], 1);
    o << ",\"krakensdr:phase_state\":" << g.ci.phase << ",\"krakensdr:noise_source\":" << (g.ci.noise ? "true" : "false");
    if (g.ci.noise) o << ",\"spatial:calibration\":{\"caltype\":\"other\",\"cal_geometry\":{\"unknown\":true}}";
    o << "}";
    return o.str();
}

std::string description(const IqSession& s, int k) {
    std::string d = "KrakenSDR coherent IQ recording, antenna " + std::to_string(k) + " of " + std::to_string(s.M) +
                    " (ANT" + std::to_string(k) + "), ";
    d += s.band ? "VFO D" + std::to_string(s.vfo) + " band " + fmt(s.band_bw_hz / 1e3, 1) + " kHz"
                : "full band 2.4 MHz";
    if (!s.notes.empty()) d += " - " + s.notes;
    return d;
}

std::string meta_json(const IqSession& s, int k, const std::string& sha) {
    std::ostringstream o;
    o << "{\"global\":{\"core:datatype\":\"" << (s.band ? "ci16_le" : "cu8") << "\",\"core:sample_rate\":" << fmt(s.rate, 3)
      << ",\"core:version\":\"" << sigmf::CORE_VERSION << "\",\"core:description\":\"" << json_escape(description(s, k))
      << "\",\"core:author\":\"" << json_escape(s.station_id) << "\",\"core:recorder\":\"KrakenSDR kraken_doa\",\"core:hw\":\""
      << json_escape(s.hw) << "\",\"core:collection\":\"" << json_escape(s.name) << "\"";
    if (!sha.empty()) o << ",\"core:sha512\":\"" << sha << "\"";
    o << ",\"core:extensions\":" << extensions_json();
    if (s.location && s.st_valid)
        o << ",\"core:geolocation\":{\"type\":\"Point\",\"coordinates\":[" << fmt(s.st_lon, 7) << "," << fmt(s.st_lat, 7) << "]}";
    o << ",\"spatial:num_elements\":" << s.M << ",\"spatial:channel_index\":" << k << ",\"krakensdr:format\":\""
      << (s.band ? "band" : "full") << "\"";
    if (s.band) o << ",\"krakensdr:iq_scale\":" << fmt(BAND_SCALE, 0);
    o << "},\"captures\":[";
    for (size_t i = 0; i < s.segs.size(); i++) o << (i ? "," : "") << seg_json(s, s.segs[i], k);
    o << "],\"annotations\":[]}";
    return o.str();
}

std::string collection_json(const IqSession& s, const std::vector<std::string>& hashes, bool final,
                            const std::string& reason) {
    std::ostringstream o;
    std::string desc = std::string("KrakenSDR coherent IQ recording: ") + std::to_string(s.M) + " antennas, " +
                       (s.band ? "VFO D" + std::to_string(s.vfo) + " band " + fmt(s.band_bw_hz / 1e3, 1) + " kHz"
                               : "full band 2.4 MHz");
    if (!s.notes.empty()) desc += " - " + s.notes;
    o << "{\"collection\":{\"core:version\":\"" << sigmf::CORE_VERSION << "\",\"core:description\":\"" << json_escape(desc)
      << "\",\"core:author\":\"" << json_escape(s.station_id) << "\",\"core:extensions\":" << extensions_json()
      << ",\"core:streams\":[";
    for (int k = 0; k < s.M; k++)
        o << (k ? "," : "") << "{\"name\":\"" << json_escape(s.name) << "_ch" << k << "\",\"hash\":\"" << hashes[static_cast<size_t>(k)] << "\"}";
    o << "],\"spatial:element_geometry\":[";
    for (int k = 0; k < s.M; k++) {
        o << (k ? "," : "");
        if (static_cast<int>(s.geometry.size()) == s.M)
            o << "{\"point\":[" << fmt(s.geometry[static_cast<size_t>(k)][0], 5) << "," << fmt(s.geometry[static_cast<size_t>(k)][1], 5)
              << "," << fmt(s.geometry[static_cast<size_t>(k)][2], 5) << "]}";
        else
            o << "{\"unknown\":true}";
    }
    const double secs = static_cast<double>(s.written) / s.rate;
    o << "],\"krakensdr:format\":\"" << (s.band ? "band" : "full") << "\",\"krakensdr:channels\":" << s.M
      << ",\"krakensdr:start\":\"" << sigmf::iso_utc(s.start_ms) << "\",\"krakensdr:seconds\":" << fmt(secs, 3)
      << ",\"krakensdr:complete\":" << (final ? "true" : "false");
    if (final) o << ",\"krakensdr:stop_reason\":\"" << json_escape(reason) << "\"";
    if (s.gaps) o << ",\"krakensdr:gap_samples\":" << s.gaps;
    o << ",\"krakensdr:array_commands\":[";
    for (size_t i = 0; i < s.array_cmds.size(); i++) o << (i ? "," : "") << "\"" << json_escape(s.array_cmds[i]) << "\"";
    o << "]";
    if (!s.vfos_snapshot.empty()) o << ",\"krakensdr:vfos\":\"" << json_escape(s.vfos_snapshot) << "\"";
    if (s.band)
        o << ",\"krakensdr:band\":{\"vfo\":" << s.vfo << ",\"offset_hz\":" << fmt(s.band_offset_hz, 1) << ",\"bandwidth_hz\":"
          << fmt(s.band_bw_hz, 1) << ",\"tuner_hz\":" << fmt(s.tuner_hz0, 0) << ",\"decimation\":" << s.decim << "}";
    o << ",\"krakensdr:receiver\":{\"kerberos\":" << (s.kerberos ? "true" : "false")
      << ",\"wideband_variant\":" << (s.wb_variant ? "true" : "false") << "}";
    if (s.location) {
        o << ",\"krakensdr:station\":{\"id\":\"" << json_escape(s.station_id) << "\"";
        if (s.st_valid) o << ",\"lat\":" << fmt(s.st_lat, 7) << ",\"lon\":" << fmt(s.st_lon, 7);
        if (s.st_hdg >= 0) o << ",\"heading\":" << fmt(s.st_hdg, 1);
        o << ",\"source\":\"" << json_escape(s.st_src) << "\"}";
        if (!s.track.empty()) {
            o << ",\"krakensdr:track\":[";
            for (size_t i = 0; i < s.track.size(); i++) {
                const auto& t = s.track[i];
                o << (i ? "," : "") << "[" << fmt(t[0], 1) << "," << fmt(t[1], 7) << "," << fmt(t[2], 7) << "," << fmt(t[3], 1) << "]";
            }
            o << "]";
        }
    }
    o << "}}";
    return o.str();
}

bool write_metadata(IqSession& s, bool final, const std::string& reason) {
    std::vector<std::string> hashes;
    bool ok = true;
    for (int k = 0; k < s.M; k++) {
        const std::string sha = final && s.sha[static_cast<size_t>(k)] ? s.sha[static_cast<size_t>(k)]->hex() : "";
        const std::string m = meta_json(s, k, sha);
        hashes.push_back(sigmf::sha512_hex(m));
        ok = write_file(s.folder + "/" + s.name + "_ch" + std::to_string(k) + ".sigmf-meta", m) && ok;
    }
    ok = write_file(s.folder + "/" + s.name + ".sigmf-collection", collection_json(s, hashes, final, reason)) && ok;
    return ok;
}

// --- the writer thread ------------------------------------------------------------
void writer_main(std::shared_ptr<IqSession> sp) {
    IqSession& s = *sp;
    int64_t last_tick = mono_ms(), last_meta = 0, last_data = mono_ms(), shutdown_at = 0;
    uint64_t tick_bytes = 0, tick_queued = 0;
    bool dirty = false;
    std::string reason;
    double tlat = 1e9, tlon = 1e9, thdg = -2;
    int64_t tlast = 0;
    for (;;) {
        Chunk c;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(s.qmu);
            s.qcv.wait_for(lk, std::chrono::milliseconds(200), [&] { return !s.q.empty() || s.stop_req; });
            if (!s.q.empty()) {
                c = std::move(s.q.front());
                s.q.pop_front();
                have = true;
            } else if (s.stop_req) {
                reason = s.reason;
                break;
            }
            if (s.stop_req) s.finishing.store(true);
        }
        if (have) {
            last_data = mono_ms();
            const uint64_t per = static_cast<uint64_t>(c.n) * static_cast<uint64_t>(s.bpc);
            bool ok = !s.halt;
            if (ok && (c.cap || c.gap || s.segs.empty())) {
                s.gaps += c.gap;
                s.segs.push_back({s.written, s.written + s.gaps, c.ci});
                dirty = true;
            }
            // every antenna's file must take the block, or none does
            for (int k = 0; k < s.M && ok && s.fat; k++)
                if (s.fbytes[static_cast<size_t>(k)] + per > FAT_MAX) {
                    s.request_stop("a file reached the 4 GB limit of the FAT32 drive");
                    s.halt = true;
                    ok = false;
                }
            for (int k = 0; k < s.M && ok; k++) {
                const size_t ki = static_cast<size_t>(k);
                if (s.fat && s.fbytes[ki] + per > FAT_MAX) {
                    s.request_stop("a file reached the 4 GB limit of the FAT32 drive");
                    s.halt = true;
                    ok = false;
                    break;
                }
                const uint8_t* p = c.buf.get() + per * ki;
                if (!write_all(s.fd[ki], p, per)) {
                    s.error = std::string("writing failed: ") + strerror(errno);
                    s.request_stop(s.error);
                    s.halt = true;
                    ok = false;
                    break;
                }
                s.sha[ki]->update(p, per);
                s.fbytes[ki] += per;
                // writeback of the new 8 MB, wait for the one before, drop it from the cache
                if (s.fbytes[ki] - s.synced[ki] >= SYNC_STEP) {
                    sync_file_range(s.fd[ki], static_cast<off_t>(s.synced[ki]), static_cast<off_t>(s.fbytes[ki] - s.synced[ki]),
                                    SYNC_FILE_RANGE_WRITE);
                    if (s.synced[ki] > s.prev_synced[ki]) {
                        const off_t a = static_cast<off_t>(s.prev_synced[ki]), n = static_cast<off_t>(s.synced[ki] - s.prev_synced[ki]);
                        sync_file_range(s.fd[ki], a, n,
                                        SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
                        posix_fadvise(s.fd[ki], a, n, POSIX_FADV_DONTNEED);
                    }
                    s.prev_synced[ki] = s.synced[ki];
                    s.synced[ki] = s.fbytes[ki];
                }
            }
            if (ok) {
                s.written += c.n;
                s.a_written.store(s.written, std::memory_order_relaxed);
                s.a_bytes.fetch_add(per * static_cast<uint64_t>(s.M), std::memory_order_relaxed);
            }
            {
                std::lock_guard<std::mutex> lk(s.qmu);
                s.queued -= std::min<uint64_t>(s.queued, per * static_cast<uint64_t>(s.M));
                s.a_queued.store(s.queued, std::memory_order_relaxed);
            }
        }
        const int64_t t = mono_ms();
        if (s.shutdown.load()) {
            if (!shutdown_at) shutdown_at = t;
            // Windows: the console window was closed - the process is ended
            // within seconds, so the final metadata goes first
            if (platform::stop_is_urgent() || t - shutdown_at > 4000) {
                // kraken_doa is ending: what is still queued is dropped
                std::lock_guard<std::mutex> lk(s.qmu);
                s.q.clear();
                s.queued = 0;
                s.a_queued.store(0);
                if (!s.stop_req) s.stop_req = true;
                s.reason = "kraken_doa shut down";
            }
        }
        if (t - last_tick >= 1000) {
            const double dt = (t - last_tick) / 1000.0;
            const uint64_t b = s.a_bytes.load(), qd = s.a_queued.load();
            const double out = (b - tick_bytes) / dt;
            const double in = out + (static_cast<double>(qd) - static_cast<double>(tick_queued)) / dt;
            s.out_bps.store(s.out_bps.load() * 0.5 + out * 0.5);
            s.in_bps.store(s.in_bps.load() * 0.5 + std::max(0.0, in) * 0.5);
            tick_bytes = b;
            tick_queued = qd;
            last_tick = t;
            // free space
            uint64_t fr = 0, tot = 0;
            if (diskutil::disk_space(s.folder, &fr, &tot) && fr < s.reserve && !s.halt) {
                s.request_stop("the drive is full (reserve kept free)");
                s.halt = true;   // what is still queued would eat into the reserve
            }
            // no data
            if (t - last_data > NO_DATA_MS && !s.stopping())
                s.request_stop(s.band ? "no data from the VFO for 5 s" : "no data from the receiver for 5 s");
            // where the station is (a moving one: its track)
            if (s.location) {
                const StationLocation st = station_info.resolve();
                const bool pos = !(st.lat == 0 && st.lon == 0);
                if (pos && st.source != LocationSource::STATIC) {
                    const double d = std::hypot((st.lat - tlat) * 111320.0, (st.lon - tlon) * 111320.0 * std::cos(st.lat * M_PI / 180));
                    const double h = st.heading_valid ? st.heading : -1;
                    if (d > 2 || std::fabs(h - thdg) > 1 || t - tlast > 30000) {
                        if (s.track.size() < 200000)
                            s.track.push_back({static_cast<double>(s.written) / s.rate, st.lat, st.lon, h});
                        tlat = st.lat;
                        tlon = st.lon;
                        thdg = h;
                        tlast = t;
                        dirty = true;
                    }
                }
            }
            if (dirty && t - last_meta >= 2000) {
                if (!write_metadata(s, false, "")) {
                    s.error = "can't write the metadata";
                    s.request_stop(s.error);
                }
                dirty = false;
                last_meta = t;
            }
            detail::changed();
        }
    }
    // finish: the files' last pages, the final metadata
    s.finishing.store(true);
    // an urgent stop (Windows) doesn't wait for the drive to take the data:
    // the system's cache outlives the process and is written all the same
    const bool urgent = platform::stop_is_urgent();
    for (size_t k = 0; k < s.fd.size(); k++) {
        if (s.fd[k] < 0) continue;
        if (!urgent) fdatasync(s.fd[k]);
        posix_fadvise(s.fd[k], 0, 0, POSIX_FADV_DONTNEED);
        ::close(s.fd[k]);
        s.fd[k] = -1;
    }
    uint64_t bytes = 0;
    for (uint64_t b : s.fbytes) bytes += b;
    const double secs = static_cast<double>(s.written) / s.rate;
    std::string err = s.error;
    if (s.written == 0) {
        // nothing recorded: no recording
        DIR* d = opendir(s.folder.c_str());
        if (d) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n != "." && n != "..") unlink((s.folder + "/" + n).c_str());
            }
            closedir(d);
        }
        rmdir(s.folder.c_str());
        if (err.empty()) err = "nothing was recorded (" + reason + ")";
    } else if (!write_metadata(s, true, reason) && err.empty()) {
        err = "can't write the final metadata";
    }
    // the drive's speed, seen while the buffer filled
    if (s.a_peak.load() > (64u << 20) && s.out_bps.load() > 0) {
        std::lock_guard<std::mutex> lk(g_mu);
        auto& sp2 = g_speed[diskutil::absolute(s.folder.substr(0, s.folder.rfind('/')))];
        sp2.mbps = s.out_bps.load() / 1e6;
        sp2.t_ms = detail::now_ms();
        sp2.measured_live = true;
    }
    std::cout << "Recorder: " << s.name << " finished (" << reason << "): " << fmt(secs, 1) << " s, " << bytes / 1000000 << " MB"
              << (err.empty() ? "" : " - " + err) << std::endl;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_iq == sp) g_iq.reset();
        g_last = {"iq", s.written ? s.name : "", reason, err, bytes, secs, detail::now_ms()};
        bump_list();
    }
    g_done_cv.notify_all();
}

// --- start -------------------------------------------------------------------------
// the array's settings as commands (playback puts them back), WIDEBAND as the
// UCA of its ring
std::vector<std::string> array_commands(std::shared_ptr<DecimatorManager::DecimatorInstance> any) {
    static const char* keys[] = {"TOPOLOGY:", "CUSTOM_POSITIONS:", "ARRAY_LAYOUT:", "RADIUS:", "SPACING:",
                                 "ULA_MODE:", "CUSTOM_MODE:", "ARRAY_OFFSET:"};
    std::vector<std::string> out;
    bool wideband = false;
    for (const auto& c : SettingsStore::apply_commands())
        for (const char* k : keys)
            if (c.rfind(k, 0) == 0) {
                if (c == "TOPOLOGY:WIDEBAND") wideband = true;
                else out.push_back(c);
            }
    if (wideband) {
        out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& c) { return c.rfind("RADIUS:", 0) == 0; }), out.end());
        out.insert(out.begin(), "TOPOLOGY:UCA");
        if (any && any->music_processor) out.push_back("RADIUS:" + fmt(any->music_processor->getArrayRadius(), 1));
    }
    return out;
}

std::vector<std::array<double, 3>> geometry(std::shared_ptr<DecimatorManager::DecimatorInstance> any, int M) {
    std::vector<std::array<double, 3>> g;
    if (!any || !any->music_processor) return g;
    std::vector<double> x, y, z;
    if (any->music_processor->nominalPositions(&x, &y, &z) && static_cast<int>(x.size()) == M) {
        for (int k = 0; k < M; k++) g.push_back({x[static_cast<size_t>(k)], y[static_cast<size_t>(k)], z[static_cast<size_t>(k)]});
    } else if (any->music_processor->getArrayTopology() == ArrayTopology::ULA) {
        const double d = any->music_processor->getElementSpacing() / 1000.0;   // as MUSIC: along x from ANT0
        for (int k = 0; k < M; k++) g.push_back({k * d, 0.0, 0.0});
    }
    return g;
}

std::string unique_folder(const std::string& dir, const std::string& base) {
    std::string n = base;
    struct stat st{};
    for (int i = 2; i < 1000 && (stat((dir + "/" + n).c_str(), &st) == 0 || stat((dir + "/" + n + ".sigmf").c_str(), &st) == 0); i++)
        n = base + "_" + std::to_string(i);
    return n;
}

struct Need {
    double bps = 0;            // bytes / s of the whole recording
    double per_file_bps = 0;
    int files = 0;
    std::string why;           // can't record now ("" = can)
    double rate = 0;           // IQ sample rate
    int M = 0, vfo = -1, decim = 1;
    std::vector<int> vfos;     // audio
};

// what the current settings would record (uWS loop, g_mu held)
Need need_locked() {
    Need n;
    const Settings& s = g_set;
    if (s.mode == "audio") {
        auto list = decimator_manager.getDecimatorInfoList();
        if (s.avfos == "all") {
            for (const auto& d : list) n.vfos.push_back(d.id);
        } else {
            std::stringstream ss(s.avfos);
            std::string t;
            while (std::getline(ss, t, ',')) {
                const int id = atoi(t.c_str());
                for (const auto& d : list)
                    if (d.id == id && !t.empty()) n.vfos.push_back(id);
            }
        }
        if (n.vfos.empty()) n.why = "no VFO chosen";
        n.per_file_bps = detail::audio_bytes_per_s(s.aformat == "wav", s.arate, s.akbps);
        n.files = static_cast<int>(n.vfos.size());
        n.bps = n.per_file_bps * n.files;
        return n;
    }
    n.M = std::clamp(active_num_elements.load(), 1, MAX_CHANNELS);
    if (multi_tuner_mode()) n.why = "IQ recording needs coherent mode (the antennas as one array)";
    else if (playback::active()) n.why = "not during a playback";
    if (s.iq == "band") {
        n.vfo = s.band_vfo;
        if (n.vfo < 0) {
            // the VFO you listen to (the first 🔊), else the first VFO
            n.vfo = decimator_manager.primaryAudioVfo();
            if (n.vfo < 0) {
                const auto all = decimator_manager.getAllDecimators();
                if (!all.empty() && all.front()) n.vfo = all.front()->id;
            }
        }
        auto inst = decimator_manager.getDecimator(n.vfo);
        if (!inst || !inst->decimator) {
            if (n.why.empty()) n.why = "no VFO D" + std::to_string(n.vfo);
            n.rate = 0;
        } else {
            n.decim = inst->decimator->getDecimationFactor();
            n.rate = FS / std::max(1, n.decim);
            if (n.decim <= 1 && n.why.empty()) n.why = "VFO D" + std::to_string(n.vfo) + " is 2.4 MHz wide - record the full band instead";
        }
        n.per_file_bps = n.rate * 4;
    } else {
        n.rate = FS;
        n.per_file_bps = FS * 2;
    }
    n.files = n.M;
    n.bps = n.per_file_bps * n.M;
    return n;
}

// (g_mu held) why a recording of `limit_s` can't start on this drive, "" = it can
std::string disk_check_locked(const Need& n, double limit_s, uint64_t* reserve, bool* fat, std::string* note) {
    const std::string dir = dir_abs_locked();
    uint64_t fr = 0, tot = 0;
    if (!diskutil::disk_space(dir, &fr, &tot)) return "can't read the free space of " + dir;
    *reserve = reserve_for(tot);
    *fat = diskutil::fs_type(dir) == "vfat";
    const double usable = fr > *reserve ? static_cast<double>(fr - *reserve) : 0.0;
    auto gb = [](double b) { return fmt(b / 1e9, b < 10e9 ? 2 : 1) + " GB"; };
    if (limit_s > 0) {
        const double size = n.bps * limit_s;
        if (size > usable)
            return "needs " + gb(size) + " but " + gb(usable) + " is free (keeping " + gb(static_cast<double>(*reserve)) +
                   " for the system)";
        if (*fat && n.per_file_bps * limit_s > static_cast<double>(FAT_MAX))
            return "the drive is FAT32: a file can't reach 4 GB - " + fmt(static_cast<double>(FAT_MAX) / n.per_file_bps / 60, 0) +
                   " min at most (or use an exFAT / ext4 drive)";
    } else if (usable < n.bps * 5) {
        return "less than 5 seconds fit on the drive (" + gb(usable) + " free beyond the reserve)";
    }
    // a slower disk: the difference is held in memory
    auto it = g_speed.find(dir);
    if (it != g_speed.end() && it->second.mbps > 0 && it->second.mbps * 1e6 < n.bps) {
        const double deficit = n.bps - it->second.mbps * 1e6;
        const double ram_s = static_cast<double>(detail::mem_budget()) / deficit;
        if (limit_s > 0 && limit_s > ram_s)
            return "the drive writes " + fmt(it->second.mbps, 1) + " MB/s and this recording needs " + fmt(n.bps / 1e6, 1) +
                   " MB/s: only " + fmt(ram_s, 0) + " s fit in the memory buffer - pick a shorter time, the VFO band, or a faster drive";
        *note = "drive " + fmt(it->second.mbps, 1) + " MB/s < " + fmt(n.bps / 1e6, 1) + " MB/s: buffered in memory, stops after ~" +
                fmt(ram_s, 0) + " s";
    }
    return "";
}

std::string start_iq_locked() {
    const Settings& set = g_set;
    Need n = need_locked();
    if (!n.why.empty()) return n.why;
    if (playback::active()) return "not during a playback";
    uint64_t reserve = 0;
    bool fat = false;
    std::string note;
    const double limit_s = set.duration_s;
    if (std::string e = disk_check_locked(n, limit_s, &reserve, &fat, &note); !e.empty()) return e;
    const std::string dir = dir_abs_locked();
    if (!diskutil::mkdirs(dir)) return "can't create the folder " + dir;
    auto s = std::make_shared<IqSession>();
    s->band = set.iq == "band";
    s->M = n.M;
    s->bpc = s->band ? 4 : 2;
    s->rate = n.rate;
    s->vfo = n.vfo;
    s->decim = n.decim;
    s->budget = detail::mem_budget();
    s->reserve = reserve;
    s->fat = fat;
    s->limit = limit_s > 0 ? static_cast<uint64_t>(std::llround(limit_s * n.rate)) : 0;
    s->start_ms = detail::now_ms();
    s->location = set.location;
    s->notes = set.notes;
    s->station_id = station_info.getId();
    s->kerberos = server_kerberos_mode.load();
    s->wb_variant = wb_variant_enabled.load();
    s->hw = std::string(s->wb_variant ? "KrakenSDR Wideband (downconverter)" : s->kerberos ? "KerberosSDR" : "KrakenSDR") + ", " +
            std::to_string(n.M) + " coherent RTL-SDR (R820T) channels, heimdall_v2 phase calibration";
    auto all = decimator_manager.getAllDecimators();
    auto any = all.empty() ? nullptr : all.front();
    s->array_cmds = array_commands(any);
    s->geometry = geometry(any, n.M);
    for (const auto& c : SettingsStore::apply_commands())
        if (c.rfind("DECIMATORS:", 0) == 0) s->vfos_snapshot = c;
    if (set.location) {
        const StationLocation st = station_info.resolve();
        s->st_valid = !(st.lat == 0 && st.lon == 0);
        s->st_lat = st.lat;
        s->st_lon = st.lon;
        s->st_hdg = st.heading_valid ? st.heading : -1;
        s->st_src = st.source_str();
    }
    const uint64_t tuner = coherent_tuner_hz();
    if (s->band) {
        auto inst = decimator_manager.getDecimator(n.vfo);
        s->band_offset_hz = inst->frequency_offset_hz.load();
        s->band_bw_hz = inst->decimator->getBandwidthMhz() * 1e6;
        s->tuner_hz0 = static_cast<double>(tuner);
    }
    std::string base = clean_name(set.name);
    if (base.empty())
        base = std::string(s->band ? "band" : "iq") + "_" + detail::name_time(s->start_ms) + "_" +
               detail::mhz_tag(static_cast<double>(tuner) + s->band_offset_hz);
    s->name = unique_folder(dir, base);
    s->folder = dir + "/" + s->name;
    if (mkdir(s->folder.c_str(), 0755) != 0) return "can't create " + s->folder + ": " + strerror(errno);
    for (int k = 0; k < s->M; k++) {
        const std::string p = s->folder + "/" + s->name + "_ch" + std::to_string(k) + ".sigmf-data";
        const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            const std::string e = "can't create " + p + ": " + strerror(errno);
            for (int f : s->fd) ::close(f);
            return e;
        }
        s->fd.push_back(fd);
        s->sha.push_back(std::make_unique<sigmf::Sha512>());
    }
    s->fbytes.assign(static_cast<size_t>(s->M), 0);
    s->synced.assign(static_cast<size_t>(s->M), 0);
    s->prev_synced.assign(static_cast<size_t>(s->M), 0);
    g_iq = s;
    g_band_vfo.store(s->vfo);
    g_iq_mode.store(s->band ? 2 : 1);
    std::thread(writer_main, s).detach();
    std::cout << "Recorder: IQ " << s->name << " - " << s->M << " antennas, " << (s->band ? "VFO band " : "full band ")
              << fmt(s->rate / 1e3, 3) << " kHz, " << fmt(n.bps / 1e6, 2) << " MB/s"
              << (s->limit ? ", " + std::to_string(set.duration_s) + " s" : ", no time limit")
              << (note.empty() ? "" : " (" + note + ")") << std::endl;
    return "";
}

std::string start_audio_locked() {
    const Settings& set = g_set;
    Need n = need_locked();
    if (!n.why.empty()) return n.why;
    uint64_t reserve = 0;
    bool fat = false;
    std::string note;
    if (std::string e = disk_check_locked(n, set.duration_s, &reserve, &fat, &note); !e.empty()) return e;
    const std::string dir = dir_abs_locked();
    if (!diskutil::mkdirs(dir)) return "can't create the folder " + dir;
    detail::AudioConfig c;
    const int64_t t = detail::now_ms();
    std::string base = clean_name(set.name);
    if (base.empty()) base = "audio_" + detail::name_time(t);
    c.name = unique_folder(dir, base);
    c.folder = dir + "/" + c.name;
    if (mkdir(c.folder.c_str(), 0755) != 0) return "can't create " + c.folder + ": " + strerror(errno);
    c.vfos = n.vfos;
    c.wav = set.aformat == "wav";
    c.rate = set.arate;
    c.kbps = set.akbps;
    c.skip_closed = set.askip;
    c.limit_s = set.duration_s;
    c.reserve = reserve;
    c.fat = fat;
    c.station = station_info.getId();
    c.notes = set.notes;
    if (set.location) {
        const StationLocation st = station_info.resolve();
        c.has_pos = !(st.lat == 0 && st.lon == 0);
        c.lat = st.lat;
        c.lon = st.lon;
    }
    std::string err;
    if (!detail::audio_start(c, &err)) {
        rmdir(c.folder.c_str());
        return err;
    }
    return "";
}

bool busy_locked() { return g_iq != nullptr || detail::audio_running(); }

void stop_recording_locked(const std::string& why) {
    if (g_iq) {
        g_iq_mode.store(0);
        g_iq->request_stop(why);
    }
    if (detail::audio_running()) detail::audio_request_stop(why);
}

// --- the folder ------------------------------------------------------------------
double audio_file_seconds(const std::string& path, uint64_t size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    if (ends_with(path, ".wav")) {
        // RIFF: walk the chunks to "fmt " (byte rate) and "data"
        char h[12];
        if (!f.read(h, 12) || memcmp(h, "RIFF", 4) != 0) return 0;
        uint32_t byte_rate = 0;
        for (int i = 0; i < 32; i++) {
            char ch[8];
            if (!f.read(ch, 8)) break;
            uint32_t len;
            memcpy(&len, ch + 4, 4);
            if (memcmp(ch, "fmt ", 4) == 0) {
                char fm[16];
                if (!f.read(fm, 16)) break;
                memcpy(&byte_rate, fm + 8, 4);
                f.seekg(len - 16 + (len & 1), std::ios::cur);
            } else if (memcmp(ch, "data", 4) == 0) {
                const uint64_t data = std::min<uint64_t>(len, size - static_cast<uint64_t>(f.tellg()));
                return byte_rate ? static_cast<double>(data) / byte_rate : 0;
            } else {
                f.seekg(len + (len & 1), std::ios::cur);
            }
        }
        return 0;
    }
    if (ends_with(path, ".opus")) {
        // the last Ogg page's granule position (48 kHz), minus the pre-skip
        const uint64_t tail = std::min<uint64_t>(size, 65536);
        std::string b(static_cast<size_t>(tail), '\0');
        f.seekg(static_cast<std::streamoff>(size - tail));
        f.read(b.data(), static_cast<std::streamsize>(tail));
        for (size_t p = b.rfind("OggS"); p != std::string::npos; p = p ? b.rfind("OggS", p - 1) : std::string::npos) {
            if (p + 14 > b.size()) continue;
            int64_t g;
            memcpy(&g, b.data() + p + 6, 8);
            if (g > 0) return (g - 312) / 48000.0;
        }
    }
    return 0;
}

std::string list_json_locked() {
    const std::string dir = dir_abs_locked();
    const std::string cur_iq = g_iq ? g_iq->name : "";
    const std::string aj = detail::audio_status_json();
    struct Item {
        int64_t mtime;
        std::string json;
    };
    std::vector<Item> items;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || !valid_name(n)) continue;
            const std::string p = dir + "/" + n;
            struct stat st{};
            if (stat(p.c_str(), &st) != 0) continue;
            std::ostringstream o;
            int64_t mtime = static_cast<int64_t>(st.st_mtime) * 1000;
            const bool writing = n == cur_iq || (!aj.empty() && aj.find("\"name\":\"" + json_escape(n) + "\"") != std::string::npos);
            if (S_ISDIR(st.st_mode)) {
                bool iq = false;
                uint64_t bytes = 0;
                std::vector<std::pair<std::string, uint64_t>> audio;
                if (DIR* sd = opendir(p.c_str())) {
                    while (dirent* f = readdir(sd)) {
                        const std::string fn = f->d_name;
                        if (fn.empty() || fn[0] == '.') continue;
                        struct stat fs{};
                        if (stat((p + "/" + fn).c_str(), &fs) != 0 || !S_ISREG(fs.st_mode)) continue;
                        bytes += static_cast<uint64_t>(fs.st_size);
                        mtime = std::max<int64_t>(mtime, static_cast<int64_t>(fs.st_mtime) * 1000);
                        if (ends_with(fn, ".sigmf-meta") || ends_with(fn, ".sigmf-collection")) iq = true;
                        if (ends_with(fn, ".wav") || ends_with(fn, ".opus")) audio.emplace_back(fn, static_cast<uint64_t>(fs.st_size));
                    }
                    closedir(sd);
                }
                o << "{\"name\":\"" << json_escape(n) << "\",\"bytes\":" << bytes << ",\"mtime\":" << mtime
                  << ",\"writing\":" << (writing ? "true" : "false");
                if (iq) {
                    o << ",\"kind\":\"iq\",\"form\":\"folder\"";
                    sigmf::Recording r;
                    std::string err;
                    if (writing) {
                    } else if (sigmf::open_recording(p, &r, &err)) {
                        o << ",\"channels\":" << r.channels << ",\"rate\":" << fmt(r.rate, 1) << ",\"datatype\":\""
                          << json_escape(r.datatype) << "\",\"seconds\":" << fmt(r.rate > 0 ? r.samples / r.rate : 0, 2)
                          << ",\"freq_hz\":" << fmt(r.streams[0].captures.front().freq_hz, 0) << ",\"start\":" << r.start_ms
                          << ",\"format\":\"" << json_escape(r.format) << "\",\"description\":\"" << json_escape(r.description) << "\"";
                    } else {
                        o << ",\"error\":\"" << json_escape(err) << "\"";
                    }
                } else if (!audio.empty()) {
                    std::sort(audio.begin(), audio.end());
                    o << ",\"kind\":\"audio\",\"files\":[";
                    for (size_t i = 0; i < audio.size(); i++)
                        o << (i ? "," : "") << "{\"name\":\"" << json_escape(audio[i].first) << "\",\"bytes\":" << audio[i].second
                          << ",\"seconds\":" << fmt(writing ? 0 : audio_file_seconds(p + "/" + audio[i].first, audio[i].second), 2) << "}";
                    o << "]";
                } else {
                    continue;   // not a recording
                }
                o << "}";
            } else if (S_ISREG(st.st_mode) && ends_with(n, ".sigmf")) {
                sigmf::Recording r;
                std::string err;
                o << "{\"name\":\"" << json_escape(n) << "\",\"kind\":\"iq\",\"form\":\"archive\",\"bytes\":" << st.st_size
                  << ",\"mtime\":" << mtime << ",\"writing\":false";
                if (sigmf::open_recording(p, &r, &err))
                    o << ",\"channels\":" << r.channels << ",\"rate\":" << fmt(r.rate, 1) << ",\"datatype\":\"" << json_escape(r.datatype)
                      << "\",\"seconds\":" << fmt(r.rate > 0 ? r.samples / r.rate : 0, 2) << ",\"freq_hz\":"
                      << fmt(r.streams[0].captures.front().freq_hz, 0) << ",\"start\":" << r.start_ms << ",\"format\":\""
                      << json_escape(r.format) << "\",\"description\":\"" << json_escape(r.description) << "\"";
                else
                    o << ",\"error\":\"" << json_escape(err) << "\"";
                o << "}";
            } else {
                continue;
            }
            items.push_back({mtime, o.str()});
        }
        closedir(d);
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.mtime > b.mtime; });
    std::ostringstream o;
    o << "{\"recordings\":{\"dir\":\"" << json_escape(g_set.dir) << "\",\"dir_abs\":\"" << json_escape(dir) << "\",\"items\":[";
    for (size_t i = 0; i < items.size(); i++) o << (i ? "," : "") << items[i].json;
    o << "]}}";
    return o.str();
}

// a recording's folder / archive in the current folder (by its listed name), "" = none
std::string item_path_locked(const std::string& name) {
    if (!valid_name(name)) return "";
    const std::string p = dir_abs_locked() + "/" + name;
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) return "";
    if (S_ISREG(st.st_mode) && ends_with(name, ".sigmf")) return p;
    if (!S_ISDIR(st.st_mode)) return "";
    // a folder only when it holds a recording's files
    bool rec = false;
    if (DIR* d = opendir(p.c_str())) {
        while (dirent* e = readdir(d))
            if (recording_file(e->d_name)) { rec = true; break; }
        closedir(d);
    }
    return rec ? p : "";
}

// is `name` (as a page sent it) the IQ or audio recording running now?
// fs_same_name: on Windows "iq_x" names the folder "IQ_x" too
bool being_recorded_locked(const std::string& name) {
    if (g_iq && fs_same_name(g_iq->name, name)) return true;
#ifdef _WIN32
    // the running audio recording's name = its status' first "name"
    const std::string aj = detail::audio_status_json();
    const size_t a = aj.find("\"name\":\"");
    const size_t b = a == std::string::npos ? std::string::npos : aj.find('"', a + 8);
    return b != std::string::npos && fs_same_name(aj.substr(a + 8, b - a - 8), name);
#else
    return detail::audio_status_json().find("\"name\":\"" + json_escape(name) + "\"") != std::string::npos;
#endif
}

std::string delete_locked(const std::string& name) {
    const std::string p = item_path_locked(name);
    if (p.empty()) return "no recording " + name;
    if (being_recorded_locked(name)) return "it is being recorded - stop it first";
    if (fs_same_name(playback::playing_path(), p)) return "it is playing - stop the playback first";
    struct stat st{};
    stat(p.c_str(), &st);
    if (S_ISREG(st.st_mode)) {
        if (unlink(p.c_str()) != 0) return std::string("can't delete: ") + strerror(errno);
        return "";
    }
    // a folder: only the files a recording consists of
    [[maybe_unused]] bool in_use = false;
    if (DIR* d = opendir(p.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            // its files + the metadata's tmp leftovers (".<file>.tmp")
            if ((recording_file(n) || (n.size() > 5 && n[0] == '.' && ends_with(n, ".tmp") && recording_file(n.substr(1, n.size() - 5)))) &&
                unlink((p + "/" + n).c_str()) != 0)
                in_use = true;
        }
        closedir(d);
    }
#ifdef _WIN32
    // Windows can't delete a file another program has open (a download being
    // sent, a media player, Explorer's preview) - not "other files"
    if (in_use) return "a file of the recording is in use (a download, a player?) - try again later";
#endif
    if (rmdir(p.c_str()) != 0) return "deleted the recording's files; the folder holds other files and was kept";
    return "";
}

// --- speed test ------------------------------------------------------------------
void speed_test(std::string dir) {
    const std::string path = dir + "/.kraken_speedtest";
    std::string err;
    double mbps = 0;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        err = std::string("can't write to ") + dir + ": " + strerror(errno);
    } else {
        std::vector<uint8_t> buf(4u << 20);
        uint64_t x = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < buf.size(); i++) {   // incompressible (some drives compress)
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            buf[i] = static_cast<uint8_t>(x);
        }
        const auto t0 = clk::now();
        const int steps = SPEEDTEST_MB / 4;
        uint64_t done = 0;
        for (int i = 0; i < steps; i++) {
            if (!write_all(fd, buf.data(), buf.size())) { err = std::string("writing failed: ") + strerror(errno); break; }
            done += buf.size();
            // keep it honest: write back as it goes (the recorder does the same)
            sync_file_range(fd, static_cast<off_t>(done - buf.size()), static_cast<off_t>(buf.size()), SYNC_FILE_RANGE_WRITE);
            std::lock_guard<std::mutex> lk(g_mu);
            g_test.pct = (i + 1) * 100 / steps;
            bump();
        }
        fdatasync(fd);
        const double s = std::chrono::duration<double>(clk::now() - t0).count();
        ::close(fd);
        if (err.empty() && s > 0) mbps = done / s / 1e6;
    }
    unlink(path.c_str());
    std::lock_guard<std::mutex> lk(g_mu);
    g_test.running = false;
    g_test.error = err;
    if (err.empty()) {
        g_speed[dir] = {mbps, detail::now_ms(), false};
        std::cout << "Recorder: " << dir << " writes " << fmt(mbps, 1) << " MB/s" << std::endl;
    }
    bump();
}

std::string settings_json_locked() {
    const Settings& s = g_set;
    std::ostringstream o;
    o << "{\"dir\":\"" << json_escape(s.dir) << "\",\"mode\":\"" << s.mode << "\",\"iq\":\"" << s.iq << "\",\"duration\":"
      << s.duration_s << ",\"band_vfo\":" << s.band_vfo << ",\"aformat\":\"" << s.aformat << "\",\"arate\":" << s.arate
      << ",\"akbps\":" << s.akbps << ",\"askip\":" << (s.askip ? "true" : "false") << ",\"avfos\":\"" << json_escape(s.avfos)
      << "\",\"location\":" << (s.location ? "true" : "false") << ",\"name\":\"" << json_escape(s.name) << "\",\"notes\":\""
      << json_escape(s.notes) << "\"}";
    return o.str();
}

}  // namespace

// --- detail (shared with recorder_audio.cpp) ----------------------------------------
namespace detail {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string name_time(int64_t ms) {
    const time_t t = static_cast<time_t>(ms / 1000);
    struct tm tm{};
    localtime_r(&t, &tm);
    char b[32];
    strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tm);
    return b;
}

std::string mhz_tag(double hz) {
    char b[48];
    const long long khz = std::llround(hz / 1000.0);
    snprintf(b, sizeof b, "%lld_%03lldMHz", khz / 1000, khz % 1000);
    return b;
}

uint64_t mem_budget() {
    // what a recording may hold in memory while the disk catches up: 40 % of
    // what is available, 64 MB .. 1.5 GB
    const uint64_t avail = mem_available();
    return std::clamp<uint64_t>(avail * 2 / 5, 64ull << 20, 1536ull << 20);
}

void finished(const std::string& kind, const std::string& name, const std::string& reason, uint64_t bytes, double seconds,
              const std::string& error) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_last = {kind, name, reason, error, bytes, seconds, now_ms()};
    bump_list();
}

void changed() { bump(); }

}  // namespace detail

// --- public ------------------------------------------------------------------------
void start() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::cout << "Recordings folder: " << dir_abs_locked() << std::endl;
}

void stop() {
    std::shared_ptr<IqSession> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_iq;
        g_iq_mode.store(0);
    }
    if (s) {
        s->shutdown.store(true);
        s->request_stop("kraken_doa shut down");
        // the writer drops what is still queued after 4 s (an urgent stop:
        // at once), then writes the metadata
        std::unique_lock<std::mutex> lk(g_mu);
        g_done_cv.wait_for(lk, std::chrono::seconds(platform::stop_is_urgent() ? 2 : 15),
                           [] { return g_iq == nullptr; });
    }
    detail::audio_shutdown();
}

bool iq_recording() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_iq != nullptr;
}

std::string item_path(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_mu);
    return item_path_locked(name);
}

std::string recording_name() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_iq ? g_iq->name : "";
}

bool is_setting(std::string_view m) {
    static const char* keys[] = {"RECORDER_DIR:", "RECORDER_MODE:", "RECORDER_IQ:", "RECORDER_DURATION:", "RECORDER_BAND_VFO:",
                                 "RECORDER_AUDIO_FORMAT:", "RECORDER_AUDIO_RATE:", "RECORDER_AUDIO_KBPS:",
                                 "RECORDER_AUDIO_SKIP:", "RECORDER_AUDIO_VFOS:", "RECORDER_LOCATION:"};
    for (const char* k : keys)
        if (m.starts_with(k)) return true;
    return false;
}

bool handle_command(std::string_view m, std::string* err, std::string* applied) {
    auto val = [&](size_t n) { return std::string(m.substr(n)); };
    std::lock_guard<std::mutex> lk(g_mu);
    applied->clear();
    if (m == "RECORDER_START") {
        if (busy_locked()) return *err = "already recording - stop it first", false;
        *err = g_set.mode == "audio" ? start_audio_locked() : start_iq_locked();
        if (!err->empty()) {
            g_error = *err;
            std::cout << "Recorder: not started - " << *err << std::endl;
            bump();
            return false;
        }
        g_error.clear();
        g_set.name.clear();    // the name and notes were this recording's
        g_set.notes.clear();
        bump_list();
        return true;
    }
    if (m == "RECORDER_STOP") {
        stop_recording_locked("stopped");
        bump();
        return true;
    }
    if (m == "RECORDER_LIST" || m == "GET_RECORDER") {
        bump_list();
        return true;
    }
    if (m.starts_with("RECORDER_DELETE:")) {
        *err = delete_locked(val(16));
        if (!err->empty() && err->rfind("deleted", 0) != 0) return false;
        std::cout << "Recorder: deleted " << val(16) << std::endl;
        err->clear();
        bump_list();
        return true;
    }
    if (m == "RECORDER_SPEEDTEST") {
        if (g_test.running) return *err = "a speed test is running", false;
        if (busy_locked()) return *err = "not while recording", false;
        const std::string dir = dir_abs_locked();
        if (!diskutil::mkdirs(dir)) return *err = "can't create the folder " + dir, false;
        g_test = {true, dir, "", 0};
        std::thread(speed_test, dir).detach();
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_NAME:")) {
        g_set.name = clean_name(val(14));
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_NOTES:")) {
        std::string n = val(15);
        if (n.size() > 500) n.resize(500);
        for (char& c : n)
            if (static_cast<unsigned char>(c) < 0x20) c = ' ';
        g_set.notes = n;
        bump();
        return true;
    }
    // settings
    if (busy_locked() && !m.starts_with("RECORDER_LOCATION:")) {
        // what a running recording uses can't change under it
        if (m.starts_with("RECORDER_DIR:") || m.starts_with("RECORDER_MODE:") || m.starts_with("RECORDER_IQ:") ||
            m.starts_with("RECORDER_BAND_VFO:") || m.starts_with("RECORDER_AUDIO_FORMAT:"))
            return *err = "not while recording", false;
    }
    if (m.starts_with("RECORDER_DIR:")) {
        // without a trailing '/' (Windows: '\' -> '/', and "E:/" stays the
        // drive's root - "E:" would be the current folder of drive E)
        std::string d = fs_normalize_dir(val(13));
        if (d.empty()) d = DEFAULT_DIR;
        if (d.size() > 400 || d.find('\n') != std::string::npos) return *err = "bad folder", false;
#ifdef _WIN32
        // characters no Windows file name can hold (':' only after a drive letter)
        if (d.find_first_of("*?\"<>|") != std::string::npos || (d.find(':') != std::string::npos && d.find(':') != 1) ||
            d.find(':', 2) != std::string::npos)
            return *err = "bad folder: Windows folder names can't hold * ? \" < > | :", false;
        // a share: Windows would sign in to the host a page names, as this
        // user (fs_compat.hpp). The settings replay too: a stored share
        // leaves the default folder
        if (fs_is_network_path(d)) {
            const char* e = getenv("KRAKEN_ALLOW_NETWORK_DIRS");
            if (!e || std::string(e) != "1") {
                std::cerr << "Recorder: folder " << d << " refused - a network share" << std::endl;
                return *err = "bad folder: network shares (\\\\server\\share) are refused - map the share to a drive letter, "
                              "or start kraken_doa with KRAKEN_ALLOW_NETWORK_DIRS=1", false;
            }
        }
#endif
        if (d != g_set.dir) {
            if (!SettingsStore::sandboxed() && !diskutil::mkdirs(diskutil::absolute(d))) return *err = "can't create the folder " + d, false;
            g_set.dir = d;
            bump_list();
        }
        *applied = "RECORDER_DIR:" + d;
        return true;
    }
    if (m.starts_with("RECORDER_MODE:")) {
        const std::string v = val(14);
        if (v != "iq" && v != "audio") return *err = "mode: iq or audio", false;
        g_set.mode = v;
        *applied = "RECORDER_MODE:" + v;
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_IQ:")) {
        const std::string v = val(12);
        if (v != "full" && v != "band") return *err = "IQ: full or band", false;
        g_set.iq = v;
        *applied = "RECORDER_IQ:" + v;
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_DURATION:")) {
        const int v = atoi(val(18).c_str());
        if (v != 0 && std::find(std::begin(DURATIONS), std::end(DURATIONS), v) == std::end(DURATIONS))
            return *err = "duration: 0 (no limit) or one of the offered times", false;
        g_set.duration_s = v;
        *applied = "RECORDER_DURATION:" + std::to_string(v);
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_BAND_VFO:")) {
        const int v = atoi(val(18).c_str());
        if (v < -1 || v > 999) return *err = "bad VFO", false;
        g_set.band_vfo = v;
        *applied = "RECORDER_BAND_VFO:" + std::to_string(v);
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_AUDIO_FORMAT:")) {
        const std::string v = val(22);
        if (v != "wav" && v != "opus") return *err = "format: wav or opus", false;
        g_set.aformat = v;
        *applied = "RECORDER_AUDIO_FORMAT:" + v;
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_AUDIO_RATE:")) {
        const int v = atoi(val(20).c_str());
        if (v != 48000 && v != 16000 && v != 8000) return *err = "rate: 48000, 16000 or 8000", false;
        g_set.arate = v;
        *applied = "RECORDER_AUDIO_RATE:" + std::to_string(v);
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_AUDIO_KBPS:")) {
        const int v = atoi(val(20).c_str());
        if (v != 16 && v != 24 && v != 32 && v != 64 && v != 96) return *err = "bit rate: 16, 24, 32, 64 or 96 kbit/s", false;
        g_set.akbps = v;
        *applied = "RECORDER_AUDIO_KBPS:" + std::to_string(v);
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_AUDIO_SKIP:")) {
        g_set.askip = val(20) == "1" || val(20) == "true";
        *applied = std::string("RECORDER_AUDIO_SKIP:") + (g_set.askip ? "1" : "0");
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_AUDIO_VFOS:")) {
        std::string v = val(20), o;
        if (v != "all") {
            std::stringstream ss(v);
            std::string t;
            while (std::getline(ss, t, ',')) {
                if (t.empty() || t.size() > 4 || t.find_first_not_of("0123456789") != std::string::npos) return *err = "VFOs: all or ids", false;
                o += (o.empty() ? "" : ",") + t;
            }
            if (o.empty()) return *err = "VFOs: all or ids", false;
            v = o;
        }
        g_set.avfos = v;
        *applied = "RECORDER_AUDIO_VFOS:" + v;
        bump();
        return true;
    }
    if (m.starts_with("RECORDER_LOCATION:")) {
        g_set.location = val(18) == "1" || val(18) == "true";
        *applied = std::string("RECORDER_LOCATION:") + (g_set.location ? "1" : "0");
        bump();
        return true;
    }
    *err = "unknown recorder command";
    return false;
}

std::string status_message() {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string dir = dir_abs_locked();
    uint64_t fr = 0, tot = 0;
    diskutil::disk_space(dir, &fr, &tot);
    std::string mount;
    const std::string mounts = diskutil::mounts_json(dir, &mount);
    const std::string fs = diskutil::fs_type(dir);
    Need n = need_locked();
    const uint64_t reserve = reserve_for(tot);
    const double usable = fr > reserve ? static_cast<double>(fr - reserve) : 0.0;
    double max_s = n.bps > 0 ? usable / n.bps : 0;
    if (fs == "vfat" && n.per_file_bps > 0) max_s = std::min(max_s, static_cast<double>(FAT_MAX) / n.per_file_bps);
    double ram_s = -1;   // < 0: the drive keeps up (or not measured)
    auto sp = g_speed.find(dir);
    if (sp != g_speed.end() && sp->second.mbps > 0 && sp->second.mbps * 1e6 < n.bps)
        ram_s = static_cast<double>(detail::mem_budget()) / (n.bps - sp->second.mbps * 1e6);
    std::ostringstream o;
    o << "{\"recorder\":{\"settings\":" << settings_json_locked() << ",\"durations\":[";
    for (size_t i = 0; i < std::size(DURATIONS); i++) o << (i ? "," : "") << DURATIONS[i];
    o << "],\"disk\":{\"dir_abs\":\"" << json_escape(dir) << "\",\"free\":" << fr << ",\"total\":" << tot << ",\"reserve\":"
      << reserve << ",\"fs\":\"" << json_escape(fs) << "\",\"mount\":\"" << json_escape(mount) << "\"},\"mounts\":" << mounts
      << ",\"need\":{\"bps\":" << fmt(n.bps, 0) << ",\"files\":" << n.files << ",\"rate\":" << fmt(n.rate, 1) << ",\"channels\":"
      << n.M << ",\"vfo\":" << n.vfo << ",\"max_s\":" << fmt(max_s, 1) << ",\"ram_s\":" << fmt(ram_s, 1) << ",\"why\":\""
      << json_escape(n.why) << "\",\"mem\":" << detail::mem_budget() << "}";
    if (sp != g_speed.end())
        o << ",\"speed\":{\"mbps\":" << fmt(sp->second.mbps, 2) << ",\"t\":" << sp->second.t_ms << ",\"live\":"
          << (sp->second.measured_live ? "true" : "false") << "}";
    o << ",\"test\":{\"running\":" << (g_test.running ? "true" : "false") << ",\"pct\":" << g_test.pct << ",\"error\":\""
      << json_escape(g_test.error) << "\"}";
    // the running recording
    std::string cur;
    if (g_iq) {
        const IqSession& s = *g_iq;
        const uint64_t w = s.a_written.load();
        std::ostringstream c;
        c << "{\"kind\":\"iq\",\"name\":\"" << json_escape(s.name) << "\",\"iq\":\"" << (s.band ? "band" : "full")
          << "\",\"state\":\"" << (s.finishing.load() ? "finishing" : "recording") << "\",\"channels\":" << s.M << ",\"rate\":"
          << fmt(s.rate, 1) << ",\"vfo\":" << s.vfo << ",\"started\":" << s.start_ms << ",\"elapsed_s\":"
          << fmt(static_cast<double>(s.a_taken.load()) / s.rate, 2) << ",\"written_s\":" << fmt(static_cast<double>(w) / s.rate, 2)
          << ",\"limit_s\":" << fmt(s.limit / s.rate, 1) << ",\"bytes\":" << s.a_bytes.load()
          << ",\"queued\":" << s.a_queued.load() << ",\"budget\":" << s.budget << ",\"in_bps\":" << fmt(s.in_bps.load(), 0)
          << ",\"out_bps\":" << fmt(s.out_bps.load(), 0) << ",\"bps\":" << fmt(s.rate * s.bpc * s.M, 0) << "}";
        cur = c.str();
    } else {
        cur = detail::audio_status_json();
    }
    o << ",\"cur\":" << (cur.empty() ? "null" : cur);
    if (!g_last.kind.empty())
        o << ",\"last\":{\"kind\":\"" << g_last.kind << "\",\"name\":\"" << json_escape(g_last.name) << "\",\"reason\":\""
          << json_escape(g_last.reason) << "\",\"error\":\"" << json_escape(g_last.error) << "\",\"bytes\":" << g_last.bytes
          << ",\"seconds\":" << fmt(g_last.seconds, 2) << ",\"t\":" << g_last.t_ms << "}";
    o << ",\"error\":\"" << json_escape(g_error) << "\"}}";
    return o.str();
}

std::string list_message() {
    std::lock_guard<std::mutex> lk(g_mu);
    return list_json_locked();
}

std::vector<std::string> tick_messages() {
    static uint64_t sent_ver = 0, sent_list = 0;
    std::vector<std::string> out;
    const uint64_t v = g_ver.load(), l = g_list_ver.load();
    bool active;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        active = g_iq != nullptr || detail::audio_running() || g_test.running;
    }
    // idle: also every 3 s - what it would record follows the VFOs, the mode, the drive
    static int64_t sent_ms = 0;
    const int64_t t = mono_ms();
    if (v != sent_ver || active || t - sent_ms >= 3000) {
        sent_ver = v;
        sent_ms = t;
        out.push_back(status_message());
    }
    if (l != sent_list) {
        sent_list = l;
        out.push_back(list_message());
    }
    return out;
}

// --- IQ taps ------------------------------------------------------------------------
namespace {
std::shared_ptr<IqSession> session() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_iq;
}

// limit reached / a change the recording can't follow: stop (producer side)
void producer_stop(IqSession& s, const std::string& why) {
    g_iq_mode.store(0);
    s.request_stop(why);
    detail::changed();
}
}  // namespace

void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, const PacketInfo& pi) {
    if (!full_active()) return;
    auto sp = session();
    if (!sp || sp->band) return;
    IqSession& s = *sp;
    if (pi.retuning) {
        // heimdall retunes: these samples are noise - left out, a gap noted
        if (s.taken) s.gap_pending += samples;
        return;
    }
    if (static_cast<int>(channels) != s.M) return producer_stop(s, "the number of antennas changed");
    uint32_t n = samples;
    if (s.limit) {
        if (s.taken >= s.limit) return producer_stop(s, "time limit");
        n = static_cast<uint32_t>(std::min<uint64_t>(n, s.limit - s.taken));
    }
    CapInfo ci;
    const double f0 = static_cast<double>(coherent_tuner_hz());
    for (int k = 0; k < s.M; k++) {
        const double hdr = pi.freq_hz ? pi.freq_hz[k] : 0;
        ci.freq[k] = std::fabs(hdr - f0) < 500 ? f0 : hdr;
        ci.gain[k] = pi.gain_db ? pi.gain_db[k] : -999.0f;
    }
    ci.phase = pi.phase_state;
    ci.noise = pi.noise;
    bool cap = !s.have_last || s.gap_pending;
    if (!cap) {
        for (int k = 0; k < s.M && !cap; k++)
            cap = std::fabs(ci.freq[k] - s.last.freq[k]) > 0.5 || std::fabs(ci.gain[k] - s.last.gain[k]) > 0.05f;
        cap = cap || ci.noise != s.last.noise || (ci.phase == 4) != (s.last.phase == 4);
    }
    if (cap) {
        ci.t_ms = detail::now_ms() - static_cast<int64_t>(samples / FS * 1000);
        const StationLocation st = station_info.resolve();
        ci.heading = st.heading_valid ? st.heading : -1;
        s.last = ci;
        s.have_last = true;
    }
    Chunk c;
    const uint64_t per = static_cast<uint64_t>(n) * 2;
    c.buf.reset(new uint8_t[per * static_cast<uint64_t>(s.M)]);
    for (int k = 0; k < s.M; k++)
        memcpy(c.buf.get() + per * static_cast<uint64_t>(k), iq + static_cast<size_t>(k) * samples * 2, per);
    c.n = n;
    c.cap = cap;
    if (cap) c.ci = ci;
    c.gap = s.gap_pending;
    if (!s.push(std::move(c), per * static_cast<uint64_t>(s.M))) {
        g_iq_mode.store(0);
        detail::changed();
        return;
    }
    s.gap_pending = 0;
    s.taken += n;
    s.a_taken.store(s.taken, std::memory_order_relaxed);
    if (s.limit && s.taken >= s.limit) producer_stop(s, "time limit");
}

void on_band_block(int vfo, uint64_t seq, const std::complex<float>* const* ch, int m, size_t n, float rate_hz, double rf_hz,
                   float offset_hz, bool noise, uint32_t phase_state) {
    if (band_vfo() != vfo || n == 0) return;
    auto sp = session();
    if (!sp || !sp->band || sp->vfo != vfo) return;
    IqSession& s = *sp;
    if (m != s.M) return producer_stop(s, "the number of antennas changed");
    if (std::fabs(rate_hz - s.rate) > 0.5) return producer_stop(s, "the VFO's bandwidth changed");
    if (s.have_seq && seq != s.last_seq + 1 && seq > s.last_seq)
        s.gap_pending += static_cast<uint64_t>(std::llround((seq - s.last_seq - 1) * 16384.0 * rate_hz / FS));
    s.last_seq = seq;
    s.have_seq = true;
    size_t take = n;
    if (s.limit) {
        if (s.taken >= s.limit) return producer_stop(s, "time limit");
        take = static_cast<size_t>(std::min<uint64_t>(n, s.limit - s.taken));
    }
    CapInfo ci;
    for (int k = 0; k < s.M; k++) {
        ci.freq[k] = rf_hz;
        ci.gain[k] = ChannelManager::get_gain(k);
    }
    ci.phase = phase_state;
    ci.noise = noise;
    bool cap = !s.have_last || s.gap_pending;
    if (!cap) {
        cap = std::fabs(ci.freq[0] - s.last.freq[0]) > 0.5 || ci.noise != s.last.noise || (ci.phase == 4) != (s.last.phase == 4);
        for (int k = 0; k < s.M && !cap; k++) cap = std::fabs(ci.gain[k] - s.last.gain[k]) > 0.05f;
    }
    if (cap) {
        ci.t_ms = detail::now_ms() - static_cast<int64_t>(take / rate_hz * 1000);
        const StationLocation st = station_info.resolve();
        ci.heading = st.heading_valid ? st.heading : -1;
        s.last = ci;
        s.have_last = true;
    }
    (void)offset_hz;
    Chunk c;
    const uint64_t per = static_cast<uint64_t>(take) * 4;
    c.buf.reset(new uint8_t[per * static_cast<uint64_t>(s.M)]);
    for (int k = 0; k < s.M; k++) {
        auto* o = reinterpret_cast<int16_t*>(c.buf.get() + per * static_cast<uint64_t>(k));
        const std::complex<float>* x = ch[k];
        for (size_t i = 0; i < take; i++) {
            const float re = std::clamp(x[i].real() * static_cast<float>(BAND_SCALE), -32767.0f, 32767.0f);
            const float im = std::clamp(x[i].imag() * static_cast<float>(BAND_SCALE), -32767.0f, 32767.0f);
            o[2 * i] = static_cast<int16_t>(std::lrint(re));        // little-endian hosts (aarch64 / x86)
            o[2 * i + 1] = static_cast<int16_t>(std::lrint(im));
        }
    }
    c.n = static_cast<uint32_t>(take);
    c.cap = cap;
    if (cap) c.ci = ci;
    c.gap = s.gap_pending;
    if (!s.push(std::move(c), per * static_cast<uint64_t>(s.M))) {
        g_iq_mode.store(0);
        detail::changed();
        return;
    }
    s.gap_pending = 0;
    s.taken += take;
    s.a_taken.store(s.taken, std::memory_order_relaxed);
    if (s.limit && s.taken >= s.limit) producer_stop(s, "time limit");
}

// --- HTTP ----------------------------------------------------------------------------
bool download(const std::string& rel, Download* d, std::string* status, std::string* err) {
    std::lock_guard<std::mutex> lk(g_mu);
    *d = Download();
    const size_t sl = rel.find('/');
    const std::string name = rel.substr(0, sl);
    const std::string file = sl == std::string::npos ? "" : rel.substr(sl + 1);
    const std::string p = item_path_locked(name);
    if (p.empty() || (!file.empty() && !valid_name(file))) {
        *status = "404 Not Found";
        *err = "no such recording";
        return false;
    }
    if (being_recorded_locked(name)) {
        *status = "409 Conflict";
        *err = "it is still being recorded";
        return false;
    }
    struct stat st{};
    stat(p.c_str(), &st);
    auto ctype = [](const std::string& f) {
        if (ends_with(f, ".wav")) return "audio/wav";
        if (ends_with(f, ".opus")) return "audio/ogg";
        if (ends_with(f, ".sigmf-meta") || ends_with(f, ".sigmf-collection")) return "application/json";
        return "application/octet-stream";
    };
    if (!file.empty() && (!S_ISDIR(st.st_mode) || !recording_file(file))) {
        *status = "404 Not Found";
        *err = "not a file of a recording";
        return false;
    }
    if (!file.empty() || S_ISREG(st.st_mode)) {
        const std::string fp = file.empty() ? p : p + "/" + file;
        struct stat fs{};
        if (stat(fp.c_str(), &fs) != 0 || !S_ISREG(fs.st_mode)) {
            *status = "404 Not Found";
            *err = "no such file";
            return false;
        }
        d->filename = file.empty() ? name : file;
        d->content_type = ends_with(d->filename, ".sigmf") ? "application/x-tar" : ctype(d->filename);
        d->total = static_cast<uint64_t>(fs.st_size);
        d->parts.push_back({"", fp, 0, d->total});
        return true;
    }
    // a folder -> one tar file: the SigMF archive of an IQ recording
    std::vector<std::pair<std::string, struct stat>> files;
    if (DIR* dd = opendir(p.c_str())) {
        while (dirent* e = readdir(dd)) {
            const std::string fn = e->d_name;
            if (fn.empty() || fn[0] == '.') continue;
            struct stat fs{};
            if (recording_file(fn) && stat((p + "/" + fn).c_str(), &fs) == 0 && S_ISREG(fs.st_mode)) files.emplace_back(fn, fs);
        }
        closedir(dd);
    }
    if (files.empty()) {
        *status = "404 Not Found";
        *err = "not a recording";
        return false;
    }
    bool iq = false;
    for (const auto& f : files) iq = iq || ends_with(f.first, ".sigmf-meta");
    // the collection first, then each antenna's metadata + data
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        const bool ca = ends_with(a.first, ".sigmf-collection"), cb = ends_with(b.first, ".sigmf-collection");
        if (ca != cb) return ca;
        return a.first < b.first;
    });
    d->filename = name + (iq ? ".sigmf" : ".tar");
    d->content_type = "application/x-tar";
    d->parts.push_back({sigmf::tar_dir_header(name + "/", static_cast<int64_t>(st.st_mtime)), "", 0, 0});
    for (const auto& [fn, fs] : files) {
        const uint64_t size = static_cast<uint64_t>(fs.st_size);
        d->parts.push_back({sigmf::tar_file_header(name + "/" + fn, size, static_cast<int64_t>(fs.st_mtime)), "", 0, 0});
        d->parts.push_back({"", p + "/" + fn, 0, size});
        if (sigmf::tar_pad(size)) d->parts.push_back({std::string(static_cast<size_t>(sigmf::tar_pad(size)), '\0'), "", 0, 0});
    }
    d->parts.push_back({std::string(sigmf::TAR_END_BYTES, '\0'), "", 0, 0});
    for (auto& part : d->parts) {
        if (part.path.empty()) part.len = part.mem.size();
        d->total += part.len;
    }
    return true;
}

// --- upload ----------------------------------------------------------------------
struct Upload {
    std::string name, part, final_path;
    uint64_t length = 0, got = 0;
    int fd = -1;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> q;
    uint64_t queued = 0;
    bool finish = false, abort = false, failed = false;
    std::string error;
    std::function<void()> on_drain;
    std::function<void(bool, std::string)> done;
    std::thread th;
};

namespace {
constexpr uint64_t UP_HIGH = 16u << 20, UP_LOW = 4u << 20;

void upload_main(Upload* u) {
    uint64_t written = 0;
    for (;;) {
        std::string chunk;
        std::function<void()> drain;
        bool fin = false, ab = false;
        {
            std::unique_lock<std::mutex> lk(u->mu);
            u->cv.wait(lk, [&] { return !u->q.empty() || u->finish || u->abort; });
            ab = u->abort;
            if (!ab && !u->q.empty()) {
                chunk = std::move(u->q.front());
                u->q.pop_front();
            } else if (!ab) {
                fin = u->finish;
            }
        }
        if (ab) break;
        if (!chunk.empty()) {
            if (!u->failed && !write_all(u->fd, reinterpret_cast<const uint8_t*>(chunk.data()), chunk.size())) {
                std::lock_guard<std::mutex> lk(u->mu);
                u->failed = true;
                u->error = std::string("writing failed: ") + strerror(errno);
            }
            written += chunk.size();
            if (written % (8u << 20) < chunk.size()) {
                sync_file_range(u->fd, 0, 0, SYNC_FILE_RANGE_WRITE);
                posix_fadvise(u->fd, 0, static_cast<off_t>(written > (16u << 20) ? written - (16u << 20) : 0), POSIX_FADV_DONTNEED);
            }
            std::lock_guard<std::mutex> lk(u->mu);
            u->queued -= std::min<uint64_t>(u->queued, chunk.size());
            if (u->on_drain && u->queued < UP_LOW) drain = std::move(u->on_drain), u->on_drain = nullptr;
        }
        if (drain) drain();
        if (fin) break;
    }
    bool ok = false;
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(u->mu);
        if (u->abort) msg = "aborted";
        else if (u->failed) msg = u->error;
        else if (written != u->length) msg = "the upload ended after " + std::to_string(written) + " of " + std::to_string(u->length) + " bytes";
    }
    if (u->fd >= 0) {
        if (msg.empty() && fdatasync(u->fd) != 0) msg = std::string("writing failed: ") + strerror(errno);
        ::close(u->fd);
        u->fd = -1;
    }
    if (msg.empty()) {
        sigmf::Recording r;
        std::string err;
        if (!sigmf::open_recording(u->part, &r, &err)) msg = "not a usable SigMF archive: " + err;
    }
    if (msg.empty()) {
        // fs_replace = rename() as on Linux; on Windows it also waits out a
        // virus scanner that still has the just-written file open
        if (fs_replace(u->part.c_str(), u->final_path.c_str()) != 0) msg = std::string("can't rename: ") + strerror(errno);
        else ok = true;
    }
    if (!ok) unlink(u->part.c_str());
    std::cout << "Recorder: upload " << u->name << (ok ? " stored" : " failed: " + msg) << std::endl;
    if (ok) bump_list();
    auto done = std::move(u->done);
    const bool aborted = u->abort;
    std::thread self = std::move(u->th);
    self.detach();
    delete u;
    if (done && !aborted) done(ok, ok ? "stored" : msg);
}
}  // namespace

Upload* upload_begin(const std::string& filename, uint64_t length, std::string* status, std::string* err) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!ends_with(filename, ".sigmf") || !valid_name(filename)) {
        *status = "400 Bad Request";
        *err = "only SigMF archives (<name>.sigmf, letters, digits, _ - .) can be uploaded";
        return nullptr;
    }
    if (length == 0) {
        *status = "411 Length Required";
        *err = "the upload needs a length";
        return nullptr;
    }
    const std::string dir = dir_abs_locked();
    if (!diskutil::mkdirs(dir)) {
        *status = "500 Internal Server Error";
        *err = "can't create the folder " + dir;
        return nullptr;
    }
    uint64_t fr = 0, tot = 0;
    diskutil::disk_space(dir, &fr, &tot);
    if (length + reserve_for(tot) > fr) {
        *status = "507 Insufficient Storage";
        *err = "not enough free space (" + fmt(fr / 1e9, 2) + " GB free, keeping " + fmt(reserve_for(tot) / 1e9, 2) + " GB)";
        return nullptr;
    }
    if (diskutil::fs_type(dir) == "vfat" && length > FAT_MAX) {
        *status = "507 Insufficient Storage";
        *err = "the drive is FAT32: a file can't reach 4 GB";
        return nullptr;
    }
    std::string base = filename.substr(0, filename.size() - 6);
    const std::string name = unique_folder(dir, base);
    auto* u = new Upload;
    u->name = name + ".sigmf";
    u->final_path = dir + "/" + u->name;
    u->part = dir + "/.upload-" + std::to_string(detail::now_ms()) + "-" + u->name;   // still ends in .sigmf
    u->length = length;
    u->fd = ::open(u->part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (u->fd < 0) {
        *status = "500 Internal Server Error";
        *err = std::string("can't create the file: ") + strerror(errno);
        delete u;
        return nullptr;
    }
    u->th = std::thread(upload_main, u);
    std::cout << "Recorder: receiving " << u->name << " (" << length / 1000000 << " MB)" << std::endl;
    return u;
}

bool upload_data(Upload* u, std::string_view chunk, bool* pause, std::string* err) {
    std::lock_guard<std::mutex> lk(u->mu);
    if (u->failed) return *err = u->error, false;
    u->got += chunk.size();
    if (u->got > u->length) return *err = "more data than announced", false;
    if (!chunk.empty()) {
        u->q.emplace_back(chunk);
        u->queued += chunk.size();
        u->cv.notify_all();
    }
    *pause = u->queued >= UP_HIGH;
    return true;
}

void upload_on_drain(Upload* u, std::function<void()> f) {
    bool now = false;
    {
        std::lock_guard<std::mutex> lk(u->mu);
        if (u->queued < UP_LOW) now = true;
        else u->on_drain = std::move(f);
    }
    if (now && f) f();
}

void upload_finish(Upload* u, std::function<void(bool, std::string)> done) {
    std::lock_guard<std::mutex> lk(u->mu);
    u->done = std::move(done);
    u->finish = true;
    u->cv.notify_all();
}

void upload_abort(Upload* u) {
    std::lock_guard<std::mutex> lk(u->mu);
    u->abort = true;
    u->on_drain = nullptr;
    u->cv.notify_all();
}

}  // namespace rec
