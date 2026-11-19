// SPDX-License-Identifier: MIT
// Copyright (c) 2026 KrakenRF Inc. - the plugin SDK (plugins/sdk/LICENSE)

// main() of every decoder plugin executable (linked with the plugin's own
// sources by plugins/Makefile). Three modes:
//
//   decoder --info                 plugin description as JSON (kraken_doa
//                                  reads it when it scans plugins/)
//   decoder --serve                live: kraken_doa streams samples over
//                                  stdin, results go back over stdout
//                                  (binary protocol, kraken_plugin.hpp)
//   decoder --file REC [options]   offline test on a recording - prints the
//                                  events, a frame count and the final facts
//                                  (the recording is read as a stream: memory
//                                  does not grow with its length)
//
// Offline options:
//   --rate HZ        sample rate of the file (default: REC.json "rate" from
//                    ai/sigtool.py captures, or the WAV header)
//   --format F       cf32 | cu8 | cs16 | wav (default: from the extension)
//   --offset HZ      the signal sits HZ away from the file's centre
//   --start S        skip S seconds;  --seconds S  decode only S seconds
//   --verbose        verbose option on
//   --opt key=value  pass an option to the decoder (repeatable)
//   --audio OUT.wav  write the decoded 8 kHz audio
//   --channels M     a recording of every antenna of an array, sample by
//                    sample (default: REC.json "channels"): decoded with
//                    process_multi (Info::multichannel); --channel K: only
//                    antenna K, as a single-channel decoder gets it
//   --station LAT,LON  the receiver's location (Host::station)
//   --rf HZ          RF of the file's centre (Host::rf_hz; default: the
//                    REC.json "rf_hz" of ai/sigtool.py captures)
//   --raw            raw frames on (Host::raw_wanted) and printed
//   --plots OUT      write the last plot of each id (Host::plot) as JSON lines
//   --quiet          only the summary
//   --uca R          with --channels M: the antennas form kraken_doa's UCA of
//                    radius R metres (clockwise, ANT0 on +x); antenna 0 is
//                    decoded (multichannel plugins: every antenna) and the
//                    summary gives each TALKER's bearing from its own samples
//                    of every antenna, as kraken_doa's DoA per talker
//                    computes it (needs --rf) - the ID-separation test
// Array plugins (Info::array) read a MULTICHANNEL recording: every sample
// holds the antennas one after the other (cf32 / cs16 / cu8 pairs):
//   --channels M     antennas (default: REC.json "channels")
//   --all            every sample (default: bursts of Info::array_burst_s
//                    every array_period_s, like live)
//   --uca R          the antennas form kraken_doa's UCA of radius R metres
//                    (clockwise, ANT0 on +x): the summary gives each spatial
//                    report's bearing (MUSIC, one source; needs --rf)

#include "kraken_plugin.hpp"
#include "kraken_dsp.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#include <liquid/liquid.h>
#include <sys/stat.h>
#include <unistd.h>
// libjpeg-turbo is a PLUGIN-ONLY, permissive (IJG / BSD-3-Clause / zlib)
// dependency: the plugin host compresses video frames to JPEG when built with
// it (plugins/Makefile adds -DHAVE_JPEG and the link when pkg-config finds
// libjpeg). The main kraken_doa app does NOT link or include it - it forwards
// the opaque blob. Without it the host sends raw pixels (encoding 0), so the
// feature builds and runs either way. <cstdio> must precede <jpeglib.h>
// (jpeglib.h uses FILE but does not include <stdio.h> itself).
#ifdef HAVE_JPEG
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>
#include <jerror.h>   // ERREXIT / JERR_OUT_OF_MEMORY (the destination manager's failed grow)
#endif
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

// the offline reader seeks in multichannel recordings far over 2 GB (5
// antennas at 2.4 MHz are 24 MB/s as cu8): fseeko's off_t must be 64-bit,
// which MinGW's is only with _FILE_OFFSET_BITS=64 (the plugins' Makefile) -
// otherwise a 5 GB seek silently lands at 705 MB
static_assert(sizeof(off_t) == 8, "build with -D_FILE_OFFSET_BITS=64");
#endif

namespace {

std::string jesc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    return o;
}

// Host::option_choices as kraken_doa keeps them (it checks again): each
// field up to a NUL (the wire's separator), a value that is too long, has
// a control character or repeats an earlier one dropped, a label's control
// characters made spaces and cut to CHOICE_LABEL_MAX at a character
// boundary ("" = the value), at most CHOICES_MAX
using Choices = std::vector<std::pair<std::string, std::string>>;
bool option_key_ok(const std::string& k) {   // kp::Option::key: [a-z0-9_], 1..32
    if (k.empty() || k.size() > 32) return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
Choices clean_choices(const Choices& in) {
    Choices out;
    for (const auto& [v0, l0] : in) {
        if (out.size() >= kp::wire::CHOICES_MAX) break;
        const std::string v = v0.substr(0, v0.find('\0'));
        std::string l = l0.substr(0, l0.find('\0'));
        bool ok = v.size() <= kp::wire::CHOICE_VALUE_MAX;
        for (unsigned char c : v) ok = ok && c >= 0x20 && c != 0x7F;
        for (const auto& o : out) ok = ok && o.first != v;
        if (!ok) continue;
        for (char& c : l)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';
        if (l.size() > kp::wire::CHOICE_LABEL_MAX) {
            size_t e = kp::wire::CHOICE_LABEL_MAX;
            while (e > 0 && (static_cast<unsigned char>(l[e]) & 0xC0) == 0x80) e--;
            l.resize(e);
        }
        out.emplace_back(v, l.empty() ? v : l);
    }
    return out;
}

int print_info() {
    kp::Info i = kp_plugin_info();
    std::cout << "{\"api\":1,\"id\":\"" << jesc(i.id) << "\",\"name\":\"" << jesc(i.name)
              << "\",\"category\":\"" << jesc(i.category)
              << "\",\"description\":\"" << jesc(i.description) << "\",\"version\":\"" << jesc(i.version)
              << "\",\"author\":\"" << jesc(i.author) << "\",\"sample_rate\":" << i.sample_rate
              << ",\"min_vfo_rate\":" << i.min_vfo_rate << ",\"map\":" << (i.map ? "true" : "false")
              << ",\"manual_only\":" << (i.manual_only ? "true" : "false") << ",\"fixed_freq_hz\":" << std::fixed
              << std::setprecision(0) << i.fixed_freq_hz << std::defaultfloat << std::setprecision(6)
              << ",\"voice\":" << (i.voice ? "true" : "false") << ",\"messages\":" << (i.messages ? "true" : "false")
              << ",\"talkers\":" << (i.talkers ? "true" : "false") << ",\"lock_vfo\":" << (i.lock_vfo ? "true" : "false")
              << ",\"multichannel\":" << (i.multichannel ? "true" : "false") << ",\"array\":" << (i.array ? "true" : "false")
              << ",\"video\":" << (i.video ? "true" : "false")
              << ",\"array_burst_s\":" << i.array_burst_s << ",\"array_period_s\":" << i.array_period_s
              << ",\"tunes\":" << (i.tunes ? "true" : "false") << ",\"squelch_hold_s\":" << i.squelch_hold_s
              << ",\"trunking\":" << (i.trunking ? "true" : "false");
    // options twice: as JSON for people, and tab-separated for kraken_doa's
    // flat reader (key, label, default, choices, help per line; a hidden option
    // adds a sixth field "1" - a kraken_doa from before reads five and ignores
    // it; other options' lines and JSON are as they always were)
    std::string tsv;
    std::cout << ",\"options\":[";
    for (size_t k = 0; k < i.options.size(); k++) {
        const kp::Option& o = i.options[k];
        std::cout << (k ? "," : "") << "{\"key\":\"" << jesc(o.key) << "\",\"label\":\"" << jesc(o.label)
                  << "\",\"default\":\"" << jesc(o.def) << "\",\"choices\":\"" << jesc(o.choices)
                  << "\",\"help\":\"" << jesc(o.help) << "\"" << (o.hidden ? ",\"hidden\":true" : "") << "}";
        // (by position: two fields may be the same "" literal)
        const char* fields[5] = {o.key, o.label, o.def, o.choices, o.help};
        for (int f = 0; f < 5; f++) {
            for (const char* c = fields[f]; *c; c++) tsv += (*c == '\t' || *c == '\n') ? ' ' : *c;
            tsv += f < 4 ? '\t' : o.hidden ? '\t' : '\n';
        }
        if (o.hidden) tsv += "1\n";
    }
    std::cout << "],\"options_tsv\":\"" << jesc(tsv) << "\"}" << std::endl;
    return 0;
}

// 64-bit FNV-1a: cheap content hash for video_frame change detection (an
// unchanged screen is not resent). Seed carries the geometry so a same-size
// buffer of different pixels, or the same pixels at a different size, differ.
uint64_t fnv1a(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

#ifdef HAVE_JPEG
// libjpeg-turbo error manager that longjmps instead of calling exit(): the
// default error_exit aborts the whole process, so a single bad frame could
// kill the plugin. With this, an encode failure just returns 0 and the host
// falls back to raw pixels / drops the frame.
struct jpeg_err_jmp {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
};
void jpeg_error_longjmp(j_common_ptr cinfo) {
    std::longjmp(reinterpret_cast<jpeg_err_jmp*>(cinfo->err)->jb, 1);
}

// A libjpeg destination manager writing into a std::vector the CALLER owns
// (instead of jpeg_mem_dest, whose malloc'd buffer only reaches the caller at
// termination - after it has grown, an error path holds a stale pointer). The
// vector grows by doubling; on any error it is simply left for the caller to
// reuse or drop, so nothing leaks and nothing is freed twice. A failed grow
// raises a libjpeg error (longjmp to the encoder's handler) - outside the
// catch block, so no C++ exception state is skipped.
struct jpeg_vec_dest {
    struct jpeg_destination_mgr pub;
    std::vector<uint8_t>* buf;
};
void jpeg_vec_init(j_compress_ptr cinfo) {
    jpeg_vec_dest* d = reinterpret_cast<jpeg_vec_dest*>(cinfo->dest);
    bool ok = true;
    try {
        if (d->buf->size() < 65536) d->buf->resize(65536);
    } catch (...) { ok = false; }
    if (!ok) ERREXIT(cinfo, JERR_OUT_OF_MEMORY);
    d->pub.next_output_byte = d->buf->data();
    d->pub.free_in_buffer = d->buf->size();
}
boolean jpeg_vec_grow(j_compress_ptr cinfo) {
    // libjpeg calls this with the WHOLE buffer full (next_output_byte and
    // free_in_buffer are not meaningful here)
    jpeg_vec_dest* d = reinterpret_cast<jpeg_vec_dest*>(cinfo->dest);
    const size_t used = d->buf->size();
    bool ok = true;
    try {
        d->buf->resize(used * 2);
    } catch (...) { ok = false; }
    if (!ok) ERREXIT(cinfo, JERR_OUT_OF_MEMORY);
    d->pub.next_output_byte = d->buf->data() + used;
    d->pub.free_in_buffer = d->buf->size() - used;
    return TRUE;
}
void jpeg_vec_term(j_compress_ptr cinfo) {
    jpeg_vec_dest* d = reinterpret_cast<jpeg_vec_dest*>(cinfo->dest);
    d->buf->resize(d->buf->size() - d->pub.free_in_buffer);   // shrinking never throws
}

// Encode an interleaved 8-bit buffer to a baseline JPEG in memory. comp = 1
// encodes GRAY8 natively (one plane, JCS_GRAYSCALE - the whole speed/size win,
// no expand to RGB); comp = 3 encodes RGB24 (JCS_RGB). Writes the JPEG into out
// (resized to it; the caller owns it and may reuse it frame to frame) and
// returns its byte count; 0 on failure (out's contents are then meaningless).
// quality is 1..100. Between setjmp and a longjmp only plain C structs live in
// this frame (no C++ object with a destructor), and nothing modified after
// setjmp is read after the jump.
size_t jpeg_encode_mem(const uint8_t* pixels, int w, int h, int comp, int quality, std::vector<uint8_t>& out) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_err_jmp jerr;
    struct jpeg_vec_dest dest;
    std::memset(&cinfo, 0, sizeof cinfo);   // jpeg_destroy_compress is safe even if create never ran
    std::memset(&dest, 0, sizeof dest);
    dest.buf = &out;
    dest.pub.init_destination = jpeg_vec_init;
    dest.pub.empty_output_buffer = jpeg_vec_grow;
    dest.pub.term_destination = jpeg_vec_term;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_longjmp;
    if (setjmp(jerr.jb)) {   // a libjpeg error lands here
        jpeg_destroy_compress(&cinfo);
        return 0;
    }
    jpeg_create_compress(&cinfo);
    cinfo.dest = &dest.pub;   // ours, not from libjpeg's pool: destroy leaves it alone
    cinfo.image_width = static_cast<JDIMENSION>(w);
    cinfo.image_height = static_cast<JDIMENSION>(h);
    cinfo.input_components = comp;
    cinfo.in_color_space = comp == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    const size_t stride = static_cast<size_t>(w) * comp;
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(pixels + static_cast<size_t>(cinfo.next_scanline) * stride);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);   // calls jpeg_vec_term: out is now exactly the JPEG
    jpeg_destroy_compress(&cinfo);
    return out.size();
}
#endif

// decimal text for the wire, "" = NAN
std::string wnum(double v, int prec = 6) {
    if (!std::isfinite(v)) return "";
    char b[48];
    snprintf(b, sizeof b, "%.*g", prec, v);
    return b;
}

// SPATIAL / PLOT payloads (kraken_plugin.hpp kp::wire)
std::string spatial_payload(const kp::Spatial& s, double start, double end) {
    std::string p = s.id.substr(0, 32);
    for (const std::string& f : {s.label.substr(0, 64), wnum(start, 12), wnum(end, 12), wnum(s.delay_us), wnum(s.power_db),
                                 wnum(s.snr_db), std::string(s.ref ? "1" : "0"), wnum(s.avg_s), std::to_string(s.m)}) {
        p.push_back('\0');
        p += f;
    }
    p.push_back('\0');
    for (size_t i = 0; i < s.cov.size(); i++) {
        if (i) p.push_back(',');
        p += wnum(s.cov[i].real(), 7) + "," + wnum(s.cov[i].imag(), 7);
    }
    return p;
}
std::string plot_payload(const kp::Plot& pl) {
    std::string p = pl.title.substr(0, 80);
    for (const std::string& f : {pl.x_unit.substr(0, 16), pl.y_unit.substr(0, 16), wnum(pl.x0, 9), wnum(pl.dx, 9),
                                 wnum(pl.y_min), wnum(pl.y_max), wnum(pl.threshold)}) {
        p.push_back('\0');
        p += f;
    }
    p.push_back('\0');
    for (size_t i = 0; i < pl.y.size() && i < 1000; i++) {
        if (i) p.push_back(',');
        p += wnum(pl.y[i], 4);
    }
    for (size_t i = 0; i < pl.markers.size() && i < 64; i++) {
        const kp::PlotMarker& m = pl.markers[i];
        for (const std::string& f : {wnum(m.x, 9), m.label.substr(0, 16), m.talker.substr(0, 32)}) {
            p.push_back('\0');
            p += f;
        }
    }
    return p;
}
// a plot that uses any PLOT_EX field (kp::Plot::id ...) goes as PLOT_EX
bool plot_is_ex(const kp::Plot& pl) {
    return !pl.id.empty() || pl.place != kp::PLOT_TAB || !pl.click_option.empty() || !pl.click_also.empty() ||
           pl.click_snap > 0 || pl.click_step > 0;
}
std::string plot_ex_payload(const kp::Plot& pl) {
    std::string p = "6";
    for (const std::string& f : {pl.id.substr(0, 32), std::to_string(pl.place), pl.click_option.substr(0, 32),
                                 pl.click_also.substr(0, 140), wnum(pl.click_snap > 0 ? pl.click_snap : NAN, 9),
                                 wnum(pl.click_step > 0 ? pl.click_step : NAN, 9)}) {
        p.push_back('\0');
        p += f;
    }
    p.push_back('\0');
    return p + plot_payload(pl);
}

