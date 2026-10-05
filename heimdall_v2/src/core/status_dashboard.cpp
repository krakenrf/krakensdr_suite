#include "core/status_dashboard.hpp"

#include "core/types.hpp"
#include "core/config.hpp"
#include "core/log_sanitize.hpp"
#include "sdr/sdr_device.hpp"
#include "sdr/sdr_pipeline.hpp"
#include "sdr/downconverter.hpp"
#include "net/tcp_data_server.hpp"
#include "net/tcp_control_server.hpp"
#include "net/rtl_tcp_server.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

// These globals live in main.cpp with external linkage but are passed by
// reference elsewhere rather than declared extern in a header; the dashboard
// reads them directly, so declare them here.
extern CorrelationResult correlation_result;
extern std::unique_ptr<TcpDataServer> tcp_data_server;
extern std::unique_ptr<TcpControlServer> tcp_control_server;
extern std::unique_ptr<RtlTcpServer> rtl_tcp_server;

using namespace std;
using namespace std::chrono;

static constexpr int REFRESH_MS = 250;

// ---------------------------------------------------------------------------
// ANSI helpers
// ---------------------------------------------------------------------------
namespace col {
    constexpr const char* RST  = "\033[0m";
    constexpr const char* BOLD = "\033[1m";
    constexpr const char* DIM  = "\033[2m";
    constexpr const char* RED  = "\033[31m";
    constexpr const char* GRN  = "\033[32m";
    constexpr const char* YEL  = "\033[33m";
    constexpr const char* CYN  = "\033[36m";
}

// ---------------------------------------------------------------------------
// Captured-log ring: cout/cerr is reduced to persistent last-event / last-warn
// fields so nothing scrolls; a small history is kept for the shutdown dump.
// ---------------------------------------------------------------------------
namespace {

deque<string> g_log;
string        g_last_event;
string        g_last_warn = "none";
uint64_t      g_log_total = 0;
uint64_t      g_warn_total = 0;
mutex         g_log_mtx;
constexpr size_t LOG_KEEP = 200;

bool contains_ci(const string& hay, const char* needle) {
    string h;
    h.reserve(hay.size());
    for (char ch : hay) h += static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return h.find(needle) != string::npos;
}

void push_line(const string& raw) {
    // The TUI's own capture bypasses main's SanitizingBuf - clean it here.
    const string line = sanitize_for_terminal(raw);
    bool warn = contains_ci(line, "error") || contains_ci(line, "warn") ||
                contains_ci(line, "fail") || contains_ci(line, "lost");
    lock_guard<mutex> lk(g_log_mtx);
    g_log.push_back(line);
    while (g_log.size() > LOG_KEEP) g_log.pop_front();
    g_last_event = line;
    g_log_total++;
    if (warn) { g_last_warn = line; g_warn_total++; }
}

struct LogView { string last_event; string last_warn; uint64_t total; uint64_t warns; };
LogView log_view() {
    lock_guard<mutex> lk(g_log_mtx);
    return {g_last_event, g_last_warn, g_log_total, g_warn_total};
}

vector<string> recent_log(size_t n) {  // shutdown dump only
    lock_guard<mutex> lk(g_log_mtx);
    vector<string> out;
    size_t start = g_log.size() > n ? g_log.size() - n : 0;
    for (size_t i = start; i < g_log.size(); i++) out.push_back(g_log[i]);
    return out;
}

// streambuf that swallows cout/cerr and files complete lines into the ring.
// Per-thread partial buffers avoid interleaving characters from concurrent
// writers (the server logs from many threads).
class CaptureBuf : public std::streambuf {
protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        put_char(static_cast<char>(c));
        return c;
    }
    streamsize xsputn(const char* s, streamsize n) override {
        for (streamsize i = 0; i < n; i++) put_char(s[i]);
        return n;
    }
private:
    static void put_char(char ch) {
        static thread_local string partial;
        if (ch == '\n') { push_line(partial); partial.clear(); }
        else if (ch != '\r') {
            partial += ch;
            if (partial.size() > 4096) { push_line(partial); partial.clear(); }
        }
    }
};

