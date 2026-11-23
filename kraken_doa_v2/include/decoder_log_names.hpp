#pragma once

// Decoder data log: the names of its files and what retention does with a
// day - pure functions (no file system, no state), so they can be tested on
// their own (decoder_log.cpp uses them; scratch harness tests them).
//
// Layout (2026-11-19): <dir>/<name>/<name>-YYYY-MM-DD.jsonl, one folder per
// decoder (<name> = safe_folder_name(the record's "dec")), the transcripts in
// <dir>/transcripts/; older versions wrote one combined file per day,
// <dir>/decoders-YYYY-MM-DD.jsonl - still read, aged and deleted by the same
// rule. A finished day stays plain for `keep` days, then gzip
// (<file>.jsonl.gz) for `zip` more, then it is deleted.

#include "utils/log_retention.hpp"

#include <cstdio>
#include <string>

namespace declog {

constexpr const char* TRANSCRIPTS = "transcripts";   // the transcripts' folder (type "voice")
constexpr const char* CLIPS = "voice_clips";         // Voice to Text's audio (voice2text.cpp) - never a decoder's
constexpr const char* COMBINED = "decoders";         // the old combined file's prefix (in <dir> itself)

// A record's "dec" (a plugin id; NBFM / AM / WBFM on transcripts) -> a folder
// name that can't leave <dir> whatever the id is: [a-z0-9_-] only (A-Z
// lowered, anything else '_'), at most 32 characters, "other" when no letter
// or digit is left ("", "..", "/"), and a "dec-" prefix on names Windows
// reserves (con, prn, aux, nul, com1-9, lpt1-9) and on our own folders
// (transcripts, voice_clips) so a plugin of that name can't mix with them
inline std::string safe_folder_name(const std::string& id) {
    std::string s;
    bool alnum = false;
    for (unsigned char c : id) {
        if (s.size() >= 32) break;
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            s += static_cast<char>(c);
            alnum = true;
        } else {
            s += c == '-' ? '-' : '_';
        }
    }
    if (!alnum) return "other";
    static const char* const reserved[] = {"con", "prn", "aux", "nul", TRANSCRIPTS, CLIPS};
    for (const char* r : reserved)
        if (s == r) return "dec-" + s;
    if (s.size() == 4 && (s.compare(0, 3, "com") == 0 || s.compare(0, 3, "lpt") == 0) && s[3] >= '0' && s[3] <= '9')
        return "dec-" + s;
    return s;
}

// days since 1970-01-01 of a civil date (no time zone involved; the shared
// helper's); the string form: -1 = not a date
using logret::day_number;
inline long day_number(const std::string& ymd) {
    if (ymd.size() != 10) return -1;
    for (size_t i = 0; i < 10; i++)
        if (i == 4 || i == 7 ? ymd[i] != '-' : (ymd[i] < '0' || ymd[i] > '9')) return -1;
    int y, m, d;
    if (sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    return day_number(y, m, d);
}

// .jsonl, .jsonl.gz, .jsonl.gz.part (a gzip interrupted) - the shared helper's kinds
using Kind = logret::Kind;

// <prefix>-YYYY-MM-DD.jsonl[.gz[.part]] -> its date and kind; "" / NONE for any
// other name. prefix = the folder's name (a decoder folder), or COMBINED in <dir>
inline std::string log_file_date(const std::string& prefix, const std::string& name, Kind* kind) {
    *kind = Kind::NONE;
    const size_t p = prefix.size();
    if (prefix.empty() || name.size() < p + 17 || name.compare(0, p, prefix) != 0 || name[p] != '-') return "";
    const std::string d = name.substr(p + 1, 10);
    if (day_number(d) < 0) return "";
    const std::string rest = name.substr(p + 11);
    if (rest == ".jsonl") *kind = Kind::PLAIN;
    else if (rest == ".jsonl.gz") *kind = Kind::GZ;
    else if (rest == ".jsonl.gz.part") *kind = Kind::PART;
    else return "";
    return d;
}

inline std::string log_file_name(const std::string& prefix, const std::string& date, Kind kind) {
    return prefix + "-" + date + (kind == Kind::GZ ? ".jsonl.gz" : kind == Kind::PART ? ".jsonl.gz.part" : ".jsonl");
}

// What the retention pass does with a day (plain for keep = N days, gzip for
// zip = M more, then deleted; N = 0 keeps everything forever) - since
// 2026-11-23 the rule every daily log shares, utils/log_retention.hpp (the
// Wideband board and host telemetry logs follow it too); unchanged
using Action = logret::Action;
using logret::retention_action;

}  // namespace declog
