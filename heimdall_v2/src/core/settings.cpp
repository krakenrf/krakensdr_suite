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

void load() {
    std::lock_guard<std::mutex> lk(g_settings_mutex);

    std::ifstream f(FILE_PATH);
    if (!f) {
        std::cout << "Settings: no " << FILE_PATH << " found; using defaults (per-bin EQ OFF)" << std::endl;
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);  // strip comments
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty()) continue;

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
    f << "num_elements=" << active_num_elements.load(std::memory_order_acquire) << "\n";
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