CaptureBuf      g_capbuf;
std::streambuf* g_orig_cout = nullptr;
std::streambuf* g_orig_cerr = nullptr;
bool            g_active = false;
bool            g_ready = false;
steady_clock::time_point g_start;

// ---------------------------------------------------------------------------
// Terminal I/O
// ---------------------------------------------------------------------------
void term_write(const string& s) {
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0) {
        ssize_t w = ::write(STDOUT_FILENO, p, left);
        if (w <= 0) break;
        p += w;
        left -= static_cast<size_t>(w);
    }
}

void term_size(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    } else {
        rows = 24;
        cols = 80;
    }
}

// Clip a possibly-colored string to `cols` visible columns, treating ANSI CSI
// sequences as zero width so color codes are never counted or cut mid-sequence.
string clip(const string& s, int cols) {
    string out;
    int width = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\033') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                j++;
                while (j < s.size() && !((s[j] >= 'A' && s[j] <= 'Z') ||
                                         (s[j] >= 'a' && s[j] <= 'z')))
                    j++;
                if (j < s.size()) j++;
            }
            out.append(s, i, j - i);
            i = j;
        } else {
            if (width >= cols) break;
            out += s[i++];
            width++;
        }
    }
    out += col::RST;
    return out;
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
string label(const char* s) {
    string t = s;
    if (t.size() < 12) t.append(12 - t.size(), ' ');
    return string(col::DIM) + t + col::RST + " ";
}

string dot(bool ok) { return string(ok ? col::GRN : col::RED) + "\xe2\x97\x8f" + col::RST; }

string onoff(bool on) {
    return on ? string(col::GRN) + "on" + col::RST : string(col::DIM) + "off" + col::RST;
}

string f1(double v) { char b[32]; snprintf(b, sizeof(b), "%.1f", v); return b; }
string f2(double v) { char b[32]; snprintf(b, sizeof(b), "%.2f", v); return b; }
string f3(double v) { char b[32]; snprintf(b, sizeof(b), "%.3f", v); return b; }

// Browser URL for the web UI (HTTP). Uses the machine hostname so the link
// works from another device on the network; computed once.
string web_url() {
    static const string url = [] {
        char h[256] = {0};
        string host = (gethostname(h, sizeof(h) - 1) == 0 && h[0]) ? string(h) : string("localhost");
        return "http://" + host + ":" + to_string(WEB_PORT);
    }();
    return url;
}

const char* phase_name(PhaseCompensatorState s) {
    switch (s) {
        case PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION: return "WAIT-LAG";
        case PhaseCompensatorState::MEASURING_INITIAL_PHASE:    return "MEASURING";
        case PhaseCompensatorState::APPLYING_COMPENSATION:      return "APPLYING";
        case PhaseCompensatorState::VERIFYING_CONVERGENCE:      return "VERIFYING";
        case PhaseCompensatorState::CONVERGED:                  return "CONVERGED";
        case PhaseCompensatorState::WAITING_FOR_STABILITY:      return "COOLDOWN";
        case PhaseCompensatorState::MEASURING_PER_BIN:          return "PER-BIN";
    }
    return "?";
}

const char* lag_name(LagCompensatorState s) {
    switch (s) {
        case LagCompensatorState::MEASURING: return "MEASURING";
        case LagCompensatorState::SERVOING:  return "SERVOING";
        case LagCompensatorState::CONVERGED: return "CONVERGED";
    }
    return "?";
}

long uptime_seconds() {
    return duration_cast<seconds>(steady_clock::now() - g_start).count();
}

// ---------------------------------------------------------------------------
// Hardware stats (/proc, /sys) — Heimdall has no system_stats module of its own
// ---------------------------------------------------------------------------
struct HwStats { float cpu = 0, temp = -1, ram_used = 0, ram_total = 0; };

