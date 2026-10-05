#pragma once

// Decoder plugins (plugins/<id>/, see plugins/SDK.md): the registry of the
// plugins found on disk and the child process one digital decoder runs a
// plugin in. A plugin is an executable (plugins/<id>/build/decoder) speaking
// the binary protocol in plugins/sdk/kraken_plugin.hpp over its stdin/stdout,
// so a crashing or hanging plugin can't take kraken_doa down - the decoder
// reports it and restarts the process with a back-off.

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

namespace dig {

// A setting a plugin declares (kp::Option); choices "" = free text, else
// "value=Label|value=Label|..."
struct PluginOption {
    std::string key, label, def, choices, help;
};

struct PluginInfo {
    std::string id, name, description, version, author;
    std::string exe;              // plugins/<id>/build/decoder
    double sample_rate = 48000;   // complex input rate the plugin wants
    double min_vfo_rate = 12500;
    int64_t mtime = 0;            // of exe (ms) - a rebuild restarts running decoders
    bool auto_detect = true;      // runs in "Auto detect" - the user's choice (set_auto)
    std::vector<PluginOption> options;
    bool built = false;           // exe exists and answered --info
    bool stale = false;           // a source file is newer than exe
    std::string error;            // why it isn't usable
};

class PluginRegistry {
public:
    static PluginRegistry& instance();
    // KRAKEN_PLUGIN_DIR, else "plugins" (relative to the working directory)
    static std::string dir();
    static bool valid_id(const std::string& id);   // [a-z0-9_-]{1,32}, not sdk / lib
    // Re-reads plugins/*/ (runs `decoder --info` of every built plugin;
    // blocks - not for the uWS loop thread: use scan_async())
    void scan();
    // scan() on a worker thread, then done() (on that thread)
    void scan_async(std::function<void()> done);
    // Bumped by every scan that changed the list (decoders in Auto detect
    // mode rebuild their plugin set on a change)
    uint64_t generation() const { return generation_.load(); }
    bool get(const std::string& id, PluginInfo* out) const;
    std::vector<PluginInfo> list() const;
    std::string list_json() const;   // [{"id":..,"name":..,...},...]

    // Which plugins run in "Auto detect": all, except the ones the user
    // unticked (sidebar plugin list; persisted as AUTO_DETECT_OFF:id,id).
    // A change bumps generation(), so decoders in Auto detect pick it up.
    bool set_auto(const std::string& id, bool on);          // true if it changed
    void set_auto_off(const std::string& comma_list);
    std::string auto_off_list() const;                      // "id,id" (sorted)
private:
    mutable std::mutex mu_;
    std::vector<PluginInfo> plugins_;
    bool scanned_ = false;
    std::atomic<uint64_t> generation_{0};
    std::set<std::string> auto_off_;   // mu_
    std::mutex scan_mu_;   // one scan at a time
};

// Digital voice codecs as the decoder plugins find them (runs
// plugins/lib/build/codecs; blocks ~ms - call it off the uWS thread):
// {"codecs":{"checked":true,"imbe_ok":..,"imbe":"..","ambe_ok":..,"ambe":"..","acelp_ok":..,"acelp":".."}}
std::string codec_status_message();

// Runs argv (argv[0] = path), returns its stdout; false on failure/timeout
bool run_and_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out,
                     int* exit_status = nullptr);

class PluginProcess {
public:
    PluginProcess() = default;
    ~PluginProcess() { stop(); }
    PluginProcess(const PluginProcess&) = delete;
    PluginProcess& operator=(const PluginProcess&) = delete;

    bool start(const std::string& exe, std::string* err);
    // Close its stdin (the plugin exits on EOF) without waiting: call it on
    // every process first, then stop() each, so they exit in parallel
    void request_stop();
    void stop();
    bool running() const { return pid_ > 0; }
    // false once the process has exited; *status = waitpid status
    bool alive(int* status);
    // Queue a message. Samples are dropped (false) when the plugin is more
    // than max_backlog bytes behind; other messages are always queued.
    bool send(uint32_t type, const void* data, size_t len, size_t max_backlog);
    // Reads whatever the plugin sent; on_msg(type, payload, len) per message
    template <typename F>
    void poll(F&& on_msg) {
        flush();
        read_stderr();
        for (;;) {
            if (!read_stdout()) break;
        }
        size_t p = 0;
        while (rbuf_.size() - p >= 8) {
            uint32_t type, len;
            memcpy_u32(&type, rbuf_.data() + p);
            memcpy_u32(&len, rbuf_.data() + p + 4);
            if (len > (1u << 20)) { proto_error_ = true; rbuf_.clear(); return; }
            if (rbuf_.size() - p - 8 < len) break;
            on_msg(type, rbuf_.data() + p + 8, static_cast<size_t>(len));
            p += 8 + len;
        }
        rbuf_.erase(rbuf_.begin(), rbuf_.begin() + static_cast<long>(p));
    }
    bool protocol_error() const { return proto_error_; }
    // Offline (synchronous) use: write everything queued, reading replies
    // meanwhile so neither side blocks. false on timeout / a dead plugin.
    template <typename F>
    bool flush_blocking(F&& on_msg, int timeout_ms) {
        for (int i = 0; i < timeout_ms && !wbuf_.empty(); i++) {
            poll(on_msg);
            if (wbuf_.empty()) break;
            if (!alive(nullptr)) return false;
            wait_io(1);
        }
        poll(on_msg);
        return wbuf_.empty();
    }
    bool pending_writes() const { return !wbuf_.empty(); }
    void wait_io(int ms);   // until a pipe is ready or ms passed
    std::vector<std::string> stderr_tail() const;

private:
    pid_t pid_ = -1;
    int exit_status_ = 0;          // waitpid status once reaped
    int in_fd_ = -1, out_fd_ = -1, err_fd_ = -1;
    std::string wbuf_;
    std::vector<char> rbuf_;
    std::string ebuf_;
    mutable std::mutex tail_mu_;
    std::deque<std::string> tail_;
    bool proto_error_ = false;
    void flush();
    bool read_stdout();
    void read_stderr();
    static void memcpy_u32(uint32_t* d, const char* s);
};

}  // namespace dig
