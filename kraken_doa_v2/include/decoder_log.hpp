#pragma once

// Decoder data log: what the digital decoders report, to disk, for later
// analysis (⚙ Setup Settings → Decoder Logging; a sidebar box until 2026-11-19).
//
// A folder per decoder, a file per day (2026-11-19): <dir>/<name>/<name>-YYYY-MM-DD.jsonl
// (JSON Lines, local date; <name> = decoder_log_names.hpp's safe_folder_name of the
// record's "dec" - two VFOs running one decoder share its file, told apart by
// "vfo" / "mhz"; incidents in the file of the decoder whose message it was), the
// transcripts in <dir>/transcripts/transcripts-YYYY-MM-DD.jsonl. Before that one
// combined <dir>/decoders-YYYY-MM-DD.jsonl[.gz] - left where it is, read by the
// assistant's search and aged by the same rule. One record per line, e.g.
//   {"t":"2026-10-06T18:44:01.123+13:00","vfo":0,"mhz":1090.000000,"dec":"adsb","type":"raw","data":"8D4840D6... -14.8"}
// Types: "event" (a decoder's log line), "message" (a text message, e.g. a
// pager text: from + text), "position" (a map point: id, label, kind, lat,
// lon, alt_m, kmh, hdg - at most one per object every N s, the VFO decoder's
// "Positions every", dig::Options::pos_s), "raw"
// (raw frames: ADS-B Mode S hex, APRS TNC2, POCSAG / FLEX codewords),
// "incident" (an address found in a message, incidents.cpp), "voice" (a
// transcript of what was said - voice2text.cpp: text, src, dur, conf, from /
// to (talker), doa / brg, clip; t = when the transmission began).
//
// Nothing is written while set_enabled(false) ("Allow logging to disk" is a
// permission). Then what is written is chosen where the data is (2026-11-19;
// "What to log" type ticks before): each VFO decoder's Logging
// (dig::Options::log) - Off nothing, Log its events, messages, positions and
// incidents, Log All those plus raw frames (the decoder also reports the
// repeated control-channel messages then); the transcripts by their own
// Logging, the "voice" member of DECODER_LOG_TYPES.
//
// SD card friendly: records collect in memory (a buffer per file) and are
// appended every FLUSH_S seconds, one write per file; a file without a record
// for 10 min is closed. Retention (a background pass at startup once the
// settings are replayed, at the day change, on a settings change, every 10 min):
// a finished day stays plain for days_to_keep days, then gzip
// (<file>.jsonl.gz) for zip_days more, then it is deleted (days 0 = forever:
// nothing compressed or deleted; only files named like ours are touched).
// Writing pauses below MIN_FREE_MB free space.
// Settings (persisted, control_handler.cpp): DECODER_LOG:0|1,
// DECODER_LOG_TYPES:event,message,position,raw,incident,voice, DECODER_LOG_DAYS:N
// (0 = keep forever), DECODER_LOG_ZIP_DAYS:M (0 = none), DECODER_LOG_POS_S:N (the
// old global position interval: sets every VFO's pos_s), DECODER_LOG_DIR:path.

#include <cstdint>
#include <string>
#include <vector>

namespace declog {

constexpr int FLUSH_S = 5;
constexpr long MIN_FREE_MB = 100;
constexpr const char* DEFAULT_DIR = "decoder_logs";

void start();      // worker thread + the decoders' record handler
void stop();       // shutdown: write what is buffered, close the file
void flush();      // write what is buffered now (power.cpp, before a shutdown); keeps logging
// the settings replay ran (ControlHandler::apply_persisted_settings): the retention
// pass waits for it, so a stored Keep / Then compressed is what it applies
void settings_restored();

void set_enabled(bool on);
// comma-separated subset of event,message,position,raw,incident,voice; false
// if it names something else. Only "voice" (transcripts) decides what is
// written; the rest is kept as given (an older page's What to log ticks)
bool set_types(const std::string& csv);
// the types last set include "raw": VFO options stored before the per-VFO
// Logging get Log All (control_handler.cpp, the DECIMATORS replay)
bool legacy_raw();
void set_days(int days);            // plain days: 0 = keep forever (nothing compressed or deleted)
void set_zip_days(int days);        // gzip days after those: 0 = none (deleted after the plain days)
// DECODER_LOG_POS_S, 1 .. 3600: the old global "Positions every" - since
// 2026-11-19 each VFO decoder's (dig::Options::pos_s); this one is what a VFO
// stored without its own gets (pos_interval(), the DECIMATORS replay)
void set_pos_interval(int seconds);
int pos_interval();
// relative paths are relative to kraken_doa's working directory; created if
// missing. false + err if it can't be written to (the old folder stays)
bool set_dir(const std::string& path, std::string* err);

// incidents.cpp: an address found in a message; log_mode = the Logging of the
// VFO decoder whose message it was (written at Log / Log All)
void record_incident(int vfo, int log_mode, const std::string& plugin, const std::string& address, double lat,
                     double lon, const std::string& precision, const std::string& confidence, const std::string& text);

// voice2text.cpp: a transcript. t_ms = when the transmission began, source =
// the "dec" field (plugin id or NBFM / AM / WBFM), fields = the rest of the
// record as JSON members, each starting with a comma
void record_voice(int vfo, double rf_hz, int64_t t_ms, const std::string& source, const std::string& fields);
// logging is allowed and the transcripts' Logging is Log
bool voice_logged();
// The log folder (as set, possibly relative) and its Keep days (0 = forever):
// Voice to Text keeps its audio clips there (voice_clips/), for the plain days
// only (Opus is compressed already; Then compressed does not apply to them)
std::string folder();
int keep_days();

// The log files of one day (YYYY-MM-DD) in dir, plain and gz: every decoder's
// folder, transcripts/, the old combined file (folder ""). The one place that
// knows the layout (the assistant's search uses it)
struct DayFile {
    std::string path, folder;
    bool gz = false;
};
std::vector<DayFile> day_files(const std::string& dir, const std::string& date);

// {"declog":{settings ("voice": the transcripts' Logging; "zip_days", "pos_per_vfo"), "dir_abs", "free",
//  "total", "today_bytes", "per_day", "steady_bytes", "gz_ratio", "gz_measured", "files", "total_bytes",
//  "buffered", "dropped", "compressing", "open_files", "error", "mounts":[...]}}
std::string status_message();

}  // namespace declog