float read_cpu_temp() {
    ifstream f("/sys/class/thermal/thermal_zone0/temp");
    int mdeg;
    if (f && (f >> mdeg)) return mdeg / 1000.0f;
    return -1.0f;
}
float read_cpu_usage() {
    static long long pt = 0, pi = 0;
    ifstream f("/proc/stat");
    string cpu;
    long long u, n, s, i, io, ir, si, st;
    if (!(f >> cpu >> u >> n >> s >> i >> io >> ir >> si >> st)) return 0.0f;
    long long total = u + n + s + i + io + ir + si + st, idle = i + io;
    long long dt = total - pt, di = idle - pi;
    pt = total; pi = idle;
    if (dt <= 0) return 0.0f;
    return 100.0f * (1.0f - static_cast<float>(di) / static_cast<float>(dt));
}
void read_ram(float& used, float& total) {
    ifstream f("/proc/meminfo");
    string k; long long v; string unit;
    long long mt = 0, ma = 0;
    while (f >> k >> v >> unit) {
        if (k == "MemTotal:") mt = v;
        else if (k == "MemAvailable:") { ma = v; break; }
    }
    total = mt / 1024.0f;
    used = (mt - ma) / 1024.0f;
}
HwStats get_hw() {
    HwStats h;
    h.temp = read_cpu_temp();
    h.cpu = read_cpu_usage();
    read_ram(h.ram_used, h.ram_total);
    return h;
}

// ---------------------------------------------------------------------------
// Frame composition (shared by loading screen and dashboard)
// ---------------------------------------------------------------------------
void emit_frame(const vector<string>& L, const vector<string>& bottom) {
    int rows, cols;
    term_size(rows, cols);
    int nb = (int)bottom.size();

    vector<string> out;
    if ((int)L.size() + nb <= rows) {
        out = L;
        for (const auto& b : bottom) out.push_back(b);
    } else {
        int keep_top = rows - nb;
        if (keep_top < 0) keep_top = 0;
        for (int i = 0; i < keep_top && i < (int)L.size(); i++) out.push_back(L[i]);
        for (const auto& b : bottom) out.push_back(b);
    }

    string frame_out = "\033[H";
    int n = (int)out.size() < rows ? (int)out.size() : rows;
    for (int r = 0; r < n; r++) {
        frame_out += clip(out[r], cols);
        frame_out += "\033[K";
        if (r + 1 < n) frame_out += "\r\n";
    }
    frame_out += "\033[0m\033[J";
    term_write(frame_out);
}

