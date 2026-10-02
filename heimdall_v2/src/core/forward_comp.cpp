#include "forward_comp.hpp"
#include "config.hpp"
#include "utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

// Global instance (declared extern in types.hpp). Defined here, with its code,
// rather than in main.cpp.
ForwardCompensation forward_comp;

namespace {

// Interpolate a frequency-sorted (freq_Hz, S21) table at f (Hz). Linear in
// magnitude and UNWRAPPED phase (so a near-180-degree step between adjacent
// VNA points doesn't fold), clamped to the endpoints outside the measured
// range. Returns identity (1,0) for an empty table.
std::complex<double> interp_table(
    const std::vector<std::pair<double, std::complex<double>>>& t, double f) {
    if (t.empty()) return {1.0, 0.0};
    if (f <= t.front().first) return t.front().second;
    if (f >= t.back().first)  return t.back().second;

    const auto it = std::lower_bound(
        t.begin(), t.end(), f,
        [](const std::pair<double, std::complex<double>>& p, double v) { return p.first < v; });
    const size_t hi = static_cast<size_t>(it - t.begin());
    const size_t lo = hi - 1;

    const double f0 = t[lo].first, f1 = t[hi].first;
    const double w = (f1 > f0) ? (f - f0) / (f1 - f0) : 0.0;
    const std::complex<double> a = t[lo].second, b = t[hi].second;

    const double ma = std::abs(a), mb = std::abs(b);
    double pa = std::arg(a), pb = std::arg(b);
    while (pb - pa >  M_PI) pb -= 2.0 * M_PI;   // unwrap b relative to a
    while (pb - pa < -M_PI) pb += 2.0 * M_PI;
    return std::polar(ma + (mb - ma) * w, pa + (pb - pa) * w);
}

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A bare filename safe to resolve inside CAL_DIR: non-empty, no path separators
// and no ".." (defends against traversal / absolute paths from the web layer).
// Also valid UTF-8 without control characters: the name is saved and echoed
// in every web STATE message, and one invalid byte there makes every browser
// drop its WebSocket - for good, since the setting persists across restarts.
bool valid_basename(const std::string& filename, std::string& err) {
    if (filename.empty()) { err = "empty filename"; return false; }
    if (filename.size() > 255) { err = "filename too long"; return false; }
    if (!utf8_valid(filename) ||
        std::any_of(filename.begin(), filename.end(),
                    [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; })) {
        err = "invalid filename";
        return false;
    }
    // The settings file trims values, so such a name wouldn't survive a restart
    if (filename.front() == ' ' || filename.back() == ' ') {
        err = "filename can't start or end with a space";
        return false;
    }
    if (filename.find('/')  != std::string::npos ||
        filename.find('\\') != std::string::npos ||
        filename.find("..") != std::string::npos) {
        err = "invalid filename";
        return false;
    }
    return true;
}

// Port count from a Touchstone v1 extension (case-insensitive): 1 for .s1p,
// 2 for .s2p, 0 otherwise. The extension is what defines the port count.
int touchstone_ports(const std::string& filename) {
    const std::string e = upper(std::filesystem::path(filename).extension().string());
    return e == ".S2P" ? 2 : e == ".S1P" ? 1 : 0;
}

// True if the filename ends in .s2p or .s1p (case-insensitive).
bool has_touchstone_ext(const std::string& filename) {
    return touchstone_ports(filename) != 0;
}

// Parse a Touchstone stream (shared by the file and uploaded-content paths).
// `ports` (1 or 2) comes from the file extension.
bool parse_stream(std::istream& f, int ports,
                  std::vector<std::pair<double, std::complex<double>>>& out,
                  std::string& err) {
    out.clear();

    double freq_scale = 1e9;   // Touchstone default frequency unit is GHz
    char   format     = 'M';   // 'R' (RI), 'M' (MA), 'D' (DB); default MA
    std::vector<double> nums;  // all numeric data tokens, in file order

    std::string line;
    while (std::getline(f, line)) {
        // Strip '!' comments (anywhere on the line).
        if (auto bang = line.find('!'); bang != std::string::npos) line.erase(bang);
        // Option line "# <unit> S <format> R <z0>": parse units/format, ignore rest.
        if (auto hash = line.find('#'); hash != std::string::npos) {
            std::istringstream os(line.substr(hash + 1));
            std::string tok;
            while (os >> tok) {
                const std::string u = upper(tok);
                if      (u == "HZ")  freq_scale = 1.0;
                else if (u == "KHZ") freq_scale = 1e3;
                else if (u == "MHZ") freq_scale = 1e6;
                else if (u == "GHZ") freq_scale = 1e9;
                else if (u == "RI")  format = 'R';
                else if (u == "MA")  format = 'M';
                else if (u == "DB")  format = 'D';
                // "S", "R", the impedance value, etc. are ignored.
            }
            continue;
        }
        std::istringstream ds(line);
        double v;
        while (ds >> v) nums.push_back(v);
    }

    if (nums.empty()) { err = "no data points (not a Touchstone file)"; return false; }

    // Columns per frequency come from the port count (the extension), not a
    // guess from the token count: a 1-port file whose point count was a
    // multiple of 3 has a multiple of 9 tokens and was misread as 2-port.
    // 2-port is 9 values/point (freq + 4 S-params * 2), using S21; 1-port is
    // 3 (freq + S11 * 2), using S11 with a warning.
    int stride, s_off;
    if (ports == 2) {
        stride = 9; s_off = 3;   // S21 = columns 3,4 (0-based: f S11r S11i S21r S21i ...)
    } else if (ports == 1) {
        stride = 3; s_off = 1;   // S11 = columns 1,2
        std::cerr << "Forward comp: 1-port (.s1p) file; using S11 "
                     "(reflection). S2P/S21 is recommended for through cal." << std::endl;
    } else {
        err = "filename must end in .s2p or .s1p";
        return false;
    }
    if (nums.size() % static_cast<size_t>(stride) != 0) {
        err = "value count does not match a " + std::to_string(ports) +
              "-port Touchstone (" + std::to_string(stride) + " values per frequency)";
        return false;
    }

    out.reserve(nums.size() / static_cast<size_t>(stride));
    for (size_t i = 0; i + static_cast<size_t>(stride) <= nums.size(); i += static_cast<size_t>(stride)) {
        const double freq = nums[i] * freq_scale;
        const double a = nums[i + s_off];      // real / mag / dB
        const double b = nums[i + s_off + 1];  // imag / angle(deg) / angle(deg)
        std::complex<double> s;
        if      (format == 'R') s = std::complex<double>(a, b);
        else if (format == 'D') s = std::polar(std::pow(10.0, a / 20.0), b * M_PI / 180.0);
        else                    s = std::polar(a, b * M_PI / 180.0);  // MA
        out.emplace_back(freq, s);
    }

    std::sort(out.begin(), out.end(),
              [](const std::pair<double, std::complex<double>>& x,
                 const std::pair<double, std::complex<double>>& y) { return x.first < y.first; });
    if (out.empty()) { err = "parsed zero points"; return false; }
    return true;
}

}  // namespace

