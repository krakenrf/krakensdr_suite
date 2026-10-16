// ============================================
// src/recorder_audio.cpp - 📼 audio recordings: one file per VFO
// ============================================
// See include/recorder.hpp. Each recorded VFO gets the stream its FM audio /
// decoders get (beamformer output when beamforming ran, else the listened-to
// channel) and records what its demod gives - NBFM / AM / WBFM demodulated by
// its own FMDemodulatorRobust (the live audio's demodulator: the same filters,
// de-emphasis and levels), or with demod "Digital" its digital decoder's
// voice (8 kHz from the decoder's record tap, put on the VFO's clock: silence
// where there is none). With the squelch on, a closed squelch is silence - or
// left out ("Leave out silence"). The files: WAV (16-bit mono at 48 / 16 / 8
// kHz, a LIST/INFO chunk with the frequency, demod, station; the sizes in
// the header are kept current every 5 s so a crash leaves a playable file)
// or Ogg Opus (RFC 7845, 48 kHz, Vorbis comments; pages written about once
// a second). A file that would pass 4 GiB (WAV's limit, FAT32's) continues in
// <name>_2.wav ...
//
// Threads: the decimation thread copies blocks in (feed_audio), the decoders'
// workers the voice (feed_voice); ONE worker demodulates and writes. a_mu
// guards the queues and the shared status; it is never held while calling
// into recorder.cpp (whose g_mu is taken before a_mu in the status path).

#include "recorder.hpp"
#include "recorder_internal.hpp"

#include "decimator_manager.hpp"
#include "digital/digital_decoder.hpp"
#include "globals.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "utils/disk_util.hpp"
#include "utils/json_escape.hpp"
#include "utils/parse_num.hpp"

#include <liquid/liquid.h>
#include <opus/opus.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace rec {
namespace {

using cf = std::complex<float>;
constexpr uint64_t FILE_MAX = 0xFFFFFFFFull - (2u << 20);   // WAV / FAT32: below 4 GiB
constexpr size_t MAX_QUEUED_S = 3;                           // per VFO, then dropped (counted)

int64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string fmt(double v, int prec) {
    char b[64];
    snprintf(b, sizeof b, "%.*f", prec, is_finite_value(v) ? v : 0.0);
    return b;
}

std::string demod_name(DemodulatorMode m) {
    switch (m) {
        case DemodulatorMode::WBFM: return "WBFM";
        case DemodulatorMode::NBFM: return "NBFM";
        case DemodulatorMode::AM: return "AM";
        default: return "Digital";
    }
}

void put16(std::string& b, uint16_t v) { b.append(reinterpret_cast<const char*>(&v), 2); }   // little-endian hosts
void put32(std::string& b, uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); }

