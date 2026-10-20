#include "downconverter.hpp"
#include "../../config.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
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

// ---- moRFeus USB HID protocol ----
// 17-byte output report: [report id 0][mode 'w'=set / 'r'=get][function]
// [value, 8 bytes big-endian][6 zero bytes]. The box's own firmware does all
// LMX2595 synthesizer register programming; the host only ever sends absolute
// values. Same wire format the upstream heimdall_daq_fw wideband branch used
// (Firmware/_daq_core/morfeus.py), but with the frequency sent in Hz for full
// resolution (upstream's UI truncated to whole MHz before scaling).
namespace {

constexpr uint16_t MORFEUS_VID = 0x10C4;   // Silicon Labs
constexpr uint16_t MORFEUS_PID = 0xEAC9;   // USBXpress (moRFeus)

constexpr uint8_t MRF_SET  = 119;  // 'w'
constexpr uint8_t FUNC_FREQUENCY = 129;    // value = Hz
constexpr uint8_t FUNC_CURRENT   = 131;    // LO output drive current, 0..7
// Stock-moRFeus function 130 (mixer/generator mode) is deliberately NOT used:
// the WB board's firmware doesn't implement it (its output is always on) and
// v1 never sent it (see downconverter_init).

std::mutex mrf_mutex;  // serializes open/write across control paths

#ifdef __linux__
int mrf_fd = -1;

// Find the moRFeus among /dev/hidraw* by VID/PID. Returns an open fd or -1.
int open_morfeus_locked() {
    DIR* dir = opendir("/dev");
    if (!dir) return -1;

    int found_fd = -1;
    bool saw_permission_error = false;
    while (dirent* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "hidraw", 6) != 0) continue;

        std::string path = std::string("/dev/") + entry->d_name;
        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0) {
            if (errno == EACCES) saw_permission_error = true;
            continue;
        }

        hidraw_devinfo info{};
        if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0 &&
            static_cast<uint16_t>(info.vendor) == MORFEUS_VID &&
            static_cast<uint16_t>(info.product) == MORFEUS_PID) {
            std::cout << "Downconverter: moRFeus LO found at " << path << std::endl;
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
    }
    return found_fd;
}

// Write one 17-byte command, reopening the device once if the write fails
// (survives a USB replug). Caller must NOT hold mrf_mutex.
bool mrf_write(uint8_t func, uint64_t value) {
    std::lock_guard<std::mutex> lock(mrf_mutex);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (mrf_fd < 0) {
            mrf_fd = open_morfeus_locked();
            if (mrf_fd < 0) return false;
        }

        uint8_t report[17] = {0};
        report[0] = 0;        // HID report id (unnumbered)
        report[1] = MRF_SET;
        report[2] = func;
        for (int i = 0; i < 8; i++) {
            report[3 + i] = static_cast<uint8_t>(value >> (8 * (7 - i)));  // big-endian
        }

        if (write(mrf_fd, report, sizeof(report)) == static_cast<ssize_t>(sizeof(report))) {
            return true;
        }

        std::cerr << "Downconverter: moRFeus HID write failed (" << strerror(errno)
                  << ") - reopening device" << std::endl;
        close(mrf_fd);
        mrf_fd = -1;
    }
    return false;
}

void close_morfeus_locked() {
    if (mrf_fd >= 0) {
        close(mrf_fd);
        mrf_fd = -1;
    }
}

#elif defined(_WIN32)
// Windows: the moRFeus through Windows' own HID class driver (SetupAPI +
// hid.dll) - no extra library or driver. It must keep that driver: Zadig's
// "List All Devices" shows it as well, and with WinUSB instead it is no
// longer a HID device. Writes are overlapped with a 1 s limit (hidraw's write
// returns at once): a wedged box must not hang a retune holding
// device_io_mutex. Its input reports (echoes) are never read - the HID
// driver's ring drops them, as hidraw's does.
HANDLE mrf_handle = INVALID_HANDLE_VALUE;
USHORT mrf_report_len = 0;   // the interface's output report length, incl. the report id

// Find the moRFeus among the present HID interfaces by VID/PID and open it
// (mrf_handle). Caller holds mrf_mutex.
bool open_morfeus_locked() {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        std::cerr << "Downconverter: cannot list the HID devices ("
                  << std::system_category().message(static_cast<int>(GetLastError())) << ")" << std::endl;
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
        std::cout << "Downconverter: moRFeus LO found (HID, " << mrf_report_len << "-byte output report)" << std::endl;
    }
    SetupDiDestroyDeviceInfoList(set);

    if (mrf_handle == INVALID_HANDLE_VALUE) {
        std::cerr << "Downconverter: no moRFeus LO found on USB (VID 0x10C4 PID 0xEAC9)";
        if (busy) {
            std::cerr << " - it is open in another program; close that program";
        } else {
            std::cerr << " - it uses Windows' built-in HID driver: do NOT replace that driver with WinUSB in"
                         " Zadig (if it was: Device Manager, uninstall the device incl. its driver, replug)";
        }
        std::cerr << std::endl;
        return false;
    }
    return true;
}

