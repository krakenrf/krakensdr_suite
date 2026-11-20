// Antenna bias tees from the web page - see antenna_bias.hpp and
// docs/dev/bias-tees.md
#include "antenna_bias.hpp"

#include "control_handler.hpp"
#include "playback.hpp"
#include "networking/websocket_server.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace antbias {
namespace {

using Clock = std::chrono::steady_clock;
// heimdall broadcasts its status at 2 Hz; the status thread drains the socket
// every 0.25 - 1 s. Longer than this without one: heimdall is not there.
constexpr auto STALE_AFTER = std::chrono::seconds(5);
constexpr size_t MAX_PENDING = 256 * 1024;   // a longer line is garbage

std::mutex mtx;
std::string pending;              // the line being received
bool heard = false;               // a status line came at heard_at
Clock::time_point heard_at;
bool has_fields = false;          // ... with antenna_bias_tees_available (a heimdall that has the command)
bool available = false, generic = false, wideband = false, ext_noise = false;
unsigned mask = 0, reserved = 0;  // bit N = channel N
int channels = 0;                 // heimdall's open channels (num_channels)
unsigned error_seq = 0;           // bumped with every refusal / error reply
std::string error;                // the last one

bool num_of(const std::string& line, const char* key, double& v) {
    std::string s;
    if (!json_find(line, key, s) || s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != s.c_str() && is_finite_value(v);
}

bool flag_of(const std::string& line, const char* key) {
    std::string s;
    return json_find(line, key, s) && s == "true";
}

unsigned bits_of(double v) { return v >= 0 && v <= 4294967295.0 ? static_cast<unsigned>(v) : 0u; }

// Caller holds mtx
const char* state_name() {
    if (!heard || Clock::now() - heard_at > STALE_AFTER) return "none";
    if (generic) return "generic";
    if (wideband) return "wideband";
    if (!has_fields) return "old";
    if (!available) return "unavailable";
    return "ok";
}

// Why the bias tees can't be switched now ("" = they can). Caller holds mtx
std::string state_refusal(const std::string& st) {
    if (st == "none") return "heimdall is not connected";
    if (st == "generic") return "not with generic RTL-SDRs - the bias tees are KrakenSDR hardware";
    if (st == "wideband") return "not on the KrakenSDR Wideband - its GPIOs drive the RF switches";
    if (st == "old") return "heimdall is too old for this - update it";
    if (st == "unavailable") return "heimdall has no switchable bias tees";
    return "";
}

// Caller holds mtx
std::string state_json_locked() {
    const std::string st = state_name();
    std::string s = "{\"state\":\"" + st + "\"";
    // heimdall gone, too old, no bias tees: nothing is known to be on
    s += ",\"mask\":" + std::to_string(st == "ok" ? mask : 0u);
    s += ",\"reserved\":" + std::to_string(reserved);
    s += ",\"channels\":" + std::to_string(channels);
    s += std::string(",\"external_noise\":") + (ext_noise ? "true" : "false");
    s += std::string(",\"playback\":") + (playback::active() ? "true" : "false");
    s += ",\"error_seq\":" + std::to_string(error_seq);
    s += ",\"error\":\"" + json_escape(error) + "\"}";
    return s;
}

// One line from heimdall. Caller holds mtx. true = every page should hear
// about it now (an error reply to our command)
bool handle_line(const std::string& line) {
    double v = 0;
    // A status: the 2 Hz broadcast or a get_status reply - every one has num_channels
    if (num_of(line, "num_channels", v)) {
        heard = true;
        heard_at = Clock::now();
        channels = std::clamp(static_cast<int>(v), 0, 32);
        std::string a;
        has_fields = json_find(line, "antenna_bias_tees_available", a);
        available = has_fields && a == "true";
        mask = has_fields && num_of(line, "antenna_bias_tee_mask", v) ? bits_of(v) : 0u;
        reserved = has_fields && num_of(line, "antenna_bias_tee_reserved", v) ? bits_of(v) : 0u;
        generic = flag_of(line, "generic_rtlsdr");
        std::string dc;
        wideband = json_find(line, "downconverter", dc);   // the Wideband variant's object
        ext_noise = flag_of(line, "external_noise");
        return false;
    }
    // A command reply: set_antenna_bias_tees' carry the mask in effect, an
    // error reply too (no other reply has that key)
    std::string st;
    if (!json_find(line, "status", st) || !num_of(line, "antenna_bias_tee_mask", v)) return false;
    mask = bits_of(v);
    if (st == "success") return false;
    std::string msg;
    if (!json_find(line, "message", msg) || msg.empty()) msg = "heimdall refused it";
    std::cerr << "Antenna bias tees: heimdall refused (" << msg.substr(0, 200) << ")" << std::endl;
    error = msg.substr(0, 300);
    error_seq++;
    return true;
}

}  // namespace

void ingest(const char* data, size_t n) {
    bool tell = false;
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (size_t i = 0; i < n; i++) {
            if (data[i] == '\n') {
                if (handle_line(pending)) tell = true;
                pending.clear();
            } else if (pending.size() < MAX_PENDING) {
                pending.push_back(data[i]);
            }
        }
        if (pending.size() >= MAX_PENDING) pending.clear();
        if (tell) msg = "{\"antenna_bias\":" + state_json_locked() + "}";
    }
    if (tell) WebSocketServer::broadcast_json_message(msg);   // thread-safe (defers to the loop)
}