// --- WAV ---------------------------------------------------------------------------
class WavFile {
public:
    ~WavFile() { close(); }
    bool open(const std::string& path, int rate, const std::vector<std::pair<std::string, std::string>>& info) {
        f_ = fopen(path.c_str(), "wb");
        if (!f_) return false;
        rate_ = rate;
        std::string h = "RIFF";
        put32(h, 0);
        h += "WAVEfmt ";
        put32(h, 16);
        put16(h, 1);   // PCM
        put16(h, 1);   // mono
        put32(h, static_cast<uint32_t>(rate));
        put32(h, static_cast<uint32_t>(rate * 2));
        put16(h, 2);
        put16(h, 16);
        std::string list = "INFO";
        for (const auto& [id, text] : info) {
            if (id.size() != 4 || text.empty()) continue;
            std::string t = text.substr(0, 1000);
            t += '\0';
            list += id;
            put32(list, static_cast<uint32_t>(t.size()));
            list += t;
            if (t.size() & 1) list += '\0';
        }
        h += "LIST";
        put32(h, static_cast<uint32_t>(list.size()));
        h += list;
        h += "data";
        size_pos_ = static_cast<long>(h.size());
        put32(h, 0);
        header_ = h.size();
        return fwrite(h.data(), 1, h.size(), f_) == h.size();
    }
    bool write(const float* x, size_t n) {
        buf_.resize(n);
        for (size_t i = 0; i < n; i++) {
            const float v = std::clamp(x[i], -1.0f, 1.0f) * 32767.0f;
            buf_[i] = static_cast<int16_t>(std::lrint(v));
        }
        if (fwrite(buf_.data(), 2, n, f_) != n) return false;
        data_ += n * 2;
        return true;
    }
    // the sizes in the header: a crash leaves a playable file
    void update_header() {
        if (!f_) return;
        fflush(f_);
        const long end = ftell(f_);
        const uint32_t data = static_cast<uint32_t>(data_), riff = static_cast<uint32_t>(header_ - 8 + data_);
        fseek(f_, 4, SEEK_SET);
        fwrite(&riff, 4, 1, f_);
        fseek(f_, size_pos_, SEEK_SET);
        fwrite(&data, 4, 1, f_);
        fseek(f_, end, SEEK_SET);
        fflush(f_);
    }
    void close() {
        if (!f_) return;
        update_header();
        fclose(f_);
        f_ = nullptr;
    }
    uint64_t bytes() const { return header_ + data_; }
    double seconds() const { return rate_ ? data_ / 2.0 / rate_ : 0; }

private:
    FILE* f_ = nullptr;
    int rate_ = 0;
    long size_pos_ = 0;
    size_t header_ = 0;
    uint64_t data_ = 0;
    std::vector<int16_t> buf_;
};

// --- Ogg Opus (RFC 7845) -----------------------------------------------------------
uint32_t ogg_crc(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
            tab[i] = r;
        }
        init = true;
    }
    for (size_t i = 0; i < n; i++) crc = (crc << 8) ^ tab[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

class OpusFile {
public:
    ~OpusFile() { close(); }
    bool open(const std::string& path, int kbps, const std::vector<std::string>& tags) {
        int err = 0;
        enc_ = opus_encoder_create(48000, 1, kbps <= 24 ? OPUS_APPLICATION_VOIP : OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK || !enc_) return false;
        opus_encoder_ctl(enc_, OPUS_SET_BITRATE(kbps * 1000));
        opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(5));
        opus_int32 la = 0;
        opus_encoder_ctl(enc_, OPUS_GET_LOOKAHEAD(&la));
        preskip_ = static_cast<uint32_t>(la);
        f_ = fopen(path.c_str(), "wb");
        if (!f_) return false;
        serial_ = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ 0x4B524B4Eu;
        std::string head = "OpusHead";
        head += static_cast<char>(1);
        head += static_cast<char>(1);
        put16(head, static_cast<uint16_t>(preskip_));
        put32(head, 48000);
        put16(head, 0);
        head += static_cast<char>(0);
        if (!page({head}, 0, 0x02)) return false;
        std::string tg = "OpusTags";
        const std::string vendor = std::string("kraken_doa recorder, ") + opus_get_version_string();
        put32(tg, static_cast<uint32_t>(vendor.size()));
        tg += vendor;
        put32(tg, static_cast<uint32_t>(tags.size()));
        for (const auto& t : tags) {
            put32(tg, static_cast<uint32_t>(t.size()));
            tg += t;
        }
        return page({tg}, 0, 0);
    }
    bool write(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            frame_[fill_++] = x[i];
            if (fill_ == 960 && !encode()) return false;
        }
        return true;
    }
    void close() {
        if (!f_) return;
        // the last partial frame padded; the granule trims the padding
        const uint64_t real = samples_ + fill_;
        if (fill_) {
            std::fill(frame_ + fill_, frame_ + 960, 0.0f);
            fill_ = 960;
            encode(false);
        }
        page(pending_, preskip_ + real, 0x04);
        pending_.clear();
        fclose(f_);
        f_ = nullptr;
        if (enc_) opus_encoder_destroy(enc_);
        enc_ = nullptr;
    }
    uint64_t bytes() const { return bytes_; }
    double seconds() const { return (samples_ + fill_) / 48000.0; }

