#include "downconverter.hpp"
#include "../core/types.hpp"  // fatal_exit
#include "../../config.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/hidraw.h>
#elif defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <system_error>
#include <vector>
#endif

Downconverter downconverter;

// ---- Board MCU USB HID protocol (moRFeus-compatible) ----
// 17-byte output report: [report id 0][command][address][value, 8 bytes
// big-endian][6 zero bytes]. The MCU (EFM8UB10, fw-krakendown) programs the
// RFFC5071 mixer/synthesizer itself; the host only sends absolute values.
// Every command is answered with a 16-byte input report [command][address]
// [8 data bytes][...], or with the text "Invalid param." for an address the
// firmware does not know.
//   'r' (114) read   - never touches Flash on fw >= 20261109 (older firmware
//                      saved its settings on EVERY read: no polling there)
//   's' (115) set    - RAM + hardware only
//   'w' (119) write  - set AND save as power-on default to the settings Flash
//                      (a ring of 8 x 64-byte pages, ~160k saves in the life
//                      of the board): used once at a clean exit, never per retune
// Malformed frames wedge the MCU's parser until a power cycle - reports are
// always exactly 17 bytes with the leading 0 report id.
namespace {

[[maybe_unused]] constexpr uint16_t MORFEUS_VID = 0x10C4;   // Silicon Labs
[[maybe_unused]] constexpr uint16_t MORFEUS_PID = 0xEAC9;   // USBXpress (moRFeus)

constexpr uint8_t CMD_READ = 'r';
constexpr uint8_t CMD_SET  = 's';
constexpr uint8_t CMD_SAVE = 'w';

constexpr uint8_t FUNC_FREQUENCY = 129;    // value = LO in Hz
constexpr uint8_t FUNC_CURRENT   = 131;    // LO output drive current, 0..7
constexpr uint8_t FUNC_FW        = 136;    // r: low word date code, high word build
// Stock-moRFeus function 130 (mixer/generator mode) is deliberately NOT used:
// the WB board's firmware doesn't implement it (its output is always on).
constexpr uint8_t ADDR_INA219 = 80;        // data[0] = register (1 shunt, 2 bus)
constexpr uint8_t ADDR_SHT4X  = 81;        // data[0] = SHT4x command (0xFD)
constexpr uint8_t ADDR_SI70XX = 82;        // data[0] = Si70xx command (0xF5)

constexpr size_t REPLY_LEN = 16;

enum class Reply { OK, INVALID, TIMEOUT, IOERR };

std::mutex mrf_mutex;  // one owner of the HID device: retunes, saves, the telemetry poller

// What the 's' path does when the firmware rejects 's' (never seen; kept so
// a board with unexpected firmware still tunes): fall back to 'w'.
std::atomic<bool> set_falls_back_to_save{false};
std::atomic<int64_t> fw_date{-1};
std::atomic<uint32_t> fw_build{0};
std::atomic<uint32_t> no_reply_count{0};

using Clock = std::chrono::steady_clock;

int ms_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

void build_report(uint8_t report[17], uint8_t cmd, uint8_t addr, uint64_t value) {
    std::memset(report, 0, 17);
    report[0] = 0;        // HID report id (unnumbered)
    report[1] = cmd;
    report[2] = addr;
    for (int i = 0; i < 8; i++) report[3 + i] = static_cast<uint8_t>(value >> (8 * (7 - i)));  // big-endian
}

#ifdef __linux__
int mrf_fd = -1;

// Find the board MCU among /dev/hidraw* by VID/PID. Caller holds mrf_mutex.
bool hid_open_locked() {
    DIR* dir = opendir("/dev");
    if (!dir) return false;

    int found_fd = -1;
    bool saw_permission_error = false;
    while (dirent* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "hidraw", 6) != 0) continue;

        std::string path = std::string("/dev/") + entry->d_name;
        int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES) saw_permission_error = true;
            continue;
        }

        hidraw_devinfo info{};
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0 &&
            static_cast<uint16_t>(info.vendor) == MORFEUS_VID &&
            static_cast<uint16_t>(info.product) == MORFEUS_PID) {
            std::cout << "Downconverter: board MCU (moRFeus protocol) found at " << path << std::endl;
            found_fd = fd;
            break;
        }
        close(fd);
    }
    closedir(dir);

    if (found_fd < 0) {
        std::cerr << "Downconverter: no moRFeus LO found on USB (VID 0x10C4 PID 0xEAC9)";
        if (saw_permission_error) {
            std::cerr << " - some /dev/hidraw* nodes were not readable; run as root or add a"
                         " udev rule granting access to VID 10c4 PID eac9";
        }
        std::cerr << std::endl;
        return false;
    }
    mrf_fd = found_fd;
    return true;
}

bool hid_is_open() { return mrf_fd >= 0; }

bool hid_write_locked(const uint8_t report[17], std::string& err) {
    const ssize_t n = write(mrf_fd, report, 17);
    if (n == 17) return true;
    err = n < 0 ? strerror(errno) : "short write";
    return false;
}