// --------------------------------------------------------------------------
// Trunked systems (kp::Host::trunk_system ... call): the wire limits, as
// kraken_doa enforces them again (kp::wire::TRUNK_*). Every helper counts
// what it had to cut or drop in *n (the offline summary reports it).
// --------------------------------------------------------------------------
// text: valid UTF-8 without C0 / DEL / C1 controls (and NULs, the wire's
// separator), cut at a character boundary - kp::wire::clean_text, which
// kraken_doa applies again
std::string tr_text(const std::string& in, size_t max, uint64_t* n) {
    std::string s = kp::wire::clean_text(in, max);
    if (s != in) (*n)++;
    return s;
}
// TrunkSystem::key: [A-Za-z0-9._-], others '_', <= 48
std::string tr_key(const std::string& in, uint64_t* n) {
    std::string s = in.substr(0, kp::wire::TRUNK_KEY_MAX);
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) c = '_';
    if (s != in) (*n)++;
    return s;
}
// a plugin id / demod name: lowercase token or "" (dropped)
std::string tr_token(const std::string& in, size_t max, bool dash, uint64_t* n) {
    bool ok = in.size() <= max;
    for (char c : in) ok = ok && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || (dash && c == '-'));
    if (ok) return in;
    (*n)++;
    return "";
}
// valid as it goes on the wire ("%.1f": kraken_doa takes (0, 6 GHz] of that
// text, so 0.04 Hz - "0.0" - is NAN there and here)
double tr_freq(double f, uint64_t* n) {
    if (std::isnan(f)) return NAN;
    if (std::isfinite(f) && f > 0 && f <= 2 * kp::wire::TRUNK_FREQ_MAX_HZ) {
        char b[48];
        snprintf(b, sizeof b, "%.1f", f);
        const double w = strtod(b, nullptr);
        if (w > 0 && w <= kp::wire::TRUNK_FREQ_MAX_HZ) return f;
    }
    (*n)++;
    return NAN;
}
int tr_slot(int s, uint64_t* n) {
    if (s >= -1 && s <= kp::wire::TRUNK_SLOT_MAX) return s;
    (*n)++;
    return -1;
}
kp::TrunkChannel tr_channel(const kp::TrunkChannel& c, uint64_t* n) {
    kp::TrunkChannel o;
    o.name = tr_text(c.name, kp::wire::TRUNK_NAME_MAX, n);
    o.number = c.number >= 0 ? c.number : -1;
    if (c.number < -1) (*n)++;
    o.freq_hz = tr_freq(c.freq_hz, n);
    o.uplink_hz = tr_freq(c.uplink_hz, n);
    o.slot = tr_slot(c.slot, n);
    return o;
}
kp::TrunkSystem tr_system(const kp::TrunkSystem& s, uint64_t* n) {
    kp::TrunkSystem o;
    o.key = tr_key(s.key, n);
    o.name = tr_text(s.name, kp::wire::TRUNK_TEXT_MAX, n);
    o.site = tr_text(s.site, kp::wire::TRUNK_TEXT_MAX, n);
    o.control = s.control;
    return o;
}
kp::Grant tr_grant(const kp::Grant& g, uint64_t* n) {
    kp::Grant o = g;
    o.channel = tr_channel(g.channel, n);
    o.target = tr_text(g.target, kp::wire::TRUNK_ID_MAX, n);
    o.source = tr_text(g.source, kp::wire::TRUNK_ID_MAX, n);
    o.decoder = tr_token(g.decoder, kp::wire::TRUNK_ID_MAX, true, n);
    o.demod = tr_token(g.demod, kp::wire::TRUNK_DEMOD_MAX, false, n);
    if (!(std::isfinite(g.bw_hz) && g.bw_hz >= 0 && g.bw_hz <= 2.4e6)) {
        o.bw_hz = 0;
        if (!(g.bw_hz == 0)) (*n)++;
    }
    // options: a valid key that is not one of the host's own OPTIONs
    // (kp::wire::host_option), a value <= 100 bytes that is clean text
    // (kp::wire::clean_text leaves it as it is), the first of a repeated
    // key, at most 16
    o.options.clear();
    for (const auto& [k, v] : g.options) {
        bool ok = option_key_ok(k) && !kp::wire::host_option(k) && v.size() <= kp::wire::TRUNK_OPTION_VALUE_MAX &&
                  o.options.size() < kp::wire::TRUNK_OPTIONS_MAX && kp::wire::clean_text(v, v.size()) == v;
        for (const auto& x : o.options) ok = ok && x.first != k;
        if (ok) o.options.emplace_back(k, v);
        else (*n)++;
    }
    return o;
}
kp::Call tr_call(const kp::Call& c, uint64_t* n) {
    kp::Call o = c;
    if (c.state < kp::Call::ENDED || c.state > kp::Call::RELEASED) {
        o.state = kp::Call::ACTIVE;
        (*n)++;
    }
    o.target = tr_text(c.target, kp::wire::TRUNK_ID_MAX, n);
    o.source = tr_text(c.source, kp::wire::TRUNK_ID_MAX, n);
    o.slot = tr_slot(c.slot, n);
    return o;
}
// the wire's text for them ("" = NAN / none)
std::string tr_hz(double f) {
    if (!std::isfinite(f)) return "";
    char b[40];
    snprintf(b, sizeof b, "%.1f", f);
    return b;
}
void put0(std::string& p, const std::string& f) {
    p.push_back('\0');
    p += f;
}
// a CH: name, number, freq_hz, slot, uplink_hz (cleaned)
std::string tr_channel_payload(const kp::TrunkChannel& c) {
    std::string p = c.name;
    put0(p, c.number >= 0 ? std::to_string(c.number) : std::string());
    put0(p, tr_hz(c.freq_hz));
    put0(p, std::to_string(c.slot));
    put0(p, tr_hz(c.uplink_hz));
    return p;
}
std::string tr_grant_payload(const kp::Grant& g) {
    // E, then name, number, freq_hz, slot, target, group, source, flags,
    // decoder, demod, bw_hz, uplink_hz (added last: the channel is written
    // field by field, not as a CH), then the options
    std::string p = std::to_string(kp::wire::GRANT_FIXED);
    put0(p, g.channel.name);
    put0(p, g.channel.number >= 0 ? std::to_string(g.channel.number) : std::string());
    put0(p, tr_hz(g.channel.freq_hz));
    put0(p, std::to_string(g.channel.slot));
    put0(p, g.target);
    put0(p, g.group ? "1" : "0");
    put0(p, g.source);
    std::string fl;
    if (g.encrypted) fl += 'e';
    if (g.emergency) fl += 'm';
    if (g.data) fl += 'd';
    if (g.update) fl += 'u';
    put0(p, fl);
    put0(p, g.decoder);
    put0(p, g.demod);
    put0(p, g.bw_hz > 0 ? tr_hz(g.bw_hz) : std::string());
    put0(p, tr_hz(g.channel.uplink_hz));
    for (const auto& [k, v] : g.options) {
        put0(p, k);
        put0(p, v);
    }
    return p;
}
std::string tr_call_payload(const kp::Call& c) {
    std::string p = std::to_string(c.state);
    put0(p, c.target);
    put0(p, c.group ? "1" : "0");
    put0(p, c.source);
    put0(p, std::to_string(c.slot));
    std::string fl;
    if (c.encrypted) fl += 'e';
    if (c.emergency) fl += 'm';
    put0(p, fl);
    return p;
}
std::string tr_system_payload(const kp::TrunkSystem& s) {
    std::string p = s.key;
    put0(p, s.name);
    put0(p, s.site);
    put0(p, s.control ? "1" : "0");
    return p;
}

