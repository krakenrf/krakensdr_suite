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
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace diskutil {

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

}  // namespace diskutil
