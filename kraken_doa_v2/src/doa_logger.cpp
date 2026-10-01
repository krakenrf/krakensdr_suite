#include "doa_logger.hpp"

#include "globals.hpp"
#include "config.hpp"
#include "types.hpp"
#include "channel_manager.hpp"
#include "decimator_manager.hpp"
#include "scanner_manager.hpp"
#include "station_info.hpp"
#include "signal_processing/music_processor.hpp"
#include "utils/json_escape.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;

// Single global instance.
DoaLogger doa_logger;

// ---------------------------------------------------------------------------
// Shared DoA capture / formatting (one source of truth with DOA_value.html)
// ---------------------------------------------------------------------------

bool doa_is_calibrating() {
    // "Retune calibration in progress": while the server retunes it pulses the
    // noise source and bearings computed on that data are garbage. Gate ONLY on
    // the noise source (+ a short hold for bursts between polls), matching the
    // DOA_value.html handler. Steady-state DoA reports phase_state 4, so a
    // phase_state test would wrongly freeze everything.
    return scanner_manager.isNoiseRecentlyActive();
}

std::vector<DoaRecord> capture_doa_records() {
    std::vector<DoaRecord> out;

    int64_t timestamp = chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count();

    // Station identity + effective location (resolved from the selected source).
    StationLocation sl = station_info.resolve();
    double doa_gps_heading = (sl.heading_valid && !sl.from_compass) ? sl.heading : 0.0;
    double doa_compass_heading = (sl.heading_valid && sl.from_compass) ? sl.heading : 0.0;
    const char* doa_heading_sensor = sl.from_compass ? "Compass" : "GPS";

    // CSV-safe callsign: commas/newlines would corrupt the record.
    std::string station_id_csv = sl.id;
    for (char& c : station_id_csv)
        if (c == ',' || c == '\n' || c == '\r') c = ' ';

    auto all_decimators = decimator_manager.getAllDecimators();

    for (const auto& dec : all_decimators) {
        if (!dec || !dec->music_processor || dec->being_deleted.load()) continue;
        if (!dec->music_processor->isEnabled()) continue;

        auto [bearing, confidence] = dec->music_processor->getPeakAngleWithConfidence();
        if (bearing < 0) continue;

        auto spectrum = dec->music_processor->getPseudospectrum();
        int num_angles = dec->music_processor->getNumAngles();
        if (static_cast<int>(spectrum.size()) != num_angles || num_angles <= 0) continue;

        float eigen_ratio = dec->music_processor->getEigenvalueRatio();
        float power_db = 10.0f * std::log10(std::max(eigen_ratio, 0.001f));

        float center_freq = ChannelManager::get_frequency(active_channel.load());
        float freq_hz = center_freq + dec->frequency_offset_hz;

        const char* ant_type;
        switch (dec->music_processor->getArrayTopology()) {
            case ArrayTopology::UCA: ant_type = "UCA"; break;
            case ArrayTopology::ULA: ant_type = "ULA"; break;
            default: ant_type = "Custom"; break;
        }

        // Unit-circle -> compass sense, keeping the sub-degree interpolated value
        float app_bearing = 360.0f - bearing;
        if (app_bearing >= 360.0f) app_bearing -= 360.0f;
        if (app_bearing < 0.0f) app_bearing = 0.0f;

        // The legacy DOA_value.html / CSV format is fixed at one value per
        // integer degree (360 total): the KrakenSDR Android app parses exactly
        // 360 spectrum values and breaks on anything else. MUSIC may run at a
        // finer angular resolution (num_angles > 360, e.g. 3600 at 0.1 deg), so
        // resample the pseudospectrum onto the 360-point integer-degree grid.
        // At 1 deg resolution num_angles is already 360 and idx == d, so this is
        // an exact passthrough (output stays byte-identical to before).
        constexpr int LEGACY_ANGLES = 360;
        double max_amp = spectrum.cwiseAbs().maxCoeff();
        std::vector<double> doa_log(LEGACY_ANGLES);
        double min_log = 0.0;
        for (int d = 0; d < LEGACY_ANGLES; d++) {
            int idx = static_cast<int>(std::lround(
                static_cast<double>(d) * num_angles / LEGACY_ANGLES));
            if (idx >= num_angles) idx -= num_angles;  // wrap (360 deg == 0 deg)
            double val = (max_amp > 1e-15) ? (std::abs(spectrum(idx)) / max_amp)
                                           : std::abs(spectrum(idx));
            double log_val = 10.0 * std::log10(std::max(val, 1e-10));
            if (log_val < -100.0) log_val = -100.0;
            doa_log[d] = log_val;
            if (d == 0 || log_val < min_log) min_log = log_val;
        }
        double shift = std::abs(min_log);

        DoaRecord rec;
        rec.timestamp_ms = timestamp;
        rec.source_stamp_ms = dec->music_processor->getResultStampMs();
        rec.decimator_id = dec->id;
        rec.app_bearing = app_bearing;
        rec.confidence = confidence;
        rec.power_db = power_db;
        rec.freq_hz = static_cast<uint64_t>(freq_hz);
        rec.antenna = ant_type;
        rec.station_id_csv = station_id_csv;
        rec.lat = sl.lat;
        rec.lon = sl.lon;
        rec.gps_heading = doa_gps_heading;
        rec.compass_heading = doa_compass_heading;
        rec.heading_sensor = doa_heading_sensor;
        rec.spectrum.resize(LEGACY_ANGLES);
        for (int i = 0; i < LEGACY_ANGLES; i++) rec.spectrum[i] = doa_log[i] + shift;

        out.push_back(std::move(rec));
    }

    return out;
}