// --------------------------------------------------------------------------
// Live mode
// --------------------------------------------------------------------------
class WireHost : public kp::Host {
public:
    WireHost(int out_fd, double rate) : fd_(out_fd), rate_(rate) {}
    void fact(const std::string& k, const std::string& v) override {
        std::string p = k;
        p.push_back('\0');
        p += v;
        send(kp::wire::FACT, p.data(), p.size());
    }
    void event(const std::string& text, double dedup_s) override {
        std::string p(4, '\0');
        float d = static_cast<float>(dedup_s);
        memcpy(p.data(), &d, 4);
        p += text;
        send(kp::wire::EVENT, p.data(), p.size());
    }
    void valid() override { send(kp::wire::VALID, nullptr, 0); }
    void valid(double start_s, double end_s) override {
        if (!std::isfinite(start_s) || !std::isfinite(end_s) || end_s < start_s) {
            valid();
            return;
        }
        std::string p = secs(start_s);
        p.push_back('\0');
        p += secs(end_s);
        send(kp::wire::VALID, p.data(), p.size());
    }
    void audio(const float* pcm, size_t n) override {
        while (n > 0) {
            size_t k = std::min<size_t>(n, 8000);
            send(kp::wire::AUDIO, pcm, k * sizeof(float));
            pcm += k;
            n -= k;
        }
    }
    void audio_channel(int channel) override {
        if (channel == audio_channel_) return;   // sent when it changes
        audio_channel_ = channel;
        const int32_t c = channel;
        send(kp::wire::AUDIO_CHANNEL, &c, 4);
    }
    void voice_state(const std::string& s) override { send(kp::wire::VOICE_STATE, s.data(), s.size()); }
    void freq_error(float hz) override { send(kp::wire::FREQ_ERROR, &hz, 4); }
    bool verbose() const override { return verbose_; }
    bool voice_wanted() const override { return voice_wanted_; }
    double time() const override { return array_t_ >= 0 ? array_t_ : samples_ / rate_; }
    double sample_rate() const override { return rate_; }
    void log(const std::string& t) override { std::cerr << t << std::endl; }
    void spatial(const kp::Spatial& sp) override {
        if (sp.id.empty() || sp.m < 1 || sp.m > 8 || sp.cov.size() != static_cast<size_t>(sp.m * sp.m)) return;
        const std::string p = spatial_payload(sp, std::isfinite(sp.start_s) ? sp.start_s : time(),
                                              std::isfinite(sp.end_s) ? sp.end_s : time());
        send(kp::wire::SPATIAL, p.data(), p.size());
    }
    void spatial_remove(const std::string& id) override { send(kp::wire::SPATIAL_REMOVE, id.data(), std::min<size_t>(id.size(), 32)); }
    void plot(const kp::Plot& pl) override {
        // a plot without the PLOT_EX fields exactly as before (older kraken_doa builds show it)
        const bool ex = plot_is_ex(pl);
        const std::string p = ex ? plot_ex_payload(pl) : plot_payload(pl);
        send(ex ? kp::wire::PLOT_EX : kp::wire::PLOT, p.data(), p.size());
    }
    // kp::wire::VIDEO: uint16 width, height; uint8 format, encoding; uint16
    // label_len; label; then the image data (encoding 0 = width*height*bpp raw
    // pixel bytes, encoding 2 = a JPEG bitstream), all little-endian. The host
    // compresses to JPEG when built with libjpeg-turbo (-DHAVE_JPEG) and the
    // result is smaller / needed; otherwise it sends raw pixels (encoding 0),
    // exactly as before. GRAY8 frames are encoded as native grayscale (one
    // plane). An unchanged frame (same geometry + pixels as the last one sent)
    // is skipped, so a static screen costs ~no bandwidth. The whole payload
    // must stay under MAX_PAYLOAD - drop the label to make it fit, else drop
    // the frame (log once); never truncate a sent frame. width/height are
    // always the frame's own size (the compressed blob is small, so large
    // frames - up to 4K - now fit where the raw raster would not).
    bool video_wanted() const override { return video_wanted_; }
    // kraken_doa's OPTION video_wanted=0|1: is any browser showing the video?
    // Turning it on forgets the last frame's hash, so the next frame - even an
    // unchanged still - goes out at once and a new viewer gets a picture now.
    void set_video_wanted(bool on) {
        if (on && !video_wanted_) video_have_hash_ = false;
        video_wanted_ = on;
    }
    void video_frame(const kp::VideoFrame& f) override {
        if (!video_wanted_) return;   // nobody watching: no hash, no encode, no pipe traffic
        const int w = f.width, h = f.height;
        if (w < 1 || h < 1 || w > 65535 || h > 65535 || !f.pixels) return;
        const uint8_t format = f.format == 1 ? 1 : 0;
        const int bpp = format == 1 ? 3 : 1;
        const size_t npix = static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(bpp);

        // change detection: skip a frame identical to the last one SENT, but
        // only WITHIN the keyframe interval - past it, an unchanged frame is
        // re-sent anyway (see VIDEO_KEYFRAME_S) so a dropped frame self-heals
        // and a freshly-opened panel fills in.
        const uint64_t hv = fnv1a(f.pixels, npix,
                                  fnv1a(reinterpret_cast<const uint8_t*>(&format), 1,
                                        (static_cast<uint64_t>(w) << 32) ^ static_cast<uint64_t>(h)));
        const auto now = std::chrono::steady_clock::now();
        if (video_have_hash_ && hv == video_hash_ &&
            std::chrono::duration<double>(now - video_last_send_).count() < VIDEO_KEYFRAME_S)
            return;

        // pick the payload: a JPEG blob when it was built in and helps, else raw
        const uint8_t* img = f.pixels;
        size_t img_len = npix;
        uint8_t encoding = 0;
#ifdef HAVE_JPEG
        {
            // GRAY8 (format 0) encodes as native grayscale (1 component - no
            // expand to RGB); RGB24 (format 1) as 3 components. lossless frames
            // (a frozen / heavily averaged still) use max quality, since JPEG
            // has no true lossless mode.
            const int comp = format == 1 ? 3 : 1;
            const int q = f.lossless ? 100 : (f.quality < 0 ? 60 : std::clamp(f.quality, 1, 100));
            const size_t sz = jpeg_encode_mem(f.pixels, w, h, comp, q, jpeg_buf_);
            // use JPEG if it fits and it is smaller than raw OR raw wouldn't fit
            if (sz > 0 && 8 + sz <= kp::wire::MAX_PAYLOAD && (sz < npix || 8 + npix > kp::wire::MAX_PAYLOAD)) {
                encoding = 2;
                img = jpeg_buf_.data();
                img_len = sz;
            }
        }
#endif
        const size_t fixed = 8;   // width, height, format, encoding, label_len
        size_t label_len = std::min<size_t>(f.label.size(), 65535);
        if (fixed + label_len + img_len > kp::wire::MAX_PAYLOAD) {
            label_len = 0;   // first drop the caption to make room
            if (fixed + img_len > kp::wire::MAX_PAYLOAD) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    std::cerr << "video_frame: " << (fixed + img_len) << " bytes exceeds the " << kp::wire::MAX_PAYLOAD
                              << " byte limit - dropping (downscale the raster, or build with libjpeg)" << std::endl;
                }
                return;
            }
        }
        std::string p;
        p.reserve(fixed + label_len + img_len);
        auto put16 = [&](unsigned v) {
            p.push_back(static_cast<char>(v & 0xff));
            p.push_back(static_cast<char>((v >> 8) & 0xff));
        };
        put16(static_cast<unsigned>(w));
        put16(static_cast<unsigned>(h));
        p.push_back(static_cast<char>(format));
        p.push_back(static_cast<char>(encoding));
        put16(static_cast<unsigned>(label_len));
        p.append(f.label.data(), label_len);
        p.append(reinterpret_cast<const char*>(img), img_len);
        send(kp::wire::VIDEO, p.data(), p.size());
        video_hash_ = hv;   // only a frame actually sent updates the change-detection state
        video_have_hash_ = true;
        video_last_send_ = now;   // reset the keyframe timer on every real send
    }
    void option_choices(const std::string& key, const Choices& choices) override {
        if (!option_key_ok(key)) return;
        std::string p = key;
        for (const auto& [v, l] : clean_choices(choices)) {
            p.push_back('\0');
            p += v;
            p.push_back('\0');
            p += l;
        }
        send(kp::wire::OPTION_CHOICES, p.data(), p.size());
    }
    bool tune(double rf_hz) override {
        if (!std::isfinite(rf_hz) || rf_hz <= 0) return false;
        char b[32];
        snprintf(b, sizeof b, "%.0f", rf_hz);
        send(kp::wire::TUNE, b, strlen(b));
        return true;
    }
    // trunked systems: cleaned to the wire limits (kraken_doa checks again);
    // a channel without a name is not sent
    void trunk_system(const kp::TrunkSystem& s) override {
        uint64_t n = 0;
        const std::string p = tr_system_payload(tr_system(s, &n));
        send(kp::wire::TRUNK_SYSTEM, p.data(), p.size());
    }
    void trunk_channel(const kp::TrunkChannel& c) override { send_channel(kp::wire::TRUNK_CHANNEL, c); }
    void control_channel(const kp::TrunkChannel& c) override { send_channel(kp::wire::CONTROL_CHANNEL, c); }
    void grant_end(const kp::TrunkChannel& c) override { send_channel(kp::wire::GRANT_END, c); }
    void grant(const kp::Grant& g) override {
        uint64_t n = 0;
        const kp::Grant o = tr_grant(g, &n);
        if (o.channel.name.empty()) return;
        const std::string p = tr_grant_payload(o);
        send(kp::wire::GRANT, p.data(), p.size());
    }
    void call(const kp::Call& c) override {
        uint64_t n = 0;
        const std::string p = tr_call_payload(tr_call(c, &n));
        send(kp::wire::CALL, p.data(), p.size());
    }
    void send_channel(uint32_t type, const kp::TrunkChannel& c) {
        uint64_t n = 0;
        const kp::TrunkChannel o = tr_channel(c, &n);
        if (o.name.empty()) return;
        const std::string p = tr_channel_payload(o);
        send(type, p.data(), p.size());
    }
    double array_t_ = -1;   // array plugins: Host::time() = the block being processed
    void map_point(const kp::MapPoint& m) override {
        if (!std::isfinite(m.lat) || !std::isfinite(m.lon) || m.id.empty()) return;
        auto num = [](double v, const char* f) {
            if (!std::isfinite(v)) return std::string();
            char b[32];
            snprintf(b, sizeof b, f, v);
            return std::string(b);
        };
        std::string p;
        for (const std::string& f : {m.id.substr(0, 32), num(m.lat, "%.6f"), num(m.lon, "%.6f"), m.label.substr(0, 64),
                                     m.kind.substr(0, 16), num(m.heading, "%.1f"), num(m.altitude_m, "%.0f"),
                                     num(m.speed_kmh, "%.1f"), num(m.ttl_s, "%.0f"), m.info.substr(0, 1000)}) {
            p += f;
            p.push_back('\0');
        }
        p.pop_back();
        send(kp::wire::MAP_POINT, p.data(), p.size());
    }
    void map_remove(const std::string& id) override { send(kp::wire::MAP_REMOVE, id.data(), std::min<size_t>(id.size(), 32)); }
    void table_columns(const std::vector<std::string>& cols) override {
        std::string p;
        for (size_t i = 0; i < cols.size() && i < 40; i++) {
            if (i) p.push_back('\0');
            p += cols[i].substr(0, 40);
        }
        send(kp::wire::TABLE_COLUMNS, p.data(), p.size());
    }
    void table_row(const std::string& key, const std::vector<std::string>& cells) override {
        std::string p = key.substr(0, 64);
        for (size_t i = 0; i < cells.size() && i < 40; i++) {
            p.push_back('\0');
            p += cells[i].substr(0, 200);
        }
        send(kp::wire::TABLE_ROW, p.data(), p.size());
    }
    void table_remove(const std::string& key) override {
        send(kp::wire::TABLE_REMOVE, key.data(), std::min<size_t>(key.size(), 64));
    }
    void table_keep(double seconds) override {
        if (!std::isfinite(seconds)) return;
        const std::string p = std::to_string(static_cast<long long>(std::llround(seconds)));
        send(kp::wire::TABLE_KEEP, p.data(), p.size());
    }
    void table_detail(const std::string& key, const std::vector<std::pair<std::string, std::string>>& lines) override {
        std::string p = key.substr(0, 64);
        for (size_t i = 0; i < lines.size() && i < 200; i++) {
            p.push_back('\0');
            p += lines[i].first.substr(0, 80);
            p.push_back('\0');
            p += lines[i].second.substr(0, 400);
        }
        send(kp::wire::TABLE_DETAIL, p.data(), p.size());
    }
    bool raw_wanted() const override { return raw_wanted_; }
    void raw(const std::string& data) override {
        if (raw_wanted_) send(kp::wire::RAW, data.data(), std::min<size_t>(data.size(), 2000));
    }
    void message(const std::string& from, const std::string& text) override {
        std::string p = from.substr(0, 64);
        p.push_back('\0');
        p += text.substr(0, 2000);
        send(kp::wire::MESSAGE, p.data(), p.size());
    }
    void talker(const kp::Talker& t) override {
        if (t.id.empty()) return;
        std::string p = t.id.substr(0, 32);
        p.push_back('\0');
        p += t.label.substr(0, 64);
        p.push_back('\0');
        p += secs(t.start_s);
        p.push_back('\0');
        p += secs(t.end_s);
        if (t.channel != 0 || t.packet) {
            p.push_back('\0');
            p += std::to_string(std::clamp(t.channel, 0, 7));
        }
        if (t.packet) {
            p.push_back('\0');
            p += "p";
            p.push_back('\0');
            p += std::isfinite(t.freq_hz) ? fmt_hz(t.freq_hz) : "";
            p.push_back('\0');
            p += fmt_hz(std::isfinite(t.bw_hz) ? std::max(0.0, t.bw_hz) : 0.0);
            p.push_back('\0');
            p += fmt_hz(std::isfinite(t.avg_s) ? std::max(0.0, t.avg_s) : 0.0);
            const bool keep = std::isfinite(t.keep_s) && t.keep_s > 0;
            const bool cov = t.m >= 2 && t.m <= 8 && t.cov.size() == static_cast<size_t>(t.m * t.m);
            if (std::isfinite(t.lat) && std::isfinite(t.lon) && std::isfinite(t.alt_m) && std::fabs(t.lat) <= 90 &&
                std::fabs(t.lon) <= 180) {
                char b[40];
                snprintf(b, sizeof b, "%.7f", t.lat);
                p.push_back('\0');
                p += b;
                snprintf(b, sizeof b, "%.7f", t.lon);
                p.push_back('\0');
                p += b;
                snprintf(b, sizeof b, "%.1f", t.alt_m);
                p.push_back('\0');
                p += b;
            } else if (keep || cov) {
                p.append(3, '\0');   // no position (empty fields), keep_s follows
            }
            if (keep || cov) {
                p.push_back('\0');
                p += keep ? std::to_string(static_cast<long long>(std::llround(t.keep_s))) : std::string("0");
            }
            if (cov) {   // the packet's own covariance (Talker::cov)
                p.push_back('\0');
                p += std::to_string(t.m);
                p.push_back('\0');
                for (size_t i = 0; i < t.cov.size(); i++) {
                    if (i) p.push_back(',');
                    p += wnum(t.cov[i].real(), 7) + "," + wnum(t.cov[i].imag(), 7);
                }
            }
        }
        send(kp::wire::TALKER, p.data(), p.size());
    }
    void talker_end(double at_s, int channel) override {
        std::string p = secs(at_s);
        if (channel != 0) {
            p.push_back('\0');
            p += std::to_string(std::clamp(channel, 0, 7));
        }
        send(kp::wire::TALKER_END, p.data(), p.size());
    }
    void talker_forget(const std::string& id) override {
        send(kp::wire::TALKER_FORGET, id.data(), std::min<size_t>(id.size(), 32));
    }
    bool station(double* lat, double* lon) const override {
        if (!have_station_) return false;
        *lat = st_lat_;
        *lon = st_lon_;
        return true;
    }
    double rf_hz() const override { return rf_hz_; }

    bool verbose_ = false, voice_wanted_ = false, raw_wanted_ = false;
    int audio_channel_ = -1;   // last AUDIO_CHANNEL sent
    bool have_station_ = false;
    double st_lat_ = 0, st_lon_ = 0;
    double rf_hz_ = NAN;
    double samples_ = 0;
    void sync_done(const void* id, size_t len) { send(kp::wire::SYNC_DONE, id, len); }

private:
    int fd_;
    double rate_;
    // video_frame change detection (the JPEG encoder feeds the GRAY8 / RGB24
    // buffer directly - no scratch buffer needed)
    uint64_t video_hash_ = 0;
    bool video_have_hash_ = false;
    // a viewer is watching (OPTION video_wanted); true until kraken_doa says,
    // so an older kraken_doa that never sends it still gets every frame
    bool video_wanted_ = true;
#ifdef HAVE_JPEG
    std::vector<uint8_t> jpeg_buf_;   // the encoded frame (reused frame to frame)
#endif
    // Periodic keyframe: an unchanged frame is still re-sent once this long has
    // passed since the last ACTUAL send, so a frame lost to back-pressure heals
    // and a newly-subscribed browser gets the current picture. A static screen
    // then costs only ~1 JPEG keyframe / 1.5 s (a few KB/s), not every frame.
    static constexpr double VIDEO_KEYFRAME_S = 1.5;
    std::chrono::steady_clock::time_point video_last_send_{};
    static std::string fmt_hz(double v) {
        char b[40];
        snprintf(b, sizeof b, "%.3f", v);
        return b;
    }
    // a Host::time() value for the wire; NAN = now
    std::string secs(double t) const {
        if (!std::isfinite(t)) t = time();
        char b[40];
        snprintf(b, sizeof b, "%.7f", t);
        return b;
    }
    void send(uint32_t type, const void* data, size_t len) {
        if (len > kp::wire::MAX_PAYLOAD) len = kp::wire::MAX_PAYLOAD;
        uint32_t hdr[2] = {type, static_cast<uint32_t>(len)};
        write_all(hdr, 8);
        if (len) write_all(data, len);
    }
    void write_all(const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        while (n > 0) {
            ssize_t w = ::write(fd_, c, n);
            if (w < 0) {
                if (errno == EINTR) continue;
                _exit(0);   // kraken_doa went away
            }
            c += w;
            n -= static_cast<size_t>(w);
        }
    }
};