// ---------------------------------------------------------------------------
// Loading screen: live tail of captured startup messages (what is loading)
// ---------------------------------------------------------------------------
void render_loading() {
    static uint32_t tick = 0;
    const char* SPIN = "|/-\\";
    char sp = SPIN[(tick++) % 4];

    vector<string> L;
    auto add = [&](string s) { L.push_back(std::move(s)); };

    {
        ostringstream o;
        o << col::BOLD << col::CYN << "Heimdall Server" << col::RST
          << col::DIM << "   starting up " << sp << "   " << uptime_seconds() << "s" << col::RST;
        add(o.str());
    }
    add("");
    add(label("STATUS") + string(col::YEL) +
        "initializing receiver \xe2\x80\x94 opening SDR devices & starting servers\xe2\x80\xa6" + col::RST);
    add(label("WEB UI") + string(col::BOLD) + col::CYN + web_url() + col::RST);
    add("");

    int rows, cols;
    term_size(rows, cols);
    {
        string title = "-- startup log ";
        size_t pad = (cols > (int)title.size()) ? (size_t)cols - title.size() : 0;
        add(string(col::DIM) + title + string(pad, '-') + col::RST);
    }
    int avail = rows - (int)L.size();
    if (avail < 3)  avail = 3;
    if (avail > 24) avail = 24;
    for (const auto& ln : recent_log((size_t)avail))
        add("  " + ln);

    emit_frame(L, {});
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace StatusDashboard {

bool active() { return g_active; }

void mark_ready() { g_ready = true; }

void begin() {
    if (g_active) return;
    if (!isatty(STDOUT_FILENO)) return;
    const char* off = getenv("HEIMDALL_NO_TUI");
    if (off && off[0] && strcmp(off, "0") != 0) return;

    g_start = steady_clock::now();
    g_orig_cout = std::cout.rdbuf(&g_capbuf);
    g_orig_cerr = std::cerr.rdbuf(&g_capbuf);

    term_write("\033[?1049h\033[2J\033[H\033[?25l");

    // Immediate centered splash so the screen is never blank before the first
    // loading frame is gathered.
    {
        int rows, cols;
        term_size(rows, cols);
        const string title = "Heimdall Server";
        const string msg   = "Starting up\xe2\x80\xa6";
        const int title_w = 15, msg_w = 12;
        int r  = rows / 2;
        int c1 = (cols - title_w) / 2; if (c1 < 0) c1 = 0;
        int c2 = (cols - msg_w)   / 2; if (c2 < 0) c2 = 0;
        char pos[32];
        string splash;
        snprintf(pos, sizeof(pos), "\033[%d;%dH", r, c1 + 1);
        splash += pos; splash += col::BOLD; splash += col::CYN; splash += title; splash += col::RST;
        snprintf(pos, sizeof(pos), "\033[%d;%dH", r + 1, c2 + 1);
        splash += pos; splash += col::DIM; splash += msg; splash += col::RST;
        term_write(splash);
    }

    g_active = true;
}

void render() {
    if (!g_active) return;

    // Show the startup/loading view until main() marks the pipeline ready.
    if (!g_ready) { render_loading(); return; }

    static uint64_t frame = 0;
    frame++;

    int nelem = active_num_elements.load();
    if (nelem < 1) nelem = 1;
    int ndev = (int)devices.size();
    int nshow = nelem < ndev ? nelem : ndev;

    // --- snapshot correlation state under its mutex (authoritative, thread-safe) ---
    PhaseCompensatorState phase_state = PhaseCompensatorState::WAITING_FOR_LAG_COMPLETION;
    map<int, float> lags, phases, amps;
    map<int, LagCompensatorState> lstates;
    bool data_ready = false;
    uint64_t data_seq = 0;
    {
        lock_guard<mutex> lk(correlation_result.data_mutex);
        phase_state = correlation_result.phase_state;
        lags   = correlation_result.lags;
        phases = correlation_result.phases;
        amps   = correlation_result.amplitudes;
        lstates = correlation_result.channel_states;
        data_ready = correlation_result.data_ready;
        data_seq = correlation_result.data_sequence;
    }

    // --- derived correlation-frame rate ---
    static uint64_t prev_seq = 0;
    static steady_clock::time_point prev_t = g_start;
    steady_clock::time_point now = steady_clock::now();
    static double fps = 0;
    double dt = duration<double>(now - prev_t).count();
    if (dt > 0.4) {
        fps = (data_seq >= prev_seq) ? (data_seq - prev_seq) / dt : 0.0;
        prev_seq = data_seq;
        prev_t = now;
    }

    HwStats hw = get_hw();
    uint64_t freq = current_frequency.load();
    int gain = current_gain.load();
    OperatingMode mode = operating_mode.load();

    vector<string> L;
    auto add = [&](string s) { L.push_back(std::move(s)); };

    // ===== title =====
    {
        char up[40];
        long s = uptime_seconds();
        snprintf(up, sizeof(up), "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
        ostringstream o;
        o << col::BOLD << col::CYN << "Heimdall Server" << col::RST
          << col::DIM << "   coherent RX   up " << col::RST << up
          << col::DIM << "   " << (1000.0 / REFRESH_MS) << " Hz   frame " << frame << col::RST;
        add(o.str());
    }

    // ===== web UI URL (where to point a browser) =====
    add(label("WEB UI") + string(col::BOLD) + col::CYN + web_url() + col::RST);
    add("");

    // ===== RF / tuning =====
    {
        ostringstream o;
        o << label("RF")
          << col::BOLD << f3(freq / 1e6) << " MHz" << col::RST
          << col::DIM << "  sr " << col::RST << f2(SAMPLE_RATE / 1e6) << "M"
          << col::DIM << "  gain " << col::RST << (gain < 0 ? string("auto") : f1(gain / 10.0) + "dB")
          << col::DIM << "  elems " << col::RST << nelem << "/" << expected_serials.size()
          << col::DIM << "  ref ch" << col::RST << REF_CHANNEL
          << col::DIM << "  mode " << col::RST
          << (mode == OperatingMode::WIDEBAND_SCAN ? "WIDEBAND" : mode == OperatingMode::INDEPENDENT ? "INDEPENDENT" : "COHERENT")
          << col::DIM << "  bias " << col::RST << onoff(bias_tee_enabled.load());
        add(o.str());

        // Wideband (downconverter) variant: the freq above is the RF; show how
        // it is reached (LO, injection side, fixed IF the tuners sit at).
        if (downconverter.enabled.load()) {
            ostringstream w;
            w << label("DOWNCONV")
              << col::BOLD << "LO " << f3(downconverter.lo_hz.load() / 1e6) << " MHz" << col::RST
              << col::DIM << "  side " << col::RST
              << (downconverter.side.load() == MixerSide::HIGH  ? "HIGH (inverted, corrected)"
                : downconverter.side.load() == MixerSide::BELOW ? "BELOW (LO under RF)"
                                                                : "LOW")
              << col::DIM << "  IF " << col::RST << f3(WB_VARIANT_IF_HZ / 1e6) << " MHz";
            add(w.str());
        }
    }

    // ===== calibration (phase) =====
    {
        bool conv = (phase_state == PhaseCompensatorState::CONVERGED);
        bool pb_on = per_bin_cal.enabled.load();
        bool pb_ready = per_bin_cal.ready.load();
        bool fwd_on = forward_comp.enabled.load(std::memory_order_relaxed);
        bool fwd_ready = forward_comp.ready.load(std::memory_order_acquire);
        ostringstream o;
        o << label("CALIBRATION")
          << (conv ? col::GRN : col::YEL) << "phase " << phase_name(phase_state) << col::RST
          << col::DIM << "  data " << col::RST << (data_ready ? string(col::GRN) + "yes" + col::RST
                                                             : string(col::YEL) + "no" + col::RST)
          << col::DIM << "  per-bin " << col::RST
          << (!pb_on ? string(col::DIM) + "off" + col::RST
                     : pb_ready ? string(col::GRN) + "ready" + col::RST
                                : string(col::YEL) + "measuring(" + to_string(per_bin_cal.snapshots.load()) + ")" + col::RST)
          << col::DIM << "  fwd-comp " << col::RST
          << (!fwd_on ? string(col::DIM) + "off" + col::RST
                      : fwd_ready ? string(col::GRN) + "ready" + col::RST
                                  : string(col::YEL) + "pending" + col::RST);
        add(o.str());
    }

    // ===== coherence / recovery =====
    {
        bool lost = coherence_lost.load();
        bool recov = recovery_in_progress.load();
        ostringstream o;
        o << label("COHERENCE")
          << (lost ? string(col::RED) + "LOST" + col::RST
                   : recov ? string(col::YEL) + "recovering" + col::RST
                           : string(col::GRN) + "ok" + col::RST)
          << col::DIM << "  events " << col::RST
          << (coherence_event_count.load() ? col::YEL : col::DIM) << coherence_event_count.load() << col::RST
          << col::DIM << "  periodic " << col::RST
          << (periodic_recal_enabled.load() ? to_string(periodic_recal_minutes.load()) + "min"
                                            : string(col::DIM) + "off" + col::RST)
          << col::DIM << "  recal-fail lag " << col::RST << periodic_recal_lag_fail_count.load()
          << col::DIM << " / phase " << col::RST << periodic_recal_phase_fail_count.load();
        add(o.str());
    }

    // ===== buffers =====
    {
        size_t l2 = l2_buffer_size.load();
        size_t l2raw = l2_raw_buffer_size.load();
        size_t l2cap = l2_raw_cap.load();
        ostringstream o;
        o << label("BUFFERS")
          << col::DIM << "L2 " << col::RST << l2
          << col::DIM << "  L2-raw " << col::RST << l2raw << "/" << l2cap
          << col::DIM << "  corr-frames " << col::RST << data_seq
          << col::DIM << " (" << col::RST << (long)llround(fps) << col::DIM << "/s)" << col::RST;
        add(o.str());
    }

    // ===== network =====
    {
        size_t dc = tcp_data_server ? tcp_data_server->client_count() : 0;
        size_t cc = tcp_control_server ? tcp_control_server->client_count() : 0;
        bool rtl = rtl_tcp_server && rtl_tcp_server->has_client();
        int rtl_ch = rtl_tcp_server ? rtl_tcp_server->get_source_channel() : 0;
        ostringstream o;
        o << label("NETWORK")
          << col::DIM << "data:" << TCP_DATA_PORT << " " << col::RST << dc << col::DIM << " cli"
          << "   ctrl:" << TCP_CONTROL_PORT << " " << col::RST << cc << col::DIM << " cli"
          << "   rtl:" << RTL_TCP_PORT << " " << col::RST << dot(rtl) << col::DIM << " ch" << rtl_ch
          << "   web:" << WEB_PORT << col::RST;
        add(o.str());
    }

    // ===== scanner / wideband (only when active) =====
    if (discrete_scanner.enabled.load()) {
        ostringstream o;
        o << label("SCANNER")
          << col::GRN << "on" << col::RST
          << col::DIM << "  group " << col::RST << discrete_scanner.current_group_index.load()
          << "/" << discrete_scanner.get_num_groups()
          << col::DIM << "  dwell " << col::RST << discrete_scanner.dwell_time_ms.load() << "ms"
          << (discrete_scanner.retuning_in_progress.load()
                ? string("  ") + col::YEL + "retuning" + col::RST : "");
        add(o.str());
    } else if (wideband_config.enabled.load()) {
        ostringstream o;
        o << label("WIDEBAND") << col::GRN << "on" << col::RST
          << col::DIM << "  edge-clip " << col::RST << f2(wideband_config.edge_clip.load());
        for (int i = 0; i < nshow; i++)
            o << col::DIM << "  t" << i << " " << col::RST << f1(wideband_config.get_tuner_frequency(i) / 1e6);
        add(o.str());
    } else if (operating_mode.load() == OperatingMode::INDEPENDENT) {
        ostringstream o;
        o << label("INDEPENDENT");
        for (int i = 0; i < nshow; i++) {
            const int g = wideband_config.get_tuner_gain(i);
            o << col::DIM << "  t" << i << " " << col::RST << f1(wideband_config.get_tuner_frequency(i) / 1e6)
              << col::DIM << "/" << col::RST << (g == -1 ? string("AGC") : f1(g / 10.0));
        }
        add(o.str());
    }

    add("");

    // ===== per-channel table =====
    // One shared format string for the header and the data rows so their
    // columns line up exactly (the section title sits on its own line).
    auto ch_row = [](const string& a, const string& b, const string& c, const string& d,
                     const string& e, const string& f, const string& g) {
        char r[220];
        snprintf(r, sizeof(r), "   %-3s %-11s %-10s %9s %8s %6s %5s",
                 a.c_str(), b.c_str(), c.c_str(), d.c_str(), e.c_str(), f.c_str(), g.c_str());
        return string(r);
    };
    add(string(col::DIM) + "CHANNELS" + col::RST);
    add(string(col::DIM) + ch_row("ch", "serial", "state", "lag(smp)", "phase", "amp", "L1") + col::RST);
    for (int ch = 0; ch < nshow; ch++) {
        SDRDevice* d = (ch < ndev) ? devices[ch].get() : nullptr;
        string serial = d ? d->serial_number : "-";
        size_t l1 = d ? d->l1_buffer_size.load() : 0;
        bool running = d && d->running.load();

        string st = "-", lag = "-", ph = "-", am = "-";
        if (ch == REF_CHANNEL) {
            st = "REF";
        } else {
            auto it = lstates.find(ch);
            if (it != lstates.end()) st = lag_name(it->second);
            auto il = lags.find(ch);   if (il != lags.end())   lag = (il->second >= 0 ? "+" : "") + f3(il->second);
            auto ip = phases.find(ch); if (ip != phases.end()) ph = f1(ip->second);
            auto ia = amps.find(ch);   if (ia != amps.end())   am = f2(ia->second);
        }

        string line = ch_row(to_string(ch), serial, st, lag, ph, am, to_string(l1));
        if (!running) line = string(col::DIM) + line + col::RST;
        else if (ch != REF_CHANNEL && st == "SERVOING")
            line = string(col::GRN) + line + col::RST;
        add(line);
    }

    add("");

    // ===== system =====
    {
        const char* tc = hw.temp > 75 ? col::RED : hw.temp > 65 ? col::YEL : col::GRN;
        const char* uc = hw.cpu > 90 ? col::RED : hw.cpu > 70 ? col::YEL : col::GRN;
        ostringstream o;
        o << label("SYSTEM")
          << col::DIM << "cpu " << col::RST << uc << f1(hw.cpu) << "%" << col::RST
          << col::DIM << "  temp " << col::RST << tc
          << (hw.temp < 0 ? string("n/a") : f1(hw.temp) + "\xc2\xb0" + "C") << col::RST
          << col::DIM << "  ram " << col::RST << (long)hw.ram_used << "/" << (long)hw.ram_total << "MB";
        add(o.str());
    }

    // ===== activity / alert (pinned to the bottom; the "logs", never scrolling) =====
    vector<string> bottom;
    {
        LogView lv = log_view();
        int rows, cols;
        term_size(rows, cols);
        string title = "-- log ";
        size_t pad = (cols > (int)title.size()) ? (size_t)cols - title.size() : 0;
        bottom.push_back(string(col::DIM) + title + string(pad, '-') + col::RST);
        {
            ostringstream o;
            o << label("ACTIVITY")
              << (lv.last_event.empty() ? string(col::DIM) + "-" + col::RST : lv.last_event)
              << col::DIM << "   (" << lv.total << " msgs)" << col::RST;
            bottom.push_back(o.str());
        }
        {
            bool none = (lv.last_warn == "none");
            ostringstream o;
            o << label("ALERT")
              << (none ? string(col::DIM) + "none" + col::RST
                       : string(col::YEL) + lv.last_warn + col::RST)
              << col::DIM << "   (" << lv.warns << " total)" << col::RST;
            bottom.push_back(o.str());
        }
    }

    emit_frame(L, bottom);
}

void end() {
    if (!g_active) return;
    g_active = false;

    term_write("\033[?25h\033[?1049l");

    if (g_orig_cout) std::cout.rdbuf(g_orig_cout);
    if (g_orig_cerr) std::cerr.rdbuf(g_orig_cerr);
    g_orig_cout = g_orig_cerr = nullptr;

    auto logs = recent_log(20);
    if (!logs.empty()) {
        std::cout << "---- recent log ----\n";
        for (const auto& ln : logs) std::cout << ln << "\n";
        std::cout.flush();
    }
}

}  // namespace StatusDashboard
