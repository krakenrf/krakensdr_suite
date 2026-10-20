// ============================================
// src/utils/disk_util.cpp - folders, free space, drives (see disk_util.hpp)
// ============================================

#include "utils/disk_util.hpp"

#include "utils/json_escape.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>

#include <cctype>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <map>
#include <mutex>
#else
#include <sys/statvfs.h>
#endif
#include <unistd.h>
#include <vector>

namespace diskutil {

#ifndef _WIN32
namespace {

// /proc/mounts escapes " " as \040
std::string unescape_mount(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 3 < s.size()) {
            o += static_cast<char>(strtol(s.substr(i + 1, 3).c_str(), nullptr, 8));
            i += 3;
        } else {
            o += s[i];
        }
    }
    return o;
}

struct Mount {
    std::string dev, mp, type;
};

std::vector<Mount> read_mounts() {
    std::vector<Mount> all;
    std::ifstream pm("/proc/mounts");
    std::string line;
    while (std::getline(pm, line)) {
        std::istringstream ls(line);
        Mount m;
        ls >> m.dev >> m.mp >> m.type;
        m.mp = unescape_mount(m.mp);
        if (!m.mp.empty()) all.push_back(m);
    }
    return all;
}

// the mount point holding the absolute path p
const Mount* mount_of(const std::vector<Mount>& all, const std::string& p) {
    const Mount* best = nullptr;
    for (const auto& m : all) {
        if (p.compare(0, m.mp.size(), m.mp) == 0 && (p.size() == m.mp.size() || p[m.mp.size()] == '/' || m.mp == "/") &&
            (!best || m.mp.size() >= best->mp.size()))
            best = &m;
    }
    return best;
}

}  // namespace

bool mkdirs(const std::string& path) {
    std::string cur;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') cur = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        cur += part;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        cur += "/";
    }
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string absolute(const std::string& p) {
    if (!p.empty() && p[0] == '/') return p;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return p;
    return std::string(cwd) + "/" + p;
}

bool disk_space(std::string path, uint64_t* free_b, uint64_t* total_b) {
    path = absolute(path);
    for (int i = 0; i < 64 && !path.empty(); i++) {
        struct statvfs v{};
        if (statvfs(path.c_str(), &v) == 0) {
            *free_b = static_cast<uint64_t>(v.f_bavail) * v.f_frsize;
            *total_b = static_cast<uint64_t>(v.f_blocks) * v.f_frsize;
            return true;
        }
        const size_t sl = path.rfind('/');
        path = sl == 0 ? "/" : (sl == std::string::npos ? "" : path.substr(0, sl));
    }
    return false;
}

std::string fs_type(const std::string& path) {
    const auto all = read_mounts();
    const Mount* m = mount_of(all, absolute(path));
    return m ? m->type : "";
}

std::string mounts_json(const std::string& dir_abs, std::string* mount_of_dir) {
    const auto all = read_mounts();
    const Mount* own = mount_of(all, dir_abs);
    *mount_of_dir = own ? own->mp : "/";
    std::ostringstream o;
    bool first = true;
    std::vector<std::string> listed;
    o << "[";
    for (const auto& m : all) {
        // the folder's own drive is always listed
        const bool want = m.mp == *mount_of_dir ||
                          ((m.mp == "/" || m.mp.rfind("/media/", 0) == 0 || m.mp.rfind("/mnt/", 0) == 0 ||
                            m.mp.rfind("/run/media/", 0) == 0 || m.mp.rfind("/srv", 0) == 0) &&
                           m.type != "tmpfs" && m.type != "devtmpfs" && m.type != "squashfs" && m.type != "overlay" &&
                           m.type != "autofs" && m.type != "proc" && m.type != "sysfs" &&
                           // statvfs on a dead network mount blocks (this runs on the uWS loop)
                           m.type.rfind("nfs", 0) != 0 && m.type != "cifs" && m.type != "smb3" &&
                           m.type.rfind("fuse.", 0) != 0);
        if (!want || std::find(listed.begin(), listed.end(), m.mp) != listed.end()) continue;
        listed.push_back(m.mp);
        uint64_t mf = 0, mt = 0;
        if (!disk_space(m.mp, &mf, &mt) || mt == 0) continue;
        o << (first ? "" : ",") << "{\"path\":\"" << json_escape(m.mp) << "\",\"dev\":\"" << json_escape(m.dev)
          << "\",\"type\":\"" << json_escape(m.type) << "\",\"free\":" << mf << ",\"total\":" << mt << "}";
        first = false;
    }
    o << "]";
    return o.str();
}