// One input report into out[REPLY_LEN]: 1 = got one, 0 = timeout, -1 = error
int hid_read_locked(uint8_t* out, int timeout_ms) {
    pollfd p{mrf_fd, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    uint8_t buf[64];
    const ssize_t n = read(mrf_fd, buf, sizeof(buf));
    if (n <= 0) return n == 0 ? 0 : -1;
    // unnumbered reports arrive without the report id; tolerate one anyway
    // (a reply never starts with 0: byte 0 is the command or 'I')
    const uint8_t* src = buf;
    size_t len = static_cast<size_t>(n);
    if (len > REPLY_LEN && buf[0] == 0) { src++; len--; }
    std::memset(out, 0, REPLY_LEN);
    std::memcpy(out, src, std::min(len, REPLY_LEN));
    return 1;
}

// Throw away replies nobody waited for (the old driver never read them, and
// hidraw keeps up to 64 reports)
void hid_drain_locked() {
    uint8_t junk[REPLY_LEN];
    for (int i = 0; i < 256 && hid_read_locked(junk, 0) == 1; i++) {}
}

void hid_close_locked() {
    if (mrf_fd >= 0) {
        close(mrf_fd);
        mrf_fd = -1;
    }
}

#elif defined(_WIN32)
// Windows: the board MCU through Windows' own HID class driver (SetupAPI +
// hid.dll) - no extra library or driver. It must keep that driver: Zadig's
// "List All Devices" shows it as well, and with WinUSB instead it is no
// longer a HID device. Reads and writes are overlapped with a time limit: a
// wedged board must not hang a retune holding device_io_mutex.
HANDLE mrf_handle = INVALID_HANDLE_VALUE;
USHORT mrf_report_len = 0;   // the interface's output report length, incl. the report id
USHORT mrf_input_len = 0;    // input report length, incl. the report id

std::string win_err(DWORD e) { return std::system_category().message(static_cast<int>(e)); }

// Find the MCU among the present HID interfaces by VID/PID and open it.
// Caller holds mrf_mutex.
bool hid_open_locked() {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        std::cerr << "Downconverter: cannot list the HID devices (" << win_err(GetLastError()) << ")" << std::endl;
        return false;
    }
    bool busy = false;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; mrf_handle == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<unsigned char> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;

        // No access rights asked for: the attributes can be read even from a
        // device another program holds (keyboards, mice: the system does)
        HANDLE q = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        const bool match = HidD_GetAttributes(q, &attr) && attr.VendorID == MORFEUS_VID && attr.ProductID == MORFEUS_PID;
        CloseHandle(q);
        if (!match) continue;

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION) busy = true;
            continue;
        }
        // 17 = report id + 16 data bytes; a longer report is zero-padded
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
        }
        if (caps.OutputReportByteLength < 17) {
            CloseHandle(h);   // another collection of the device (no 17-byte output report)
            continue;
        }
        mrf_handle = h;
        mrf_report_len = caps.OutputReportByteLength;
        mrf_input_len = caps.InputReportByteLength;
        std::cout << "Downconverter: board MCU (moRFeus protocol) found (HID, " << mrf_report_len << "-byte output / "
                  << mrf_input_len << "-byte input report)" << std::endl;
    }
    SetupDiDestroyDeviceInfoList(set);

    if (mrf_handle == INVALID_HANDLE_VALUE) {
        std::cerr << "Downconverter: no moRFeus LO found on USB (VID 0x10C4 PID 0xEAC9)";
        if (busy) {
            std::cerr << " - it is open in another program (moRFeusQt, the manufacturing test?); close that program";
        } else {
            std::cerr << " - it uses Windows' built-in HID driver: do NOT replace that driver with WinUSB in"
                         " Zadig (if it was: Device Manager, uninstall the device incl. its driver, replug)";
        }
        std::cerr << std::endl;
        return false;
    }
    return true;
}

bool hid_is_open() { return mrf_handle != INVALID_HANDLE_VALUE; }

// One overlapped transfer with a time limit; cancelled (and waited for: the
// buffer and OVERLAPPED must outlive the I/O) on timeout.
// 1 = done (*n bytes), 0 = timeout, -1 = error (*err)
int overlapped_io(bool is_write, void* buf, DWORD len, int timeout_ms, DWORD* n, DWORD* err) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        *err = GetLastError();
        return -1;
    }
    *n = 0;
    int result = -1;
    const BOOL started = is_write ? WriteFile(mrf_handle, buf, len, n, &ov) : ReadFile(mrf_handle, buf, len, n, &ov);
    if (started) {
        result = 1;
    } else {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0) {
                if (GetOverlappedResult(mrf_handle, &ov, n, FALSE)) result = 1;
                else *err = GetLastError();
            } else {
                CancelIoEx(mrf_handle, &ov);
                if (GetOverlappedResult(mrf_handle, &ov, n, TRUE)) result = 1;   // completed meanwhile
                else result = 0;
            }
        } else {
            *err = e;
        }
    }
    CloseHandle(ov.hEvent);
    return result;
}

bool hid_write_locked(const uint8_t report[17], std::string& err) {
    std::vector<uint8_t> buf(mrf_report_len, 0);
    std::memcpy(buf.data(), report, 17);   // 17 <= mrf_report_len (checked at open)
    DWORD n = 0, e = 0;
    const int r = overlapped_io(true, buf.data(), static_cast<DWORD>(buf.size()), 1000, &n, &e);
    if (r == 1 && n == buf.size()) return true;
    err = r == 0 ? "timeout" : r == 1 ? "short write" : win_err(e);
    return false;
}