std::string format_doa_csv_line(const DoaRecord& rec) {
    // Station location block (positions 9-13): lat, lon, gps_heading,
    // compass_heading, heading_sensor. Lat/lon at 6dp, headings at 1dp.
    std::ostringstream gps_ss;
    gps_ss << std::fixed << std::setprecision(6) << rec.lat << ", " << rec.lon
           << ", " << std::setprecision(1) << rec.gps_heading << ", "
           << rec.compass_heading << ", " << rec.heading_sensor;

    // LEGACY FORMAT LOCK: the KrakenSDR Android app parses this line (served
    // verbatim on DOA_value.html), so the bearing stays an INTEGER degree
    // exactly as the original krakensdr_doa software emitted it. The
    // sub-degree interpolated bearing is preserved in rec.app_bearing and in
    // the SQLite log; only this text format rounds.
    int legacy_bearing = static_cast<int>(std::lround(rec.app_bearing)) % 360;

    std::ostringstream line;
    line << std::fixed;
    line << rec.timestamp_ms << ", "
         << legacy_bearing << ", "
         << std::setprecision(2) << rec.confidence << ", "
         << std::setprecision(1) << rec.power_db << ", "
         << rec.freq_hz << ", "
         << rec.antenna << ", 0, " << rec.station_id_csv << ", "
         << gps_ss.str() << ", R, R, R, R";

    for (double v : rec.spectrum) {
        line << ", " << std::setprecision(2) << v;
    }
    line << " \n";
    return line.str();
}

// ---------------------------------------------------------------------------
// Recordings folder management (fixed "doa_recordings/" under the working dir)
// ---------------------------------------------------------------------------

std::string doa_recordings_dir() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::current_path(ec) / "doa_recordings";
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

std::string doa_sanitize_filename(const std::string& name) {
    // Keep only the final path component and reject anything that could escape
    // the recordings folder or create hidden/odd files.
    std::filesystem::path p(name);
    std::string base = p.filename().string();
    if (base.empty() || base == "." || base == "..") return "";
    if (base.find('/') != std::string::npos || base.find('\\') != std::string::npos) return "";
    if (base[0] == '.') return "";  // no hidden files
    // Names reach browsers (the recordings list) and JSON; markup and quote
    // characters in a name were an HTML-injection route into every client.
    for (unsigned char c : base) {
        if (c < 0x20 || c == 0x7f) return "";
        if (std::strchr("\"'<>`&", c)) return "";
    }
    return base;
}

