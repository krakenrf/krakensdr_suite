#pragma once

// Decoder data log: everything the digital decoders report, to disk, for
// later analysis (sidebar → 🗂 Decoder Logging).
//
// One file per day, <dir>/decoders-YYYY-MM-DD.jsonl (JSON Lines, local date),
// one record per line, e.g.
//   {"t":"2026-10-06T18:44:01.123+13:00","vfo":0,"mhz":1090.000000,"dec":"adsb","type":"raw","data":"8D4840D6... -14.8"}
// Types (each can be switched off): "event" (a decoder's log line), "message"
// (a text message, e.g. a pager text: from + text), "position" (a map point:
// id, label, kind, lat, lon, alt_m, kmh, hdg - at most one per object every
// pos_interval_s), "raw" (raw frames: ADS-B Mode S hex, APRS TNC2, POCSAG
// codewords), "incident" (an address found in a message, incidents.cpp),
// "voice" (a transcript of what was said - voice2text.cpp: text, src, dur,
// conf, from / to (talker), doa / brg, clip; t = when the transmission began).
//
// SD card friendly: records collect in memory and are appended every
// FLUSH_S seconds. At local midnight the day's file is closed and gzipped
// (decoders-YYYY-MM-DD.jsonl.gz, in the background); files older than
// days_to_keep are deleted (only files named like that - nothing else in
// the folder is touched). Writing pauses below MIN_FREE_MB free space.
// Settings (persisted, control_handler.cpp): DECODER_LOG:0|1,
// DECODER_LOG_TYPES:event,message,position,raw,incident,voice, DECODER_LOG_DAYS:N
// (0 = keep forever), DECODER_LOG_POS_S:N, DECODER_LOG_DIR:path.

#include <cstdint>
#include <string>

namespace declog {

constexpr int FLUSH_S = 5;
constexpr long MIN_FREE_MB = 100;
constexpr const char* DEFAULT_DIR = "decoder_logs";

void start();      // worker thread + the decoders' record handler
void stop();       // shutdown: write what is buffered, close the file

void set_enabled(bool on);
// comma-separated subset of event,message,position,raw,incident,voice; false
// if it names something else
bool set_types(const std::string& csv);
void set_days(int days);            // 0 = keep forever
void set_pos_interval(int seconds); // 1 .. 3600
// relative paths are relative to kraken_doa's working directory; created if
// missing. false + err if it can't be written to (the old folder stays)
bool set_dir(const std::string& path, std::string* err);

// incidents.cpp: an address found in a message
void record_incident(int vfo, const std::string& plugin, const std::string& address, double lat, double lon,
                     const std::string& precision, const std::string& confidence, const std::string& text);

// voice2text.cpp: a transcript. t_ms = when the transmission began, source =
// the "dec" field (plugin id or NBFM / AM / WBFM), fields = the rest of the
// record as JSON members, each starting with a comma
void record_voice(int vfo, double rf_hz, int64_t t_ms, const std::string& source, const std::string& fields);
// logging is on and this record type is ticked
bool type_logged(const std::string& type);
// The log folder (as set, possibly relative) and its Keep days (0 = forever):
// Voice to Text keeps its audio clips there (voice_clips/), as long as the logs
std::string folder();
int keep_days();

// {"declog":{settings, "dir_abs", "free", "total", "today_bytes", "per_day",
//  "files", "total_bytes", "buffered", "dropped", "error", "mounts":[...]}}
std::string status_message();

}  // namespace declog