int hid_read_locked(uint8_t* out, int timeout_ms) {
    std::vector<uint8_t> buf(std::max<USHORT>(mrf_input_len, 17), 0);
    DWORD n = 0, e = 0;
    const int r = overlapped_io(false, buf.data(), static_cast<DWORD>(buf.size()), timeout_ms, &n, &e);
    if (r != 1) return r;
    if (n < 2) return 0;
    std::memset(out, 0, REPLY_LEN);
    std::memcpy(out, buf.data() + 1, std::min<size_t>(n - 1, REPLY_LEN));   // buf[0] = report id
    return 1;
}

void hid_drain_locked() { HidD_FlushQueue(mrf_handle); }

void hid_close_locked() {
    if (mrf_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(mrf_handle);
        mrf_handle = INVALID_HANDLE_VALUE;
    }
}

#else
// No HID backend on this platform: --wideband reports the LO missing
bool hid_open_locked() {
    std::cerr << "Downconverter: moRFeus control is not supported on this platform" << std::endl;
    return false;
}
bool hid_is_open() { return false; }
bool hid_write_locked(const uint8_t*, std::string& err) { err = "unsupported"; return false; }
int hid_read_locked(uint8_t*, int) { return -1; }
void hid_drain_locked() {}
void hid_close_locked() {}
#endif

// One command and its reply. Opens the device if needed and reopens it once
// after an I/O error (survives a USB replug); stale replies (and the version
// string some commands get) are skipped until the one matching cmd/addr, or
// "Invalid param.", arrives. Caller holds mrf_mutex.
Reply query_locked(uint8_t cmd, uint8_t addr, uint64_t value, uint8_t* reply, int timeout_ms) {
    uint8_t report[17];
    build_report(report, cmd, addr, value);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!hid_is_open()) {
            if (!hid_open_locked()) return Reply::IOERR;
            hid_drain_locked();
        }
        std::string err;
        if (!hid_write_locked(report, err)) {
            std::cerr << "Downconverter: HID write failed (" << err << ") - reopening device" << std::endl;
            hid_close_locked();
            continue;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            uint8_t r[REPLY_LEN];
            const int got = hid_read_locked(r, ms_left(deadline));
            if (got < 0) {
                std::cerr << "Downconverter: HID read failed - reopening device" << std::endl;
                hid_close_locked();
                return Reply::IOERR;   // the command went out; don't send it twice
            }
            if (got == 0) {
                no_reply_count.fetch_add(1, std::memory_order_relaxed);
                return Reply::TIMEOUT;
            }
            if (std::memcmp(r, "Invalid param", 13) == 0) {
                if (reply) std::memcpy(reply, r, REPLY_LEN);
                return Reply::INVALID;
            }
            if (r[0] == cmd && r[1] == addr) {
                if (reply) std::memcpy(reply, r, REPLY_LEN);
                return Reply::OK;
            }
            if (ms_left(deadline) == 0) {
                no_reply_count.fetch_add(1, std::memory_order_relaxed);
                return Reply::TIMEOUT;
            }
        }
    }
    return Reply::IOERR;
}

uint64_t be_u64(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

// Set a local parameter in RAM ('s'). A missing reply is not fatal (the
// command went out; it is counted and shows in the status), a rejected 's'
// falls back to 'w' for the rest of the run.
bool mrf_set(uint8_t func, uint64_t value) {
    std::lock_guard<std::mutex> lock(mrf_mutex);
    const uint8_t cmd = set_falls_back_to_save.load() ? CMD_SAVE : CMD_SET;
    uint8_t r[REPLY_LEN];
    switch (query_locked(cmd, func, value, r, WB_HID_REPLY_MS)) {
    case Reply::OK:
        return true;
    case Reply::TIMEOUT: {
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 3)
            std::cerr << "Downconverter: no reply to '" << static_cast<char>(cmd) << "' " << int(func)
                      << " within " << WB_HID_REPLY_MS << " ms (sent; continuing)" << std::endl;
        return true;
    }
    case Reply::INVALID:
        if (cmd == CMD_SET) {
            std::cerr << "Downconverter: firmware rejects 's' - falling back to 'w' (Flash save per retune!)."
                         " Update the board firmware to fw20261110.3 or later." << std::endl;
            set_falls_back_to_save = true;
            return query_locked(CMD_SAVE, func, value, nullptr, WB_HID_REPLY_MS) != Reply::IOERR;
        }
        return false;
    case Reply::IOERR:
    default:
        return false;
    }
}

bool sensors_allowed() {
    const int64_t d = fw_date.load();
    return d >= WB_MIN_SENSOR_FW;
}

// ---- telemetry ----

enum class Level : int { OK = 0, WARN = 1, ALARM = 2 };
const char* level_name(Level l) { return l == Level::ALARM ? "alarm" : l == Level::WARN ? "warn" : "ok"; }