#else  // _WIN32

// Windows has no single tree of mount points: every volume is a drive letter
// (or a share, or a folder it is mounted in), paths start with "C:/" or
// "//server/share", and there is no /proc/mounts or statvfs. The paths
// returned here use '/' like the Linux ones (Win32 takes both), so the
// callers' rfind('/') and dir + "/" + name keep working. The drive
// queries never pop up a "no disk in the drive" box: win/win_init.cpp sets
// SEM_FAILCRITICALERRORS for the process.

namespace {

// file names are UTF-8 in this code base, Win32 takes UTF-16
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// "c:\" / "\\srv\share\" / "C:\mnt\vol\" -> "C:/" / "//srv/share" / "C:/mnt/vol":
// one spelling per volume, so a page can compare a listed drive with the
// folder's own
std::string root_form(std::wstring v) {
    for (wchar_t& c : v)
        if (c == L'\\') c = L'/';
    while (v.size() > 1 && v.back() == L'/' && !(v.size() == 3 && v[1] == L':')) v.pop_back();
    if (v.size() >= 2 && v[1] == L':') v[0] = static_cast<wchar_t>(std::towupper(v[0]));
    return narrow(v);
}

// length of the root of an absolute '/'-path: 3 for "C:/...", the length of
// "//srv/share" for a share; 0 = none
size_t root_len(const std::string& p) {
    if (p.size() >= 3 && p[1] == ':' && p[2] == '/') return 3;
    if (p.rfind("//", 0) == 0) {
        const size_t a = p.find('/', 2);
        const size_t b = a == std::string::npos ? std::string::npos : p.find('/', a + 1);
        return b == std::string::npos ? p.size() : b;
    }
    return 0;
}

// the root (with the trailing '\' Win32 wants) of the volume holding the
// absolute path p - a drive, a share or a folder a volume is mounted in;
// L"" = unknown. p itself doesn't have to exist. GetVolumePathNameW opens
// every component of the path (0.1-0.2 ms each: 0.5-2.5 ms for a usual
// folder) and the status pages ask several times a second: each answer is
// kept for 10 s
std::wstring volume_of(const std::string& p) {
    // never destroyed: a status may still be built while the process exits
    static auto* mu = new std::mutex;
    static auto* cache = new std::map<std::string, std::pair<std::wstring, ULONGLONG>>;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(*mu);
        auto it = cache->find(p);
        if (it != cache->end() && now - it->second.second < 10000) return it->second.first;
    }
    std::wstring w = widen(p), v;
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';
    if (!w.empty()) {
        v.assign(std::max<size_t>(w.size() + 2, MAX_PATH + 1), L'\0');
        if (GetVolumePathNameW(w.c_str(), v.data(), static_cast<DWORD>(v.size()))) v.resize(wcslen(v.c_str()));
        else v.clear();
    }
    std::lock_guard<std::mutex> lk(*mu);
    if (cache->size() > 32) cache->clear();
    (*cache)[p] = {v, now};
    return v;
}

// the file system of a volume root, named as Linux names it ("vfat" = FAT32:
// no file of 4 GiB; "exfat", "ntfs", "refs"); "" = unknown
std::string volume_type(const std::wstring& root, std::string* label) {
    wchar_t name[MAX_PATH + 1] = {}, fsn[MAX_PATH + 1] = {};
    if (!GetVolumeInformationW(root.c_str(), name, MAX_PATH + 1, nullptr, nullptr, nullptr, fsn, MAX_PATH + 1)) return "";
    if (label) *label = narrow(name);
    std::string t = narrow(fsn);
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t.rfind("fat", 0) == 0) return "vfat";   // "FAT32", or "FAT" (FAT16 / FAT12)
    return t;
}

}  // namespace

bool mkdirs(const std::string& path) {
    // create_directories copes with "C:/", "//srv/share/x" and both separators
    // (creating each '/'-prefix in turn tried mkdir("C:"), which fails with
    // EACCES, not EEXIST)
    std::error_code ec;
    const std::filesystem::path p(widen(path));
    std::filesystem::create_directories(p, ec);
    return std::filesystem::is_directory(p, ec);
}