bool read_all(int fd, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n > 0) {
        ssize_t r = ::read(fd, c, n);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        c += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

int serve(int argc, char** argv) {
    kp::Info info = kp_plugin_info();
    // the input rate: the plugin's own, or (Info::sample_rate 0) the VFO's,
    // which kraken_doa passes as --rate HZ
    double rate = info.sample_rate;
    for (int i = 2; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--rate") && info.sample_rate <= 0) rate = atof(argv[i + 1]);
    if (!(rate > 0)) {
        fprintf(stderr, "error: this plugin takes the VFO's rate: --serve --rate HZ\n");
        return 2;
    }
    // stdout carries the protocol: keep a private copy of it and point fd 1
    // at stderr, so a stray printf / std::cout in a plugin can't corrupt it
    int out_fd = dup(1);
#ifdef _WIN32
    if (out_fd >= 0) {
        _setmode(out_fd, _O_BINARY);
        // UCRT's dup() makes an INHERITABLE handle: a program the plugin
        // starts would hold kraken_doa's end of the protocol pipe open, and
        // kraken_doa would see no end when the plugin dies
        SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(out_fd)), HANDLE_FLAG_INHERIT, 0);
    }
    // no stderr to point fd 1 at (a parent without a console that passed
    // none): fd 1 would stay the protocol pipe - stray output goes to NUL
    if (dup2(2, 1) < 0) freopen("NUL", "w", stdout);
#else
    dup2(2, 1);
#endif
    WireHost host(out_fd, rate);
    auto dec = kp_plugin_create(host);
    for (const auto& o : info.options) dec->option(o.key, o.def);
    std::vector<char> buf;
    std::vector<std::vector<kp::cf>> mc;   // SAMPLES_MC, per antenna
    std::vector<kp::cf> arr;   // array plugins: the block, antenna-major
    for (;;) {
        uint32_t hdr[2];
        if (!read_all(0, hdr, 8)) return 0;
        if (hdr[1] > kp::wire::MAX_PAYLOAD) return 2;
        buf.resize(hdr[1]);
        if (hdr[1] && !read_all(0, buf.data(), hdr[1])) return 0;
        try {
            switch (hdr[0]) {
                case kp::wire::SAMPLES: {
                    size_t n = hdr[1] / sizeof(kp::cf);
                    dec->process(reinterpret_cast<const kp::cf*>(buf.data()), n);
                    host.samples_ += n;
                    break;
                }
                case kp::wire::SAMPLES_MC: {
                    uint32_t m = 0;
                    if (hdr[1] >= 4) memcpy(&m, buf.data(), 4);
                    if (m < 2 || m > 8) break;
                    const size_t n = (hdr[1] - 4) / (4 * static_cast<size_t>(m));
                    if (!n) break;
                    mc.resize(m);
                    std::vector<const kp::cf*> ptrs(m);
                    const int16_t* q = reinterpret_cast<const int16_t*>(buf.data() + 4);
                    const float sc = 1.0f / kp::wire::MC_SCALE;
                    for (uint32_t k = 0; k < m; k++) {
                        mc[k].resize(n);
                        float* __restrict d = reinterpret_cast<float*>(mc[k].data());
                        for (size_t i = 0; i < n; i++) {
                            d[2 * i] = q[2 * (i * m + k)] * sc;
                            d[2 * i + 1] = q[2 * (i * m + k) + 1] * sc;
                        }
                        ptrs[k] = mc[k].data();
                    }
                    dec->process_multi(ptrs.data(), static_cast<int>(m), n);
                    host.samples_ += n;
                    break;
                }
                case kp::wire::SAMPLES_ARRAY: {
                    // uint32 m, n, flags; float32 scale; float64 rate, t0; int16 I/Q, sample-major
                    if (buf.size() < 32) break;
                    uint32_t m, n, flags;
                    float scale;
                    double rate, t0;
                    memcpy(&m, buf.data(), 4);
                    memcpy(&n, buf.data() + 4, 4);
                    memcpy(&flags, buf.data() + 8, 4);
                    memcpy(&scale, buf.data() + 12, 4);
                    memcpy(&rate, buf.data() + 16, 8);
                    memcpy(&t0, buf.data() + 24, 8);
                    if (m < 1 || m > 8 || buf.size() < 32 + static_cast<size_t>(m) * n * 4) break;
                    arr.resize(static_cast<size_t>(m) * n);
                    const int16_t* q = reinterpret_cast<const int16_t*>(buf.data() + 32);
                    for (uint32_t i = 0; i < n; i++)
                        for (uint32_t k = 0; k < m; k++, q += 2) arr[k * n + i] = kp::cf(q[0] * scale, q[1] * scale);
                    kp::ArrayBlock b;
                    b.m = static_cast<int>(m);
                    b.n = n;
                    b.rate = rate;
                    b.t0 = t0;
                    b.gap = flags & 1;
                    for (uint32_t k = 0; k < m; k++) b.ch[k] = arr.data() + static_cast<size_t>(k) * n;
                    host.array_t_ = t0;
                    dec->process_array(b);
                    break;
                }
                case kp::wire::OPTION: {
                    std::string s(buf.begin(), buf.end());
                    size_t eq = s.find('=');
                    std::string k = s.substr(0, eq), v = eq == std::string::npos ? "" : s.substr(eq + 1);
                    if (k == "verbose") host.verbose_ = v == "1";
                    if (k == "log_raw") { host.raw_wanted_ = v == "1"; break; }   // host-side only
                    if (k == "video_wanted") { host.set_video_wanted(v == "1"); break; }   // host-side only
                    // "" = back to the declared default
                    if (v.empty())
                        for (const auto& o : info.options)
                            if (k == o.key) v = o.def;
                    dec->option(k, v);
                    break;
                }
                case kp::wire::RESET:
                    dec->reset();
                    break;
                case kp::wire::VOICE_WANTED:
                    host.voice_wanted_ = !buf.empty() && buf[0];
                    break;
                case kp::wire::SYNC:
                    host.sync_done(buf.data(), buf.size());
                    break;
                case kp::wire::STATION: {
                    std::string s(buf.begin(), buf.end());
                    double la = 0, lo = 0;
                    host.have_station_ = sscanf(s.c_str(), "%lf,%lf", &la, &lo) == 2 && std::isfinite(la) &&
                                         std::isfinite(lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180;
                    host.st_lat_ = la;
                    host.st_lon_ = lo;
                    break;
                }
                case kp::wire::RF: {
                    std::string s(buf.begin(), buf.end());
                    double f = NAN;   // an older host may add ",0" / ",1" (mirrored, no more)
                    host.rf_hz_ = sscanf(s.c_str(), "%lf", &f) == 1 && std::isfinite(f) && f > 0 ? f : NAN;
                    break;
                }
                default:
                    break;
            }
        } catch (const std::exception& e) {
            std::cerr << "decoder exception: " << e.what() << std::endl;
        }
    }
}

// --------------------------------------------------------------------------
// Offline test mode
// --------------------------------------------------------------------------
class TestHost : public kp::Host {
public:
    TestHost(double rate, bool quiet) : rate_(rate), quiet_(quiet) {}
    void fact(const std::string& k, const std::string& v) override {
        for (auto& f : facts_)
            if (f.first == k) {
                if (v.empty()) { facts_.erase(facts_.begin() + (&f - &facts_[0])); return; }
                f.second = v;
                return;
            }
        if (!v.empty()) facts_.emplace_back(k, v);
    }
    void event(const std::string& text, double dedup_s) override {
        double t = time();
        auto it = last_.find(text);
        if (it != last_.end() && t - it->second < dedup_s) return;
        last_[text] = t;
        events_++;
        if (!quiet_) printf("[%9.3f s] %s\n", t, text.c_str());
    }
    void valid() override {
        valid_++;
        if (first_valid_ < 0) first_valid_ = time();
    }
    // valid frames with their samples (Host::valid(start, end)): counted, the
    // time they cover, and each one with --verbose
    uint64_t valid_spans_ = 0;
    double valid_span_s_ = 0, valid_span_last_ = -1;
    void valid(double start_s, double end_s) override {
        valid();
        if (!std::isfinite(start_s) || !std::isfinite(end_s) || end_s < start_s) return;
        valid_spans_++;
        // covered time without counting overlaps twice
        valid_span_s_ += end_s - std::max(start_s, std::min(end_s, valid_span_last_));
        valid_span_last_ = std::max(valid_span_last_, end_s);
        if (verbose_ && !quiet_) printf("[%9.3f s] (valid) %.4f .. %.4f s\n", time(), start_s, end_s);
    }
    // counted; with --audio written to its WAV at once (wav8k_header)
    void audio(const float* pcm, size_t n) override {
        audio_n_ += n;
        if (!audio_f_) return;
        audio_buf_.resize(n);
        for (size_t i = 0; i < n; i++) audio_buf_[i] = static_cast<int16_t>(std::clamp(pcm[i], -1.0f, 1.0f) * 32767);
        fwrite(audio_buf_.data(), 2, n, audio_f_);
    }
    FILE* audio_f_ = nullptr;
    uint64_t audio_n_ = 0;
    std::vector<int16_t> audio_buf_;
    void voice_state(const std::string& s) override {
        if (s != vstate_ && !quiet_ && !s.empty()) printf("[%9.3f s] (voice) %s\n", time(), s.c_str());
        vstate_ = s;
    }
    void freq_error(float hz) override { ferr_ = hz; nferr_++; }
    bool verbose() const override { return verbose_; }
    bool voice_wanted() const override { return want_audio_; }
    double time() const override { return array_t_ >= 0 ? array_t_ : samples_ / rate_; }
    double sample_rate() const override { return rate_; }
    double array_t_ = -1;
    void log(const std::string& t) override {
        if (!quiet_) fprintf(stderr, "[%9.3f s] log: %s\n", time(), t.c_str());
    }
    // spatial reports (array plugins): per id, the trace-normalised covariances summed
    struct Sp {
        std::string label;
        uint64_t n = 0;
        int m = 0;
        double delay = NAN, power = NAN, snr = NAN, first = 0, last = 0;
        bool ref = false;
        std::vector<std::complex<double>> sum;
    };
    std::map<std::string, Sp> spatial_;
    void spatial(const kp::Spatial& s) override {
        if (s.id.empty() || s.m < 1 || s.m > 8 || s.cov.size() != static_cast<size_t>(s.m * s.m)) return;
        Sp& k = spatial_[s.id];
        if (k.m != s.m) {
            k.m = s.m;
            k.sum.assign(s.cov.size(), {});
        }
        double tr = 0;
        for (int i = 0; i < s.m; i++) tr += s.cov[static_cast<size_t>(i * s.m + i)].real();
        if (!(tr > 0)) return;
        for (size_t i = 0; i < s.cov.size(); i++) k.sum[i] += std::complex<double>(s.cov[i]) / tr;
        if (!k.n) k.first = time();
        k.n++;
        k.last = time();
        k.label = s.label;
        k.delay = s.delay_us;
        k.power = s.power_db;
        k.snr = s.snr_db;
        k.ref = s.ref;
        if (verbose_ && !quiet_)
            printf("[%9.3f s] (spatial) %s %s delay %.2f us %.1f dB snr %.1f dB%s\n", time(), s.id.c_str(), s.label.c_str(),
                   s.delay_us, s.power_db, s.snr_db, s.ref ? " ref" : "");
    }
    // a removed one leaves the summary (as kraken_doa forgets it)
    uint64_t spatial_removed_ = 0;
    void spatial_remove(const std::string& id) override {
        size_t n = 0;
        if (id.empty()) {
            n = spatial_.size();
            spatial_.clear();
        } else {
            n = spatial_.erase(id);
        }
        spatial_removed_ += n;
        if (n && !quiet_) printf("[%9.3f s] (spatial removed) %s\n", time(), id.empty() ? "all" : id.c_str());
    }
    // the latest plot per id (kp::Plot::id; "" = the plugin's one plot) and how many came
    std::map<std::string, std::pair<kp::Plot, uint64_t>> plots_;
    std::string plots_path_;   // --plots FILE: the last plot of each id as JSON lines
    void plot(const kp::Plot& p) override {
        auto& e = plots_[p.id];
        e.first = p;
        e.second++;
    }
    void print_plots() const {
        for (const auto& kv : plots_) {
            const kp::Plot& pl = kv.second.first;
            printf("plot%s%s: %llu sent; last \"%s\" %zu points %s .. %s %s, %zu markers:", kv.first.empty() ? "" : " ",
                   kv.first.c_str(), static_cast<unsigned long long>(kv.second.second), pl.title.c_str(), pl.y.size(),
                   wnum(pl.x0).c_str(), wnum(pl.x0 + pl.dx * std::max<double>(0, pl.y.size() - 1.0)).c_str(),
                   pl.x_unit.c_str(), pl.markers.size());
            for (const auto& mk : pl.markers) printf(" %s@%.6g", mk.label.c_str(), mk.x);
            // the highest point, and what a click would set (PLOT_EX)
            size_t pk = 0;
            for (size_t i = 1; i < pl.y.size(); i++)
                if (pl.y[i] > pl.y[pk]) pk = i;
            if (!pl.y.empty()) printf("; peak %.4g at %.9g", pl.y[pk], pl.x0 + pl.dx * static_cast<double>(pk));
            if (pl.place == kp::PLOT_BESIDE_VIDEO) printf("; beside the video");
            if (!pl.click_option.empty())
                printf("; click sets %s%s%s (snap %g, step %g)", pl.click_option.c_str(), pl.click_also.empty() ? "" : " + ",
                       pl.click_also.c_str(), pl.click_snap, pl.click_step);
            printf("\n");
        }
        if (plots_path_.empty() || plots_.empty()) return;
        FILE* f = fopen(plots_path_.c_str(), "w");
        if (!f) {
            fprintf(stderr, "--plots: can't write %s\n", plots_path_.c_str());
            return;
        }
        auto num = [](double v) { return std::isfinite(v) ? wnum(v, 9) : std::string("null"); };
        auto str = [](const std::string& s) {
            std::string o = "\"";
            for (unsigned char c : s) {
                if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
                else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
            }
            return o + "\"";
        };
        for (const auto& kv : plots_) {
            const kp::Plot& pl = kv.second.first;
            std::string o = "{\"id\":" + str(pl.id) + ",\"title\":" + str(pl.title) + ",\"x_unit\":" + str(pl.x_unit) +
                            ",\"y_unit\":" + str(pl.y_unit) + ",\"x0\":" + num(pl.x0) + ",\"dx\":" + num(pl.dx) +
                            ",\"place\":" + std::to_string(pl.place) + ",\"click_option\":" + str(pl.click_option) +
                            ",\"click_also\":" + str(pl.click_also) + ",\"click_snap\":" + num(pl.click_snap) +
                            ",\"click_step\":" + num(pl.click_step) + ",\"y\":[";
            for (size_t i = 0; i < pl.y.size(); i++) o += (i ? "," : "") + num(pl.y[i]);
            o += "],\"markers\":[";
            for (size_t i = 0; i < pl.markers.size(); i++)
                o += std::string(i ? "," : "") + "{\"x\":" + num(pl.markers[i].x) + ",\"label\":" + str(pl.markers[i].label) + "}";
            fprintf(f, "%s]}\n", o.c_str());
        }
        fclose(f);
    }
    // Host::option_choices: the latest list per option (as kraken_doa keeps
    // it), how often it was sent and how often it changed
    struct Ch { Choices list; uint64_t sent = 0, changed = 0; bool declared = false; };
    std::map<std::string, Ch> choices_;
    void option_choices(const std::string& key, const Choices& choices) override {
        if (!option_key_ok(key)) return;
        Ch& c = choices_[key];
        const kp::Info info = kp_plugin_info();
        c.declared = false;
        for (const auto& o : info.options) c.declared = c.declared || key == o.key;
        Choices l = clean_choices(choices);
        c.sent++;
        if (l == c.list && c.sent > 1) return;
        c.changed++;
        c.list = std::move(l);
        if (!quiet_) printf("[%9.3f s] (choices) %s: %zu\n", time(), key.c_str(), c.list.size());
    }
    void print_choices() const {
        for (const auto& [k, c] : choices_) {
            printf("option choices \"%s\": %zu (sent %llu times, %llu changes)%s\n", k.c_str(), c.list.size(),
                   static_cast<unsigned long long>(c.sent), static_cast<unsigned long long>(c.changed),
                   c.declared ? "" : " - not one of the plugin's options: kraken_doa ignores it");
            for (const auto& [v, l] : c.list) printf("  %-10s %s\n", v.empty() ? "\"\"" : v.c_str(), l.c_str());
        }
    }
    // a recording can't be retuned: shown, refused
    bool tune(double rf_hz) override {
        if (!quiet_) printf("[%9.3f s] (tune) %.6f MHz asked - a recording can't be retuned\n", time(), rf_hz / 1e6);
        return false;
    }

    // --- trunked systems: each call as one line, cleaned as the live host
    // sends it. A line identical to the LAST one printed for the same thing
    // (the system; a channel: name + slot; the control channel: its name; a
    // grant: channel name + slot + target; a grant end: name + slot; a call: its
    // slot) repeats an unchanged state and is not printed again, except with
    // --verbose - every change prints, a change back too (a grant update
    // every second prints once; ACTIVE, ENDED, ACTIVE, ENDED all four). A
    // printed grant end forgets its channel's grants, a printed grant its
    // channel's grant end. The summary counts all
    std::map<std::string, std::string> trunk_last_;   // tag '\0' key -> the last line printed
    uint64_t trunk_msgs_ = 0, trunk_quiet_ = 0, trunk_cut_ = 0, trunk_dropped_ = 0;
    // true = printed (not a repeat)
    bool trunk_line(const std::string& tag, const std::string& key, const std::string& body) {
        trunk_msgs_++;
        const std::string line = "(" + tag + ") " + body;
        std::string& last = trunk_last_[tag + '\0' + key];
        if (last == line && !verbose_) {
            trunk_quiet_++;
            return false;
        }
        last = line;
        if (!quiet_) printf("[%9.3f s] %s\n", time(), line.c_str());
        return true;
    }
    // forget the lines of tag for channel name (slot -1: every slot)
    void trunk_forget(const std::string& tag, const std::string& name, int slot) {
        const std::string pre = tag + '\0' + name + '\0';
        for (auto it = trunk_last_.lower_bound(pre); it != trunk_last_.end() && it->first.compare(0, pre.size(), pre) == 0;) {
            const int s = atoi(it->first.c_str() + pre.size());
            if (slot < 0 || s < 0 || s == slot) it = trunk_last_.erase(it);
            else ++it;
        }
    }
    static std::string tkey(const kp::TrunkChannel& c) { return c.name + '\0' + std::to_string(c.slot); }
    static std::string tq(const std::string& s) {   // "text", \" and \\ escaped
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') o += '\\';
            o += c;
        }
        return o + "\"";
    }
    static std::string tmhz(double f) {
        if (!std::isfinite(f)) return "?";
        char b[40];
        snprintf(b, sizeof b, "%.6f", f / 1e6);
        return b;
    }
    // ch="1-0412" num=412 mhz=851.012500 slot=-1 [up=806.012500]
    static std::string tch(const kp::TrunkChannel& c) {
        std::string s = "ch=" + tq(c.name) + " num=" + std::to_string(c.number) + " mhz=" + tmhz(c.freq_hz) +
                        " slot=" + std::to_string(c.slot);
        if (std::isfinite(c.uplink_hz)) s += " up=" + tmhz(c.uplink_hz);
        return s;
    }
    static std::string tflags(bool e, bool m, bool d, bool u) {
        std::string f;
        if (e) f += 'e';
        if (m) f += 'm';
        if (d) f += 'd';
        if (u) f += 'u';
        return f.empty() ? "-" : f;
    }
    void trunk_drop(const char* what) {
        trunk_dropped_++;
        trunk_line("trunk dropped", what, std::string(what) + ": channel without a name (kraken_doa drops it)");
    }
    kp::TrunkSystem sys_;
    uint64_t sys_n_ = 0;
    void trunk_system(const kp::TrunkSystem& s0) override {
        const kp::TrunkSystem s = tr_system(s0, &trunk_cut_);
        sys_ = s;
        sys_n_++;
        trunk_line("system", "", "key=" + tq(s.key) + " name=" + tq(s.name) + " site=" + tq(s.site) +
                                 " control=" + (s.control ? "1" : "0"));
    }
    std::map<std::string, kp::TrunkChannel> tchannels_;   // name/slot -> the latest
    void trunk_channel(const kp::TrunkChannel& c0) override {
        const kp::TrunkChannel c = tr_channel(c0, &trunk_cut_);
        if (c.name.empty()) return trunk_drop("channel");
        tchannels_[c.name + "/" + std::to_string(c.slot)] = c;
        trunk_line("channel", tkey(c), tch(c));
    }
    kp::TrunkChannel control_;
    uint64_t control_n_ = 0;
    void control_channel(const kp::TrunkChannel& c0) override {
        const kp::TrunkChannel c = tr_channel(c0, &trunk_cut_);
        if (c.name.empty()) return trunk_drop("control");
        control_ = c;
        control_n_++;
        trunk_line("control", c.name, tch(c));
    }
    struct TGrant {
        kp::Grant g;
        double first = 0, last = 0;
        uint64_t n = 0, updates = 0;
    };
    std::vector<TGrant> tgrants_;   // per channel name + slot + target, in order of first
    uint64_t grants_ = 0, grant_updates_ = 0, grant_data_ = 0, grant_enc_ = 0, grant_emerg_ = 0;
    void grant(const kp::Grant& g0) override {
        const kp::Grant g = tr_grant(g0, &trunk_cut_);
        if (g.channel.name.empty()) return trunk_drop("grant");
        grants_++;
        grant_updates_ += g.update;
        grant_data_ += g.data;
        grant_enc_ += g.encrypted;
        grant_emerg_ += g.emergency;
        TGrant* k = nullptr;
        for (auto& x : tgrants_)
            if (x.g.channel.name == g.channel.name && x.g.channel.slot == g.channel.slot && x.g.target == g.target) k = &x;
        if (!k) {
            tgrants_.push_back({g, time(), time(), 0, 0});
            k = &tgrants_.back();
        }
        k->g = g;
        k->last = time();
        k->n++;
        k->updates += g.update;
        if (trunk_line("grant", tkey(g.channel) + '\0' + g.target, grant_text(g)))
            trunk_forget("grant end", g.channel.name, g.channel.slot);
    }
    static std::string grant_text(const kp::Grant& g) {
        std::string s = tch(g.channel) + " target=" + tq(g.target) + (g.group ? " group" : " unit") + " source=" +
                        tq(g.source) + " flags=" + tflags(g.encrypted, g.emergency, g.data, g.update) +
                        " decoder=" + tq(g.decoder) + " demod=" + tq(g.demod) + " bw=" +
                        std::to_string(static_cast<long long>(std::llround(g.bw_hz)));
        for (const auto& [k, v] : g.options) s += " opt." + k + "=" + tq(v);
        return s;
    }
    std::map<std::string, std::pair<kp::TrunkChannel, uint64_t>> grant_ends_;
    uint64_t grant_end_n_ = 0;
    void grant_end(const kp::TrunkChannel& c0) override {
        const kp::TrunkChannel c = tr_channel(c0, &trunk_cut_);
        if (c.name.empty()) return trunk_drop("grant end");
        grant_end_n_++;
        auto& e = grant_ends_[c.name + "/" + std::to_string(c.slot)];
        e.first = c;
        e.second++;
        if (trunk_line("grant end", tkey(c), tch(c))) trunk_forget("grant", c.name, c.slot);
    }
    // calls as the host would see them: per slot an open call; ACTIVE with
    // another known target (or source) starts a new one, ENDED / RELEASED
    // closes it (a RELEASED after an ENDED releases that call)
    struct TCall {
        kp::Call c;
        double first = 0, last = 0;
        int end = -1;   // -1 open, else Call::ENDED / RELEASED
    };
    std::vector<TCall> tcalls_;
    uint64_t call_msgs_ = 0;
    static const char* call_state(int s) {
        return s == kp::Call::ENDED ? "ENDED" : s == kp::Call::RELEASED ? "RELEASED" : "ACTIVE";
    }
    void call(const kp::Call& c0) override {
        const kp::Call c = tr_call(c0, &trunk_cut_);
        call_msgs_++;
        TCall* open = nullptr;
        for (auto& x : tcalls_)
            if (x.end < 0 && x.c.slot == c.slot) open = &x;
        auto differs = [](const std::string& a, const std::string& b) { return !a.empty() && !b.empty() && a != b; };
        if (c.state == kp::Call::ACTIVE) {
            if (open && (differs(open->c.target, c.target) || differs(open->c.source, c.source))) {
                open->end = kp::Call::ENDED;
                open = nullptr;
            }
            if (!open) {
                tcalls_.push_back({c, time(), time(), -1});
                open = &tcalls_.back();
            }
            if (!c.target.empty()) open->c.target = c.target, open->c.group = c.group;
            if (!c.source.empty()) open->c.source = c.source;
            open->c.encrypted = open->c.encrypted || c.encrypted;
            open->c.emergency = open->c.emergency || c.emergency;
            open->last = time();
        } else if (open) {
            open->end = c.state;
            open->last = time();
        } else if (c.state == kp::Call::RELEASED) {
            // the clear-down after the hang time: the slot's last call, ended, is released now
            for (auto it = tcalls_.rbegin(); it != tcalls_.rend(); ++it)
                if (it->c.slot == c.slot) {
                    if (it->end == kp::Call::ENDED && !differs(it->c.target, c.target)) {
                        it->end = kp::Call::RELEASED;
                        it->last = time();
                    }
                    break;
                }
        }
        trunk_line("call", std::to_string(c.slot), std::string("state=") + call_state(c.state) + " target=" + tq(c.target) +
                               (c.group ? " group" : " unit") + " source=" + tq(c.source) + " slot=" +
                               std::to_string(c.slot) + " flags=" + tflags(c.encrypted, c.emergency, false, false));
    }
    void print_trunk() const {
        if (!trunk_msgs_) return;
        if (sys_n_)
            printf("trunk system: key=%s name=%s site=%s control=%d (%llu reports)\n", tq(sys_.key).c_str(),
                   tq(sys_.name).c_str(), tq(sys_.site).c_str(), sys_.control ? 1 : 0, static_cast<unsigned long long>(sys_n_));
        else
            printf("trunk system: none\n");
        if (control_n_)
            printf("trunk control channel: %s (%llu reports)\n", tch(control_).c_str(), static_cast<unsigned long long>(control_n_));
        if (!tchannels_.empty()) {
            printf("trunk channels: %zu\n", tchannels_.size());
            size_t i = 0;
            for (const auto& kv : tchannels_)
                if (i++ < 100) printf("  %s\n", tch(kv.second).c_str());
        }
        printf("trunk grants: %llu messages (%llu updates, %llu data, %llu encrypted, %llu emergency), %zu distinct channel+target\n",
               static_cast<unsigned long long>(grants_), static_cast<unsigned long long>(grant_updates_),
               static_cast<unsigned long long>(grant_data_), static_cast<unsigned long long>(grant_enc_),
               static_cast<unsigned long long>(grant_emerg_), tgrants_.size());
        for (size_t i = 0; i < tgrants_.size() && i < 200; i++) {
            const TGrant& k = tgrants_[i];
            printf("  %s  %.3f .. %.3f s  %llu messages\n", grant_text(k.g).c_str(), k.first, k.last,
                   static_cast<unsigned long long>(k.n));
        }
        if (grant_end_n_) {
            printf("trunk grant ends: %llu messages, %zu channels\n", static_cast<unsigned long long>(grant_end_n_), grant_ends_.size());
            for (const auto& kv : grant_ends_)
                printf("  %s  %llu messages\n", tch(kv.second.first).c_str(), static_cast<unsigned long long>(kv.second.second));
        }
        size_t active = 0, ended = 0, released = 0;
        for (const auto& x : tcalls_) (x.end < 0 ? active : x.end == kp::Call::RELEASED ? released : ended)++;
        printf("trunk calls: %llu messages, %zu calls (%zu still active, %zu ended, %zu released)\n",
               static_cast<unsigned long long>(call_msgs_), tcalls_.size(), active, ended, released);
        for (size_t i = 0; i < tcalls_.size() && i < 200; i++) {
            const TCall& x = tcalls_[i];
            printf("  slot=%d target=%s %s source=%s flags=%s  %.3f .. %.3f s  %s\n", x.c.slot, tq(x.c.target).c_str(),
                   x.c.group ? "group" : "unit", tq(x.c.source).c_str(),
                   tflags(x.c.encrypted, x.c.emergency, false, false).c_str(), x.first, x.last,
                   x.end < 0 ? "open" : call_state(x.end));
        }
        if (trunk_quiet_)
            printf("trunk lines not printed (an unchanged state repeated; --verbose prints them): %llu\n",
                   static_cast<unsigned long long>(trunk_quiet_));
        if (trunk_cut_ || trunk_dropped_)
            printf("trunk fields cut / dropped by the wire limits: %llu, messages dropped: %llu (kraken_doa does the same)\n",
                   static_cast<unsigned long long>(trunk_cut_), static_cast<unsigned long long>(trunk_dropped_));
    }
    void map_point(const kp::MapPoint& m) override {
        if (!std::isfinite(m.lat) || !std::isfinite(m.lon) || m.id.empty()) return;
        map_updates_++;
        map_[m.id] = m;
    }
    void map_remove(const std::string& id) override { map_.erase(id); }
    void table_columns(const std::vector<std::string>& cols) override { cols_ = cols; }
    void table_row(const std::string& key, const std::vector<std::string>& cells) override { rows_[key] = cells; }
    void table_remove(const std::string& key) override {
        rows_.erase(key);
        details_.erase(key);
    }
    void table_detail(const std::string& key, const std::vector<std::pair<std::string, std::string>>& lines) override { details_[key] = lines; }
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> details_;
    // the rows + their details
    void print_table(size_t max_rows) const {
        if (rows_.empty()) return;
        printf("table: %zu rows\n", rows_.size());
        std::string h = "  ";
        for (const auto& c : cols_) h += c + " | ";
        printf("%s\n", h.c_str());
        size_t n = 0;
        for (const auto& kv : rows_) {
            if (++n > max_rows) {
                printf("  ...\n");
                break;
            }
            std::string r = "  ";
            for (const auto& c : kv.second) r += c + " | ";
            printf("%s\n", r.c_str());
            const auto it = details_.find(kv.first);
            if (it == details_.end()) continue;
            for (const auto& [k, v] : it->second) printf("      %-34s %s\n", (k + ":").c_str(), v.c_str());
        }
    }
    bool raw_wanted() const override { return raw_; }
    void raw(const std::string& data) override {
        raws_++;
        if (raw_ && !quiet_) printf("[%9.3f s] (raw) %s\n", time(), data.c_str());
    }
    bool raw_ = false;
    uint64_t raws_ = 0;
    void message(const std::string& from, const std::string& text) override {
        messages_++;
        if (!quiet_) printf("[%9.3f s] (message) %s: %s\n", time(), from.c_str(), text.c_str());
    }
    uint64_t messages_ = 0;
    // talker spans as the live host cuts them: a new id ends the channel's previous one
    struct Span { std::string id, label; double start, end; bool open; int channel; };
    std::vector<Span> spans_;
    Span* open_span(int ch) {
        for (auto it = spans_.rbegin(); it != spans_.rend(); ++it)
            if (it->open && it->channel == ch) return &*it;
        return nullptr;
    }
    // packets (Talker::packet): counted per talker
    struct Pk {
        std::string label;
        uint64_t n = 0, npos = 0;   // packets, those with a position (Talker::lat / lon / alt_m)
        double first = 0, last = 0, len_us = 0;
        double lat = NAN, lon = NAN, alt = NAN;   // the latest position
    };
    std::map<std::string, Pk> packets_;
    uint64_t packets_total_ = 0;
    // every packet with its channel, for the per-talker bearings (--uca on a
    // multichannel recording)
    struct PkRec {
        std::string id;
        double start, end, freq, bw;
        int m = 0;                                   // Talker::cov (separation by code; 0 = none)
        std::vector<std::complex<double>> cov;
    };
    std::vector<PkRec> pk_recs_;
    bool keep_pk_ = false;
    void talker(const kp::Talker& t) override {
        if (t.id.empty()) return;
        const double st = std::isfinite(t.start_s) ? t.start_s : time();
        const double en = std::isfinite(t.end_s) ? t.end_s : time();
        if (t.packet) {
            if (keep_pk_) {
                pk_recs_.push_back({t.id, st, en, t.freq_hz, t.bw_hz});
                if (t.m >= 2 && t.cov.size() == static_cast<size_t>(t.m * t.m)) {
                    pk_recs_.back().m = t.m;
                    pk_recs_.back().cov.assign(t.cov.begin(), t.cov.end());
                }
            }
            Pk& k = packets_[t.id];
            if (!k.n) k.first = st;
            k.n++;
            k.last = st;
            k.len_us += (en - st) * 1e6;
            if (!t.label.empty()) k.label = t.label;
            const bool pos = std::isfinite(t.lat) && std::isfinite(t.lon) && std::isfinite(t.alt_m);
            if (pos) {
                k.npos++;
                k.lat = t.lat;
                k.lon = t.lon;
                k.alt = t.alt_m;
            }
            packets_total_++;
            if (verbose_ && !quiet_) {
                char where[96] = "";
                if (pos) snprintf(where, sizeof where, " at %.5f, %.5f, %.0f m", t.lat, t.lon, t.alt_m);
                printf("[%9.3f s] (packet) %s %.7f .. %.7f s%s%s\n", time(), t.id.c_str(), st, en,
                       std::isfinite(t.freq_hz) ? (" at " + std::to_string(static_cast<long long>(std::llround(t.freq_hz))) + " Hz").c_str() : "",
                       where);
            }
            return;
        }
        Span* s = open_span(t.channel);
        if (!s || s->id != t.id) {
            if (s) talker_end(st, t.channel);
            spans_.push_back({t.id, t.label, st, en, true, t.channel});
            s = &spans_.back();
            if (!quiet_)
                printf("[%9.3f s] (talker) %s %s: from %.3f s%s\n", time(), t.id.c_str(), t.label.c_str(), st,
                       t.channel ? (" (channel " + std::to_string(t.channel) + ")").c_str() : "");
        }
        s->end = std::max(s->end, en);
        if (!t.label.empty()) s->label = t.label;
    }
    void talker_end(double at_s, int channel) override {
        Span* s = open_span(channel);
        if (!s) return;
        s->open = false;
        if (std::isfinite(at_s)) s->end = std::max(s->start, at_s);
        if (!quiet_) printf("[%9.3f s] (talker) %s ended: %.3f .. %.3f s\n", time(), s->id.c_str(), s->start, s->end);
    }
    std::vector<std::string> cols_;
    std::map<std::string, std::vector<std::string>> rows_;
    bool station(double* lat, double* lon) const override {
        if (!have_station_) return false;
        *lat = st_lat_;
        *lon = st_lon_;
        return true;
    }
    bool have_station_ = false;
    double st_lat_ = 0, st_lon_ = 0;
    double rf_hz() const override { return rf_hz_; }
    double rf_hz_ = NAN;
    uint64_t map_updates_ = 0;
    std::map<std::string, kp::MapPoint> map_;

    double rate_;
    bool quiet_, verbose_ = false, want_audio_ = false;
    double samples_ = 0;
    uint64_t valid_ = 0, events_ = 0, nferr_ = 0;
    double first_valid_ = -1;
    float ferr_ = 0;
    std::string vstate_;
    std::vector<std::pair<std::string, std::string>> facts_;
    std::map<std::string, double> last_;
};

