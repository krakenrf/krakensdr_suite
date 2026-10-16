#pragma once

// SigMF (https://sigmf.org, specification 1.2) for the 📼 recorder and the
// playback of recordings: metadata, SigMF Archives (.sigmf = a POSIX.1-2001
// tar file), SHA-512.
//
// A KrakenSDR recording is a SigMF COLLECTION - the spec's own answer for
// "multiple channels of IQ data (e.g. array processing)": one SigMF Recording
// (<name>_ch<k>.sigmf-meta + .sigmf-data) per antenna and a
// <name>.sigmf-collection naming them in channel order with the SHA-512 of
// each metadata file, all in the folder <name>/. For transport the folder is
// packed into ONE SigMF Archive <name>.sigmf (an uncompressed tar holding the
// same folder) - streamed on the fly by the download, never stored twice.
// Playback reads either form (an archive in place: tar members are
// contiguous). Array geometry and calibration segments use the official
// `spatial` extension; what the KrakenSDR needs to put the receiver back the
// way it was uses the `krakensdr` namespace (kraken_doa_v2/SIGMF.md).

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sigmf {

constexpr const char* CORE_VERSION = "1.2.0";
constexpr const char* SPATIAL_VERSION = "1.1.0";
constexpr const char* KRAKEN_VERSION = "1.0.0";

// "2026-10-16T05:15:03.123Z" (RFC 3339, UTC as the spec requires)
std::string iso_utc(int64_t unix_ms);
// RFC 3339 -> ms since the epoch (fractional seconds and offsets accepted)
bool parse_iso(const std::string& s, int64_t* unix_ms);

// SHA-512 (OpenSSL EVP), hex
class Sha512 {
public:
    Sha512();
    ~Sha512();
    Sha512(const Sha512&) = delete;
    Sha512& operator=(const Sha512&) = delete;
    void update(const void* p, size_t n);
    std::string hex();   // finishes; update() afterwards starts over
private:
    void* ctx_ = nullptr;
    void reset();
};
std::string sha512_hex(const std::string& data);

// --- tar (POSIX.1-2001: ustar headers, a pax header when a name or size
// doesn't fit; reading also takes GNU long names and base-256 sizes) ---
struct TarMember {
    std::string name;
    uint64_t offset = 0;   // of the data in the tar file
    uint64_t size = 0;
    int64_t mtime = 0;
};
// the regular files of a tar file (directories etc. skipped)
bool tar_index(const std::string& path, std::vector<TarMember>* out, std::string* err);
std::string tar_file_header(const std::string& name, uint64_t size, int64_t mtime);
std::string tar_dir_header(const std::string& name, int64_t mtime);
inline uint64_t tar_pad(uint64_t size) { return (512 - size % 512) % 512; }
constexpr uint64_t TAR_END_BYTES = 1024;   // two zero blocks

// --- datatypes (complex only: the receiver needs I and Q) ---
struct Datatype {
    char kind = 0;       // 'u' / 'i' / 'f'
    int bits = 0;        // 8 / 16 / 32 / 64
    bool big = false;
    int bytes() const { return 2 * bits / 8; }   // per complex sample
};
bool parse_datatype(const std::string& s, Datatype* dt, std::string* err);

// --- reading a recording ---
struct Capture {
    uint64_t sample_start = 0;
    uint64_t global_index = 0;
    double freq_hz = 0;
    int64_t t_ms = 0;        // core:datetime, 0 = none
    double gain_db = -999;   // krakensdr:gain_db, -999 = unknown
    int phase_state = -1;    // krakensdr:phase_state (heimdall), -1 = unknown
    bool calibration = false;// spatial:calibration / krakensdr:noise_source
    double heading = -1;     // spatial:aperture_azimuth, < 0 = none
};

struct Stream {
    std::string name;        // base name (no extension)
    std::string data_path;   // the file holding the samples
    uint64_t data_offset = 0;// where they start in it (tar member / NCD header)
    uint64_t data_bytes = 0;
    Datatype dt;
    int nch = 1;             // core:num_channels (interleaved)
    int index = -1;          // spatial:channel_index (-1 = not given)
    double rate = 0;
    double iq_scale = 0;     // krakensdr:iq_scale (counts per receiver full scale)
    std::vector<Capture> captures;
};

struct Recording {
    std::string name;        // as listed (folder or archive name)
    std::string path;
    std::string form;        // "folder" | "archive" | "meta"
    std::string description, author, hw, recorder;
    std::vector<Stream> streams;   // in channel order
    int channels = 0;              // antennas (sum of nch)
    Datatype dt;
    std::string datatype;
    double rate = 0;
    uint64_t samples = 0;          // per antenna (the shortest stream)
    std::vector<std::array<double, 3>> geometry;   // spatial:element_geometry (m)
    // krakensdr namespace (collection, else the first metadata's global)
    std::string format;            // "full" | "band" | "" (not a KrakenSDR recording)
    std::vector<std::string> commands;   // krakensdr:array_commands
    std::string decimators;        // krakensdr:vfos (DECIMATORS snapshot)
    double band_offset_hz = 0;     // krakensdr:band (the VFO's offset from the tuner centre)
    double tuner_hz = 0;           // the tuner centre at the start (band recordings)
    bool station_valid = false;
    double station_lat = 0, station_lon = 0, station_heading = -1;
    std::string station_id, station_source;
    std::vector<std::array<double, 4>> track;   // [t_s, lat, lon, heading (-1 = none)]
    int64_t start_ms = 0;
};
// path: a recording folder, a .sigmf archive, a .sigmf-collection or a
// .sigmf-meta file
bool open_recording(const std::string& path, Recording* out, std::string* err);

}  // namespace sigmf
