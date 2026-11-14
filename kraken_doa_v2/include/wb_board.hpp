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
//                             10 s (the period, the on/off switch and how many
//                             days are kept are settings: WB_LOG_PERIOD /
//                             WB_LOG_ENABLE / WB_LOG_KEEP, default 10 s / on /
//                             30 days; older days are deleted)
//   wb_logs/wb_events.csv     one row per alarm change, firmware warning,
//                             board identification and lost / restored data
//                             (rotates at 5 MB, 3 old files kept)
// so a long deployment cannot fill the SD card. wb_telemetry.csv[.N] of an
// older build is still read for the history and expires by its file date.

// Bytes read from heimdall's control socket (any chunking; lines are split here)
void wb_board_ingest(const char* data, size_t n);

// The heimdall "downconverter" object plus "age_s" (seconds since it was
// received), for system_status; empty when heimdall sent none (not
// --wideband) or nothing for 30 s.
std::string wb_board_json();

// Logging settings (remembered by SettingsStore). The setters clamp and
// return the value in force.
void wb_board_set_log_enabled(bool on);
int wb_board_set_log_period(int seconds);    // 1 .. 3600
int wb_board_set_log_keep_days(int days);    // 1 .. 365

// The telemetry sheet's data: the last `range_s` seconds of the log reduced to
// at most ~600 buckets (min / mean / max per bucket of voltage, current,
// temperature, humidity; null where nothing was logged), the events in that
// range and the size of the log. Reads the files: call it off the uWS loop.
std::string wb_board_history_json(long range_s);

// The logged rows of the last `range_s` seconds as one CSV file in wb_logs/
// (a temporary export file, removed by a later export). Returns its path, ""
// if nothing could be written; *download_name gets a name for the browser.
std::string wb_board_export_csv(long range_s, std::string* download_name);