private:
    FILE* f_ = nullptr;
    OpusEncoder* enc_ = nullptr;
    uint32_t serial_ = 0, seq_ = 0, preskip_ = 312;
    float frame_[960]{};
    size_t fill_ = 0;
    uint64_t samples_ = 0, bytes_ = 0;
    std::vector<std::string> pending_;
    size_t pending_bytes_ = 0;

    bool encode(bool flush_page = true) {
        unsigned char out[1500];
        const int len = opus_encode_float(enc_, frame_, 960, out, sizeof out);
        fill_ = 0;
        if (len < 0) return false;
        samples_ += 960;
        pending_.emplace_back(reinterpret_cast<const char*>(out), static_cast<size_t>(len));
        pending_bytes_ += static_cast<size_t>(len);
        // a page about every second (or when the segment table fills up)
        if (flush_page && (pending_.size() >= 50 || pending_bytes_ > 8000)) {
            const bool ok = page(pending_, preskip_ + samples_, 0);
            pending_.clear();
            pending_bytes_ = 0;
            return ok;
        }
        return true;
    }
    bool page(const std::vector<std::string>& packets, uint64_t granule, uint8_t type) {
        std::string seg, body;
        for (const auto& p : packets) {
            size_t n = p.size();
            while (n >= 255) {
                seg += static_cast<char>(255);
                n -= 255;
            }
            seg += static_cast<char>(n);
            body += p;
        }
        if (seg.size() > 255) return false;
        std::string h = "OggS";
        h += static_cast<char>(0);
        h += static_cast<char>(type);
        h.append(reinterpret_cast<const char*>(&granule), 8);
        put32(h, serial_);
        put32(h, seq_++);
        put32(h, 0);
        h += static_cast<char>(seg.size());
        h += seg;
        uint32_t crc = ogg_crc(reinterpret_cast<const uint8_t*>(h.data()), h.size());
        crc = ogg_crc(reinterpret_cast<const uint8_t*>(body.data()), body.size(), crc);
        memcpy(h.data() + 22, &crc, 4);
        const bool ok = fwrite(h.data(), 1, h.size(), f_) == h.size() && fwrite(body.data(), 1, body.size(), f_) == body.size();
        fflush(f_);
        bytes_ += h.size() + body.size();
        return ok;
    }
};

// --- one VFO's file -----------------------------------------------------------------
struct Track {
    int vfo = -1;
    std::string base;               // file name without the extension / part
    int part = 1;
    double mhz = 0;
    std::string demod;
    std::unique_ptr<WavFile> wav;
    std::unique_ptr<OpusFile> opus;
    std::unique_ptr<FMDemodulatorRobust> fm;
    DemodulatorMode fm_mode = DemodulatorMode::NBFM;
    float fm_rate = 0;
    msresamp_rrrf down = nullptr;   // 48 kHz -> the WAV rate
    firinterp_rrrf up = nullptr;    // digital voice 8 -> 48 kHz
    std::deque<float> voice;        // 48 kHz, from the decoder
    double frac = 0;                // digital: output samples owed by the clock
    bool open_now = false;
    bool sq_now = false;            // the VFO's squelch is on (else "open" = always)
    uint64_t done_bytes = 0;        // closed parts
    double done_s = 0;
    std::vector<float> a48, out;
    ~Track() {
        if (down) msresamp_rrrf_destroy(down);
        if (up) firinterp_rrrf_destroy(up);
    }
};

struct In {
    int vfo;
    std::vector<cf> x;
    float rate;
    double rf;
    DemodulatorMode mode;
    bool sq_on, sq_open, digital;
};

struct Session {
    detail::AudioConfig cfg;
    int64_t start_ms = 0, start_mono = 0;
    std::map<int, std::unique_ptr<Track>> tracks;   // worker only
    std::thread th;
};

std::mutex a_mu;                    // below
std::condition_variable a_cv;
std::unique_ptr<Session> g_s;       // the running recording (worker owns its tracks)
std::vector<int> g_vfos;            // the VFOs it records (taps check)
std::atomic<bool> g_active{false};
std::deque<In> g_in;
std::map<int, std::vector<float>> g_voice;   // 8 kHz per VFO
std::map<int, size_t> g_queued;              // input samples waiting per VFO
bool g_stop = false;
std::string g_reason;
std::string g_status;               // the worker's latest status object
uint64_t g_dropped = 0;

