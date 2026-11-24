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
// compressed days (WB_LOG_ZIP, 0-3650, default 30; 0 = deleted after Keep),
// then deleted - the decoder log's rule and code (utils/log_retention.hpp),
// so a long deployment cannot fill the SD card. The pass runs on its own
// thread: at the first use, with the STORED settings (read from the settings
// store - no wait for heimdall), at the UTC day change and every hour (a
// timer of its own, not behind the sampler's statfs), and a few seconds after
// the last change of the two (gzip only; the deletions 10 min after it);
// never while a debug playback runs. The gzip is
// durable against a power cut. An older build's wb_telemetry.csv[.N] /
// wb_events.csv[.N] (size-rotated) are still read and are deleted by their
// modification time after Keep + Then compressed days (never compressed).
// The folders are created by the first write only.

// Bytes read from heimdall's control socket (any chunking; lines are split here)
void wb_board_ingest(const char* data, size_t n);

// The heimdall "downconverter" object plus "age_s" (seconds since it was
// received), for system_status; empty when heimdall sent none (not
// --wideband) or nothing for 30 s.
std::string wb_board_json();

// Logging settings (remembered by SettingsStore; the stored values are read at
// the first use). The setters clamp and return the value in force; a Keep /
// Then compressed change runs a gzip-only pass a few seconds after the last
// change, and the deletions 10 min after it (the grace: two changes made one
// after the other never delete by the rule in between).
void wb_board_set_log_enabled(bool on);
int wb_board_set_log_period(int seconds);    // 1 .. 3600
int wb_board_set_log_keep_days(int days);    // 1 .. 365: days plain
int wb_board_set_log_zip_days(int days);     // 0 .. 3650: then days gzipped (0 = none)
// WB_LOG_RETENTION: both at once (the page) - one change, one pass; the values
// in force (clamped) in *keep_out / *zip_out
void wb_board_set_log_retention(int keep, int zip, int* keep_out, int* zip_out);
// The settings replay ran (ControlHandler::apply_persisted_settings). The
// passes no longer wait for it (the stored values are read at the first use);
// it only makes sure the sampler and the passes are started
void wb_board_settings_restored();

// The telemetry sheet's data: the last `range_s` seconds of the log reduced to
// at most ~600 buckets (min / mean / max per bucket of voltage, current,
// temperature, humidity; null where nothing was logged), the events in that
// range and the size of the log (plain and gzipped). Reads the files (plain
// and .gz): call it off the uWS loop.
std::string wb_board_history_json(long range_s, bool host = false);

// The logged rows of the last `range_s` seconds (at most 31 days) as one CSV file in wb_logs/
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
// board's log. The alarm levels have hysteresis (up at once, down after 30 s
// back inside the limit minus a margin). Call with every hardware-stats
// reading (it rate-limits itself; the uWS loop's 2 Hz status): the free space
// and the throttling flags come from a sampler thread that measures them every
// 10 s (statfs, the Pi 5's vcgencmd), so this never waits for statfs or a
// child process; it still appends the row (every WB_LOG_PERIOD) and an event
// line (on a change) to today's files on the loop - small writes. The fields
// go into system_status.hw (",\"disk_free_mb\":...", alarms, logging).
void host_log_tick(double temp_c, double cpu_pct, double ram_used_mb, double ram_total_mb);
std::string host_log_json_fields();