// Alarm state with hysteresis: rises at once, clears one level only after
// the value has been back inside the limit minus `hyst` for 30 s.
struct AlarmState {
    Level level = Level::OK;
    Clock::time_point inside_since{};
    bool inside = false;
    // raw = level by the thresholds; raw_hyst = level by the thresholds moved
    // inward by the hysteresis band (the stricter test for clearing)
    bool update(Level raw, Level raw_hyst, Clock::time_point now) {
        const Level before = level;
        if (raw > level) {
            level = raw;
            inside = false;
        } else if (raw_hyst < level) {
            if (!inside) { inside = true; inside_since = now; }
            if (now - inside_since >= std::chrono::seconds(30)) {
                level = raw_hyst;
                inside = false;
            }
        } else {
            inside = false;
        }
        return level != before;
    }
};

struct Telemetry {
    // board identification (once at start, retried while unknown)
    std::string revision = "unknown";   // "3v0" / "3v2" / "unknown"
    std::string sensor;                 // "Si7020" / "SHT4x" / ""
    int r_shunt_mohm = 0;               // 0 = not known yet
    bool shunt_consistent = true;
    // last good values and when they were read (unix seconds; 0 = never)
    double vin_v = 0, iin_a = 0, vshunt_mv = 0;
    bool iin_saturated = false;
    double power_ts = 0;
    double temp_c = 0, rh_pct = 0;
    double env_ts = 0;
    uint64_t lo_hz_read = 0;
    double ts = 0;                      // last poll cycle
    uint32_t errors = 0;                // failed reads since start
    int power_fail_run = 0, env_fail_run = 0;
    bool power_stale = true, env_stale = true;
    AlarmState a_iin, a_vin, a_temp, a_rh, a_sensor;
};

std::mutex telem_mutex;
Telemetry telem;
std::atomic<bool> poller_started{false};
std::atomic<bool> poller_stop{false};

double unix_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

uint8_t crc8(const uint8_t* d, int n, uint8_t init) {
    uint8_t c = init;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 0x80) ? static_cast<uint8_t>((c << 1) ^ 0x31) : static_cast<uint8_t>(c << 1);
    }
    return c;
}

// One telemetry read. The poller never blocks a retune for longer than one
// query: it only try-locks, and gives up the cycle when a retune holds the device.
enum class Poll { OK, BUSY, FAIL };
Poll poll_query(uint8_t addr, uint8_t sub, uint8_t* r, int timeout_ms, bool echo_sub = true) {
    std::unique_lock<std::mutex> lock(mrf_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return Poll::BUSY;
    // data[0] = register / sensor command; the value goes MSB first
    const Reply rep = query_locked(CMD_READ, addr, static_cast<uint64_t>(sub) << 56, r, timeout_ms);
    if (rep != Reply::OK || (echo_sub && r[2] != sub)) return Poll::FAIL;
    return Poll::OK;
}

bool read_sht4x(double& t_c, double& rh) {
    uint8_t r[REPLY_LEN];
    if (poll_query(ADDR_SHT4X, 0xFD, r, 300) != Poll::OK) return false;
    if (crc8(r + 3, 2, 0xFF) != r[7] || crc8(r + 5, 2, 0xFF) != r[8]) return false;
    t_c = -45.0 + 175.0 * be_u64(r + 3, 2) / 65535.0;
    rh = -6.0 + 125.0 * be_u64(r + 5, 2) / 65535.0;
    return true;
}

bool read_si70xx(double& t_c, double& rh) {
    if (fw_date.load() < WB_MIN_SI70XX_FW) return false;
    uint8_t r[REPLY_LEN];
    if (poll_query(ADDR_SI70XX, 0xF5, r, 300) != Poll::OK) return false;
    if (crc8(r + 3, 2, 0x00) != r[7]) return false;   // the temperature word carries no CRC
    rh = 125.0 * be_u64(r + 3, 2) / 65536.0 - 6.0;
    t_c = 175.72 * be_u64(r + 5, 2) / 65536.0 - 46.85;
    return true;
}

// Shunt values that give a plausible board current (0.4-3.5 A): 10 mOhm ->
// 4-35 mV, 100 mOhm -> 40-350 mV; the windows cannot overlap.
int shunt_from_voltage(double vsh_mv) {
    const double a10 = vsh_mv / 10.0, a100 = vsh_mv / 100.0;
    const bool fits10 = a10 >= 0.4 && a10 <= 3.5, fits100 = a100 >= 0.4 && a100 <= 3.5;
    return fits10 == fits100 ? 0 : (fits10 ? 10 : 100);
}

void log_alarm(const char* what, const AlarmState& a, const std::string& value) {
    std::cout << "Downconverter telemetry: " << what << " " << level_name(a.level) << " (" << value << ")" << std::endl;
}

std::string fmt(double v, int prec) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", prec, v);
    return b;
}