bool open_file(Session& s, Track& t, std::string* err) {
    const std::string name = t.base + (t.part > 1 ? "_" + std::to_string(t.part) : "") + (s.cfg.wav ? ".wav" : ".opus");
    const std::string path = s.cfg.folder + "/" + name;
    const std::string date = detail::name_time(detail::now_ms());
    const std::string title = "VFO D" + std::to_string(t.vfo) + " " + fmt(t.mhz, 4) + " MHz " + t.demod;
    std::string comment = "KrakenSDR " + title + ", recorded " + date;
    if (s.cfg.has_pos) comment += ", station " + fmt(s.cfg.lat, 6) + "," + fmt(s.cfg.lon, 6);
    if (!s.cfg.notes.empty()) comment += " - " + s.cfg.notes;
    if (s.cfg.wav) {
        t.wav = std::make_unique<WavFile>();
        if (!t.wav->open(path, s.cfg.rate, {{"INAM", title}, {"ICMT", comment}, {"IART", s.cfg.station},
                                            {"ICRD", date}, {"ISFT", "KrakenSDR kraken_doa"}}))
            return *err = "can't create " + path, false;
    } else {
        std::vector<std::string> tags = {"TITLE=" + title, "ARTIST=" + s.cfg.station, "DATE=" + date, "COMMENT=" + comment,
                                         "KRAKEN_VFO=" + std::to_string(t.vfo), "KRAKEN_MHZ=" + fmt(t.mhz, 6),
                                         "KRAKEN_DEMOD=" + t.demod};
        if (s.cfg.has_pos) {
            tags.push_back("KRAKEN_LAT=" + fmt(s.cfg.lat, 6));
            tags.push_back("KRAKEN_LON=" + fmt(s.cfg.lon, 6));
        }
        t.opus = std::make_unique<OpusFile>();
        if (!t.opus->open(path, s.cfg.kbps, tags)) return *err = "can't create " + path, false;
    }
    return true;
}

uint64_t file_bytes(const Track& t) { return t.wav ? t.wav->bytes() : t.opus ? t.opus->bytes() : 0; }
double file_seconds(const Track& t) { return t.wav ? t.wav->seconds() : t.opus ? t.opus->seconds() : 0; }

void close_file(Track& t) {
    t.done_bytes += file_bytes(t);
    t.done_s += file_seconds(t);
    if (t.wav) t.wav->close();
    if (t.opus) t.opus->close();
    t.wav.reset();
    t.opus.reset();
}

// 48 kHz audio -> the file (resampled for a lower WAV rate); a new part before 4 GiB
bool emit(Session& s, Track& t, const float* x, size_t n, std::string* err) {
    if (n == 0) return true;
    const float* p = x;
    size_t m = n;
    if (s.cfg.wav && s.cfg.rate != 48000) {
        if (!t.down) t.down = msresamp_rrrf_create(static_cast<float>(s.cfg.rate) / 48000.0f, 60.0f);
        t.out.resize(static_cast<size_t>(n * s.cfg.rate / 48000.0) + 64);
        unsigned w = 0;
        msresamp_rrrf_execute(t.down, const_cast<float*>(x), static_cast<unsigned>(n), t.out.data(), &w);
        p = t.out.data();
        m = w;
    }
    const uint64_t add = s.cfg.wav ? m * 2 : m / 4 + 64;
    if (file_bytes(t) + add > FILE_MAX) {
        close_file(t);
        t.part++;
        if (!open_file(s, t, err)) return false;
    }
    const bool ok = t.wav ? t.wav->write(p, m) : t.opus->write(p, m);
    if (!ok) *err = std::string("writing failed: ") + strerror(errno);
    return ok;
}