std::string absolute(const std::string& p) {
    // GetFullPathNameW: drive letters, shares, '\' or '/', no 260-character
    // buffer (std::filesystem::absolute turned "//srv/share" into
    // "C:/srv/share"); '/'-separated out, and an absolute path stays itself
    const std::wstring w = widen(p.empty() ? "." : p);
    std::wstring out(MAX_PATH + 1, L'\0');
    for (int i = 0; i < 4; i++) {
        const DWORD n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
        if (n == 0) return p;
        if (n < out.size()) {
            out.resize(n);
            for (wchar_t& c : out)
                if (c == L'\\') c = L'/';
            return narrow(out);
        }
        out.assign(n, L'\0');   // too small: n = the size it needs
    }
    return p;
}

bool disk_space(std::string path, uint64_t* free_b, uint64_t* total_b) {
    path = absolute(path);
    for (int i = 0; i < 64 && !path.empty(); i++) {
        std::wstring w = widen(path);
        for (wchar_t& c : w)
            if (c == L'/') c = L'\\';
        if (!w.empty() && w.back() != L'\\') w += L'\\';   // a share's root needs it ("\\srv\share\")
        ULARGE_INTEGER fr{}, tot{};
        // free to this user (disk quotas) = statvfs's f_bavail
        if (GetDiskFreeSpaceExW(w.c_str(), &fr, &tot, nullptr)) {
            *free_b = fr.QuadPart;
            *total_b = tot.QuadPart;
            return true;
        }
        // the nearest existing parent, down to the drive / share root
        const size_t root = root_len(path), sl = path.rfind('/');
        if (path.size() <= root || sl == std::string::npos) path.clear();
        else path = sl < root ? path.substr(0, root) : path.substr(0, sl);
    }
    return false;
}

std::string fs_type(const std::string& path) {
    const std::wstring v = volume_of(absolute(path));
    return v.empty() ? "" : volume_type(v, nullptr);
}

std::string mounts_json(const std::string& dir_abs, std::string* mount_of_dir) {
    const std::wstring own = volume_of(dir_abs);
    *mount_of_dir = own.empty() ? "" : root_form(own);
    // the drive letters: fixed disks, USB drives / card readers (one without
    // a medium has no free space and is left out below), RAM disks. Not
    // network drives - a dead share blocks the query and this runs on the uWS
    // loop (Linux leaves out nfs / cifs for the same reason) - nor CD / DVD
    // drives
    std::vector<std::wstring> vols;
    wchar_t drives[1024] = {};
    const DWORD n = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives) - 1), drives);
    if (n > 0 && n < std::size(drives)) {
        for (const wchar_t* d = drives; *d; d += wcslen(d) + 1) {
            const UINT t = GetDriveTypeW(d);
            if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE || t == DRIVE_RAMDISK) vols.emplace_back(d);
        }
    }
    // the folder's own drive is always listed (also a share or a mounted folder)
    if (!own.empty() &&
        std::none_of(vols.begin(), vols.end(), [&](const std::wstring& v) { return root_form(v) == *mount_of_dir; }))
        vols.push_back(own);
    // the drive of the working directory = where the default (relative)
    // folders are: the "system disk" of the UI's drive pickers (Linux: "/")
    wchar_t cwd[MAX_PATH * 4] = {};
    const DWORD cn = GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwd)), cwd);
    const std::wstring cwd_vol = (cn > 0 && cn < std::size(cwd)) ? volume_of(narrow(std::wstring(cwd, cn))) : std::wstring();
    std::ostringstream o;
    bool first = true;
    o << "[";
    for (const auto& v : vols) {
        ULARGE_INTEGER fr{}, tot{};
        if (!GetDiskFreeSpaceExW(v.c_str(), &fr, &tot, nullptr) || tot.QuadPart == 0) continue;
        std::string label;
        const std::string type = volume_type(v, &label);
        o << (first ? "" : ",") << "{\"path\":\"" << json_escape(root_form(v)) << "\",\"dev\":\"" << json_escape(label)
          << "\",\"type\":\"" << json_escape(type) << "\",\"free\":" << fr.QuadPart << ",\"total\":" << tot.QuadPart
          << (!cwd_vol.empty() && root_form(v) == root_form(cwd_vol) ? ",\"sys\":true" : "") << "}";
        first = false;
    }
    o << "]";
    return o.str();
}

#endif  // _WIN32

}  // namespace diskutil
