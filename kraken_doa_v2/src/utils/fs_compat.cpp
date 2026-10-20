// See fs_compat.hpp. Identical in kraken_doa_v2/src/utils/ and
// heimdall_v2/src/core/ except for the #include line below.
#include "utils/fs_compat.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>

#include <thread>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
namespace {

std::wstring widen(const char* s) {
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

int errno_from_win32(DWORD e) {
    switch (e) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return EACCES;
        case ERROR_NOT_SAME_DEVICE:
            return EXDEV;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return EEXIST;
        case ERROR_FILENAME_EXCED_RANGE:
            return ENAMETOOLONG;
        case ERROR_INVALID_NAME:
            return EINVAL;
        default:
            return EIO;
    }
}

bool ieq_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

int fs_replace(const char* from, const char* to) {
    const std::wstring wf = widen(from), wt = widen(to);
    for (int attempt = 0;; attempt++) {
        if (MoveFileExW(wf.c_str(), wt.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
        const DWORD e = GetLastError();
        const bool transient = e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
        if (!transient || attempt >= 10) {
            errno = errno_from_win32(e);
            return -1;
        }
        Sleep(20);
    }
}

void fs_fsync_dir(const std::string&) {}

int64_t fs_mtime_ms(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(widen(path.c_str()).c_str(), GetFileExInfoStandard, &a)) return 0;
    const uint64_t t = (static_cast<uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    // 100 ns ticks since 1601-01-01 -> ms since 1970-01-01
    return static_cast<int64_t>((t - 116444736000000000ULL) / 10000ULL);
}

uint64_t fs_mem_available() {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? ms.ullAvailPhys : 0;
}

bool fs_portable_name(std::string_view name) {
    if (name.empty()) return false;
    for (unsigned char c : name)
        if (c < 0x20 || std::strchr("<>:\"/\\|?*", c)) return false;
    if (name.back() == '.' || name.back() == ' ') return false;
    std::string_view stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.remove_suffix(1);   // "nul .txt" is NUL too
    for (const char* r : {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"})
        if (ieq_ascii(stem, r)) return false;
    if (stem.size() == 4 && (ieq_ascii(stem.substr(0, 3), "COM") || ieq_ascii(stem.substr(0, 3), "LPT")) &&
        std::isdigit(static_cast<unsigned char>(stem[3])))
        return false;
    // COM / LPT + superscript 1, 2, 3 (UTF-8 C2 B9 / C2 B2 / C2 B3)
    if (stem.size() == 5 && (ieq_ascii(stem.substr(0, 3), "COM") || ieq_ascii(stem.substr(0, 3), "LPT")) &&
        static_cast<unsigned char>(stem[3]) == 0xC2 &&
        (static_cast<unsigned char>(stem[4]) == 0xB9 || static_cast<unsigned char>(stem[4]) == 0xB2 ||
         static_cast<unsigned char>(stem[4]) == 0xB3))
        return false;
    return true;
}

bool fs_same_name(std::string_view a, std::string_view b) {
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.remove_suffix(1);
        return s;
    };
    return ieq_ascii(trim(a), trim(b));
}

std::string fs_normalize_dir(std::string d) {
    for (char& c : d)
        if (c == '\\') c = '/';
    while (d.size() > 1 && d.back() == '/' && !(d.size() == 3 && d[1] == ':')) d.pop_back();
    return d;
}

bool fs_is_network_path(const std::string& path) {
    auto sep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    auto two_sep = [&](const std::wstring& w) { return w.size() >= 2 && sep(w[0]) && sep(w[1]); };
    // GetFullPathNameW does no I/O: it only joins the path with the cwd
    auto full = [](const std::wstring& w) {
        std::wstring out(MAX_PATH + 1, L'\0');
        for (int i = 0; i < 4; i++) {
            const DWORD n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
            if (n == 0) return w;
            if (n < out.size()) {
                out.resize(n);
                return out;
            }
            out.assign(n, L'\0');   // too small: n = the size it needs
        }
        return w;
    };
    // "\\host\share" of "\\host\share\x\y"
    auto share_of = [&](const std::wstring& w) {
        size_t i = 2;
        for (int parts = 0; i < w.size(); i++)
            if (sep(w[i]) && ++parts == 2) break;
        return w.substr(0, i);
    };
    const std::wstring w = widen(path.c_str());
    // every spelling of a share or a device path (\\?\..., \\.\...) starts
    // with two separators
    if (two_sep(w)) return true;
    // Win32 hands a "\??\" path to the kernel as it is: GetFullPathNameW
    // makes "\??\UNC\host\share" a folder on C:, the file calls open the share
    if (w.size() >= 4 && sep(w[0]) && w[1] == L'?' && w[2] == L'?' && sep(w[3])) return true;
    // the rest lands on a share only below a cwd on one ("x", "\x") - started
    // from a share, its folders are the user's own
    const std::wstring f = full(w);
    if (!two_sep(f)) return false;
    const std::wstring cwd = full(L".");
    return !two_sep(cwd) || share_of(f) != share_of(cwd);
}

bool FsMappedFile::open(const std::string& path, std::string* err) {
    close();
    HANDLE f = CreateFileW(widen(path.c_str()).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (err) *err = std::strerror(errno_from_win32(GetLastError()));
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) {
        CloseHandle(f);
        if (err) *err = "empty file";
        return false;
    }
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const DWORD map_err = GetLastError();
    CloseHandle(f);   // the mapping keeps the file open
    if (!m) {
        if (err) *err = "cannot map: " + std::string(std::strerror(errno_from_win32(map_err)));
        return false;
    }
    void* p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    const DWORD view_err = GetLastError();
    CloseHandle(m);   // the view keeps the mapping
    if (!p) {
        if (err) *err = "cannot map: " + std::string(std::strerror(errno_from_win32(view_err)));
        return false;
    }
    data_ = static_cast<const uint8_t*>(p);
    size_ = static_cast<uint64_t>(sz.QuadPart);
    return true;
}

void FsMappedFile::close() {
    if (data_) UnmapViewOfFile(data_);
    data_ = nullptr;
    size_ = 0;
}

#else  // Linux

int fs_replace(const char* from, const char* to) { return ::rename(from, to); }

void fs_fsync_dir(const std::string& dir) {
    const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

int64_t fs_mtime_ms(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000 + st.st_mtim.tv_nsec / 1000000;
}

uint64_t fs_mem_available() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 13, "MemAvailable:") == 0) {
            std::istringstream ss(line.substr(13));
            uint64_t kb = 0;
            ss >> kb;
            return kb * 1024;
        }
    }
    return 0;
}

bool fs_portable_name(std::string_view) { return true; }

bool fs_same_name(std::string_view a, std::string_view b) { return a == b; }

std::string fs_normalize_dir(std::string d) {
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    return d;
}

bool FsMappedFile::open(const std::string& path, std::string* err) {
    close();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (err) *err = std::strerror(errno);
        return false;
    }
    struct stat sb{};
    if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
        ::close(fd);
        if (err) *err = "empty file";
        return false;
    }
    void* m = mmap(nullptr, static_cast<size_t>(sb.st_size), PROT_READ, MAP_SHARED, fd, 0);
    const int e = errno;
    ::close(fd);   // the mapping keeps the file
    if (m == MAP_FAILED) {
        if (err) *err = "cannot map: " + std::string(std::strerror(e));
        return false;
    }
    madvise(m, static_cast<size_t>(sb.st_size), MADV_SEQUENTIAL);
    data_ = static_cast<const uint8_t*>(m);
    size_ = static_cast<uint64_t>(sb.st_size);
    return true;
}

void FsMappedFile::close() {
    if (data_) munmap(const_cast<uint8_t*>(data_), static_cast<size_t>(size_));
    data_ = nullptr;
    size_ = 0;
}

#endif