std::vector<RecFile> doa_list_recordings() {
    std::vector<RecFile> out;
    std::error_code ec;
    std::filesystem::path dir = doa_recordings_dir();
    auto ends_with = [](const std::string& s, const char* suf) {
        size_t n = std::strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    for (std::filesystem::directory_iterator it(dir,
             std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        std::string name = it->path().filename().string();
        // Hide SQLite sidecar / temp files; only the openable recordings show.
        if (ends_with(name, "-wal") || ends_with(name, "-shm") ||
            ends_with(name, "-journal") || ends_with(name, ".tmp")) continue;
        RecFile rf;
        rf.name = name;
        auto sz = it->file_size(fec);
        rf.size = fec ? 0 : static_cast<uint64_t>(sz);
        struct stat stt;
        if (::stat(it->path().c_str(), &stt) == 0)
            rf.mtime_ms = static_cast<int64_t>(stt.st_mtime) * 1000;
        out.push_back(std::move(rf));
    }
    std::sort(out.begin(), out.end(),
              [](const RecFile& a, const RecFile& b) { return a.mtime_ms > b.mtime_ms; });  // newest first
    return out;
}

bool doa_delete_recording(const std::string& name) {
    std::string base = doa_sanitize_filename(name);
    if (base.empty()) return false;
    std::error_code ec;
    std::filesystem::path dir = doa_recordings_dir();
    bool removed = std::filesystem::remove(dir / base, ec);
    // Best-effort cleanup of SQLite sidecars.
    std::filesystem::remove(dir / (base + "-wal"), ec);
    std::filesystem::remove(dir / (base + "-shm"), ec);
    return removed;
}

// ---------------------------------------------------------------------------
// DoaLogger
// ---------------------------------------------------------------------------

namespace {
    std::string iso8601_utc_now() {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        gmtime_r(&t, &tmv);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        return std::string(buf);
    }
}

DoaLogger::~DoaLogger() {
    stop();
}

const char* DoaLogger::formatToString(Format fmt) {
    return (fmt == Format::CSV) ? "csv" : "sqlite";
}

void DoaLogger::setIntervalSeconds(double seconds) {
    if (seconds < 0.001) seconds = 0.001;
    if (seconds > 60.0)  seconds = 60.0;
    int ms = static_cast<int>(std::lround(seconds * 1000.0));
    if (ms < 1)     ms = 1;
    if (ms > 60000) ms = 60000;
    interval_ms_.store(ms, std::memory_order_relaxed);
}

void DoaLogger::setFormat(Format fmt) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    format_ = fmt;
}

void DoaLogger::setFormatString(const std::string& s) {
    setFormat((s == "csv") ? Format::CSV : Format::SQLITE);
}

void DoaLogger::setFilename(const std::string& name) {
    std::string base = doa_sanitize_filename(name);  // bare name, "" if invalid
    std::lock_guard<std::mutex> lk(state_mtx_);
    filename_ = base;
}

std::string DoaLogger::filename() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return filename_;
}

std::string DoaLogger::activeFilename() const {
    std::lock_guard<std::mutex> lk(state_mtx_);
    return active_filename_;
}

void DoaLogger::setError(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        last_error_ = msg;
    }
    if (!msg.empty()) cerr << "DoaLogger: " << msg << endl;
}

bool DoaLogger::start() {
    if (running_.load()) return true;

    Format fmt;
    std::string fname;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        fmt = format_;
        fname = filename_;
    }
    if (fname.empty()) {
        setError("No recording filename set");
        return false;
    }
    std::string full = doa_recordings_dir() + "/" + fname;

    if (!openSink(fmt, full)) return false;  // openSink sets last_error_

    entry_count_.store(0, std::memory_order_relaxed);
    file_bytes_.store(0, std::memory_order_relaxed);
    last_stamp_.clear();
    last_write_ms_.clear();
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        active_format_ = fmt;
        active_filename_ = fname;
        last_error_.clear();
    }

    running_.store(true);
    logger_thread_ = std::thread(&DoaLogger::loggerThread, this);
    refreshFileBytes();

    cout << "DoaLogger: recording started (" << formatToString(fmt) << ") -> " << full << endl;
    return true;
}

void DoaLogger::stop() {
    if (!running_.exchange(false)) return;
    if (logger_thread_.joinable()) logger_thread_.join();
    closeSink();
    refreshFileBytes();
    cout << "DoaLogger: recording stopped (" << entry_count_.load() << " entries)" << endl;
}

