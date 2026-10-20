#pragma once
// File-system calls whose Windows behaviour differs from Linux. The same file
// in kraken_doa_v2/include/utils/ and heimdall_v2/src/core/ (fs_compat.cpp
// next to each: identical but for its #include line) - keep them identical.
// On Linux every function is the code it replaced. Plain POSIX calls that
// only lack a name on Windows (fsync, mkdir(path, mode), O_CLOEXEC, pread ...)
// are shimmed in win/posix_compat.h instead.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Executable suffix of the tools the apps start (plugins/<id>/build/decoder,
// lib/build/codecs, transcribe/build/kraken_whisper ...).
#ifdef _WIN32
inline constexpr const char* FS_EXE_SUFFIX = ".exe";
#else
inline constexpr const char* FS_EXE_SUFFIX = "";
#endif

// rename() that replaces an existing `to` (the write-temp-then-rename
// pattern). POSIX rename does; the Windows C runtime refuses an existing
// target (EEXIST), so every save after the first failed. Windows:
// MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), retried for ~200 ms while
// the target or the fresh temp file is briefly held open (Defender, the
// search indexer). 0, or -1 with errno set.
int fs_replace(const char* from, const char* to);
inline int fs_replace(const std::string& from, const std::string& to) { return fs_replace(from.c_str(), to.c_str()); }

// After fs_replace: make the directory entry durable (Linux: fsync the
// directory, best effort). Windows: nothing to do (WRITE_THROUGH above;
// NTFS journals the rename).
void fs_fsync_dir(const std::string& dir);

// Last modification time in ms since the epoch, 0 when the file is missing.
int64_t fs_mtime_ms(const std::string& path);

// Bytes of RAM available without swapping (Linux: MemAvailable of
// /proc/meminfo); 0 if unknown.
uint64_t fs_mem_available();

// Can `name` be one file name in a folder, unchanged? Always true on Linux
// (callers keep their own checks). Windows also refuses what NTFS / Win32
// would turn into something else: < > : " / \ | ? * and control characters
// (':' = an alternate data stream or a drive), a trailing '.' or space
// (stripped by Win32), and the reserved device names CON PRN AUX NUL
// COM0-9 LPT0-9 CONIN$ CONOUT$ with any extension ("nul.txt" opens NUL).
bool fs_portable_name(std::string_view name);

// Do two names in one folder name the same entry? Linux: a == b. Windows:
// ASCII case-insensitive, trailing dots / spaces ignored (NTFS defaults).
bool fs_same_name(std::string_view a, std::string_view b);

// A directory path in the form the apps compare and display: Windows turns
// '\' into '/'; a trailing '/' is dropped except for "/" (and "C:/").
std::string fs_normalize_dir(std::string dir);

// Is `path` on another machine? Windows: a share (\\host\share, //host/share,
// \\?\UNC\..., \\.\GLOBALROOT\Device\Mup\..., \??\UNC\..., WebDAV
// \\host@port\...), or a path Win32 resolves onto a share other than the one
// the working directory is on. Opening a folder there signs in to that host
// as the logged-on user (NTLM: a web page naming its own host gets the hash)
// and a dead host blocks the caller for many seconds - folders a page sets
// are checked with this. String work only (no I/O); a share mapped to a drive
// letter is not one (the user connected it), a local device path (\\?\C:\x)
// counts as one (its drive letter spelling works). Linux: always false.
#ifdef _WIN32
bool fs_is_network_path(const std::string& path);
#else
inline bool fs_is_network_path(const std::string&) { return false; }
#endif

// A whole file mapped read-only (playback of recordings). Linux: mmap +
// MADV_SEQUENTIAL; Windows: a file mapping opened with FILE_SHARE_DELETE, so
// the file can still be deleted while it plays, as on Linux.
class FsMappedFile {
public:
    FsMappedFile() = default;
    FsMappedFile(const FsMappedFile&) = delete;
    FsMappedFile& operator=(const FsMappedFile&) = delete;
    ~FsMappedFile() { close(); }
    // false + *err on failure (also for an empty file)
    bool open(const std::string& path, std::string* err);
    void close();
    const uint8_t* data() const { return data_; }
    uint64_t size() const { return size_; }
    bool is_open() const { return data_ != nullptr; }

private:
    const uint8_t* data_ = nullptr;
    uint64_t size_ = 0;
};
