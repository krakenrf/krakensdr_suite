#pragma once

#include <cstddef>
#include <string>

// KrakenSDR Wideband board telemetry, relayed from heimdall.
//
// heimdall polls the board MCU (INA219 input current / voltage, SHT4x or
// Si7020 temperature / humidity) once a second and puts the result in the
// "downconverter" object of its 2 Hz control-port status JSON. kraken_doa
// used to read and discard that status; the drain now hands every chunk to
// wb_board_ingest(), which keeps the latest object for the UI
// (system_status.wb_telemetry) and logs it:
//   wb_logs/wb_telemetry_YYYYMMDD.csv  one file per UTC day, one row every
//                             10 s (the period and the on/off switch are
//                             settings: WB_LOG_PERIOD / WB_LOG_ENABLE, default
//                             10 s / on)
//   wb_logs/wb_events_YYYYMMDD.csv  one row per alarm change, firmware
//                             warning, board identification and lost /
//                             restored data (at most 5 MB a day)
// A finished day stays a plain file for the Keep days (WB_LOG_KEEP, 1-365,
// default 7), then is gzipped in place (..._YYYYMMDD.csv.gz) for the Then
// compressed days (WB_LOG_ZIP, 0-3650, default 365; 0 = deleted after Keep),
// then deleted - the decoder log's rule and code (utils/log_retention.hpp),
// so a long deployment cannot fill the SD card. The pass runs on its own
// thread, not before the settings replay (wb_board_settings_restored), then
// on a change of the two, at the UTC day change and every hour. An older
// build's wb_telemetry.csv[.N] / wb_events.csv[.N] (size-rotated) are still
// read and are deleted by their modification time after Keep + Then
// compressed days (never compressed).

// Bytes read from heimdall's control socket (any chunking; lines are split here)
void wb_board_ingest(const char* data, size_t n);

// The heimdall "downconverter" object plus "age_s" (seconds since it was
// received), for system_status; empty when heimdall sent none (not
// --wideband) or nothing for 30 s.
std::string wb_board_json();

// Logging settings (remembered by SettingsStore). The setters clamp and
// return the value in force; a Keep / Then compressed change runs the pass.
void wb_board_set_log_enabled(bool on);
int wb_board_set_log_period(int seconds);    // 1 .. 3600
int wb_board_set_log_keep_days(int days);    // 1 .. 365: days plain
int wb_board_set_log_zip_days(int days);     // 0 .. 3650: then days gzipped (0 = none)
// The settings replay ran (ControlHandler::apply_persisted_settings): the
// retention pass waits for it - a pass with the defaults could compress or
// delete days the stored settings keep
void wb_board_settings_restored();

// The telemetry sheet's data: the last `range_s` seconds of the log reduced to
// at most ~600 buckets (min / mean / max per bucket of voltage, current,
// temperature, humidity; null where nothing was logged), the events in that
// range and the size of the log (plain and gzipped). Reads the files (plain
// and .gz): call it off the uWS loop.
std::string wb_board_history_json(long range_s, bool host = false);

// The logged rows of the last `range_s` seconds as one CSV file in wb_logs/
// (a temporary export file, removed by a later export; gz: gzipped, .csv.gz).
// Returns its path, "" if nothing could be written; *download_name gets a
// name for the browser.
std::string wb_board_export_csv(long range_s, std::string* download_name, bool host = false, bool gz = false);

// The computer itself, logged the same way (host = true above reads this log):
//   host_logs/host_telemetry_YYYYMMDD.csv  CPU temperature and load, memory
//                             used / total, free / total space of the drive the
//                             logs are on, the Raspberry Pi's throttling flags
//   host_logs/host_events_YYYYMMDD.csv alarm changes: temperature (warn 75 C,
//                             alarm 82 C), memory (85 % / 95 % used), storage
//                             (under 10 % or 2 GB / under 3 % or 500 MB free),
//                             under-voltage or throttling reported by the Pi
// with the same on/off, period, Keep and Then compressed settings as the
// board's log. Call with every hardware-stats reading (it rate-limits itself;
// the uWS loop's 2 Hz status): the free space and the throttling flags come
// from a sampler thread that measures them every 10 s (statfs, the Pi 5's
// vcgencmd), so this never waits on a disk or a child process. The fields go
// into system_status.hw (",\"disk_free_mb\":...", alarms, logging).
void host_log_tick(double temp_c, double cpu_pct, double ram_used_mb, double ram_total_mb);
std::string host_log_json_fields();