bool process(Session& s, Track& t, In& in, std::string* err) {
    t.a48.clear();
    bool open;
    if (in.mode == DemodulatorMode::DIGITAL) {
        // the decoder's voice on the VFO's clock
        t.frac += in.x.size() * 48000.0 / in.rate;
        const size_t want = static_cast<size_t>(t.frac);
        t.frac -= static_cast<double>(want);
        const size_t have = std::min(want, t.voice.size());
        open = have > 0;
        if (s.cfg.skip_closed) t.a48.assign(t.voice.begin(), t.voice.begin() + static_cast<long>(have));
        else {
            t.a48.assign(want, 0.0f);
            std::copy(t.voice.begin(), t.voice.begin() + static_cast<long>(have), t.a48.begin());
        }
        t.voice.erase(t.voice.begin(), t.voice.begin() + static_cast<long>(have));
    } else {
        if (!t.fm || in.mode != t.fm_mode || std::fabs(in.rate - t.fm_rate) > 0.5f) {
            t.fm = std::make_unique<FMDemodulatorRobust>();
            t.fm->setInputSampleRate(in.rate);
            t.fm->setDemodulatorMode(in.mode);
            t.fm_mode = in.mode;
            t.fm_rate = in.rate;
        }
        t.fm->process_decimated_samples(std::move(in.x), t.a48);
        open = !in.sq_on || in.sq_open;
        if (!open) {
            if (s.cfg.skip_closed) t.a48.clear();
            else std::fill(t.a48.begin(), t.a48.end(), 0.0f);
        }
    }
    t.open_now = open;
    t.sq_now = in.mode == DemodulatorMode::DIGITAL || in.sq_on;
    return emit(s, t, t.a48.data(), t.a48.size(), err);
}

std::string status_json(Session& s, bool finishing) {
    std::ostringstream o;
    uint64_t bytes = 0;
    o << "{\"kind\":\"audio\",\"name\":\"" << json_escape(s.cfg.name) << "\",\"state\":\"" << (finishing ? "finishing" : "recording")
      << "\",\"started\":" << s.start_ms << ",\"elapsed_s\":" << fmt((mono_ms() - s.start_mono) / 1000.0, 1)
      << ",\"limit_s\":" << fmt(s.cfg.limit_s, 0) << ",\"format\":\"" << (s.cfg.wav ? "wav" : "opus") << "\",\"vfos\":[";
    bool first = true;
    for (auto& [id, t] : s.tracks) {
        const uint64_t b = t->done_bytes + file_bytes(*t);
        bytes += b;
        o << (first ? "" : ",") << "{\"id\":" << id << ",\"file\":\""
          << json_escape(t->base + (t->part > 1 ? "_" + std::to_string(t->part) : "") + (s.cfg.wav ? ".wav" : ".opus"))
          << "\",\"bytes\":" << b << ",\"seconds\":" << fmt(t->done_s + file_seconds(*t), 1) << ",\"open\":"
          << (t->open_now ? "true" : "false") << ",\"sq\":" << (t->sq_now ? "true" : "false") << ",\"mhz\":" << fmt(t->mhz, 4) << ",\"demod\":\"" << t->demod << "\"}";
        first = false;
    }
    o << "],\"bytes\":" << bytes << ",\"bps\":" << fmt(detail::audio_bytes_per_s(s.cfg.wav, s.cfg.rate, s.cfg.kbps) * s.tracks.size(), 0)
      << ",\"dropped_s\":" << fmt(g_dropped / 48000.0, 1) << "}";
    return o.str();
}

