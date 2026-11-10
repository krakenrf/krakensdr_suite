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
//   wb_logs/wb_telemetry.csv  one row every 10 s
//   wb_logs/wb_events.csv     one row per alarm change, firmware warning,
//                             board identification and lost / restored data
// Both files rotate at 5 MB (3 old files kept), so a long deployment cannot
// fill the SD card.

// Bytes read from heimdall's control socket (any chunking; lines are split here)
void wb_board_ingest(const char* data, size_t n);

// The heimdall "downconverter" object plus "age_s" (seconds since it was
// received), for system_status; empty when heimdall sent none (not
// --wideband) or nothing for 30 s.
std::string wb_board_json();