namespace fwdcomp {

bool parse_touchstone(const std::string& path,
                      std::vector<std::pair<double, std::complex<double>>>& out,
                      std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot open " + path; return false; }
    return parse_stream(f, touchstone_ports(path), out, err);
}

bool set_channel_file(int ch, const std::string& filename, std::string& err) {
    if (ch < 0 || ch >= ForwardCompensation::N) { err = "channel out of range"; return false; }

    std::lock_guard<std::mutex> lk(forward_comp.mutex);

    if (filename.empty()) {
        forward_comp.files[ch].clear();
        forward_comp.tables[ch].clear();
        forward_comp.loaded[ch] = false;
        return true;
    }
    // A bare filename only - it must live inside CAL_DIR (no traversal / abs path).
    if (!valid_basename(filename, err)) return false;

    const std::string p = std::string(CAL_DIR) + "/" + filename;
    std::vector<std::pair<double, std::complex<double>>> tbl;
    forward_comp.files[ch] = filename;   // record the selection regardless of parse result
    if (!parse_touchstone(p, tbl, err)) {
        forward_comp.tables[ch].clear();
        forward_comp.loaded[ch] = false;
        return false;
    }
    forward_comp.tables[ch] = std::move(tbl);
    forward_comp.loaded[ch] = true;
    return true;
}

void recompute(double fc_hz) {
    std::lock_guard<std::mutex> lk(forward_comp.mutex);
    const bool amp = forward_comp.correct_amplitude.load(std::memory_order_relaxed);

    // Reference channel through-response (identity if the ref has no file, which
    // makes the correction each channel's own absolute response).
    std::complex<double> sref(1.0, 0.0);
    if (forward_comp.loaded[REF_CHANNEL])
        sref = interp_table(forward_comp.tables[REF_CHANNEL], fc_hz);

    for (int k = 0; k < ForwardCompensation::N; ++k) {
        std::complex<double> corr(1.0, 0.0);
        if (forward_comp.loaded[k]) {
            const std::complex<double> sk = interp_table(forward_comp.tables[k], fc_hz);
            if (std::abs(sk) > 1e-12) corr = sref / sk;   // divide out the path
            if (!amp) {
                const double m = std::abs(corr);
                if (m > 1e-12) corr /= m;                 // phase-only: unit magnitude
            }
        }
        forward_comp.vector.store(k,
            std::complex<float>(static_cast<float>(corr.real()),
                                static_cast<float>(corr.imag())));
    }
    forward_comp.ready.store(true, std::memory_order_release);
}

std::vector<std::string> list_files() {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    std::error_code ec;

    fs::create_directories(CAL_DIR, ec);   // best-effort; ignore failure
    if (fs::is_directory(CAL_DIR, ec)) {
        for (fs::directory_iterator it(CAL_DIR, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec)) continue;
            const std::string ext = upper(it->path().extension().string());
            if (ext == ".S2P" || ext == ".S1P")
                files.push_back(it->path().filename().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool save_uploaded_file(const std::string& filename,
                        const std::string& content, std::string& err) {
    namespace fs = std::filesystem;
    if (!valid_basename(filename, err)) return false;
    if (!has_touchstone_ext(filename)) { err = "filename must end in .s2p or .s1p"; return false; }

    // Validate by parsing the uploaded bytes BEFORE writing anything to disk, so
    // a non-Touchstone upload is rejected instead of littering the folder.
    {
        std::istringstream ss(content);
        std::vector<std::pair<double, std::complex<double>>> tbl;
        if (!parse_stream(ss, touchstone_ports(filename), tbl, err)) return false;
    }

    std::error_code ec;
    fs::create_directories(CAL_DIR, ec);   // best-effort
    const std::string p = std::string(CAL_DIR) + "/" + filename;
    std::ofstream of(p, std::ios::binary | std::ios::trunc);
    if (!of) { err = "cannot write " + p; return false; }
    of.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!of) { err = "write failed for " + p; return false; }
    return true;
}

bool delete_file(const std::string& filename, std::string& err) {
    namespace fs = std::filesystem;
    if (!valid_basename(filename, err)) return false;

    const std::string p = std::string(CAL_DIR) + "/" + filename;
    std::error_code ec;
    if (!fs::exists(p, ec)) { err = "no such file"; return false; }
    if (!fs::remove(p, ec) || ec) { err = "delete failed"; return false; }

    // Drop the file from any channel that referenced it; recompute() (called by
    // the caller) then reverts those channels to identity.
    std::lock_guard<std::mutex> lk(forward_comp.mutex);
    for (int k = 0; k < ForwardCompensation::N; ++k) {
        if (forward_comp.files[k] == filename) {
            forward_comp.files[k].clear();
            forward_comp.tables[k].clear();
            forward_comp.loaded[k] = false;
        }
    }
    return true;
}

}  // namespace fwdcomp