bool is_command(std::string_view m) { return m.starts_with("ANT_BIAS:"); }

std::string handle_command(std::string_view m, bool from_page) {
    const std::string cmd(m.substr(0, 40));
    // a page's refused request: every page gets the state with the reason (the
    // one that asked shows it, error_seq tells it apart)
    auto refuse = [&](const std::string& why) {
        std::cout << "Antenna bias tees: " << cmd << " refused (" << why << ")" << std::endl;
        std::string msg;
        {
            std::lock_guard<std::mutex> lock(mtx);
            error = why;
            error_seq++;
            msg = "{\"antenna_bias\":" + state_json_locked() + "}";
        }
        WebSocketServer::broadcast_json_message(msg);
        return why;
    };
    if (!from_page) {
        // DC on the antenna ports is the user's call: not the settings replay,
        // the AI assistant, the web mapper, a test transmission or the scanners
        std::cout << "Antenna bias tees: " << cmd << " refused (only from a page)" << std::endl;
        return "the antenna bias tees are switched from the page only";
    }
    const std::string_view v = m.substr(9);
    if (v.empty() || v.size() > 3 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return refuse("mask must be 0-255");
    const unsigned want = static_cast<unsigned>(std::atoi(std::string(v).c_str()));
    if (want > 255) return refuse("mask must be 0-255");
    if (playback::active())
        return refuse("Playback: the bias tees are the real receiver's - stop the playback to switch them");
    std::string why;
    unsigned open_bits = 0, res = 0, was = 0;
    bool ext = false;
    {
        std::lock_guard<std::mutex> lock(mtx);   // (refuse() takes it itself)
        why = state_refusal(state_name());
        open_bits = channels >= 32 ? ~0u : ((1u << channels) - 1);
        res = reserved;
        ext = ext_noise;
        was = mask;
    }
    if (!why.empty()) return refuse(why);
    if (want & ~open_bits) return refuse("no such antenna");
    if (want & res & open_bits)
        return refuse(ext && (want & res & 1u) ? "Antenna 0 powers the external noise source (--ext_noise)"
                                               : "that antenna's bias tee can't be switched");
    std::cout << "Antenna bias tees: mask " << was << " -> " << want << " (asking heimdall)" << std::endl;
    ControlHandler::send_control_command("{\"command\":\"set_antenna_bias_tees\",\"mask\":" + std::to_string(want) + "}");
    return "";
}

std::string state_json() {
    std::lock_guard<std::mutex> lock(mtx);
    return state_json_locked();
}

std::string state_message() { return "{\"antenna_bias\":" + state_json() + "}"; }

}  // namespace antbias