// a number from the recording's JSON sidecar (ai/sigtool.py captures:
// "rate", "rf_hz")
bool read_sidecar(const std::string& path, const char* key, double* out) {
    std::ifstream f(path + ".json");
    if (!f) {
        // also accept capture.json next to capture.cf32
        size_t dot = path.rfind('.');
        if (dot == std::string::npos) return false;
        f.open(path.substr(0, dot) + ".json");
        if (!f) return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    size_t p = s.find("\"" + std::string(key) + "\"");
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    *out = atof(s.c_str() + p + 1);
    return *out > 0;
}
bool read_sidecar_rate(const std::string& path, double* rate) { return read_sidecar(path, "rate", rate); }

// The recording's bytes, read where they are asked for: a regular file
// through fseeko / fread, so memory does not grow with its size. Anything
// else (a pipe: --file /dev/stdin) can't seek - its bytes are read whole
// once (as raw bytes: a cu8 pipe takes 2 bytes per sample, not 8)
struct ByteSource {
    FILE* f = nullptr;
    std::vector<char> mem;
    bool in_mem = false;
    uint64_t size = 0, pos = 0;
    bool open(const std::string& path) {
        f = fopen(path.c_str(), "rb");
        if (!f) return false;
        struct stat st;
        if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) {
            size = static_cast<uint64_t>(st.st_size);
            return true;
        }
        in_mem = true;
        std::vector<char> b(1 << 16);
        for (size_t got; (got = fread(b.data(), 1, b.size(), f)) > 0;) mem.insert(mem.end(), b.begin(), b.begin() + got);
        size = mem.size();
        fclose(f);
        f = nullptr;
        return true;
    }
    // bytes [off, off + n) -> dst; returns how many there are (fewer at the end)
    size_t read_at(uint64_t off, void* dst, size_t n) {
        if (off >= size) return 0;
        n = static_cast<size_t>(std::min<uint64_t>(n, size - off));
        if (in_mem) {
            memcpy(dst, mem.data() + off, n);
            return n;
        }
        if (off != pos && fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) {
            pos = UINT64_MAX;
            return 0;
        }
        const size_t got = fread(dst, 1, n, f);
        pos = off + got;
        return got;
    }
    ~ByteSource() {
        if (f) fclose(f);
    }
};