void worker(Session* s) {
    std::string reason, err;
    int64_t last_tick = 0, last_header = mono_ms();
    for (;;) {
        std::deque<In> in;
        std::map<int, std::vector<float>> voice;
        bool stop;
        {
            std::unique_lock<std::mutex> lk(a_mu);
            a_cv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_in.empty() || !g_voice.empty() || g_stop; });
            in.swap(g_in);
            voice.swap(g_voice);
            g_queued.clear();
            stop = g_stop;
            reason = g_reason;
        }
        for (auto& [vfo, x] : voice) {
            auto it = s->tracks.find(vfo);
            if (it == s->tracks.end() || x.empty()) continue;
            Track& t = *it->second;
            if (!t.up) t.up = firinterp_rrrf_create_kaiser(6, 12, 60.0f);
            for (float v : x) {
                float y[6];
                firinterp_rrrf_execute(t.up, v, y);
                t.voice.insert(t.voice.end(), y, y + 6);
            }
            if (t.voice.size() > 48000 * 10) t.voice.erase(t.voice.begin(), t.voice.end() - 48000 * 10);
        }
        for (auto& i : in) {
            auto it = s->tracks.find(i.vfo);
            if (it == s->tracks.end() || !err.empty()) continue;
            if (!process(*s, *it->second, i, &err)) {
                std::lock_guard<std::mutex> lk(a_mu);
                g_stop = true;
                g_reason = err;
            }
        }
        const int64_t t = mono_ms();
        if (s->cfg.limit_s > 0 && t - s->start_mono >= static_cast<int64_t>(s->cfg.limit_s * 1000)) {
            std::lock_guard<std::mutex> lk(a_mu);
            if (!g_stop) {
                g_stop = true;
                g_reason = "time limit";
            }
        }
        if (t - last_header >= 5000) {
            for (auto& [id, tr] : s->tracks)
                if (tr->wav) tr->wav->update_header();
            last_header = t;
        }
        if (t - last_tick >= 1000 || stop) {
            last_tick = t;
            // the decoders of the recorded VFOs give their voice (one switched on later too)
            for (auto& [id, tr] : s->tracks)
                if (auto inst = decimator_manager.getDecimator(id); inst && inst->digital) inst->digital->set_record_tap(true);
            std::string why;
            uint64_t fr = 0, tot = 0;
            if (diskutil::disk_space(s->cfg.folder, &fr, &tot) && fr < s->cfg.reserve) why = "the drive is full (reserve kept free)";
            const std::string st = status_json(*s, stop);
            std::lock_guard<std::mutex> lk(a_mu);
            g_status = st;
            if (!why.empty() && !g_stop) {
                g_stop = true;
                g_reason = why;
            }
        }
        detail::changed();
        if (stop) break;
    }
    uint64_t bytes = 0;
    double secs = 0;
    for (auto& [id, tr] : s->tracks) {
        close_file(*tr);
        bytes += tr->done_bytes;
        secs = std::max(secs, tr->done_s);
        if (auto inst = decimator_manager.getDecimator(id); inst && inst->digital) inst->digital->set_record_tap(false);
    }
    std::cout << "Recorder: audio " << s->cfg.name << " finished (" << reason << "): " << s->tracks.size() << " file(s), "
              << bytes / 1000 << " kB" << std::endl;
    const std::string name = s->cfg.name;
    {
        std::lock_guard<std::mutex> lk(a_mu);
        g_active.store(false);
        g_vfos.clear();
        g_in.clear();
        g_voice.clear();
        g_status.clear();
        g_s->th.detach();
        g_s.reset();   // s is gone
    }
    a_cv.notify_all();
    detail::finished("audio", name, reason, bytes, secs, err);
}

}  // namespace

