#pragma once

// Local DoA recording.
//
// Periodically captures the current direction-of-arrival "lobe" data (the same
// payload served by /DOA_value.html) and appends it to a file on the device:
//   - Legacy CSV: byte-identical to DOA_value.html, one capture per line.
//   - SQLite:     a single indexed .sqlite file (peak fields + quantized
//                 pseudospectrum blob) that a downstream map app can open.
//
// The capture/format helpers below are the single source of truth shared with
// the DOA_value.html HTTP handler so the logged CSV can never drift from it.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

// One bearing line for one decimator at one instant. Mirrors a DOA_value.html row.
struct DoaRecord {
    int64_t  timestamp_ms = 0;     // capture time; internal logging/record provenance
    // Android Doppler compatibility adjustment (2026): keep source-frame identity.
    int64_t  source_stamp_ms = 0;  // MUSIC frame stamp — legacy freshness + dedup key
    int      decimator_id = 0;
    float    app_bearing = 0;      // 360 - bearing, wrapped to [0,360); sub-degree
                                   // (parabolic-interpolated peak), as in DOA_value.html
    double   confidence = 0.0;
    double   power_db = 0.0;          // Internal MUSIC eigenvalue-derived metric
    double   legacy_power_db = -100.0; // Legacy Android RF-like peak [-100, 0] dB
    uint64_t freq_hz = 0;
    std::string antenna;           // "UCA" / "ULA" / "Custom"
    std::string station_id_csv;    // comma/newline-stripped callsign
    double   lat = 0.0, lon = 0.0;
    double   gps_heading = 0.0, compass_heading = 0.0;
    std::string heading_sensor;    // "GPS" / "Compass"
    std::vector<double> spectrum;  // doa_log[i] + shift (>= 0), one value per angle
};

// True while the server is pulsing the noise source for a retune calibration;
// bearings computed then are garbage and must not be logged/served.
bool doa_is_calibrating();

// A VFO's received power as V1 and the Android app report it ("power"): the
// peak of the averaged FFT across the VFO passband in dBFS, clamped to
// [-100, 0] (-100 also = no FFT data yet). Read from the FFT of the VFO's
// tuner - the active channel in coherent mode, its own tuner_channel in
// wideband scan / independent mode, as the FFT squelch does. The one source
// for DOA_value.html field 4 and system_status decimator_squelch[].power_dbfs
// (the app's V2 connection), so both connection methods report one scale.
float vfo_power_dbfs(int tuner_channel, float offset_hz, int bandwidth_hz);

// Snapshot the current bearing rows for all enabled decimators. Caller must
// have already checked doa_is_calibrating() (returns rows regardless).
std::vector<DoaRecord> capture_doa_records();

// Exact DOA_value.html line for one record (", " separators, trailing " \n").
std::string format_doa_csv_line(const DoaRecord& rec);

// All recordings live in a single fixed folder ("doa_recordings/" under the
// app's working directory) so the browser can list/download/delete them without
// ever exposing arbitrary device paths.
struct RecFile {
    std::string name;
    uint64_t    size = 0;
    int64_t     mtime_ms = 0;
};
std::string doa_recordings_dir();                            // absolute path, created on demand
std::string doa_sanitize_filename(const std::string& name); // basename only; "" if invalid
std::vector<RecFile> doa_list_recordings();                 // sorted, sidecar files hidden
bool doa_delete_recording(const std::string& name);         // deletes file (+ -wal/-shm)

class DoaLogger {
public:
    enum class Format { CSV, SQLITE };

    DoaLogger() = default;
    ~DoaLogger();

    // Config (thread-safe). Interval applies live; format/path take effect on
    // the next start().
    void setIntervalSeconds(double seconds);   // clamped to [0.001, 60]
    void setFormat(Format fmt);
    void setFormatString(const std::string& s); // "csv" | "sqlite"
    void setFilename(const std::string& name);  // bare name; saved under doa_recordings/

    int  intervalMs() const { return interval_ms_.load(std::memory_order_relaxed); }
    bool isRunning() const  { return running_.load(std::memory_order_relaxed); }
    std::string filename() const;        // configured target filename
    std::string activeFilename() const;  // file currently being written ("" if stopped)
    uint64_t    fileBytes() const { return file_bytes_.load(std::memory_order_relaxed); }  // live size of active file
    uint64_t    entries() const { return entry_count_.load(std::memory_order_relaxed); }   // entries written this session

    // Open the sink and begin recording. Returns false (and sets last_error_)
    // without starting if the path is empty or the file can't be opened.
    bool start();
    void stop();

    // Append the "recording":{...} object (no leading comma) to a JSON stream,
    // for inclusion in the periodic system_status broadcast.
    void appendStatusJson(std::string& out) const;

private:
    void loggerThread();
    bool openSink(Format fmt, const std::string& path);
    void closeSink();
    bool writeRecords(const std::vector<DoaRecord>& recs);  // CSV append or SQLite tx
    bool sqliteWriteRecord(const DoaRecord& rec);
    void syncSink();          // force durable to disk (fsync / WAL checkpoint)
    void refreshFileBytes();
    void setError(const std::string& msg);
    static const char* formatToString(Format fmt);

    std::atomic<bool> running_{false};
    std::atomic<int>  interval_ms_{1000};          // 1 ms .. 60 s
    std::atomic<uint64_t> entry_count_{0};         // entries written this session
    std::atomic<uint64_t> file_bytes_{0};

    mutable std::mutex state_mtx_;                  // guards the strings below
    Format       format_ = Format::SQLITE;          // configured
    std::string  filename_;                         // configured (bare name)
    Format       active_format_ = Format::SQLITE;    // currently open
    std::string  active_filename_;                   // currently open (bare name)
    std::string  last_error_;

    std::thread logger_thread_;

    // Per-decimator dedup/throttle state (logger thread only).
    std::unordered_map<int, int64_t> last_stamp_;     // last frame stamp written
    std::unordered_map<int, int64_t> last_write_ms_;  // last write wall-clock

    // Durability: fsync / WAL-checkpoint at most this often so a power cut loses
    // only a bounded tail (an app crash never loses flushed data). Logger-thread only.
    static constexpr int64_t SYNC_INTERVAL_MS = 30000;
    int64_t last_sync_ms_ = 0;
    bool    dirty_since_sync_ = false;

    // CSV sink
    FILE* csv_file_ = nullptr;

    // SQLite sink
    sqlite3*      db_ = nullptr;
    sqlite3_stmt* insert_stmt_ = nullptr;
};

extern DoaLogger doa_logger;