// A one-antenna recording (cf32, cu8, cs16, 16 / 24-bit or float IQ WAV) as
// complex float, read block by block where asked (the selected range
// [s0, s1) of --start / --seconds). The checks and messages are the ones the
// whole-file loader had; the values are bit for bit the same.
struct IqFile {
    ByteSource src;
    int kind = 0;        // 0 cf32, 1 cs16, 2 cs24, 3 cu8
    uint64_t off = 0;    // first sample byte (after a WAV header)
    size_t bps = 8, s0 = 0, s1 = 0;
    std::vector<char> raw;
    size_t count() const { return s1 - s0; }
    bool open(const std::string& path, std::string fmt, double* rate, double start, double seconds, std::string& err) {
        if (fmt.empty()) {
            std::string ext = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.') + 1);
            if (ext == "cu8" || ext == "u8") fmt = "cu8";
            else if (ext == "cs16" || ext == "s16") fmt = "cs16";
            else if (ext == "wav") fmt = "wav";
            else fmt = "cf32";
        }
        if (!src.open(path)) { err = "cannot open " + path; return false; }
        if (fmt == "wav") {
            // minimal RIFF parser: 2 channels (I, Q), int16 or float32; the
            // data runs from its chunk to the end of the file
            char h[24];
            auto at = [&](uint64_t p, size_t n) {   // n header bytes at p, zeros past the end
                memset(h, 0, sizeof h);
                src.read_at(p, h, n);
                return h;
            };
            if (src.size < 44 || memcmp(at(0, 12), "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
                err = "not a WAV file";
                return false;
            }
            uint64_t p = 12;
            int ch = 0, bits = 0;
            bool wav_float = false;
            while (p + 8 <= src.size) {
                uint32_t len;
                at(p, 24);
                memcpy(&len, h + 4, 4);
                if (!memcmp(h, "fmt ", 4)) {
                    uint16_t afmt, nch, bp;
                    uint32_t sr;
                    memcpy(&afmt, h + 8, 2);
                    memcpy(&nch, h + 10, 2);
                    memcpy(&sr, h + 12, 4);
                    memcpy(&bp, h + 22, 2);
                    ch = nch;
                    bits = bp;
                    wav_float = afmt == 3;
                    if (*rate <= 0) *rate = sr;
                } else if (!memcmp(h, "data", 4)) {
                    off = p + 8;
                    break;
                }
                p += 8 + len + (len & 1);   // (32-bit sum, as it always was)
            }
            if (ch != 2 || off == 0) { err = "WAV must be 2-channel (I/Q)"; return false; }
            fmt = wav_float ? "cf32" : (bits == 16 ? "cs16" : (bits == 24 ? "cs24" : ""));
            if (fmt.empty()) { err = "unsupported WAV sample format"; return false; }
        }
        if (*rate <= 0 && !read_sidecar_rate(path, rate)) {
            err = "sample rate unknown: pass --rate";
            return false;
        }
        kind = fmt == "cf32" ? 0 : fmt == "cs16" ? 1 : fmt == "cs24" ? 2 : 3;
        bps = kind == 0 ? 8 : kind == 1 ? 4 : kind == 2 ? 6 : 2;
        const size_t total = static_cast<size_t>((src.size - off) / bps);
        s0 = std::min(total, static_cast<size_t>(std::max(0.0, start) * *rate));
        s1 = seconds > 0 ? std::min(total, s0 + static_cast<size_t>(seconds * *rate)) : total;
        return true;
    }
    // samples [i, i + n) of the selected range -> out; returns how many
    size_t read(size_t i, size_t n, kp::cf* out) {
        if (i >= count()) return 0;
        n = std::min(n, count() - i);
        raw.resize(n * bps);
        n = src.read_at(off + static_cast<uint64_t>(s0 + i) * bps, raw.data(), n * bps) / bps;
        const char* d = raw.data();
        for (size_t k = 0; k < n; k++) {
            kp::cf v;
            if (kind == 0) {
                float iq[2];
                memcpy(iq, d + k * 8, 8);
                v = {iq[0], iq[1]};
            } else if (kind == 1) {
                int16_t iq[2];
                memcpy(iq, d + k * 4, 4);
                v = {iq[0] / 32768.0f, iq[1] / 32768.0f};
            } else if (kind == 2) {
                auto q = reinterpret_cast<const uint8_t*>(d + k * 6);
                int32_t a = static_cast<int32_t>(static_cast<uint32_t>(q[0]) << 8 | static_cast<uint32_t>(q[1]) << 16 |
                                                 static_cast<uint32_t>(q[2]) << 24) >> 8;
                int32_t b = static_cast<int32_t>(static_cast<uint32_t>(q[3]) << 8 | static_cast<uint32_t>(q[4]) << 16 |
                                                 static_cast<uint32_t>(q[5]) << 24) >> 8;
                v = {a / 8388608.0f, b / 8388608.0f};
            } else {
                auto u = reinterpret_cast<const uint8_t*>(d + k * 2);
                v = {(u[0] - 127.5f) / 127.5f, (u[1] - 127.5f) / 127.5f};
            }
            out[k] = v;
        }
        return n;
    }
};

// A recording of every antenna (--channels M / the sidecar's "channels"):
// sample by sample, antenna 0 first, cf32 / cs16 / cu8. Read a block at a
// time (5 antennas at 2.4 MHz are far too big to load whole)
struct McReader {
    FILE* f = nullptr;
    std::string fmt;
    int m = 1;
    size_t bps = 8;
    std::vector<char> raw;
    bool regular = false;   // a regular file: samples() is known up front (not a pipe)
    uint64_t size = 0, skip = 0;
    bool open(const std::string& path, std::string fm, int channels, double rate, double start, std::string& err) {
        if (fm.empty()) {
            std::string ext = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.') + 1);
            fm = ext == "cu8" || ext == "u8" ? "cu8" : ext == "cs16" || ext == "s16" ? "cs16" : "cf32";
        }
        if (fm != "cf32" && fm != "cs16" && fm != "cu8") { err = "multichannel recordings: cf32, cs16 or cu8"; return false; }
        fmt = fm;
        m = channels;
        bps = fmt == "cf32" ? 8 : fmt == "cs16" ? 4 : 2;
        f = fopen(path.c_str(), "rb");
        if (!f) { err = "cannot open " + path; return false; }
        struct stat st;
        if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) {
            regular = true;
            size = static_cast<uint64_t>(st.st_size);
        }
        skip = static_cast<uint64_t>(static_cast<long long>(std::max(0.0, start) * rate) * m * static_cast<long long>(bps));
        if (skip && fseeko(f, static_cast<off_t>(skip), SEEK_SET) != 0) { err = "cannot seek"; return false; }
        return true;
    }
    // the samples (of every antenna) from the start position to the end; regular files only
    size_t samples() const { return size > skip ? static_cast<size_t>((size - skip) / (static_cast<uint64_t>(m) * bps)) : 0; }
    // up to n samples of every antenna -> out[k] (only >= 0: only that
    // antenna is converted, out[only]); returns the samples read
    size_t read(size_t n, std::vector<std::vector<kp::cf>>& out, int only = -1) {
        raw.resize(n * m * bps);
        const size_t got = fread(raw.data(), m * bps, n, f);
        out.resize(static_cast<size_t>(m));
        for (int k = 0; k < m; k++) out[static_cast<size_t>(k)].resize(got);
        const int kind = fmt == "cf32" ? 0 : fmt == "cs16" ? 1 : 2;
        for (int k = only >= 0 ? only : 0; k < (only >= 0 ? only + 1 : m); k++) {
            kp::cf* o = out[static_cast<size_t>(k)].data();
            for (size_t i = 0; i < got; i++) {
                const char* d = raw.data() + (i * m + static_cast<size_t>(k)) * bps;
                if (kind == 0) {
                    float iq[2];
                    memcpy(iq, d, 8);
                    o[i] = {iq[0], iq[1]};
                } else if (kind == 1) {
                    int16_t iq[2];
                    memcpy(iq, d, 4);
                    o[i] = {iq[0] * (1.0f / 32768.0f), iq[1] * (1.0f / 32768.0f)};
                } else {
                    auto u = reinterpret_cast<const uint8_t*>(d);
                    o[i] = {(u[0] - 127.5f) * (1.0f / 127.5f), (u[1] - 127.5f) * (1.0f / 127.5f)};
                }
            }
        }
        return got;
    }
    ~McReader() { if (f) fclose(f); }
};