void evaluate_alarms(Telemetry& t) {
    const auto now = Clock::now();
    // input current: about 2.8 A idle and 3.0 A with the noise source on (every
    // calibration), so warn from 3.3 A and alarm from 3.6 A. 3v0's INA219
    // saturates at 3.2 A: no warn level there, the alarm is "saturated".
    if (t.power_ts > 0) {
        auto lvl = [&](double h) {
            if (t.iin_saturated) return Level::ALARM;
            if (t.revision != "3v0" && t.iin_a > 3.6 - h) return Level::ALARM;
            return t.iin_a > 3.3 - h ? Level::WARN : Level::OK;
        };
        if (t.a_iin.update(lvl(0), lvl(0.1), now)) log_alarm("input current", t.a_iin, fmt(t.iin_a, 2) + " A");
        auto vl = [&](double h) {
            if (t.vin_v < 4.4 + h) return Level::ALARM;
            return t.vin_v < 4.6 + h ? Level::WARN : Level::OK;
        };
        if (t.a_vin.update(vl(0), vl(0.05), now)) log_alarm("input voltage", t.a_vin, fmt(t.vin_v, 2) + " V");
    }
    if (t.env_ts > 0) {
        auto tl = [&](double h) {
            if (t.temp_c > 70 - h) return Level::ALARM;
            return t.temp_c > 60 - h ? Level::WARN : Level::OK;
        };
        if (t.a_temp.update(tl(0), tl(3), now)) log_alarm("board temperature", t.a_temp, fmt(t.temp_c, 1) + " C");
        auto hl = [&](double h) { return t.rh_pct > 80 - h ? Level::WARN : Level::OK; };
        if (t.a_rh.update(hl(0), hl(3), now)) log_alarm("humidity", t.a_rh, fmt(t.rh_pct, 0) + " %RH");
    }
    const Level s = (t.power_stale || (t.env_stale && !t.sensor.empty())) ? Level::WARN : Level::OK;
    if (t.a_sensor.update(s, s, now)) log_alarm("sensor", t.a_sensor, t.power_stale ? "INA219 stale" : "RH sensor stale");
}

// Board revision: the humidity sensor that answers (SHT4x -> 3v2 with a
// 10 mOhm R6, Si7020 -> 3v0 with 100 mOhm), cross-checked with the shunt
// voltage like the manufacturing test does.
void identify_board(Telemetry& t, double vsh_mv) {
    double tc, rh;
    if (t.sensor.empty()) {
        if (read_sht4x(tc, rh)) t.sensor = "SHT4x";
        else if (read_si70xx(tc, rh)) t.sensor = "Si7020";
    }
    const std::string by_sensor = t.sensor == "SHT4x" ? "3v2" : t.sensor == "Si7020" ? "3v0" : "";
    const int by_shunt = shunt_from_voltage(vsh_mv);
    int shunt = WB_R_SHUNT_MOHM;
    if (shunt == 0) shunt = by_sensor == "3v2" ? 10 : by_sensor == "3v0" ? 100 : by_shunt;
    t.r_shunt_mohm = shunt;
    t.shunt_consistent = by_shunt == 0 || by_shunt == shunt;
    if (!by_sensor.empty() && t.shunt_consistent) t.revision = by_sensor;
    else if (by_sensor.empty() && shunt != 0) t.revision = shunt == 100 ? "3v0" : "3v2";
    else t.revision = "unknown";
}

