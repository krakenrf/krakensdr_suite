#include "utils/status_dashboard.hpp"

#include "globals.hpp"
#include "config.hpp"
#include "utils/log_sanitize.hpp"
#include "channel_manager.hpp"
#include "decimator_manager.hpp"
#include "scanner_manager.hpp"
#include "doa_logger.hpp"
#include "networking/data_receiver.hpp"
#include "networking/tcp_client.hpp"
#include "networking/gpsd_client.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "signal_processing/beamformer.hpp"
#include "signal_processing/music_processor.hpp"
#include "utils/system_stats.hpp"
#include "utils/raw_data_buffer.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

using namespace std;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Refresh cadence
// ---------------------------------------------------------------------------
static constexpr int REFRESH_MS = 250;   // frame interval driven by the status thread

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
    constexpr const char* BLU  = "\033[34m";
    constexpr const char* MAG  = "\033[35m";
    constexpr const char* CYN  = "\033[36m";
}

// ---------------------------------------------------------------------------
// Captured-log ring (all cout/cerr goes here while the dashboard owns the TTY)
// ---------------------------------------------------------------------------
namespace {

// Captured cout/cerr is reduced to a few persistent live fields (last event,
// last warning/error, and running counts) so nothing scrolls. A small bounded
// history is kept ONLY for the tail dump printed once on shutdown.
deque<string> g_log;                 // shutdown tail dump only
string        g_last_event;          // most recent log line (any)
string        g_last_warn = "none";  // most recent warning/error (sticky)
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
                contains_ci(line, "fail");
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

vector<string> recent_log(size_t n) {  // used only by end()
    lock_guard<mutex> lk(g_log_mtx);
    vector<string> out;
    size_t start = g_log.size() > n ? g_log.size() - n : 0;
    for (size_t i = start; i < g_log.size(); i++) out.push_back(g_log[i]);
    return out;
}

// streambuf that swallows cout/cerr writes and files complete lines into the
// ring. Per-thread partial buffers avoid interleaving characters from threads
// that write concurrently (the app already writes cout from many threads).
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
        if (ch == '\n') {
            push_line(partial);
            partial.clear();
        } else if (ch != '\r') {
            partial += ch;
            if (partial.size() > 4096) { push_line(partial); partial.clear(); }
        }
    }
};

CaptureBuf     g_capbuf;
std::streambuf* g_orig_cout = nullptr;
std::streambuf* g_orig_cerr = nullptr;
bool           g_active = false;
steady_clock::time_point g_start;