// One output report, zero-padded to the interface's report length.
// Caller holds mrf_mutex; mrf_handle is open. false + *err on failure.
bool hid_write_locked(const uint8_t* report, size_t len, DWORD* err) {
    std::vector<uint8_t> buf(mrf_report_len, 0);
    std::memcpy(buf.data(), report, len);   // len <= mrf_report_len (checked at open)
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        *err = GetLastError();
        return false;
    }
    DWORD n = 0;
    bool ok = WriteFile(mrf_handle, buf.data(), static_cast<DWORD>(buf.size()), &n, &ov) != 0;
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(mrf_handle, &ov, &n, FALSE) != 0;
                if (!ok) e = GetLastError();
            } else {
                // cancel, and wait for it: buf and ov must outlive the I/O
                CancelIoEx(mrf_handle, &ov);
                GetOverlappedResult(mrf_handle, &ov, &n, TRUE);
                e = ERROR_TIMEOUT;
            }
        }
        if (!ok) *err = e;
    }
    CloseHandle(ov.hEvent);
    if (ok && n != buf.size()) {
        *err = ERROR_WRITE_FAULT;
        ok = false;
    }
    return ok;
}

// Write one 17-byte command (the report layout above), reopening the device
// once if the write fails (survives a USB replug). Caller must NOT hold
// mrf_mutex.
bool mrf_write(uint8_t func, uint64_t value) {
    std::lock_guard<std::mutex> lock(mrf_mutex);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (mrf_handle == INVALID_HANDLE_VALUE && !open_morfeus_locked()) return false;

        uint8_t report[17] = {0};
        report[0] = 0;        // HID report id (unnumbered)
        report[1] = MRF_SET;
        report[2] = func;
        for (int i = 0; i < 8; i++) {
            report[3 + i] = static_cast<uint8_t>(value >> (8 * (7 - i)));  // big-endian
        }

        DWORD err = 0;
        if (hid_write_locked(report, sizeof(report), &err)) return true;

        std::cerr << "Downconverter: moRFeus HID write failed ("
                  << std::system_category().message(static_cast<int>(err)) << ") - reopening device" << std::endl;
        CloseHandle(mrf_handle);
        mrf_handle = INVALID_HANDLE_VALUE;
    }
    return false;
}

void close_morfeus_locked() {
    if (mrf_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(mrf_handle);
        mrf_handle = INVALID_HANDLE_VALUE;
    }
}

#else
// No HID backend on this platform: --wideband reports the LO missing
bool mrf_write(uint8_t, uint64_t) {
    std::cerr << "Downconverter: moRFeus control is not supported on this platform" << std::endl;
    return false;
}

void close_morfeus_locked() {}
#endif

}  // namespace

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
    if (!mrf_write(FUNC_FREQUENCY, lo)) {
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
    if (!mrf_write(FUNC_CURRENT, static_cast<uint64_t>(current))) {
        std::cerr << "Downconverter: failed to set LO current " << current << std::endl;
        return false;
    }
    downconverter.lo_current.store(current);
    std::cout << "Downconverter: LO drive current set to " << current << " (0-7)" << std::endl;
    return true;
}

bool downconverter_init(uint64_t initial_rf_hz) {
    std::cout << "Downconverter: initializing KrakenSDR Wideband LO (IF "
              << WB_VARIANT_IF_HZ / 1e6 << " MHz)" << std::endl;

    // Set the LO drive current, then the frequency. The Wideband unit's
    // on-board synthesizer (custom "WB fw" firmware behind the moRFeus
    // VID/PID) implements frequency set/get and its LO output is always on;
    // the stock-moRFeus generator-mode command is not sent (unimplemented
    // there; a real moRFeus box would need it set from its front panel).
    // The current write is harmless if this firmware ignores it, and the
    // client UI can adjust it at runtime (set_lo_current).
    if (!downconverter_set_current(WB_LO_CURRENT)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (!downconverter_apply_rf(initial_rf_hz)) return false;

    std::cout << "Downconverter: ready - tuners parked at IF, retunes reprogram the LO"
              << std::endl;
    return true;
}

void downconverter_close() {
    std::lock_guard<std::mutex> lock(mrf_mutex);
    close_morfeus_locked();
}