// --- detail -------------------------------------------------------------------------
namespace detail {

double audio_bytes_per_s(bool wav, int rate, int kbps) { return wav ? rate * 2.0 : kbps * 1000.0 / 8.0 * 1.02; }

bool audio_start(const AudioConfig& c, std::string* err) {
    std::lock_guard<std::mutex> lk(a_mu);
    if (g_s) return *err = "already recording", false;
    auto s = std::make_unique<Session>();
    s->cfg = c;
    s->start_ms = now_ms();
    s->start_mono = mono_ms();
    for (int id : c.vfos) {
        auto t = std::make_unique<Track>();
        t->vfo = id;
        auto inst = decimator_manager.getDecimator(id);
        int tuner = 0;
        double off = 0;
        DemodulatorMode m = DemodulatorMode::NBFM;
        if (inst) {
            tuner = multi_tuner_mode() ? inst->tuner_channel.load() : 0;
            off = inst->frequency_offset_hz.load();
            m = inst->demod_mode.load();
        }
        const double tuner_hz = multi_tuner_mode() ? static_cast<double>(exact_tuner_hz(tuner)) : static_cast<double>(coherent_tuner_hz());
        t->mhz = (tuner_hz + off) / 1e6;
        t->demod = demod_name(m);
        t->base = c.name + "_D" + std::to_string(id) + "_" + mhz_tag(tuner_hz + off);
        if (!open_file(*s, *t, err)) return false;
        s->tracks[id] = std::move(t);
    }
    g_vfos = c.vfos;
    g_stop = false;
    g_reason.clear();
    g_dropped = 0;
    g_in.clear();
    g_voice.clear();
    g_status = status_json(*s, false);
    Session* raw = s.get();
    g_s = std::move(s);
    g_s->th = std::thread(worker, raw);
    g_active.store(true);
    std::cout << "Recorder: audio " << c.name << " - " << c.vfos.size() << " VFO(s), "
              << (c.wav ? "WAV " + std::to_string(c.rate) + " Hz" : "Opus " + std::to_string(c.kbps) + " kbit/s")
              << (c.limit_s > 0 ? ", " + fmt(c.limit_s, 0) + " s" : ", no time limit") << std::endl;
    static bool handler_set = false;
    if (!handler_set) {
        dig::set_record_voice_handler([](dig::DigitalDecoder* d, const float* x, size_t n) { feed_voice(d->vfo(), x, n); });
        handler_set = true;
    }
    return true;
}

void audio_request_stop(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lk(a_mu);
        if (!g_s || g_stop) return;
        g_stop = true;
        g_reason = reason;
    }
    a_cv.notify_all();
}

bool audio_running() {
    std::lock_guard<std::mutex> lk(a_mu);
    return g_s != nullptr;
}

void audio_shutdown() {
    audio_request_stop("kraken_doa shut down");
    std::unique_lock<std::mutex> lk(a_mu);
    a_cv.wait_for(lk, std::chrono::seconds(5), [] { return g_s == nullptr; });
}

std::string audio_status_json() {
    std::lock_guard<std::mutex> lk(a_mu);
    return g_s ? g_status : "";
}

}  // namespace detail

// --- taps -----------------------------------------------------------------------------
bool audio_active() { return g_active.load(std::memory_order_relaxed); }

bool audio_wants(int vfo) {
    if (!audio_active()) return false;
    std::lock_guard<std::mutex> lk(a_mu);
    return std::find(g_vfos.begin(), g_vfos.end(), vfo) != g_vfos.end();
}

void feed_audio(int vfo, const std::complex<float>* x, size_t n, float rate_hz, double rf_hz, DemodulatorMode mode,
                bool squelch_on, bool squelch_open, bool digital_voice) {
    if (!audio_active() || n == 0 || !(rate_hz > 0)) return;
    {
        std::lock_guard<std::mutex> lk(a_mu);
        if (!g_s || g_stop || std::find(g_vfos.begin(), g_vfos.end(), vfo) == g_vfos.end()) return;
        if (g_s->cfg.limit_s > 0 && mono_ms() - g_s->start_mono >= static_cast<int64_t>(g_s->cfg.limit_s * 1000)) return;
        size_t& q = g_queued[vfo];
        if (q + n > static_cast<size_t>(rate_hz * MAX_QUEUED_S)) {
            g_dropped += static_cast<uint64_t>(n * 48000.0 / rate_hz);
            return;
        }
        q += n;
        g_in.push_back({vfo, std::vector<cf>(x, x + n), rate_hz, rf_hz, mode, squelch_on, squelch_open, digital_voice});
    }
    a_cv.notify_all();
}

void feed_voice(int vfo, const float* x8k, size_t n) {
    if (!audio_active() || n == 0) return;
    std::lock_guard<std::mutex> lk(a_mu);
    if (!g_s || std::find(g_vfos.begin(), g_vfos.end(), vfo) == g_vfos.end()) return;
    auto& v = g_voice[vfo];
    if (v.size() < 8000 * 10) v.insert(v.end(), x8k, x8k + n);
}

}  // namespace rec