// -----------------------------------------------------------------------
// Terminal I/O
// -----------------------------------------------------------------------
void term_write(const string& s) {
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0) {
        ssize_t w = ::write(STDOUT_FILENO, p, left);
        if (w <= 0) break;   // give up on error; next frame retries
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

// Clip a (possibly colored) string to `cols` visible columns, treating ANSI CSI
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
                if (j < s.size()) j++;  // include the final command letter
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

// -----------------------------------------------------------------------
// Small formatting helpers
// -----------------------------------------------------------------------
string label(const char* s) {
    string t = s;
    if (t.size() < 12) t.append(12 - t.size(), ' ');
    return string(col::DIM) + t + col::RST + " ";
}

string dot(bool ok) {
    return string(ok ? col::GRN : col::RED) + "\xe2\x97\x8f" + col::RST;  // ●
}

string onoff(bool on) {
    return on ? string(col::GRN) + "on" + col::RST
              : string(col::DIM) + "off" + col::RST;
}

string f1(float v) { char b[32]; snprintf(b, sizeof(b), "%.1f", v); return b; }
string f2(float v) { char b[32]; snprintf(b, sizeof(b), "%.2f", v); return b; }

// Browser URL for the client web UI (HTTPS). Uses the machine hostname so the
// link works from another device on the network; computed once.
string web_url() {
    static const string url = [] {
        char h[256] = {0};
        string host = (gethostname(h, sizeof(h) - 1) == 0 && h[0]) ? string(h) : string("localhost");
        return "https://" + host + ":" + to_string(WEB_PORT);
    }();
    return url;
}

string bandwidth_str(float mhz) {
    char b[24];
    if (mhz >= 1.0f) snprintf(b, sizeof(b), "%.2fMHz", mhz);
    else             snprintf(b, sizeof(b), "%.0fkHz", mhz * 1000.0f);
    return b;
}

// Phase-state names MUST match the web UI mapping (kraken_doa.html):
//   0=Waiting 1=Measuring 2=Applying 3=Verifying 4=Converged 5=Cooldown.
// (Do not copy data_receiver.cpp's debug array — it has an extra state and is
// off by one from index 2 on, which made steady-state 4=Converged read as
// "VERIFYING" here while the web UI correctly showed "Converged".)
const char* phase_state_name(uint32_t s) {
    static const char* names[] = {"WAITING", "MEASURING", "APPLYING",
                                  "VERIFYING", "CONVERGED", "COOLDOWN"};
    return s < 6 ? names[s] : "UNKNOWN";
}

const char* topo_name(ArrayTopology t) {
    switch (t) {
        case ArrayTopology::UCA: return "UCA";
        case ArrayTopology::ULA: return "ULA";
        default:                 return "Custom";
    }
}

const char* bf_mode_name(BeamformingMode m) {
    switch (m) {
        case BeamformingMode::DELAY_AND_SUM:       return "DAS";
        case BeamformingMode::MVDR:                return "MVDR";
        case BeamformingMode::SELECTION_DIVERSITY: return "SelDiv";
        case BeamformingMode::FREQ_DOMAIN_DAS:     return "FD-DAS";
        default:                                   return "?";
    }
}

long uptime_seconds() {
    return duration_cast<seconds>(steady_clock::now() - g_start).count();
}

// Emit one full-screen frame: `L` top content (clipped if the screen is short),
// `bottom` pinned to the last rows. Never trails a newline on the last line, so
// the terminal never scrolls. Shared by the loading screen and the dashboard.
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

// The dashboard shows a "startup" view until the pipeline is actually up (data
// server connected AND first packet received), latched so a later disconnect
// shows the full dashboard rather than dropping back to the loading screen. A
// grace period guarantees the full view even if the server never appears.
bool g_ready = false;
bool startup_ready() {
    if (g_ready) return true;
    bool connected = data_client.is_connected();
    bool got_data  = DataReceiver::get_raw_buffer_stats().packets_added > 0;
    if ((connected && got_data) || uptime_seconds() >= 10)
        g_ready = true;
    return g_ready;
}

// Loading screen: shows the live tail of captured startup messages (i.e. WHAT is
// actually loading) plus the current connection step, until startup_ready().
void render_loading() {
    static uint32_t tick = 0;
    const char* SPIN = "|/-\\";
    char sp = SPIN[(tick++) % 4];

    vector<string> L;
    auto add = [&](string s) { L.push_back(std::move(s)); };

    {
        ostringstream o;
        o << col::BOLD << col::CYN << "Heimdall DoA Client" << col::RST
          << col::DIM << "   starting up " << sp << "   " << uptime_seconds() << "s" << col::RST;
        add(o.str());
    }
    add("");

    bool connected = data_client.is_connected();
    {
        ostringstream o;
        o << label("STATUS")
          << (connected
                ? string(col::GRN) + "connected \xe2\x80\x94 waiting for first data\xe2\x80\xa6" + col::RST
                : string(col::YEL) + "connecting to Heimdall server (port " +
                      to_string(TCP_DATA_PORT) + ")\xe2\x80\xa6" + col::RST);
        add(o.str());
    }
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
    if (avail > 22) avail = 22;
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

void begin() {
    if (g_active) return;
    if (!isatty(STDOUT_FILENO)) return;               // piped/redirected -> plain path
    const char* off = getenv("KRAKEN_DOA_NO_TUI");
    if (off && off[0] && strcmp(off, "0") != 0) return;

    g_start = steady_clock::now();

    // Redirect cout/cerr into the capture ring, then take over the terminal.
    g_orig_cout = std::cout.rdbuf(&g_capbuf);
    g_orig_cerr = std::cerr.rdbuf(&g_capbuf);

    // Enter alternate screen, clear it, hide the cursor.
    term_write("\033[?1049h\033[2J\033[H\033[?25l");

    // Paint an immediate centered splash so the screen is never blank during the
    // brief gap before the first frame's live state has been gathered.
    {
        int rows, cols;
        term_size(rows, cols);
        const string title = "Heimdall DoA Client";
        const string msg   = "Starting up\xe2\x80\xa6";  // "Starting up…" (ellipsis = 1 col)
        const int title_w = 19;  // display width of title
        const int msg_w   = 12;  // display width of "Starting up…"
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

    // Until the pipeline is up, show the startup/loading view (what is loading).
    if (!startup_ready()) { render_loading(); return; }

    static uint64_t frame = 0;
    frame++;

    // ---- derived throughput (packets/sec) between frames ----
    static uint64_t prev_added = 0, prev_consumed = 0;
    static steady_clock::time_point prev_t = g_start;
    RawDataBuffer::Stats rb = DataReceiver::get_raw_buffer_stats();
    steady_clock::time_point now = steady_clock::now();
    double dt = duration<double>(now - prev_t).count();
    double add_rate = 0, cons_rate = 0;
    if (dt > 0.05) {
        add_rate  = (rb.packets_added    - prev_added)    / dt;
        cons_rate = (rb.packets_consumed - prev_consumed) / dt;
        prev_added = rb.packets_added;
        prev_consumed = rb.packets_consumed;
        prev_t = now;
    }

    HardwareStats hw = get_hardware_stats();
    int ch = active_channel.load();
    float center_hz = ChannelManager::get_frequency(ch);
    float gain_db   = ChannelManager::get_gain(ch);
    uint32_t phase  = scanner_manager.getPhaseState();
    bool noise      = scanner_manager.isNoiseSourceActive();

    vector<string> L;         // scrollable top content (clipped if the screen is short)
    vector<string> bottom;    // pinned to the last rows so the log fields never clip
    auto add  = [&](string s) { L.push_back(std::move(s)); };
    auto addb = [&](string s) { bottom.push_back(std::move(s)); };

    // ===== title =====
    {
        char up[40];
        long s = uptime_seconds();
        snprintf(up, sizeof(up), "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
        ostringstream o;
        o << col::BOLD << col::CYN << "Heimdall DoA Client" << col::RST
          << col::DIM << "   live status   up " << col::RST << up
          << col::DIM << "   " << (1000.0 / REFRESH_MS) << " Hz   frame " << frame << col::RST;
        add(o.str());
    }

    // ===== web UI URL (where to point a browser) =====
    add(label("WEB UI") + string(col::BOLD) + col::CYN + web_url() + col::RST +
        string(col::DIM) + "  (accept the self-signed cert)" + col::RST);

    // ===== network =====
    {
        ostringstream o;
        o << label("NETWORK")
          << dot(data_client.is_connected()) << col::DIM << " data:" << col::RST << TCP_DATA_PORT << "  "
          << dot(control_client.is_connected()) << col::DIM << " ctrl:" << col::RST << TCP_CONTROL_PORT << "   "
          << col::DIM << "clients " << col::RST << col::BOLD << ws_client_count.load() << col::RST
          << col::DIM << " wss:" << WEB_PORT << " http:" << DOA_HTTP_PORT << col::RST;
        add(o.str());
    }

    // ===== stream / tuning =====
    {
        ostringstream o;
        o << label("STREAM")
          << col::BOLD << f2(center_hz / 1e6f) << " MHz" << col::RST
          << col::DIM << "  ch " << col::RST << ch
          << col::DIM << "  gain " << col::RST << (gain_db < 0 ? string("auto") : f1(gain_db) + "dB")
          << col::DIM << "  sr " << col::RST << f2(SAMPLE_RATE / 1e6f) << "M"
          << col::DIM << "  elems " << col::RST << active_num_elements.load() << "/" << DOA_NUM_ELEMENTS
          << col::DIM << "  wideband " << col::RST << onoff(wideband_mode_enabled.load());
        add(o.str());
    }
    {
        ostringstream o;
        bool conv = (phase == 4);  // 4 = Converged (see phase_state_name / web UI)
        o << label("CALIBRATION")
          << (conv ? col::GRN : col::YEL) << "phase " << phase_state_name(phase) << col::RST
          << col::DIM << "  noise src " << col::RST
          << (noise ? string(col::YEL) + "ACTIVE" + col::RST : string(col::DIM) + "quiet" + col::RST)
          << col::DIM << "  fft " << col::RST << current_fft_size.load()
          << col::DIM << "/dec " << col::RST << current_fft_decimation.load();
        add(o.str());
    }

    // ===== buffer =====
    {
        ostringstream o;
        const char* fillc = rb.fill_percentage > 80 ? col::RED
                          : rb.fill_percentage > 50 ? col::YEL : col::GRN;
        o << label("BUFFER")
          << col::DIM << "raw q " << col::RST << rb.current_queue_size
          << " " << fillc << "(" << f1(rb.fill_percentage) << "%)" << col::RST
          << col::DIM << "  " << col::RST << f1(rb.memory_usage_bytes / (1024.0f * 1024.0f)) << "MB"
          << col::DIM << "  in " << col::RST << (long)llround(add_rate) << "/s"
          << col::DIM << " out " << col::RST << (long)llround(cons_rate) << "/s"
          << col::DIM << "  dropF " << col::RST
          << (rb.packets_dropped_full ? col::YEL : col::DIM) << rb.packets_dropped_full << col::RST
          << col::DIM << " dropS " << col::RST
          << (rb.packets_dropped_stale ? col::YEL : col::DIM) << rb.packets_dropped_stale << col::RST;
        add(o.str());
    }

    // ===== system =====
    {
        ostringstream o;
        const char* tc = hw.cpu_temp_c > 75 ? col::RED : hw.cpu_temp_c > 65 ? col::YEL : col::GRN;
        const char* uc = hw.cpu_usage_pct > 90 ? col::RED : hw.cpu_usage_pct > 70 ? col::YEL : col::GRN;
        o << label("SYSTEM")
          << col::DIM << "cpu " << col::RST << uc << f1(hw.cpu_usage_pct) << "%" << col::RST
          << col::DIM << "  temp " << col::RST << tc
          << (hw.cpu_temp_c < 0 ? string("n/a") : f1(hw.cpu_temp_c) + "\xc2\xb0" + "C") << col::RST
          << col::DIM << "  ram " << col::RST << (long)hw.ram_used_mb << "/" << (long)hw.ram_total_mb << "MB";
        add(o.str());
    }

    // ===== DoA / MUSIC =====
    {
        auto infos = decimator_manager.getDecimatorInfoList();
        auto decs  = decimator_manager.getAllDecimators();

        // A representative MUSIC processor for the global config summary line.
        MUSICProcessor* mp0 = nullptr;
        for (auto& d : decs)
            if (d && d->music_processor) { mp0 = d->music_processor.get(); break; }

        ostringstream o;
        o << label("DoA MUSIC") << onoff(doa_enabled.load());
        if (mp0) {
            MUSICConfig cfg = mp0->getConfig();
            string src_str;
            if (mp0->isAutoNumSources()) {
                int est = mp0->getEstimatedNumSources();
                src_str = "auto(" + (est >= 0 ? to_string(est) : string("-")) + ")";
            } else {
                src_str = to_string(mp0->getNumSignalSources());
            }
            o << col::DIM << "  res " << col::RST << f1(mp0->getAngularResolution()) << "\xc2\xb0"
              << col::DIM << "  snaps " << col::RST << cfg.num_snapshots << "x" << cfg.snapshot_length
              << col::DIM << "  src " << col::RST << src_str
              << col::DIM << "  " << col::RST << topo_name(mp0->getArrayTopology())
              << col::DIM << " r=" << col::RST << f1(mp0->getArrayRadius()) << "mm"
              << col::DIM << " rot " << col::RST << f1(mp0->getArrayOffset()) << "\xc2\xb0";
        }
        add(o.str());

        // Per-decimator table
        add(string(col::DIM) +
            "   id  freq(MHz)     bw      bearing  conf   eig(dB)  proc(ms)  sq   role" +
            col::RST);

        size_t shown = 0;
        for (const auto& info : infos) {
            if (shown++ >= 8) {
                add(string(col::DIM) + "   ... " +
                    to_string(infos.size() - 8) + " more" + col::RST);
                break;
            }
            // Find the matching live instance for MUSIC readouts.
            MUSICProcessor* mp = nullptr;
            for (auto& d : decs)
                if (d && d->id == info.id && d->music_processor) { mp = d->music_processor.get(); break; }

            string bearing = "  -", conf = "  -", eig = "   -", proc = "   -";
            if (mp && mp->isEnabled()) {
                auto pk = mp->getPeakAngleWithConfidence();
                if (pk.first >= 0) {
                    char b[24]; snprintf(b, sizeof(b), "%5.1f\xc2\xb0", pk.first); bearing = b;
                    conf = f2(pk.second);
                }
                float er = mp->getEigenvalueRatio();
                eig = f1(10.0f * log10f(er > 0.001f ? er : 0.001f));
                proc = f2((float)mp->getStats().avg_processing_time_ms);
            }

            float freq_mhz = (center_hz + info.frequency_offset_hz) / 1e6f;
            char row[200];
            // bearing/eig contain a UTF-8 degree sign (2 bytes) so plain %s width
            // padding is approximate; that is fine for a monospace readout.
            snprintf(row, sizeof(row),
                     "   %-3d %9.4f  %8s  %6s  %5s  %6s   %6s   %-3s  %s",
                     info.id, freq_mhz, bandwidth_str(info.bandwidth_mhz).c_str(),
                     bearing.c_str(), conf.c_str(), eig.c_str(), proc.c_str(),
                     info.squelch_open ? "op" : (info.squelch_enabled ? "sq" : "--"),
                     info.is_fm_source ? "FM+DoA" : "DoA");
            string line = row;
            if (!info.enabled) line = string(col::DIM) + line + col::RST;
            add(line);
        }
    }

    add("");

    // ===== FM =====
    {
        ostringstream o;
        o << label("FM AUDIO") << onoff(fm_enabled.load());
        if (fm_enabled.load() && fm_demod.is_processing_enabled()) {
            size_t w, r, over, under;
            fm_demod.get_buffer_stats(w, r, over, under);
            const char* mode = "?";
            switch (fm_demod.getDemodulatorMode()) {
                case DemodulatorMode::WBFM: mode = "WBFM"; break;
                case DemodulatorMode::NBFM: mode = "NBFM"; break;
                case DemodulatorMode::AM:   mode = "AM";   break;
                case DemodulatorMode::DIGITAL: mode = "DIGI"; break;
            }
            o << col::DIM << "  " << col::RST << mode
              << col::DIM << "  fill " << col::RST << f1(fm_demod.get_buffer_fill()) << "%"
              << col::DIM << "  sig " << col::RST << f1(fm_demod.get_signal_strength()) << "dB"
              << col::DIM << "  over " << col::RST << (over ? col::YEL : col::DIM) << over << col::RST
              << col::DIM << " under " << col::RST << (under ? col::YEL : col::DIM) << under << col::RST;
        }
        add(o.str());
    }

    // ===== beamforming =====
    {
        ostringstream o;
        o << label("BEAMFORM") << onoff(beamforming_enabled.load());
        if (beamforming_enabled.load()) {
            auto decs = decimator_manager.getAllDecimators();
            Beamformer* bf = nullptr;
            for (auto& d : decs) if (d && d->beamformer) { bf = d->beamformer.get(); break; }
            if (bf) {
                Beamformer::Stats st = bf->getStats();
                o << col::DIM << "  " << col::RST << bf_mode_name(bf->getBeamformingMode())
                  << col::DIM << "  steer " << col::RST << f1(st.current_steering_angle) << "\xc2\xb0"
                  << col::DIM << "  snr +" << col::RST << f1(st.estimated_snr_improvement_db) << "dB";
                if (bf->getBeamformingMode() == BeamformingMode::SELECTION_DIVERSITY)
                    o << col::DIM << "  sel ch " << col::RST << st.selected_channel
                      << " (" << f1(st.selected_channel_snr) << "dB)";
            }
            if (manual_steering_enabled.load())
                o << col::YEL << "  [manual " << f1(manual_steering_angle.load()) << "\xc2\xb0]" << col::RST;
        }
        add(o.str());
    }

    // ===== GPS =====
    {
        GpsFix gps = gps_client.get();
        ostringstream o;
        bool ok = gps.has_fix();
        o << label("GPS")
          << (ok ? col::GRN : col::DIM) << gps.fix_str() << col::RST;
        if (ok) {
            char pos[48];
            snprintf(pos, sizeof(pos), "%.5f, %.5f", gps.lat, gps.lon);
            o << col::DIM << "  " << col::RST << pos
              << col::DIM << "  hdg " << col::RST << f1(gps.heading()) << "\xc2\xb0"
              << col::DIM << " (" << gps.heading_source() << ")";
        }
        o << col::DIM << "  sats " << col::RST << gps.sats_used << "/" << gps.sats_visible;
        add(o.str());
    }

    // ===== recording =====
    {
        ostringstream o;
        o << label("RECORDING");
        if (doa_logger.isRunning()) {
            o << col::RED << "\xe2\x97\x8f rec" << col::RST
              << col::DIM << "  " << col::RST << doa_logger.activeFilename()
              << col::DIM << "  " << col::RST << f1(doa_logger.fileBytes() / (1024.0f * 1024.0f)) << "MB"
              << col::DIM << "  " << col::RST << doa_logger.entries() << " entries"
              << col::DIM << "  every " << col::RST << f1(doa_logger.intervalMs() / 1000.0f) << "s";
        } else {
            o << col::DIM << "idle" << col::RST;
        }
        add(o.str());
    }

    // ===== performance (decimation timing) =====
    {
        double dms = decimator_manager.getLastProcessMs();
        int ndec = (int)decimator_manager.getDecimatorCount();
        const char* dc = (ndec > 1 && dms > 4.0) ? col::RED
                       : (dms > 8.0)             ? col::YEL : col::GRN;
        ostringstream o;
        o << label("PERF")
          << col::DIM << "decimate " << col::RST << dc << f2((float)dms) << "ms" << col::RST
          << col::DIM << "  (" << ndec << " decimator" << (ndec == 1 ? "" : "s") << " in parallel)" << col::RST;
        add(o.str());
    }

    // ===== per-thread throughput table (persistent; replaces scrolling stats) =====
    {
        auto snaps = global_stats.snapshot();

        // Per-thread ops/sec, sampled between frames.
        static unordered_map<string, size_t> prev_ops;
        static steady_clock::time_point prev_thr_t = g_start;
        double tdt = duration<double>(now - prev_thr_t).count();
        bool upd = tdt > 0.4;   // refresh rates ~2x/sec so they don't jitter

        add(string(col::DIM) +
            "THREADS      name                  ops          rate/s   err       MB" +
            col::RST);
        for (const auto& s : snaps) {
            size_t p = prev_ops.count(s.name) ? prev_ops[s.name] : s.operations;
            double rate = (tdt > 0.05 && s.operations >= p) ? (s.operations - p) / tdt : 0.0;
            char row[220];
            snprintf(row, sizeof(row),
                     "   %-18s %11llu  %9lld   %s%-6llu%s %7.1f",
                     s.name.c_str(),
                     (unsigned long long)s.operations,
                     (long long)llround(rate),
                     (s.errors ? col::RED : col::DIM),
                     (unsigned long long)s.errors,
                     col::RST,
                     s.bytes_processed / (1024.0 * 1024.0));
            add(row);
        }
        if (snaps.empty())
            add(string("   ") + col::DIM + "no registered threads yet" + col::RST);

        if (upd) {
            prev_ops.clear();
            for (const auto& s : snaps) prev_ops[s.name] = s.operations;
            prev_thr_t = now;
        }
    }

    // ===== activity / alerts (single persistent lines; pinned to the bottom so
    // they never clip on a short terminal — this is where "everything else in
    // the logs" now lives, updating in place instead of scrolling) =====
    {
        LogView lv = log_view();
        addb(string(col::DIM) + string(200, '-') + col::RST);  // rule row (clipped to width)
        {
            ostringstream o;
            o << label("ACTIVITY")
              << (lv.last_event.empty() ? string(col::DIM) + "-" + col::RST : lv.last_event)
              << col::DIM << "   (" << lv.total << " msgs)" << col::RST;
            addb(o.str());
        }
        {
            bool none = (lv.last_warn == "none");
            ostringstream o;
            o << label("ALERT")
              << (none ? string(col::DIM) + "none" + col::RST
                       : string(col::YEL) + lv.last_warn + col::RST)
              << col::DIM << "   (" << lv.warns << " total)" << col::RST;
            addb(o.str());
        }
    }

    // ===== compose the frame =====
    // Top content is drawn from row 1 (clipped if the screen is short); the
    // bottom (log) lines are pinned to the last rows so they never fall off.
    emit_frame(L, bottom);
}

void end() {
    if (!g_active) return;
    g_active = false;

    // Leave the alternate screen and show the cursor.
    term_write("\033[?25h\033[?1049l");

    // Restore the real cout/cerr so shutdown messages print normally.
    if (g_orig_cout) std::cout.rdbuf(g_orig_cout);
    if (g_orig_cerr) std::cerr.rdbuf(g_orig_cerr);
    g_orig_cout = g_orig_cerr = nullptr;

    // Echo the tail of the session log so recent context isn't lost with the
    // alternate screen.
    auto logs = recent_log(20);
    if (!logs.empty()) {
        std::cout << "---- recent log ----\n";
        for (const auto& ln : logs) std::cout << ln << "\n";
        std::cout.flush();
    }
}

}  // namespace StatusDashboard
