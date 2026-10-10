#include "navaid.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace nav {

namespace {

// ILS localizer -> glide slope (MHz), ICAO Annex 10 Vol I Table A
const double LOC_GS[][2] = {
    {108.10, 334.70}, {108.15, 334.55}, {108.30, 334.10}, {108.35, 333.95}, {108.50, 329.90}, {108.55, 329.75},
    {108.70, 330.50}, {108.75, 330.35}, {108.90, 329.30}, {108.95, 329.15}, {109.10, 331.40}, {109.15, 331.25},
    {109.30, 332.00}, {109.35, 331.85}, {109.50, 332.60}, {109.55, 332.45}, {109.70, 333.20}, {109.75, 333.05},
    {109.90, 333.80}, {109.95, 333.65}, {110.10, 334.40}, {110.15, 334.25}, {110.30, 335.00}, {110.35, 334.85},
    {110.50, 329.60}, {110.55, 329.45}, {110.70, 330.20}, {110.75, 330.05}, {110.90, 330.80}, {110.95, 330.65},
    {111.10, 331.70}, {111.15, 331.55}, {111.30, 332.30}, {111.35, 332.15}, {111.50, 332.90}, {111.55, 332.75},
    {111.70, 333.50}, {111.75, 333.35}, {111.90, 331.10}, {111.95, 330.95},
};

// frequencies in 50 kHz steps as integers (units of 10 kHz), so 110.30 == 11030
long k10(double mhz) { return std::lround(mhz * 100.0); }

const char* MORSE[][2] = {
    {".-", "A"},    {"-...", "B"},  {"-.-.", "C"},  {"-..", "D"},   {".", "E"},     {"..-.", "F"},  {"--.", "G"},
    {"....", "H"},  {"..", "I"},    {".---", "J"},  {"-.-", "K"},   {".-..", "L"},  {"--", "M"},    {"-.", "N"},
    {"---", "O"},   {".--.", "P"},  {"--.-", "Q"},  {".-.", "R"},   {"...", "S"},   {"-", "T"},     {"..-", "U"},
    {"...-", "V"},  {".--", "W"},   {"-..-", "X"},  {"-.--", "Y"},  {"--..", "Z"},  {"-----", "0"}, {".----", "1"},
    {"..---", "2"}, {"...--", "3"}, {"....-", "4"}, {".....", "5"}, {"-....", "6"}, {"--...", "7"}, {"---..", "8"},
    {"----.", "9"},
};

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

std::string DmeChannel::name() const {
    return number > 0 ? std::to_string(number) + mode : "";
}

DmeChannel dme_channel(int number, char mode) {
    DmeChannel c;
    if (number < 1 || number > 126 || (mode != 'X' && mode != 'Y')) return c;
    c.number = number;
    c.mode = mode;
    c.interrogation_mhz = 1024 + number;
    const bool low = number <= 63;
    c.reply_mhz = (mode == 'X') == low ? c.interrogation_mhz - 63 : c.interrogation_mhz + 63;
    const double y = mode == 'Y' ? 0.05 : 0.0;
    if (number >= 17 && number <= 59) c.vhf_mhz = 108.00 + (number - 17) * 0.1 + y;
    else if (number >= 70) c.vhf_mhz = 112.30 + (number - 70) * 0.1 + y;
    if (std::isfinite(c.vhf_mhz)) c.vhf_mhz = std::round(c.vhf_mhz * 100) / 100;
    c.gs_mhz = glideslope_for(c.vhf_mhz);
    return c;
}

DmeChannel dme_by_reply(int mhz) {
    if (mhz >= 962 && mhz <= 1024) return dme_channel(mhz - 961, 'X');
    if (mhz >= 1025 && mhz <= 1087) return dme_channel(mhz - 961, 'Y');
    if (mhz >= 1088 && mhz <= 1150) return dme_channel(mhz - 1087, 'Y');
    if (mhz >= 1151 && mhz <= 1213) return dme_channel(mhz - 1087, 'X');
    return {};
}

DmeChannel dme_by_interrogation(int mhz, char mode) {
    if (mhz < 1025 || mhz > 1150) return {};
    return dme_channel(mhz - 1024, mode);
}

DmeChannel dme_by_vhf(double mhz) {
    const long f = k10(mhz);
    if (std::fabs(mhz * 100 - f) > 0.2 || f % 5) return {};
    if (f >= 10800 && f <= 11225) {
        const long k = (f - 10800) / 5;
        return dme_channel(17 + static_cast<int>(k / 2), k % 2 ? 'Y' : 'X');
    }
    if (f >= 11230 && f <= 11795) {
        const long k = (f - 11230) / 5;
        return dme_channel(70 + static_cast<int>(k / 2), k % 2 ? 'Y' : 'X');
    }
    return {};
}

bool is_localizer(double mhz) {
    const long f = k10(mhz);
    if (std::fabs(mhz * 100 - f) > 0.2 || f % 5 || f < 10810 || f > 11195) return false;
    return (f / 10) % 2 == 1;
}

bool is_vor(double mhz) {
    const long f = k10(mhz);
    if (std::fabs(mhz * 100 - f) > 0.2 || f % 5 || f < 10800 || f > 11795) return false;
    return !is_localizer(mhz);
}

bool is_glideslope(double mhz) { return std::isfinite(localizer_for(mhz)); }

double glideslope_for(double loc_mhz) {
    if (!std::isfinite(loc_mhz)) return NAN;
    for (const auto& p : LOC_GS)
        if (k10(p[0]) == k10(loc_mhz) && std::fabs(loc_mhz - p[0]) < 0.002) return p[1];
    return NAN;
}

double localizer_for(double gs_mhz) {
    if (!std::isfinite(gs_mhz)) return NAN;
    for (const auto& p : LOC_GS)
        if (k10(p[1]) == k10(gs_mhz) && std::fabs(gs_mhz - p[1]) < 0.002) return p[0];
    return NAN;
}

std::string mhz_text(double mhz, int decimals) {
    char b[32];
    snprintf(b, sizeof b, "%.*f MHz", decimals, mhz);
    return b;
}

std::string pairing_text(const DmeChannel& c) {
    if (!c.valid()) return "";
    if (!std::isfinite(c.vhf_mhz)) return "no VOR / ILS (a DME / TACAN-only channel)";
    if (c.ils()) return "ILS localizer " + mhz_text(c.vhf_mhz) + ", glide slope " + mhz_text(c.gs_mhz);
    return "VOR " + mhz_text(c.vhf_mhz);
}

const char* morse_char(const std::string& code) {
    for (const auto& m : MORSE)
        if (code == m[0]) return m[1];
    return nullptr;
}

// --- MorseDecoder -----------------------------------------------------------
MorseDecoder::MorseDecoder(double tick_s, double unit_min_s, double unit_max_s, double end_gap_s)
    : tick_(tick_s), umin_(unit_min_s), umax_(unit_max_s), endgap_(end_gap_s) {}

void MorseDecoder::reset() {
    runs_.clear();
    cur_ = false;
    curlen_ = 0;
}

bool MorseDecoder::push(bool key) {
    if (key == cur_) {
        curlen_++;
        // key up long enough after marks: the transmission is complete
        if (!key && !runs_.empty() && curlen_ * tick_ >= endgap_) {
            const bool ok = decode();
            gap_s_ = curlen_ * tick_;
            runs_.clear();
            return ok;
        }
        return false;
    }
    if (cur_ || !runs_.empty()) runs_.emplace_back(cur_, curlen_);   // leading key-up isn't kept
    cur_ = key;
    curlen_ = 1;
    return false;
}

bool MorseDecoder::decode() {
    // glitches: marks / gaps shorter than half the shortest dot are merged
    // into their neighbours
    const int glitch = std::max(1, static_cast<int>(umin_ * 0.5 / tick_));
    std::vector<std::pair<bool, int>> r;
    for (const auto& x : runs_) {
        if (!r.empty() && (x.first == r.back().first || x.second < glitch)) {
            r.back().second += x.second;
            continue;
        }
        r.push_back(x);
    }
    while (!r.empty() && !r.back().first) r.pop_back();   // trailing gap
    while (!r.empty() && (!r.front().first || r.front().second < glitch)) r.erase(r.begin());
    if (r.empty()) return false;
    std::vector<double> marks, gaps;
    for (const auto& x : r) (x.first ? marks : gaps).push_back(x.second * tick_);
    // dot / dash: split at the largest ratio between neighbouring sorted
    // lengths, if the longest is clearly longer than the shortest
    std::vector<double> s = marks;
    std::sort(s.begin(), s.end());
    double cut;
    if (s.back() >= 2.0 * s.front()) {
        size_t at = 0;
        double best = 0;
        for (size_t i = 0; i + 1 < s.size(); i++)
            if (s[i + 1] / s[i] > best) {
                best = s[i + 1] / s[i];
                at = i;
            }
        cut = std::sqrt(s[at] * s[at + 1]);
    } else {
        // one length only: dots if it's no longer than 1.8 x the slowest dot
        cut = median(s) <= 1.8 * umax_ ? s.back() * 1.01 : s.front() * 0.99;
    }
    std::vector<double> dots, dashes;
    for (double m : marks) (m < cut ? dots : dashes).push_back(m);
    unit_s_ = !dots.empty() ? median(dots) : median(dashes) / 3;
    // characters
    text_.clear();
    code_.clear();
    std::string cur;
    int fit = 0, n = 0;
    auto flush = [&]() {
        if (cur.empty()) return;
        const char* c = morse_char(cur);
        text_ += c ? c : "?";
        if (!code_.empty() && code_.back() != ' ') code_ += ' ';
        code_ += cur;
        cur.clear();
    };
    for (size_t i = 0; i < r.size(); i++) {
        const double len = r[i].second * tick_;
        n++;
        if (r[i].first) {
            const bool dot = len < cut;
            cur += dot ? '.' : '-';
            fit += std::fabs(len / unit_s_ - (dot ? 1 : 3)) <= (dot ? 0.5 : 1.2);
        } else {
            const double u = len / unit_s_;
            if (u >= 2.0) {
                flush();
                if (u >= 5.5) text_ += ' ', code_ += " /";
            }
            fit += u < 2.0 ? std::fabs(u - 1) <= 0.6 : (u < 5.5 ? std::fabs(u - 3) <= 1.5 : true);
        }
    }
    flush();
    quality_ = n ? static_cast<double>(fit) / n : 0;
    len_s_ = 0;
    for (const auto& x : r) len_s_ += x.second * tick_;
    return !text_.empty();
}

}  // namespace nav