void poller_main() {
    std::cout << "Downconverter: board telemetry polling every " << WB_TELEMETRY_PERIOD_MS << " ms" << std::endl;
    bool identified = false;
    auto next_ident = Clock::now();
    while (!poller_stop.load()) {
        const auto cycle_start = Clock::now();
        uint8_t r1[REPLY_LEN], r2[REPLY_LEN], rl[REPLY_LEN];
        const Poll p1 = poll_query(ADDR_INA219, 1, r1, WB_HID_REPLY_MS);
        const Poll p2 = p1 == Poll::OK ? poll_query(ADDR_INA219, 2, r2, WB_HID_REPLY_MS) : p1;
        double tc = 0, rh = 0;
        bool env_ok = false, env_tried = false, busy = p1 == Poll::BUSY || p2 == Poll::BUSY;
        const Poll pl = busy ? Poll::BUSY : poll_query(FUNC_FREQUENCY, 0, rl, WB_HID_REPLY_MS, false);

        double vsh_mv = 0;
        if (p1 == Poll::OK && p2 == Poll::OK) {
            const int16_t raw_sh = static_cast<int16_t>(be_u64(r1 + 3, 2));
            vsh_mv = raw_sh * 0.01;
            if (!identified && Clock::now() >= next_ident) {
                std::lock_guard<std::mutex> lk(telem_mutex);
                identify_board(telem, vsh_mv);
                identified = telem.revision != "unknown" || !telem.sensor.empty();
                next_ident = Clock::now() + std::chrono::seconds(10);
                std::cout << "Downconverter: board " << (telem.revision == "unknown" ? "revision unknown" : "Testunit_" + telem.revision)
                          << ", RH sensor " << (telem.sensor.empty() ? "none" : telem.sensor)
                          << ", R6 " << telem.r_shunt_mohm << " mOhm"
                          << (telem.shunt_consistent ? "" : " (shunt voltage disagrees - check the board)") << std::endl;
            }
        }
        std::string sensor;
        { std::lock_guard<std::mutex> lk(telem_mutex); sensor = telem.sensor; }
        if (!busy && !sensor.empty()) {
            env_tried = true;
            env_ok = sensor == "SHT4x" ? read_sht4x(tc, rh) : read_si70xx(tc, rh);
        }

        {
            std::lock_guard<std::mutex> lk(telem_mutex);
            Telemetry& t = telem;
            t.ts = unix_now();
            if (p1 == Poll::OK && p2 == Poll::OK) {
                const int16_t raw_sh = static_cast<int16_t>(be_u64(r1 + 3, 2));
                const uint16_t raw_bus = static_cast<uint16_t>(be_u64(r2 + 3, 2));
                t.vshunt_mv = raw_sh * 0.01;
                t.vin_v = (raw_bus >> 3) * 0.004;
                t.iin_saturated = std::abs(static_cast<int>(raw_sh)) >= 31990 || (raw_bus & 1);   // +-320 mV end / OVF
                t.iin_a = t.r_shunt_mohm > 0 ? t.vshunt_mv / t.r_shunt_mohm : 0.0;
                t.power_ts = t.ts;
                t.power_fail_run = 0;
                t.power_stale = false;
            } else if (!busy) {
                t.errors++;
                if (++t.power_fail_run >= 3) t.power_stale = true;
            }
            if (env_tried) {
                if (env_ok) {
                    t.temp_c = tc;
                    t.rh_pct = std::clamp(rh, 0.0, 100.0);
                    t.env_ts = t.ts;
                    t.env_fail_run = 0;
                    t.env_stale = false;
                } else {
                    t.errors++;
                    if (++t.env_fail_run >= 3) t.env_stale = true;
                }
            }
            if (pl == Poll::OK) t.lo_hz_read = be_u64(rl + 2, 8);
            evaluate_alarms(t);
        }

        const auto next = cycle_start + std::chrono::milliseconds(WB_TELEMETRY_PERIOD_MS);
        while (!poller_stop.load() && Clock::now() < next) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void start_poller() {
    if (poller_started.exchange(true)) return;
    std::thread(poller_main).detach();   // global state only; the process _Exits without joining
}

}  // namespace

int64_t downconverter_fw_date() { return fw_date.load(); }
uint32_t downconverter_fw_build() { return fw_build.load(); }

std::string downconverter_status_json_fields() {
    if (!downconverter.enabled.load()) return {};
    std::ostringstream j;
    const int64_t d = fw_date.load();
    j << ",\"fw\":{\"date\":" << d << ",\"build\":" << fw_build.load()
      << ",\"telemetry_ok\":" << (sensors_allowed() ? "true" : "false")
      << ",\"set_cmd\":\"" << (set_falls_back_to_save.load() ? "w" : "s") << "\""
      << ",\"no_reply\":" << no_reply_count.load() << "}";
    if (!poller_started.load()) {
        j << ",\"telemetry\":{\"enabled\":false,\"reason\":\""
          << (d < 0 ? "no firmware version reply" : "board firmware too old: update to fw20261110.3") << "\"}";
        return j.str();
    }
    std::lock_guard<std::mutex> lk(telem_mutex);
    const Telemetry& t = telem;
    auto num = [&](double v, int p) { return fmt(v, p); };
    j << ",\"telemetry\":{\"enabled\":true"
      << ",\"ts\":" << num(t.ts, 1)
      << ",\"revision\":\"" << t.revision << "\""
      << ",\"r_shunt_mohm\":" << t.r_shunt_mohm
      << ",\"shunt_consistent\":" << (t.shunt_consistent ? "true" : "false")
      << ",\"sensor\":\"" << t.sensor << "\"";
    if (t.power_ts > 0)
        j << ",\"vin_v\":" << num(t.vin_v, 3) << ",\"iin_a\":" << num(t.iin_a, 3)
          << ",\"vshunt_mv\":" << num(t.vshunt_mv, 2)
          << ",\"iin_saturated\":" << (t.iin_saturated ? "true" : "false")
          << ",\"power_ts\":" << num(t.power_ts, 1);
    if (t.env_ts > 0)
        j << ",\"temp_c\":" << num(t.temp_c, 2) << ",\"rh_pct\":" << num(t.rh_pct, 1)
          << ",\"env_ts\":" << num(t.env_ts, 1);
    if (t.lo_hz_read) j << ",\"lo_hz_read\":" << t.lo_hz_read;
    j << ",\"errors\":" << t.errors
      << ",\"stale\":" << ((t.power_stale || (t.env_stale && !t.sensor.empty())) ? "true" : "false")
      << ",\"power_stale\":" << (t.power_stale ? "true" : "false")
      << ",\"env_stale\":" << (t.env_stale ? "true" : "false")
      << ",\"alarms\":{\"iin\":\"" << level_name(t.a_iin.level) << "\",\"vin\":\"" << level_name(t.a_vin.level)
      << "\",\"temp\":\"" << level_name(t.a_temp.level) << "\",\"rh\":\"" << level_name(t.a_rh.level)
      << "\",\"sensor\":\"" << level_name(t.a_sensor.level) << "\"}}";
    return j.str();
}

std::string downconverter_telemetry_line() {
    if (!downconverter.enabled.load()) return {};
    const int64_t d = fw_date.load();
    std::string fw = d > 0 ? "fw" + std::to_string(d) + (fw_build.load() ? "." + std::to_string(fw_build.load()) : "")
                           : (d == 0 ? "fw < 20261109" : "fw ?");
    if (!poller_started.load()) return fw + "  telemetry off (firmware too old)";
    std::lock_guard<std::mutex> lk(telem_mutex);
    const Telemetry& t = telem;
    std::string s = (t.revision == "unknown" ? std::string("rev ?") : t.revision);
    if (t.power_ts > 0)
        s += "  " + fmt(t.vin_v, 2) + " V  " + (t.iin_saturated ? std::string("> 3.2") : fmt(t.iin_a, 2)) + " A";
    if (t.env_ts > 0) s += "  " + fmt(t.temp_c, 1) + " C  " + fmt(t.rh_pct, 0) + " %RH";
    if (t.power_stale || (t.env_stale && !t.sensor.empty())) s += "  STALE";
    return s + "  " + fw;
}

const char* mixer_side_name(MixerSide side) {
    switch (side) {
        case MixerSide::LOW:   return "low";
        case MixerSide::BELOW: return "below";
        default:               return "high";
    }
}
void downconverter_rf_range(MixerSide side, uint64_t& min_hz, uint64_t& max_hz) {
    switch (side) {
    case MixerSide::LOW:
        // LO = IF - RF: the synthesizer floor caps the RF (IF - RF >= LO_MIN).
        min_hz = WB_RF_MIN_HZ;
        max_hz = WB_VARIANT_IF_HZ - WB_LO_MIN_HZ;
        break;
    case MixerSide::BELOW:
        // LO = RF - IF: the whole LO span shifted up by the IF - the only
        // side whose ceiling exceeds LO_MAX.
        min_hz = WB_VARIANT_IF_HZ + WB_LO_MIN_HZ;
        max_hz = WB_VARIANT_IF_HZ + WB_LO_MAX_HZ;
        break;
    case MixerSide::HIGH:
    default:
        // LO = IF + RF must stay within [LO_MIN, LO_MAX]. IF + RF_MIN is
        // already far above LO_MIN, so only the top constrains; the floor is
        // the front end.
        min_hz = WB_RF_MIN_HZ;
        max_hz = WB_LO_MAX_HZ - WB_VARIANT_IF_HZ;
        break;
    }
}

bool downconverter_rf_valid(uint64_t rf_hz, MixerSide side) {
    uint64_t min_hz, max_hz;
    downconverter_rf_range(side, min_hz, max_hz);
    return rf_hz >= min_hz && rf_hz <= max_hz;
}

void downconverter_rf_union_range(uint64_t& min_hz, uint64_t& max_hz) {
    min_hz = WB_RF_MIN_HZ;                     // high/low floor (front end)
    max_hz = WB_VARIANT_IF_HZ + WB_LO_MAX_HZ;  // below-side ceiling
}

MixerSide downconverter_auto_side(uint64_t rf_hz) {
    return rf_hz <= WB_LO_MAX_HZ - WB_VARIANT_IF_HZ ? MixerSide::HIGH
                                                    : MixerSide::BELOW;
}

int wb_ring_for_rf(uint64_t rf_hz) {
    if (rf_hz < WB_RING_CENTER_MIN_HZ) return 0;  // outer
    if (rf_hz < WB_RING_INNER_MIN_HZ)  return 1;  // center
    return 2;                                     // inner
}

// Program the LO for rf_hz on `side`. The side is committed only after the LO
// write succeeds: it drives the spectral-inversion correction, so a side that
// changed without its LO would mirror the spectrum (and every DoA bearing).
// 's' only - RAM and hardware, no Flash save (one round trip, ~6 ms).
static bool program_lo(uint64_t rf_hz, MixerSide side) {
    if (!downconverter_rf_valid(rf_hz, side)) {
        uint64_t min_hz, max_hz;
        downconverter_rf_range(side, min_hz, max_hz);
        std::cerr << "Downconverter: RF " << rf_hz / 1e6 << " MHz outside "
                  << mixer_side_name(side) << "-side range ("
                  << min_hz / 1e6 << " - " << max_hz / 1e6 << " MHz)" << std::endl;
        return false;
    }

    uint64_t lo = 0;
    switch (side) {
        case MixerSide::LOW:   lo = WB_VARIANT_IF_HZ - rf_hz; break;
        case MixerSide::BELOW: lo = rf_hz - WB_VARIANT_IF_HZ; break;
        case MixerSide::HIGH:
        default:               lo = WB_VARIANT_IF_HZ + rf_hz; break;
    }
    if (!mrf_set(FUNC_FREQUENCY, lo)) {
        std::cerr << "Downconverter: failed to program LO to " << lo / 1e6 << " MHz" << std::endl;
        return false;
    }

    downconverter.lo_hz.store(lo);
    downconverter.side.store(side);
    std::cout << "Downconverter: RF " << rf_hz / 1e6 << " MHz -> LO " << lo / 1e6
              << " MHz (" << mixer_side_name(side) << " side, IF "
              << WB_VARIANT_IF_HZ / 1e6 << " MHz)" << std::endl;
    return true;
}

bool downconverter_set_side(MixerSide side, uint64_t rf_hz) {
    if (!downconverter_rf_valid(rf_hz, side)) {
        uint64_t min_hz, max_hz;
        downconverter_rf_range(side, min_hz, max_hz);
        std::cerr << "Downconverter: cannot switch to " << mixer_side_name(side)
                  << " side at RF " << rf_hz / 1e6 << " MHz (side covers "
                  << min_hz / 1e6 << " - " << max_hz / 1e6 << " MHz)" << std::endl;
        return false;
    }

    return program_lo(rf_hz, side);
}

bool downconverter_apply_rf(uint64_t rf_hz) {
    return program_lo(rf_hz, downconverter.side.load());
}

bool downconverter_set_current(int current) {
    if (current < 0 || current > 7) {
        std::cerr << "Downconverter: LO current " << current << " out of range (0-7)" << std::endl;
        return false;
    }
    if (!mrf_set(FUNC_CURRENT, static_cast<uint64_t>(current))) {
        std::cerr << "Downconverter: failed to set LO current " << current << std::endl;
        return false;
    }
    downconverter.lo_current.store(current);
    std::cout << "Downconverter: LO drive current set to " << current << " (0-7)" << std::endl;
    return true;
}

// 'r' 136: date code + build number. Old firmware answers "Invalid param."
// without side effects, so the probe is safe on every version.
static void probe_firmware() {
    std::lock_guard<std::mutex> lock(mrf_mutex);
    if (hid_is_open()) hid_drain_locked();   // replies nobody read (earlier run, moRFeusQt ...)
    uint8_t r[REPLY_LEN];
    const Reply rep = query_locked(CMD_READ, FUNC_FW, 0, r, 300);
    if (rep == Reply::OK) {
        fw_build.store(static_cast<uint32_t>(be_u64(r + 2, 4)));
        fw_date.store(static_cast<int64_t>(be_u64(r + 6, 4)));
        std::cout << "Downconverter: board firmware fw" << fw_date.load()
                  << (fw_build.load() ? "." + std::to_string(fw_build.load()) : std::string()) << std::endl;
    } else if (rep == Reply::INVALID) {
        fw_date.store(0);
    } else {
        fw_date.store(-1);
    }
    if (fw_date.load() < WB_MIN_SENSOR_FW) {
        std::cerr << "Downconverter: WARNING board firmware too old ("
                  << (fw_date.load() < 0 ? "no version reply" : fw_date.load() == 0 ? "older than fw20261109"
                                                                                    : "fw" + std::to_string(fw_date.load()))
                  << "): reads would rewrite its Flash, so board telemetry is disabled. Tuning still uses 's'."
                     " Update to fw20261110.3 through the USB bootloader." << std::endl;
    }
}

bool downconverter_init(uint64_t initial_rf_hz) {
    std::cout << "Downconverter: initializing KrakenSDR Wideband LO (IF "
              << WB_VARIANT_IF_HZ / 1e6 << " MHz)" << std::endl;
    {
        std::lock_guard<std::mutex> lock(mrf_mutex);
        if (!hid_is_open() && !hid_open_locked()) return false;
    }
    probe_firmware();

    // Drive current, then the frequency, both with 's' (no Flash wear). The
    // board's LO output is always on; the stock-moRFeus generator-mode
    // command is not sent (unimplemented there).
    if (!downconverter_set_current(WB_LO_CURRENT)) return false;
    // Startup follows the same rule as a retune: keep the default side when it
    // reaches the RF, otherwise auto-select (e.g. below side above 4132 MHz).
    if (!downconverter_rf_valid(initial_rf_hz, downconverter.side.load())) {
        if (!downconverter_set_side(downconverter_auto_side(initial_rf_hz), initial_rf_hz)) return false;
    } else if (!downconverter_apply_rf(initial_rf_hz)) return false;

    if (fw_date.load() >= WB_MIN_SENSOR_FW) start_poller();
    std::cout << "Downconverter: ready - tuners parked at IF, retunes reprogram the LO ('s', no Flash save)"
              << std::endl;
    return true;
}

// Clean exit: save the last LO frequency and drive current as the board's
// power-on default - one 'w' each (the firmware skips the Flash write when
// nothing changed). Then release the device.
void downconverter_close() {
    poller_stop = true;
    std::lock_guard<std::mutex> lock(mrf_mutex);
    // Not after a fatal exit: the supervisor restarts heimdall then, possibly
    // in a loop while the receiver stays away - no Flash write per attempt.
    if (WB_SAVE_LO_ON_EXIT && !fatal_exit.load() && hid_is_open() && fw_date.load() >= WB_MIN_SENSOR_FW) {
        const uint64_t lo = downconverter.lo_hz.load();
        if (lo) query_locked(CMD_SAVE, FUNC_FREQUENCY, lo, nullptr, WB_HID_REPLY_MS);
        query_locked(CMD_SAVE, FUNC_CURRENT, static_cast<uint64_t>(downconverter.lo_current.load()), nullptr,
                     WB_HID_REPLY_MS);
        std::cout << "Downconverter: LO " << lo / 1e6 << " MHz / current " << downconverter.lo_current.load()
                  << " saved as the board's power-on default" << std::endl;
    }
    hid_close_locked();
}