bool DoaLogger::openSink(Format fmt, const std::string& path) {
    // Best-effort: create the parent directory so a typed-but-missing folder
    // still works (the browser usually picks an existing one).
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    if (fmt == Format::CSV) {
        // Appending CSV text to an existing SQLite recording of the same name
        // corrupts it (the reverse is refused by SQLite itself: "file is not a
        // database"). Check the 16-byte SQLite header first.
        if (std::FILE* probe = std::fopen(path.c_str(), "rb")) {
            char hdr[16] = {};
            const size_t got = std::fread(hdr, 1, sizeof(hdr), probe);
            std::fclose(probe);
            if (got == sizeof(hdr) && std::memcmp(hdr, "SQLite format 3", sizeof(hdr)) == 0) {
                setError("'" + p.filename().string() + "' is an SQLite recording - pick another "
                         "name, or the SQLite format, to add to it");
                return false;
            }
        }
        csv_file_ = std::fopen(path.c_str(), "a");
        if (!csv_file_) {
            setError("Cannot open CSV file for append: " + path);
            return false;
        }
        return true;
    }

    // SQLite
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        setError(std::string("Cannot open SQLite DB: ") + (db_ ? sqlite3_errmsg(db_) : "unknown"));
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }

    const char* schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA temp_store=MEMORY;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS entries("
          "id INTEGER PRIMARY KEY,"
          "ts_ms INTEGER NOT NULL,"
          "decimator_id INTEGER,"
          "bearing_deg REAL,"
          "confidence REAL,"
          "power_db REAL,"
          "freq_hz INTEGER,"
          "antenna TEXT,"
          "station_id TEXT,"
          "lat REAL, lon REAL,"
          "gps_heading REAL, compass_heading REAL, heading_sensor TEXT,"
          "spectrum BLOB, spectrum_len INTEGER, spec_min_db REAL, spec_max_db REAL);"
        "CREATE INDEX IF NOT EXISTS idx_entries_ts ON entries(ts_ms);";

    char* err = nullptr;
    if (sqlite3_exec(db_, schema, nullptr, nullptr, &err) != SQLITE_OK) {
        setError(std::string("SQLite schema error: ") + (err ? err : "unknown"));
        if (err) sqlite3_free(err);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Meta rows: set once at creation (OR IGNORE keeps created_utc stable on append).
    auto set_meta = [&](const char* k, const std::string& v) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO meta(key,value) VALUES(?,?)",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    };
    set_meta("format_version", "1");
    set_meta("app", "kraken_doa_v2");
    set_meta("created_utc", iso8601_utc_now());

    const char* ins =
        "INSERT INTO entries(ts_ms,decimator_id,bearing_deg,confidence,power_db,freq_hz,"
        "antenna,station_id,lat,lon,gps_heading,compass_heading,heading_sensor,"
        "spectrum,spectrum_len,spec_min_db,spec_max_db) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db_, ins, -1, &insert_stmt_, nullptr) != SQLITE_OK) {
        setError(std::string("SQLite prepare error: ") + sqlite3_errmsg(db_));
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    return true;
}

void DoaLogger::closeSink() {
    // Final durable flush so a clean Stop leaves a complete, self-contained file.
    if (insert_stmt_) { sqlite3_finalize(insert_stmt_); insert_stmt_ = nullptr; }
    if (db_) {
        // Fold the WAL into the main DB and fsync, so the .sqlite is complete
        // on its own (closing the last connection also checkpoints, but be explicit).
        sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
        sqlite3_close(db_);
        db_ = nullptr;
    }
    if (csv_file_) {
        std::fflush(csv_file_);
        ::fsync(::fileno(csv_file_));
        std::fclose(csv_file_);
        csv_file_ = nullptr;
    }
}

// Force everything written so far all the way to the storage medium so a sudden
// power loss can't discard it. CSV -> fsync the file; SQLite -> checkpoint the
// WAL into the main DB (which fsyncs it) and truncate the WAL.
void DoaLogger::syncSink() {
    if (csv_file_) {
        std::fflush(csv_file_);
        ::fsync(::fileno(csv_file_));
    }
    if (db_) {
        sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
    }
}

bool DoaLogger::sqliteWriteRecord(const DoaRecord& rec) {
    // Quantize the (already >= 0, min-shifted) spectrum to uint8 over [0, max].
    const int n = static_cast<int>(rec.spectrum.size());
    double maxv = 0.0;
    for (double v : rec.spectrum) if (v > maxv) maxv = v;
    std::vector<uint8_t> blob(n);
    if (maxv > 1e-12) {
        for (int i = 0; i < n; i++) {
            long b = std::lround(rec.spectrum[i] / maxv * 255.0);
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            blob[i] = static_cast<uint8_t>(b);
        }
    }

    sqlite3_stmt* st = insert_stmt_;
    sqlite3_bind_int64 (st, 1,  rec.timestamp_ms);
    sqlite3_bind_int   (st, 2,  rec.decimator_id);
    sqlite3_bind_double(st, 3,  static_cast<double>(rec.app_bearing));
    sqlite3_bind_double(st, 4,  rec.confidence);
    sqlite3_bind_double(st, 5,  rec.power_db);
    sqlite3_bind_int64 (st, 6,  static_cast<sqlite3_int64>(rec.freq_hz));
    sqlite3_bind_text  (st, 7,  rec.antenna.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (st, 8,  rec.station_id_csv.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 9,  rec.lat);
    sqlite3_bind_double(st, 10, rec.lon);
    sqlite3_bind_double(st, 11, rec.gps_heading);
    sqlite3_bind_double(st, 12, rec.compass_heading);
    sqlite3_bind_text  (st, 13, rec.heading_sensor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob  (st, 14, blob.data(), n, SQLITE_TRANSIENT);
    sqlite3_bind_int   (st, 15, n);
    sqlite3_bind_double(st, 16, 0.0);
    sqlite3_bind_double(st, 17, maxv);

    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    if (rc != SQLITE_DONE) {
        setError(std::string("SQLite insert failed: ") + sqlite3_errmsg(db_));
        return false;
    }
    return true;
}

bool DoaLogger::writeRecords(const std::vector<DoaRecord>& recs) {
    if (recs.empty()) return true;

    if (active_format_ == Format::CSV) {
        if (!csv_file_) return false;
        for (const auto& r : recs) {
            std::string line = format_doa_csv_line(r);
            if (std::fputs(line.c_str(), csv_file_) == EOF) {
                setError("CSV write failed");
                return false;
            }
        }
        std::fflush(csv_file_);
        return true;
    }

    // SQLite: one transaction per tick.
    if (!db_ || !insert_stmt_) return false;
    char* err = nullptr;
    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, &err);
    if (err) { sqlite3_free(err); err = nullptr; }

    bool ok = true;
    for (const auto& r : recs) {
        if (!sqliteWriteRecord(r)) { ok = false; break; }
    }

    sqlite3_exec(db_, ok ? "COMMIT" : "ROLLBACK", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    return ok;
}

void DoaLogger::refreshFileBytes() {
    std::string fname;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        fname = active_filename_;
    }
    if (fname.empty()) return;
    const std::string path = doa_recordings_dir() + "/" + fname;

    // SQLite: report the LOGICAL database size (page_count * page_size). That is
    // the size the .sqlite will have once the WAL is checkpointed - i.e. the size
    // of the file you download - and it grows monotonically with the data. The
    // raw on-disk sizes can't be used here: the -wal balloons with page-sized
    // frames and is truncated at every checkpoint (sawtooth), and -shm is a
    // transient index, not part of the recording.
    if (db_) {
        auto pragma_u64 = [&](const char* sql) -> uint64_t {
            uint64_t v = 0;
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW) v = static_cast<uint64_t>(sqlite3_column_int64(st, 0));
                sqlite3_finalize(st);
            }
            return v;
        };
        uint64_t bytes = pragma_u64("PRAGMA page_count;") * pragma_u64("PRAGMA page_size;");
        if (bytes) { file_bytes_.store(bytes, std::memory_order_relaxed); return; }
    }

    // CSV (or stopped / PRAGMA failed): the actual file on disk is exact. For a
    // stopped SQLite recording the WAL has been folded in, so this is the final size.
    std::error_code ec;
    auto s = std::filesystem::file_size(path, ec);
    if (!ec) file_bytes_.store(static_cast<uint64_t>(s), std::memory_order_relaxed);
}

