#include "settings.hpp"
#include "types.hpp"
#include "forward_comp.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <mutex>
#include <iostream>
#include <sstream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>

namespace {

// Replace `path` with `data` atomically AND durably: write a temp file
// (checking every write), fsync it, rename it over the old file, then fsync
// the directory so the rename itself survives a power cut. On any failure the
// temp file is removed and the existing file is left untouched - a full disk
// or a power cut can never leave the settings truncated or empty.
bool write_file_atomic(const char* path, const std::string& data) {
    const std::string tmp = std::string(path) + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0) { ::close(fd); ::unlink(tmp.c_str()); return false; }
    if (::close(fd) != 0) { ::unlink(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), path) != 0) { ::unlink(tmp.c_str()); return false; }
    const std::string p(path);
    const size_t slash = p.rfind('/');
    const std::string dir = (slash == std::string::npos) ? "." : p.substr(0, slash ? slash : 1);
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }  // best effort
    return true;
}

std::mutex g_settings_mutex;

// Trim leading/trailing ASCII whitespace.
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

bool parse_bool(const std::string& v) {
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

}  // namespace

namespace settings {

std::atomic<int> persisted_num_elements{0};
std::atomic<uint64_t> persisted_frequency{0};
std::atomic<int> persisted_gain{-999};
std::atomic<int> persisted_mode{0};
std::atomic<uint32_t> persisted_tuner_freq[8] = {};
std::atomic<int> persisted_tuner_gain[8] = {-999, -999, -999, -999, -999, -999, -999, -999};
static_assert(NUM_DEVICES <= 8, "persisted_tuner_* hold 8 tuners");

namespace {
std::atomic<bool> tuning_dirty{false};
std::atomic<long long> tuning_changed_ms{0};
long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void note_tuning(uint64_t frequency_hz, int gain) {
    if (frequency_hz > 0) persisted_frequency.store(frequency_hz, std::memory_order_release);
    if (gain != -999) persisted_gain.store(gain, std::memory_order_release);
    tuning_changed_ms.store(steady_ms(), std::memory_order_release);
    tuning_dirty.store(true, std::memory_order_release);
}

void note_mode() {
    const OperatingMode m = operating_mode.load();
    persisted_mode.store(static_cast<int>(m), std::memory_order_release);
    for (int i = 0; i < NUM_DEVICES; i++) {
        persisted_tuner_freq[i].store(m == OperatingMode::INDEPENDENT ? wideband_config.get_tuner_frequency(i) : 0,
                                      std::memory_order_release);
        persisted_tuner_gain[i].store(m == OperatingMode::INDEPENDENT ? wideband_config.get_tuner_gain(i) : -999,
                                      std::memory_order_release);
    }
    tuning_changed_ms.store(steady_ms(), std::memory_order_release);
    tuning_dirty.store(true, std::memory_order_release);
}

void save_tuning_if_due() {
    if (!tuning_dirty.load(std::memory_order_acquire)) return;
    if (steady_ms() - tuning_changed_ms.load(std::memory_order_acquire) < TUNING_SAVE_DELAY_MS) return;
    tuning_dirty.store(false, std::memory_order_release);
    save();
}

void load() {
    std::lock_guard<std::mutex> lk(g_settings_mutex);

    std::ifstream f(FILE_PATH);
    if (!f) {
        std::cout << "Settings: no " << FILE_PATH << " found; using defaults (per-bin EQ OFF)" << std::endl;
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Whole-line comments only: save() never writes a trailing comment,
        // and stripping from any '#' cut a forward-comp filename containing
        // one, so that channel's correction was lost on the next start
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;
        // A hand-written trailing comment (whitespace + '#') is still dropped,
        // except on the filename keys, where '#' can be part of the value
        if (key.rfind("forward_comp_ch", 0) != 0) {
            for (size_t i = 1; i < val.size(); i++) {
                if (val[i] == '#' && (val[i - 1] == ' ' || val[i - 1] == '\t')) {
                    val = trim(val.substr(0, i));
                    break;
                }
            }
        }

        if (key == "per_bin_eq") {
            const bool on = parse_bool(val);
            per_bin_cal.enabled.store(on, std::memory_order_release);
            std::cout << "Settings: per_bin_eq = " << (on ? "ON" : "OFF") << std::endl;
        } else if (key == "periodic_recal_enabled") {
            const bool on = parse_bool(val);
            periodic_recal_enabled.store(on, std::memory_order_release);
            std::cout << "Settings: periodic_recal_enabled = " << (on ? "ON" : "OFF") << std::endl;
        } else if (key == "periodic_recal_minutes") {
            try {
                const int m = std::max(1, std::stoi(val));
                periodic_recal_minutes.store(m, std::memory_order_release);
                std::cout << "Settings: periodic_recal_minutes = " << m << std::endl;
            } catch (...) {
                std::cerr << "Settings: invalid periodic_recal_minutes '" << val << "'" << std::endl;
            }
        } else if (key == "center_freq") {
            // Validated against the RF range in main() (needs --wideband parsed)
            try {
                persisted_frequency.store(std::stoull(val), std::memory_order_release);
            } catch (...) {
                std::cerr << "Settings: invalid center_freq '" << val << "'" << std::endl;
            }
        } else if (key == "gain") {
            try {
                persisted_gain.store(std::stoi(val), std::memory_order_release);
            } catch (...) {
                std::cerr << "Settings: invalid gain '" << val << "'" << std::endl;
            }
        } else if (key == "operating_mode") {
            const int m = val == "wideband" ? 1 : val == "independent" ? 2 : 0;
            persisted_mode.store(m, std::memory_order_release);
            if (m) std::cout << "Settings: operating_mode = " << val << std::endl;
        } else if (key.rfind("tuner_freq", 0) == 0 || key.rfind("tuner_gain", 0) == 0) {
            try {
                const int ch = std::stoi(key.substr(10));
                if (ch >= 0 && ch < NUM_DEVICES) {
                    if (key[6] == 'f') persisted_tuner_freq[ch].store(static_cast<uint32_t>(std::stoul(val)));
                    else persisted_tuner_gain[ch].store(std::stoi(val));
                }
            } catch (...) {
                std::cerr << "Settings: invalid " << key << " '" << val << "'" << std::endl;
            }
        } else if (key == "num_elements") {
            try {
                const int n = std::stoi(val);
                // Range-checked against the serial list in main() (which runs
                // after arg parsing has set expected_serials); only stash it here.
                persisted_num_elements.store(n, std::memory_order_release);
                std::cout << "Settings: num_elements = " << n << std::endl;
            } catch (...) {
                std::cerr << "Settings: invalid num_elements '" << val << "'" << std::endl;
            }
        } else if (key == "antenna_bias_tee_mask") {
            try {
                const uint32_t mask = static_cast<uint32_t>(std::stoul(val)) & ((1u << NUM_DEVICES) - 1);
                // Only the mask is stored here; the GPIOs are driven once the
                // devices are open (init_all_rtlsdr_devices).
                antenna_bias_tee_mask.store(mask, std::memory_order_release);
                std::cout << "Settings: antenna_bias_tee_mask = " << mask << std::endl;
            } catch (...) {
                std::cerr << "Settings: invalid antenna_bias_tee_mask '" << val << "'" << std::endl;
            }
        } else if (key == "forward_comp_enabled") {
            const bool on = parse_bool(val);
            forward_comp.enabled.store(on, std::memory_order_release);
            std::cout << "Settings: forward_comp_enabled = " << (on ? "ON" : "OFF") << std::endl;
        } else if (key == "forward_comp_amplitude") {
            forward_comp.correct_amplitude.store(parse_bool(val), std::memory_order_release);
        } else if (key.rfind("forward_comp_ch", 0) == 0) {
            // forward_comp_ch<N>=<filename> ; empty value clears the channel.
            try {
                const int ch = std::stoi(key.substr(std::string("forward_comp_ch").size()));
                std::string err;
                if (val.empty()) {
                    fwdcomp::set_channel_file(ch, "", err);
                } else if (fwdcomp::set_channel_file(ch, val, err)) {
                    std::cout << "Settings: forward_comp_ch" << ch << " = " << val << std::endl;
                } else {
                    std::cerr << "Settings: forward comp ch" << ch << " '" << val << "': " << err << std::endl;
                }
            } catch (...) {
                std::cerr << "Settings: bad forward comp key '" << key << "'" << std::endl;
            }
        }
        // Future persisted keys go here.
    }

    // Build the forward-comp correction from whatever was loaded (uses the
    // compiled-in center frequency for now; it is re-interpolated on the first
    // and every subsequent retune via update_sdr_settings()).
    if (forward_comp.enabled.load(std::memory_order_relaxed))
        fwdcomp::recompute(static_cast<double>(CENTER_FREQ));
}

void save() {
    std::lock_guard<std::mutex> lk(g_settings_mutex);

    std::ostringstream f;
    f << "# Heimdall runtime settings (auto-generated; edit values, keep keys)\n";
    f << "per_bin_eq=" << (per_bin_cal.enabled.load(std::memory_order_acquire) ? 1 : 0) << "\n";
    f << "periodic_recal_enabled=" << (periodic_recal_enabled.load(std::memory_order_acquire) ? 1 : 0) << "\n";
    f << "periodic_recal_minutes=" << periodic_recal_minutes.load(std::memory_order_acquire) << "\n";
    f << "antenna_bias_tee_mask=" << antenna_bias_tee_mask.load(std::memory_order_acquire) << "\n";
    // The user's saved CHOICE, not the live count: a count from -n or
    // --kerberos only applies to this run and must not replace it the first
    // time some other setting is saved. reconfigure_num_elements() updates
    // persisted_num_elements when the user picks a count. 0 = never chosen.
    if (const int n = persisted_num_elements.load(std::memory_order_acquire); n >= 2)
        f << "num_elements=" << n << "\n";
    // The last USER tuning (note_tuning), not current_frequency: a discrete
    // scan's hop must not become the startup frequency
    if (const uint64_t fr = persisted_frequency.load(std::memory_order_acquire); fr > 0)
        f << "center_freq=" << fr << "\n";
    if (const int g = persisted_gain.load(std::memory_order_acquire); g != -999)
        f << "gain=" << g << "\n";
    {
        const int m = persisted_mode.load(std::memory_order_acquire);
        f << "operating_mode=" << operating_mode_name(static_cast<OperatingMode>(m)) << "\n";
        if (m == static_cast<int>(OperatingMode::INDEPENDENT)) {
            for (int i = 0; i < NUM_DEVICES; i++) {
                if (const uint32_t tf = persisted_tuner_freq[i].load(std::memory_order_acquire); tf > 0)
                    f << "tuner_freq" << i << "=" << tf << "\n";
                if (const int tg = persisted_tuner_gain[i].load(std::memory_order_acquire); tg != -999)
                    f << "tuner_gain" << i << "=" << tg << "\n";
            }
        }
    }
    f << "forward_comp_enabled=" << (forward_comp.enabled.load(std::memory_order_acquire) ? 1 : 0) << "\n";
    f << "forward_comp_amplitude=" << (forward_comp.correct_amplitude.load(std::memory_order_acquire) ? 1 : 0) << "\n";
    {
        // forward_comp.mutex guards files[]; this nesting (g_settings_mutex ->
        // forward_comp.mutex) is the only ordering between the two locks, so it
        // cannot deadlock (set_channel_file / recompute never take g_settings_mutex).
        std::lock_guard<std::mutex> lk2(forward_comp.mutex);
        for (int i = 0; i < ForwardCompensation::N; ++i)
            f << "forward_comp_ch" << i << "=" << forward_comp.files[i] << "\n";
    }
    // Written atomically: truncating the live file in place meant a power cut
    // mid-save silently reset the element count, bias tees and forward comp.
    if (!write_file_atomic(FILE_PATH, f.str())) {
        std::cerr << "Settings: failed to write " << FILE_PATH << " (" << std::strerror(errno)
                  << ") - previous settings kept" << std::endl;
    }
}

}  // namespace settings