// --audio: the 8 kHz 16-bit mono WAV is written as the audio comes (its
// header, with the sizes, once more at the end)
void wav8k_header(FILE* f, uint64_t n) {
    std::string h;
    auto u32 = [&](uint32_t v) { h.append(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { h.append(reinterpret_cast<char*>(&v), 2); };
    h += "RIFF"; u32(static_cast<uint32_t>(36 + n * 2)); h += "WAVEfmt ";
    u32(16); u16(1); u16(1); u32(8000); u32(16000); u16(2); u16(16);
    h += "data"; u32(static_cast<uint32_t>(n * 2));
    fwrite(h.data(), 1, h.size(), f);
}

// MUSIC with one source on a covariance, kraken_doa's UCA (clockwise, ANT0
// on +x, radius r metres, wavelength lambda): the bearing (degrees, CCW from
// ANT0 as the MUSIC DoA plot shows it) and the peak's sharpness
double uca_bearing(const std::vector<std::complex<double>>& C, int m, double r, double lambda, double* conf) {
    // principal eigenvector by power iteration
    std::vector<std::complex<double>> u(static_cast<size_t>(m), 1.0), v(static_cast<size_t>(m));
    for (int it = 0; it < 200; it++) {
        double nn = 0;
        for (int i = 0; i < m; i++) {
            v[static_cast<size_t>(i)] = 0;
            for (int j = 0; j < m; j++) v[static_cast<size_t>(i)] += C[static_cast<size_t>(i * m + j)] * u[static_cast<size_t>(j)];
            nn += std::norm(v[static_cast<size_t>(i)]);
        }
        nn = std::sqrt(nn);
        if (!(nn > 0)) return NAN;
        for (int i = 0; i < m; i++) u[static_cast<size_t>(i)] = v[static_cast<size_t>(i)] / nn;
    }
    double best = -1, bdeg = 0, sum = 0;
    int cnt = 0;
    for (int a = 0; a < 3600; a++) {
        const double th = a * M_PI / 1800;
        std::complex<double> dot = 0;
        for (int k = 0; k < m; k++) {
            const double ea = -2 * M_PI * k / m;
            const double ph = 2 * M_PI * r / lambda * (std::cos(ea) * std::cos(th) + std::sin(ea) * std::sin(th));
            dot += std::conj(std::polar(1.0, ph)) * u[static_cast<size_t>(k)];
        }
        const double c = std::norm(dot) / m;    // |a^H u|^2 / |a|^2
        const double pm = 1 / std::max(1e-9, 1 - c);
        sum += pm;
        cnt++;
        if (pm > best) { best = pm; bdeg = a / 10.0; }
    }
    if (conf) *conf = best / (sum / cnt);
    return bdeg;
}

// Per-talker bearings of a decoder run on a multichannel recording (--uca R
// with --channels M, non-array plugins): every talker's own samples of every
// antenna - transmissions (Host::talker spans) 40 ms inside both ends, packets
// exactly, each through the channel it named (Talker::freq_hz / bw_hz; else
// the band the plugin receives) - summed as trace-normalised covariances ->
// MUSIC with one source on kraken_doa's UCA. The same rules as kraken_doa's
// TalkerDoa: packets longer than 0.25 s and packets overlapping another
// talker's on the same channel are dropped (counted) - except packets that
// carry their own covariance (Talker::cov, separation by code: taken as they
// are, and not dropped for an overlap). Times: plugin time -> file time
// through the resampler's delay.
struct BearingIn {
    std::string path, fmt;
    int m = 1;
    double rate = 0, start = 0, offset = 0, band = 0, delay_s = 0, uca_r = 0, lambda = NAN;
};
void talker_bearings(const BearingIn& in, const TestHost& host) {
    constexpr double GUARD_S = 0.04, MAX_PACKET_S = 0.25;
    struct Seg { std::string id, label; double a, b, freq, bw; bool packet; const std::vector<std::complex<double>>* cov = nullptr; };
    std::vector<Seg> segs;
    std::map<std::string, std::string> labels;
    for (const auto& s : host.spans_) {
        if (s.id == "?") continue;
        if (s.end - s.start > 2 * GUARD_S) segs.push_back({s.id, s.label, s.start + GUARD_S, s.end - GUARD_S, NAN, 0, false});
        labels[s.id] = s.label;
    }
    size_t too_long = 0, overlap = 0;
    std::vector<bool> bad(host.pk_recs_.size(), false);
    for (size_t i = 0; i < host.pk_recs_.size(); i++) {
        const auto& p = host.pk_recs_[i];
        if (p.end - p.start > MAX_PACKET_S && !p.m) { bad[i] = true; too_long++; continue; }
        for (size_t j = i + 1; j < host.pk_recs_.size() && j < i + 64; j++) {
            const auto& q = host.pk_recs_[j];
            const bool same = !std::isfinite(p.freq) || !std::isfinite(q.freq) || std::fabs(p.freq - q.freq) < 0.5 * std::max(p.bw, q.bw) + 1;
            if (q.id != p.id && q.start < p.end && p.start < q.end && same) {
                // a packet with its own covariance (by code) holds its transmitter alone
                if (!p.m) bad[i] = true;
                if (!q.m) bad[j] = true;
            }
        }
    }
    for (size_t i = 0; i < host.pk_recs_.size(); i++) {
        const auto& p = host.pk_recs_[i];
        if (bad[i]) { if (p.end - p.start <= MAX_PACKET_S) overlap++; continue; }
        auto it = host.packets_.find(p.id);
        segs.push_back({p.id, it != host.packets_.end() ? it->second.label : "", p.start, p.end, p.freq, p.bw, true,
                        p.m == in.m ? &p.cov : nullptr});
    }
    const int m = in.m;
    printf("talker bearings (UCA r = %.3f m, %d antennas, %.4f MHz):\n", in.uca_r, in.m, 3e8 / in.lambda / 1e6);
    if (too_long) printf("  %zu packets longer than %.2f s dropped (kraken_doa drops them too: report <= 0.24 s of a longer frame)\n", too_long, MAX_PACKET_S);
    if (overlap) printf("  %zu packets dropped: they overlap another talker's packet on the same channel\n", overlap);
    FILE* f = fopen(in.path.c_str(), "rb");
    if (!f) { printf("  cannot open %s\n", in.path.c_str()); return; }
    const size_t bps = in.fmt == "cf32" ? 8 : in.fmt == "cs16" ? 4 : 2;
    // samples [i0, i0 + n) of every antenna (zeros outside the file)
    std::vector<char> raw;
    auto read = [&](int64_t i0, size_t n, std::vector<std::vector<std::complex<double>>>& out) {
        // (the buffers reused: a fresh 1 MB per antenna and chunk is an mmap,
        // page faults and zeroing each time - 15 % of a 15 s scene's run)
        out.resize(static_cast<size_t>(m));
        for (auto& o : out) o.assign(n, std::complex<double>(0));
        const int64_t s0 = std::max<int64_t>(0, i0);
        if (static_cast<int64_t>(n) + i0 <= s0) return;
        const size_t skip = static_cast<size_t>(s0 - i0), want = n - skip;
        raw.resize(want * static_cast<size_t>(m) * bps);
        if (fseeko(f, static_cast<off_t>(s0) * m * static_cast<off_t>(bps), SEEK_SET) != 0) return;
        const size_t got = fread(raw.data(), static_cast<size_t>(m) * bps, want, f);
        for (size_t i = 0; i < got; i++)
            for (int k = 0; k < m; k++) {
                const char* d = raw.data() + (i * static_cast<size_t>(m) + static_cast<size_t>(k)) * bps;
                std::complex<double> v;
                if (bps == 8) { float iq[2]; memcpy(iq, d, 8); v = {iq[0], iq[1]}; }
                else if (bps == 4) { int16_t iq[2]; memcpy(iq, d, 4); v = {iq[0] / 32768.0, iq[1] / 32768.0}; }
                else { auto u = reinterpret_cast<const uint8_t*>(d); v = {(u[0] - 127.5) / 127.5, (u[1] - 127.5) / 127.5}; }
                out[static_cast<size_t>(k)][skip + i] = v;
            }
    };
    struct Acc { std::vector<std::complex<double>> sum; uint64_t n = 0, coded = 0; double secs = 0; bool packet = false; };
    std::map<std::string, Acc> acc;
    std::vector<std::vector<std::complex<double>>> x;
    for (const auto& s : segs) {
        if (s.cov) {   // the plugin's own covariance of the packet (by code)
            double tr = 0;
            for (int k = 0; k < m; k++) tr += (*s.cov)[static_cast<size_t>(k * m + k)].real();
            if (!(tr > 0)) continue;
            Acc& A = acc[s.id];
            if (A.sum.empty()) A.sum.assign(static_cast<size_t>(m * m), 0);
            for (size_t i = 0; i < s.cov->size(); i++) A.sum[i] += (*s.cov)[i] / tr;
            A.n++;
            A.coded++;
            A.secs += s.b - s.a;
            A.packet = true;
            if (!s.label.empty()) labels[s.id] = s.label;
            continue;
        }
        // the channel in the file: the plugin's 0 Hz is --offset; a packet's own
        // channel, else the plugin's band
        const double fc = in.offset + (std::isfinite(s.freq) ? s.freq : 0);
        const double bw = s.bw > 0 ? s.bw : in.band;
        const bool filt = bw < 0.98 * in.rate || fc != 0;
        std::vector<double> h;
        if (filt) {
            int nt = static_cast<int>(std::clamp(4.0 * in.rate / std::max(1.0, bw), 15.0, 2047.0)) | 1;
            h.resize(static_cast<size_t>(nt));
            const double fcut = 0.5 * std::min(bw, in.rate) / in.rate;
            double sum = 0;
            for (int i = 0; i < nt; i++) {
                const double t = i - (nt - 1) / 2.0;
                const double sinc = t == 0 ? 2 * fcut : std::sin(2 * M_PI * fcut * t) / (M_PI * t);
                h[static_cast<size_t>(i)] = sinc * (0.54 - 0.46 * std::cos(2 * M_PI * i / (nt - 1)));
                sum += h[static_cast<size_t>(i)];
            }
            for (auto& v : h) v /= sum;
        }
        const int nt = static_cast<int>(h.size());
        const int64_t ia = std::llround((s.a - in.delay_s + in.start) * in.rate), ib = std::llround((s.b - in.delay_s + in.start) * in.rate);
        if (ib - ia < 2) continue;
        std::vector<std::complex<double>> R(static_cast<size_t>(m * m));
        std::vector<std::complex<double>> y(static_cast<size_t>(m));
        constexpr int64_t CH = 65536;
        // the channel filter CENTRED on each sample, like kraken_doa's
        // packet_cov_raw (a causal one put a 1 kHz channel's packets 2 ms early
        // at 48 kHz - before the pulse of a short one)
        const int64_t half = nt ? (nt - 1) / 2 : 0;
        for (int64_t c0 = ia; c0 < ib; c0 += CH) {
            const int64_t c1 = std::min(ib, c0 + CH);
            const int64_t r0 = c0 - half;
            read(r0, static_cast<size_t>(c1 + half - r0), x);
            if (filt) {
                // mix the channel to 0 Hz (the phase reference is common to all antennas)
                for (int64_t i = r0; i < c1 + half; i++) {
                    const std::complex<double> rot = std::polar(1.0, -2 * M_PI * std::fmod(fc * static_cast<double>(i) / in.rate, 1.0));
                    for (int k = 0; k < m; k++) x[static_cast<size_t>(k)][static_cast<size_t>(i - r0)] *= rot;
                }
            }
            for (int64_t i = c0; i < c1; i++) {
                const size_t o = static_cast<size_t>(i - r0 + half);
                for (int k = 0; k < m; k++) {
                    if (!filt) { y[static_cast<size_t>(k)] = x[static_cast<size_t>(k)][o]; continue; }
                    std::complex<double> v = 0;
                    const auto& xk = x[static_cast<size_t>(k)];
                    for (int j = 0; j < nt; j++) v += h[static_cast<size_t>(j)] * xk[o - static_cast<size_t>(j)];
                    y[static_cast<size_t>(k)] = v;
                }
                for (int a = 0; a < m; a++)
                    for (int b = 0; b < m; b++) R[static_cast<size_t>(a * m + b)] += y[static_cast<size_t>(a)] * std::conj(y[static_cast<size_t>(b)]);
            }
        }
        double tr = 0;
        for (int k = 0; k < m; k++) tr += R[static_cast<size_t>(k * m + k)].real();
        if (!(tr > 0)) continue;
        Acc& A = acc[s.id];
        if (A.sum.empty()) A.sum.assign(static_cast<size_t>(m * m), 0);
        for (size_t i = 0; i < R.size(); i++) A.sum[i] += R[i] / tr;
        A.n++;
        A.secs += s.b - s.a;
        A.packet = s.packet;
        if (!s.label.empty()) labels[s.id] = s.label;
    }
    fclose(f);
    for (const auto& [id, A] : acc) {
        double conf = 0;
        const double b = uca_bearing(A.sum, m, in.uca_r, in.lambda, &conf);
        char by[40] = "";
        if (A.coded) snprintf(by, sizeof by, "  [%llu by code]", static_cast<unsigned long long>(A.coded));
        printf("  %-14s %-30s %5llu %s %8.2f s  bearing %6.1f deg  (peak/mean %.0f)%s\n", id.c_str(), labels[id].substr(0, 30).c_str(),
               static_cast<unsigned long long>(A.n), A.packet ? "packets" : "spans  ", A.secs, b, conf, by);
    }
}

// Array plugins: a multichannel recording through process_array()
int test_array(const std::string& path, std::string fmt, double rate, int channels, double offset, double start,
               double seconds, bool all, double uca_r, TestHost& host, kp::Decoder& dec, const kp::Info& info,
               bool quiet) {
    std::vector<kp::cf> x;
    std::string err;
    double frate = rate * std::max(1, channels);   // load_file counts complex values
    if (rate <= 0) {
        if (!read_sidecar_rate(path, &rate)) { fprintf(stderr, "error: sample rate unknown: pass --rate\n"); return 1; }
        frate = rate * std::max(1, channels);
    }
    const int m = channels;
    if (m < 1 || m > 8) { fprintf(stderr, "error: --channels 1..8\n"); return 1; }
    if (fmt.empty()) {
        const std::string ext = path.substr(path.rfind('.') == std::string::npos ? path.size() : path.rfind('.') + 1);
        fmt = ext == "cu8" || ext == "u8" ? "cu8" : ext == "cs16" || ext == "s16" ? "cs16" : ext == "wav" ? "wav" : "cf32";
    }
    // read block by block (a multichannel recording can be larger than the
    // RAM: 20 s of 5 antennas at 2.4 MHz is 1.9 GB): cf32 / cs16 / cu8 here, a
    // WAV through IqFile (its complex values are the antennas one after the other)
    const bool stream = fmt != "wav";
    const size_t bps = fmt == "cf32" ? 8 : fmt == "cs16" ? 4 : 2;
    std::ifstream sf;
    IqFile wav;
    size_t total = 0;
    if (stream) {
        sf.open(path, std::ios::binary | std::ios::ate);
        if (!sf) { fprintf(stderr, "error: cannot open %s\n", path.c_str()); return 1; }
        total = static_cast<size_t>(sf.tellg()) / bps / static_cast<size_t>(m);
    } else {
        if (!wav.open(path, fmt, &frate, 0, 0, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        total = wav.count() / static_cast<size_t>(m);
    }
    std::vector<char> raw;
    // samples p .. p + n of every antenna (sample-major, antenna-minor) -> x[0 ..]
    auto fetch = [&](size_t p, size_t n) -> const kp::cf* {
        const size_t cnt = n * static_cast<size_t>(m);
        if (!stream) {
            x.resize(cnt);
            wav.read(p * static_cast<size_t>(m), cnt, x.data());
            return x.data();
        }
        raw.resize(cnt * bps);
        sf.seekg(static_cast<std::streamoff>(p * static_cast<size_t>(m) * bps));
        sf.read(raw.data(), static_cast<std::streamsize>(raw.size()));
        x.resize(cnt);
        for (size_t i = 0; i < cnt; i++) {
            if (fmt == "cf32") {
                float iq[2];
                memcpy(iq, raw.data() + i * 8, 8);
                x[i] = {iq[0], iq[1]};
            } else if (fmt == "cs16") {
                int16_t iq[2];
                memcpy(iq, raw.data() + i * 4, 4);
                x[i] = {iq[0] / 32768.0f, iq[1] / 32768.0f};
            } else {
                auto u = reinterpret_cast<const uint8_t*>(raw.data() + i * 2);
                x[i] = {(u[0] - 127.5f) / 127.5f, (u[1] - 127.5f) / 127.5f};
            }
        }
        return x.data();
    };
    const size_t s0 = std::min(total, static_cast<size_t>(std::max(0.0, start) * rate));
    const size_t s1 = seconds > 0 ? std::min(total, s0 + static_cast<size_t>(seconds * rate)) : total;
    if (!quiet)
        printf("%s %s: %.2f s of %s, %d antennas at %.0f Hz%s%s\n", info.name, info.version, (s1 - s0) / rate, path.c_str(), m,
               rate, all ? "" : (" (bursts of " + std::to_string(info.array_burst_s).substr(0, 5) + " s every " +
                                 std::to_string(info.array_period_s).substr(0, 5) + " s)").c_str(),
               std::isfinite(host.rf_hz_) ? (", RF " + std::to_string(host.rf_hz_ / 1e6).substr(0, 11) + " MHz").c_str() : "");
    const size_t blk = std::max<size_t>(1, static_cast<size_t>(rate / 100));
    std::vector<std::vector<kp::cf>> ch(static_cast<size_t>(m), std::vector<kp::cf>(blk));
    const double burst = all ? 0 : std::max(0.0, info.array_burst_s), period = std::max(burst, info.array_period_s);
    bool gap = true;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t p = s0; p < s1; p += blk) {
        const size_t n = std::min(blk, s1 - p);
        const double t = static_cast<double>(p) / rate;
        if (!all && burst < period && std::fmod(t, period) >= burst) {   // between bursts
            gap = true;
            continue;
        }
        const kp::cf* xs = fetch(p, n);
        for (size_t i = 0; i < n; i++) {
            const kp::cf rot = offset != 0 ? std::polar(1.0f, static_cast<float>(std::remainder(-2 * M_PI * offset * static_cast<double>(p + i) / rate, 2 * M_PI)))
                                           : kp::cf(1, 0);
            for (int k = 0; k < m; k++) ch[static_cast<size_t>(k)][i] = xs[i * static_cast<size_t>(m) + static_cast<size_t>(k)] * rot;
        }
        kp::ArrayBlock b;
        b.m = m;
        b.n = n;
        b.rate = rate;
        b.t0 = t;
        b.gap = gap;
        gap = false;
        for (int k = 0; k < m; k++) b.ch[k] = ch[static_cast<size_t>(k)].data();
        host.array_t_ = t;
        dec.process_array(b);
    }
    host.array_t_ = static_cast<double>(s1) / rate;
    double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("\n=== %s summary ===\n", info.name);
    printf("input: %.2f s, process time %.2f s (%.1f%% of real time)\n", (s1 - s0) / rate, cpu, 100.0 * cpu / std::max(1e-9, (s1 - s0) / rate));
    printf("events: %llu\n", static_cast<unsigned long long>(host.events_));
    if (!host.facts_.empty()) {
        printf("facts:\n");
        for (auto& f : host.facts_) printf("  %-22s %s\n", f.first.c_str(), f.second.c_str());
    }
    const double lambda = std::isfinite(host.rf_hz_) && host.rf_hz_ > 0 ? 3e8 / host.rf_hz_ : NAN;
    if (host.spatial_removed_) printf("spatial removed: %llu\n", static_cast<unsigned long long>(host.spatial_removed_));
    if (!host.spatial_.empty()) {
        printf("spatial: %zu transmitters\n", host.spatial_.size());
        std::vector<std::pair<std::string, TestHost::Sp>> v(host.spatial_.begin(), host.spatial_.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.delay < b.second.delay; });
        for (const auto& [id, k] : v) {
            char brg[64] = "";
            if (uca_r > 0 && std::isfinite(lambda)) {
                double conf = 0;
                const double b = uca_bearing(k.sum, k.m, uca_r, lambda, &conf);
                snprintf(brg, sizeof brg, "  bearing %6.1f deg", b);
            }
            printf("  %-8s %-34s %5llu reports  delay %8.3f us  %6.1f dB  snr %5.1f dB%s%s\n", id.c_str(), k.label.c_str(),
                   static_cast<unsigned long long>(k.n), k.delay, k.power, k.snr, k.ref ? " ref" : "    ", brg);
        }
    }
    host.print_plots();
    host.print_choices();
    host.print_table(1000);
    host.print_trunk();
    return 0;
}

#ifdef _WIN32
// the summary is UTF-8 (deg, us, warning signs): a Windows console shows it
// in its OEM code page even with the exe's UTF-8 manifest - switched to
// UTF-8 for the run and back at exit (the console outlives the plugin).
// A pipe, a file or the MSYS2 terminal take the bytes as they are.
void console_utf8() {
    DWORD mode = 0;
    if (!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) return;
    static const UINT old_cp = GetConsoleOutputCP();
    if (old_cp == CP_UTF8 || !SetConsoleOutputCP(CP_UTF8)) return;
    atexit([] { SetConsoleOutputCP(old_cp); });
}
#endif

int test_file(int argc, char** argv) {
#ifdef _WIN32
    console_utf8();
#endif
    std::string path, fmt, audio_path, plots_path;
    double rate = 0, offset = 0, start = 0, seconds = 0, rf = NAN, uca_r = 0;
    int channels = 0, pick = -1;   // a recording of every antenna: how many / decode only this one
    bool all = false;              // array plugins: every sample, not bursts
    bool quiet = false, verbose = false, have_station = false, raw = false;
    double st_lat = 0, st_lon = 0;
    std::vector<std::pair<std::string, std::string>> opts;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "--file") path = next();
        else if (a == "--rate") rate = atof(next().c_str());
        else if (a == "--format") fmt = next();
        else if (a == "--offset") offset = atof(next().c_str());
        else if (a == "--rf") rf = atof(next().c_str());
        else if (a == "--start") start = atof(next().c_str());
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--audio") audio_path = next();
        else if (a == "--plots") plots_path = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--raw") raw = true;
        else if (a == "--channels") channels = atoi(next().c_str());
        else if (a == "--channel") pick = atoi(next().c_str());
        else if (a == "--all") all = true;
        else if (a == "--uca") uca_r = atof(next().c_str());
        else if (a == "--station") {
            std::string v = next();
            if (sscanf(v.c_str(), "%lf,%lf", &st_lat, &st_lon) != 2) { fprintf(stderr, "--station needs LAT,LON\n"); return 2; }
            have_station = true;
        }
        else if (a == "--opt") {
            std::string kv = next();
            size_t eq = kv.find('=');
            opts.emplace_back(kv.substr(0, eq), eq == std::string::npos ? "" : kv.substr(eq + 1));
        } else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }
    kp::Info info = kp_plugin_info();
    if (info.array) {   // a multichannel recording through process_array()
        double ch = channels;
        if (ch <= 0 && !read_sidecar(path, "channels", &ch)) ch = 1;
        TestHost host(rate > 0 ? rate : 1, quiet);
        host.verbose_ = verbose;
        host.have_station_ = have_station;
        host.st_lat_ = st_lat;
        host.st_lon_ = st_lon;
        host.plots_path_ = plots_path;
        if (!std::isfinite(rf) && !read_sidecar(path, "rf_hz", &rf)) rf = NAN;
        if (std::isfinite(rf) && rf > 0) host.rf_hz_ = rf + offset;
        auto dec = kp_plugin_create(host);
        for (const auto& o : info.options) dec->option(o.key, o.def);
        if (verbose) dec->option("verbose", "1");
        for (auto& o : opts) dec->option(o.first, o.second);
        return test_array(path, fmt, rate, static_cast<int>(ch), offset, start, seconds, all, uca_r, host, *dec, info, quiet);
    }
    // The recording is read as a stream, block by block (memory independent
    // of its length): one antenna through IqFile, one antenna of a
    // multichannel recording or all of them through McReader. Only a
    // multichannel recording from a pipe, decoded one antenna, is gathered
    // in x first (its length must be known before the first block).
    std::vector<kp::cf> x;
    IqFile iq;
    size_t nin = 0;   // samples to decode (one antenna)
    std::string err;
    if (channels <= 0) {
        double c = 0;
        channels = read_sidecar(path, "channels", &c) ? static_cast<int>(c) : 1;
    }
    if (channels > 8) {
        fprintf(stderr, "error: at most 8 channels\n");
        return 1;
    }
    McReader mcr;
    // every antenna through process_multi (Info::multichannel); other plugins
    // decode antenna 0 (or --channel K) like a single-channel VFO
    if (channels > 1 && pick < 0 && !info.multichannel) pick = 0;
    const bool mc_all = channels > 1 && pick < 0;
    if (channels > 1) {
        if (rate <= 0 && !read_sidecar_rate(path, &rate)) {
            fprintf(stderr, "error: sample rate unknown: pass --rate\n");
            return 1;
        }
        if (!mcr.open(path, fmt, channels, rate, start, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        if (pick >= channels) {
            fprintf(stderr, "error: --channel %d of %d\n", pick, channels);
            return 1;
        }
        if (!mc_all) {   // one antenna, as a single-channel decoder gets it
            const size_t want = seconds > 0 ? static_cast<size_t>(seconds * rate) : SIZE_MAX;
            if (mcr.regular) {
                nin = std::min(want, mcr.samples());
            } else {   // a pipe: its length only shows at its end
                std::vector<std::vector<kp::cf>> b;
                for (size_t got; x.size() < want && (got = mcr.read(std::min<size_t>(1 << 16, want - x.size()), b, pick)) > 0;)
                    x.insert(x.end(), b[static_cast<size_t>(pick)].begin(), b[static_cast<size_t>(pick)].end());
                nin = x.size();
            }
        }
    } else if (!iq.open(path, fmt, &rate, start, seconds, err)) {
        fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    } else {
        nin = iq.count();
    }
    // the next n (<= one block) samples of the antenna decoded -> dst; returns how many came
    size_t nread = 0;
    std::vector<std::vector<kp::cf>> mb;
    auto pull = [&](kp::cf* dst, size_t n) -> size_t {
        size_t got = 0;
        if (channels <= 1) {
            got = iq.read(nread, n, dst);
        } else if (!mcr.regular) {
            got = std::min(n, x.size() - nread);
            std::copy(x.begin() + static_cast<std::ptrdiff_t>(nread), x.begin() + static_cast<std::ptrdiff_t>(nread + got), dst);
        } else {
            got = mcr.read(n, mb, pick);
            std::copy(mb[static_cast<size_t>(pick)].begin(), mb[static_cast<size_t>(pick)].end(), dst);
        }
        nread += got;
        return got;
    };
    // the plugin's input rate: its own, or the file's (Info::sample_rate 0 =
    // the VFO's rate: no resampling)
    const double prate = info.sample_rate > 0 ? info.sample_rate : rate;
    if (mc_all && (prate != rate || offset != 0)) {
        fprintf(stderr, "error: every antenna at once needs the plugin's rate (%.0f Hz) and no --offset\n", prate);
        return 1;
    }
    TestHost host(prate, quiet);
    host.verbose_ = verbose;
    host.want_audio_ = !audio_path.empty();
    host.have_station_ = have_station;
    host.raw_ = raw;
    host.st_lat_ = st_lat;
    host.st_lon_ = st_lon;
    host.plots_path_ = plots_path;
    // RF of the file's centre (--rf, else the sidecar's "rf_hz"): the
    // decoder's 0 Hz is --offset away from it
    if (!std::isfinite(rf) && !read_sidecar(path, "rf_hz", &rf)) rf = NAN;
    if (std::isfinite(rf) && rf > 0) host.rf_hz_ = rf + offset;
    host.keep_pk_ = uca_r > 0 && channels > 1;
    auto dec = kp_plugin_create(host);
    for (const auto& o : info.options) dec->option(o.key, o.def);
    if (verbose) dec->option("verbose", "1");
    for (auto& o : opts) dec->option(o.first, o.second);

    if (!quiet && mc_all)
        printf("%s %s: %d antennas of %s at %.0f Hz (process_multi)\n", info.name, info.version, channels, path.c_str(), rate);
    else if (!quiet)
        printf("%s %s: %.2f s of %s at %.0f Hz -> %.0f Hz%s%s\n", info.name, info.version,
               nin / rate, path.c_str(), rate, prate,
               offset != 0 ? (" (offset " + std::to_string(static_cast<long long>(offset)) + " Hz)").c_str() : "",
               std::isfinite(host.rf_hz_) ? (", RF " + std::to_string(host.rf_hz_ / 1e6).substr(0, 11) + " MHz").c_str() : "");
    // mix + resample to the plugin's rate, in 10 ms blocks like live
    kp::Nco nco(static_cast<float>(offset), static_cast<float>(rate));
    msresamp_crcf rs = msresamp_crcf_create(static_cast<float>(prate / rate), 60.0f);
    // the plugin's samples lag the file by the resampler's delay (per-talker bearings)
    const double rs_delay_s = prate == rate && offset == 0 ? 0.0 : msresamp_crcf_get_delay(rs) / prate;
    const size_t blk = std::max<size_t>(1, static_cast<size_t>(rate / 100));
    std::vector<kp::cf> in(blk), outb(static_cast<size_t>(blk * prate / rate) + 64);
    if (!audio_path.empty()) {   // written as it comes; the header again at the end
        host.audio_f_ = fopen(audio_path.c_str(), "wb");
        if (host.audio_f_) wav8k_header(host.audio_f_, 0);
    }
    auto t0 = std::chrono::steady_clock::now();
    if (mc_all) {   // every antenna, 10 ms blocks like live
        std::vector<std::vector<kp::cf>> b;
        std::vector<const kp::cf*> ptrs(static_cast<size_t>(channels));
        const size_t want = seconds > 0 ? static_cast<size_t>(seconds * rate) : SIZE_MAX;
        size_t done = 0;
        for (size_t got; done < want && (got = mcr.read(std::min(blk, want - done), b)) > 0; done += got) {
            for (int k = 0; k < channels; k++) ptrs[static_cast<size_t>(k)] = b[static_cast<size_t>(k)].data();
            dec->process_multi(ptrs.data(), channels, got);
            host.samples_ += got;
        }
    }
    // the one antenna, read block by block: the NCO and the resampler keep
    // their state from block to block, so this is the whole recording in one pass
    for (size_t p = 0; p < nin; p += blk) {
        const size_t want = std::min(blk, nin - p), n = pull(in.data(), want);
        if (!n) break;   // (the file got shorter)
        for (size_t i = 0; i < n; i++) in[i] = nco.mix(in[i]);
        unsigned int ny = 0;
        if (prate == rate && offset == 0) {
            std::copy(in.begin(), in.begin() + n, outb.begin());
            ny = static_cast<unsigned int>(n);
        } else {
            msresamp_crcf_execute(rs, in.data(), static_cast<unsigned int>(n), outb.data(), &ny);
        }
        if (ny) {
            dec->process(outb.data(), ny);
            host.samples_ += ny;
        }
        if (n < want) break;
    }
    msresamp_crcf_destroy(rs);
    double cpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("\n=== %s summary ===\n", info.name);
    printf("input: %.2f s, decode time %.2f s (%.1f%% of real time)\n", host.time(), cpu,
           host.time() > 0 ? 100.0 * cpu / host.time() : 0.0);
    printf("VALID FRAMES: %llu", static_cast<unsigned long long>(host.valid_));
    if (host.first_valid_ >= 0) printf(" (first at %.3f s)", host.first_valid_);
    if (host.valid_spans_)
        printf("\nvalid frames with their samples: %llu, covering %.2f s", static_cast<unsigned long long>(host.valid_spans_),
               host.valid_span_s_);
    printf("\nevents: %llu\n", static_cast<unsigned long long>(host.events_));
    if (host.nferr_) printf("last frequency error: %.0f Hz\n", host.ferr_);
    if (!host.facts_.empty()) {
        printf("facts:\n");
        for (auto& f : host.facts_) printf("  %-22s %s\n", f.first.c_str(), f.second.c_str());
    }
    if (host.raws_) printf("raw frames: %llu\n", static_cast<unsigned long long>(host.raws_));
    if (!host.spans_.empty()) {
        printf("talkers: %zu transmissions\n", host.spans_.size());
        for (const auto& s : host.spans_)
            printf("  %-10s %-32s %8.3f .. %8.3f s%s%s\n", s.id.c_str(), s.label.c_str(), s.start, s.end,
                   s.channel ? (" ch " + std::to_string(s.channel)).c_str() : "", s.open ? " (open)" : "");
    }
    if (!host.packets_.empty()) {
        printf("talker packets: %llu from %zu talkers\n", static_cast<unsigned long long>(host.packets_total_),
               host.packets_.size());
        std::vector<std::pair<std::string, TestHost::Pk>> v(host.packets_.begin(), host.packets_.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
        for (size_t i = 0; i < v.size() && i < 40; i++) {
            const TestHost::Pk& k = v[i].second;
            char pos[128] = "";
            if (k.npos)
                snprintf(pos, sizeof pos, "  %llu with position, last %.5f, %.5f, %.0f m",
                         static_cast<unsigned long long>(k.npos), k.lat, k.lon, k.alt);
            printf("  %-10s %-24s %6llu packets  %8.3f .. %8.3f s  mean %.0f us%s\n", v[i].first.c_str(),
                   k.label.c_str(), static_cast<unsigned long long>(k.n), k.first, k.last,
                   k.len_us / static_cast<double>(k.n), pos);
        }
    }
    host.print_trunk();
    host.print_choices();
    host.print_table(30);
    host.print_plots();
    if (host.map_updates_ || !host.map_.empty()) {
        printf("map: %llu updates, %zu points\n", static_cast<unsigned long long>(host.map_updates_), host.map_.size());
        for (const auto& kv : host.map_) {
            const kp::MapPoint& m = kv.second;
            printf("  %-10s %-10s %10.5f %11.5f", m.id.c_str(), m.label.c_str(), m.lat, m.lon);
            if (std::isfinite(m.altitude_m)) printf("  %6.0f m", m.altitude_m);
            if (std::isfinite(m.speed_kmh)) printf("  %5.0f km/h", m.speed_kmh);
            if (std::isfinite(m.heading)) printf("  %5.1f deg", m.heading);
            printf("  [%s]\n", m.kind.c_str());
        }
    }
    if (!audio_path.empty()) {
        if (host.audio_f_) {
            fseeko(host.audio_f_, 0, SEEK_SET);
            wav8k_header(host.audio_f_, host.audio_n_);
            fclose(host.audio_f_);
            host.audio_f_ = nullptr;
        }
        printf("audio: %.2f s -> %s\n", host.audio_n_ / 8000.0, audio_path.c_str());
    }
    if (uca_r > 0 && channels > 1) {
        if (!std::isfinite(host.rf_hz_) || !(host.rf_hz_ > 0)) {
            printf("talker bearings: need the RF (--rf or the sidecar's rf_hz)\n");
        } else {
            BearingIn b;
            b.path = path;
            b.fmt = mcr.fmt;
            b.m = channels;
            b.rate = rate;
            b.start = start;
            b.offset = offset;
            b.band = std::min(prate, rate);
            b.delay_s = mc_all ? 0.0 : rs_delay_s;
            b.uca_r = uca_r;
            b.lambda = 3e8 / host.rf_hz_;
            talker_bearings(b, host);
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // the protocol is bytes: binary stdin / stdout / stderr (in text mode a
    // 0x1A in a sample block reads as end of file and every 0x0A of a reply
    // goes out as 0D 0A). win/win_init.cpp does it before main already -
    // this keeps the protocol right whatever the exe was linked with
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
    _setmode(2, _O_BINARY);
#endif
    if (argc >= 2 && !strcmp(argv[1], "--info")) return print_info();
    if (argc >= 2 && !strcmp(argv[1], "--serve")) return serve(argc, argv);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--file")) return test_file(argc, argv);
    kp::Info info = kp_plugin_info();
    fprintf(stderr,
            "%s (%s) - KrakenSDR decoder plugin\n"
            "usage: %s --info | --serve | --file REC [--rate HZ] [--format cf32|cu8|cs16|wav]\n"
            "          [--offset HZ] [--start S] [--seconds S] [--verbose]\n"
            "          [--opt key=value] [--audio out.wav] [--plots out.jsonl] [--station LAT,LON] [--rf HZ] [--raw] [--quiet]\n"
            "          [--channels M (every antenna, interleaved) [--channel K (only that one)] [--uca RADIUS_M]]\n"
            "          array plugins: [--channels M] [--all] [--uca RADIUS_M]\n",
            info.name, info.description, argv[0]);
    return 2;
}