void DoaLogger::loggerThread() {
    last_sync_ms_ = chrono::duration_cast<chrono::milliseconds>(
        chrono::system_clock::now().time_since_epoch()).count();
    dirty_since_sync_ = false;

    while (running_.load(std::memory_order_relaxed)) {
        int interval_ms = interval_ms_.load(std::memory_order_relaxed);
        // Poll in small slices: keeps Stop prompt and honors sub-frame intervals.
        int sleep_ms = (interval_ms < 5) ? interval_ms : 5;
        if (sleep_ms < 1) sleep_ms = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        if (!running_.load(std::memory_order_relaxed)) break;

        int64_t now_ms = chrono::duration_cast<chrono::milliseconds>(
            chrono::system_clock::now().time_since_epoch()).count();

        // Periodic durability flush (runs even during calibration / idle gaps so a
        // power cut loses at most SYNC_INTERVAL_MS of already-written data).
        if (dirty_since_sync_ && (now_ms - last_sync_ms_) >= SYNC_INTERVAL_MS) {
            syncSink();
            last_sync_ms_ = now_ms;
            dirty_since_sync_ = false;
        }

        if (doa_is_calibrating()) continue;  // skip garbage during retune

        // Cheap pre-check before the (heavier) capture: is any decimator due?
        // Due = produced a NEW frame since its last write AND the requested
        // interval has elapsed. This is what makes the effective rate
        // min(requested, data rate) without ever logging a frame twice.
        bool any_due = false;
        for (const auto& d : decimator_manager.getAllDecimators()) {
            if (!d || !d->music_processor || d->being_deleted.load()) continue;
            if (!d->music_processor->isEnabled()) continue;
            int64_t stamp = d->music_processor->getResultStampMs();
            if (stamp == 0) continue;  // nothing computed yet
            int id = d->id;
            auto its = last_stamp_.find(id);
            if (its != last_stamp_.end() && its->second == stamp) continue;  // not new
            auto itw = last_write_ms_.find(id);
            if (itw != last_write_ms_.end() && (now_ms - itw->second) < interval_ms) continue;
            any_due = true;
            break;
        }
        if (!any_due) continue;

        std::vector<DoaRecord> records = capture_doa_records();

        std::vector<DoaRecord> due;
        due.reserve(records.size());
        for (auto& rec : records) {
            int id = rec.decimator_id;
            auto its = last_stamp_.find(id);
            if (its != last_stamp_.end() && its->second == rec.source_stamp_ms) continue;  // dup frame
            auto itw = last_write_ms_.find(id);
            if (itw != last_write_ms_.end() && (now_ms - itw->second) < interval_ms) continue;  // throttle
            due.push_back(std::move(rec));
        }
        if (due.empty()) continue;

        if (writeRecords(due)) {
            for (const auto& rec : due) {
                last_stamp_[rec.decimator_id] = rec.source_stamp_ms;
                last_write_ms_[rec.decimator_id] = now_ms;
            }
            entry_count_.fetch_add(due.size(), std::memory_order_relaxed);
            dirty_since_sync_ = true;   // mark for the next periodic fsync/checkpoint
            refreshFileBytes();
        }
    }
}

void DoaLogger::appendStatusJson(std::string& out) const {
    bool running = running_.load(std::memory_order_relaxed);
    std::string fname, err;
    Format fmt;
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        fname = running ? active_filename_ : filename_;
        err = last_error_;
        fmt = running ? active_format_ : format_;
    }

    out += "\"recording\":{";
    out += "\"running\":";
    out += running ? "true" : "false";
    out += ",\"format\":\"";
    out += formatToString(fmt);
    out += "\",\"file\":\"";
    out += json_escape(fname);
    out += "\",\"bytes\":";
    out += std::to_string(file_bytes_.load(std::memory_order_relaxed));
    out += ",\"entries\":";
    out += std::to_string(entry_count_.load(std::memory_order_relaxed));
    out += ",\"interval_ms\":";
    out += std::to_string(interval_ms_.load(std::memory_order_relaxed));
    out += ",\"error\":\"";
    out += json_escape(err);
    out += "\"}";
}
