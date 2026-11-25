// Offline test harness for the Digital Decoder (not part of kraken_doa): runs
// the decoder plugins exactly as a VFO does (out of process, through the
// same resampling), synchronously. Run from kraken_doa_v2 (plugins/), or set
// KRAKEN_PLUGIN_DIR.
//   digi_test MODE FORMAT FILE [--fs HZ] [--offset HZ] [--vfo-rate HZ] [--conj] [--verbose] [--seconds S]
//                              [--opt plugin.key=value] [--voice out.wav] [--station LAT,LON]
//                              [--rf HZ] [--retune S:HZ] [--stop S]
// MODE: AUTO, PLUGIN:<id>, or an old protocol name (P25 DMR TETRA DSTAR NXDN MPT1327)
// FORMAT: u8 (rtl_sdr interleaved uint8 IQ, --fs required), wav (PCM IQ, 16/24 bit),
//         u8mc (every antenna of an array, uint8 IQ sample by sample, --channels M
//         --fs HZ, played at the VFO rate = fs: a multichannel plugin gets them all -
//         SAMPLES_MC -, the others antenna 0; --rf HZ the RF of its centre),
//         dis (S16LE discriminator samples at 48 kHz, DSDcc test files - FM
//         modulated again for the plugins), cf32
// Trunked systems: every report the engine hands over (kp::Host::grant ...,
// after its checks, and its own RESETs) is printed as it comes, one line
//   [trunk] p25 GRANT t=1.234 ch="1-0412" num=412 mhz=851.012500 up=? slot=-1 ...
// (text escaped: \xNN = a byte < 0x20 / 0x7F - there must be none). The
// regression hook of the plugin wire protocol. --retune S:HZ moves the VFO's
// RF at S seconds (the engine resets the plugins: a RESET follows), --stop S
// turns the decoder off at S seconds (RESET too); --rf HZ = the RF of the
// input (default 100 MHz)
// Memory: u8, wav, cf32 and u8mc are read as a stream (chunks of 64 k
// samples, mixed and resampled with the state carried on - the same samples
// as one pass over the whole file), and --voice is written as it comes: a
// few MB whatever the recording's length. A pipe as FILE is read whole once
// (raw bytes; a regular file is read where needed). dis is loaded whole
// (its mean and rms scale it before the first sample): 16 bytes per 48 kHz
// sample, 46 MB per minute - meant for the short DSDcc test files.
// Build (from kraken_doa_v2, liquid-dsp built by `make` first):
//   g++ -std=c++20 -O2 -Iinclude -isystem liquid-dsp/build-kraken/include tools/digi_test.cpp src/digital/*.cpp
//       src/utils/process_posix.cpp src/utils/fs_compat.cpp src/ai_sandbox.cpp src/geo_address.cpp
//       liquid-dsp/build-kraken/libliquid.a
// Windows (MSYS2 UCRT64 shell): process_win.cpp instead, the Windows flags and
// win/win_init.cpp, static (no MinGW DLLs needed):
//   g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNOGDI
//       -D_USE_MATH_DEFINES -D_FILE_OFFSET_BITS=64 -include win/posix_compat.h -Iinclude
//       -isystem liquid-dsp/build-kraken/include tools/digi_test.cpp src/digital/*.cpp src/utils/process_win.cpp
//       src/utils/fs_compat.cpp src/ai_sandbox.cpp src/geo_address.cpp win/win_init.cpp
//       liquid-dsp/build-kraken/libliquid.a -static -lws2_32 -lwinmm -lbcrypt
#include "digital/digital_decoder.hpp"
#include "digital/dig_plugin.hpp"
#include <liquid/liquid.h>
#include <sys/stat.h>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
using cf = std::complex<float>;

// a delivered trunk event as one line
static std::string tq(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7F) {
            char b[8];
            snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        } else {
            if (c == '"' || c == '\\') o += '\\';
            o += static_cast<char>(c);
        }
    }
    return o + "\"";
}
static std::string tmhz(double f) {
    if (!std::isfinite(f)) return "?";
    char b[40];
    snprintf(b, sizeof b, "%.6f", f / 1e6);
    return b;
}
static void print_trunk(const std::string& plugin, const trunk::PluginEvent& e) {
    static const char* kinds[] = {"SYSTEM", "CHANNEL", "CONTROL", "GRANT", "GRANT_END", "CALL", "RESET"};
    char t[32];
    snprintf(t, sizeof t, "%.3f", e.t_s);
    std::string o = "  [trunk] " + plugin + " " + kinds[e.kind] + " t=" + t;
    auto ch = [&]() {
        o += " ch=" + tq(e.ch.name) + " num=" + std::to_string(e.ch.number) + " mhz=" + tmhz(e.ch.freq_hz) +
             " up=" + tmhz(e.ch.uplink_hz) + " slot=" + std::to_string(e.ch.slot);
    };
    switch (e.kind) {
        case trunk::PluginEvent::SYSTEM:
            o += " key=" + tq(e.sys_key) + " name=" + tq(e.sys_name) + " site=" + tq(e.sys_site) +
                 " control=" + (e.control ? "1" : "0");
            break;
        case trunk::PluginEvent::CHANNEL:
        case trunk::PluginEvent::CONTROL:
        case trunk::PluginEvent::GRANT_END:
            ch();
            break;
        case trunk::PluginEvent::GRANT: {
            ch();
            std::string fl = std::string(e.encrypted ? "e" : "") + (e.emergency ? "m" : "") + (e.data ? "d" : "") + (e.update ? "u" : "");
            o += " target=" + tq(e.target) + (e.group ? " group" : " unit") + " source=" + tq(e.source) +
                 " flags=" + (fl.empty() ? "-" : fl) + " decoder=" + tq(e.decoder) + " demod=" + tq(e.demod) +
                 " bw=" + std::to_string(static_cast<long long>(std::llround(e.bw_hz))) +
                 " options=" + std::to_string(e.options.size());
            for (const auto& [k, v] : e.options) o += " opt." + k + "=" + tq(v);
            break;
        }
        case trunk::PluginEvent::CALL: {
            std::string fl = std::string(e.encrypted ? "e" : "") + (e.emergency ? "m" : "");
            o += " state=" + std::to_string(e.call_state) + " target=" + tq(e.target) + (e.group ? " group" : " unit") +
                 " source=" + tq(e.source) + " slot=" + std::to_string(e.call_slot) + " flags=" + (fl.empty() ? "-" : fl);
            break;
        }
        case trunk::PluginEvent::RESET:
            break;
    }
    std::cout << o << "\n" << std::flush;
}

// The input's bytes, read where they are asked for: a regular file through
// fseeko / fread (memory independent of its length); anything else (a pipe)
// can't seek, so its bytes are read whole once
struct Input {
    FILE* f = nullptr;
    std::vector<unsigned char> mem;
    bool in_mem = false;
    uint64_t size = 0, pos = 0;
    bool open(const std::string& path) {
        f = fopen(path.c_str(), "rb");
        if (!f) return false;
        struct stat st;
        if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode)) {
            size = static_cast<uint64_t>(st.st_size);
            return true;
        }
        in_mem = true;
        std::vector<unsigned char> b(1 << 16);
        for (size_t got; (got = fread(b.data(), 1, b.size(), f)) > 0;) mem.insert(mem.end(), b.begin(), b.begin() + got);
        size = mem.size();
        fclose(f);
        f = nullptr;
        return true;
    }
    // bytes [off, off + n) -> dst; returns how many there are
    size_t read_at(uint64_t off, void* dst, size_t n) {
        if (off >= size) return 0;
        n = static_cast<size_t>(std::min<uint64_t>(n, size - off));
        if (in_mem) {
            memcpy(dst, mem.data() + off, n);
            return n;
        }
        if (off != pos && fseeko(f, static_cast<off_t>(off), SEEK_SET) != 0) {
            pos = UINT64_MAX;
            return 0;
        }
        const size_t got = fread(dst, 1, n, f);
        pos = off + got;
        return got;
    }
    ~Input() {
        if (f) fclose(f);
    }
};

// A 2-channel PCM IQ WAV: where its samples are (the data chunk, cut at the
// end of the file), the frames and the rate; false = not one
struct WavInfo { uint64_t off = 0; size_t frames = 0; int ch = 0, bps = 0; uint32_t rate = 0; };
static bool wav_info(Input& in, WavInfo& w) {
    unsigned char h[24];
    auto at = [&](uint64_t p) {   // the 24 bytes at p, zeros past the end
        memset(h, 0, sizeof h);
        in.read_at(p, h, sizeof h);
    };
    at(0);
    if (in.size < 44 || memcmp(h, "RIFF", 4)) return false;
    uint64_t p = 12; int bits = 0;
    while (p + 8 <= in.size) {
        at(p);
        uint32_t len; memcpy(&len, h + 4, 4);
        if (!memcmp(h, "fmt ", 4)) { uint16_t c, b; memcpy(&c, h + 10, 2); memcpy(&w.rate, h + 12, 4); memcpy(&b, h + 22, 2); w.ch = c; bits = b; }
        else if (!memcmp(h, "data", 4)) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(len, in.size - p - 8));
            w.off = p + 8;
            w.bps = bits / 8;
            if (w.bps * w.ch == 0) return false;
            w.frames = n / (w.bps * w.ch);
            return w.ch == 2;
        }
        p += 8 + len + (len & 1);   // (32-bit sum, as it always was)
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: digi_test MODE FORMAT FILE [opts]\n"); return 1; }
    std::string mode = argv[1], fmt = argv[2], file = argv[3];
    float fs = 0, offset = 0, vfo_rate = 48000, seconds = 1e9;
    int channels = 0;
    double rf_hz = 100e6;
    std::string voice_out;
    bool conj = false;
    double retune_at = -1, retune_hz = 0, stop_at = -1;
    dig::Options opt;
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--fs") fs = atof(argv[++i]);
        else if (a == "--offset") offset = atof(argv[++i]);
        else if (a == "--vfo-rate") vfo_rate = atof(argv[++i]);
        else if (a == "--conj") conj = true;
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--seconds") seconds = atof(argv[++i]);
        else if (a == "--channels") channels = atoi(argv[++i]);
        else if (a == "--rf") rf_hz = atof(argv[++i]);
        else if (a == "--voice") voice_out = argv[++i];
        else if (a == "--retune") {
            if (sscanf(argv[++i], "%lf:%lf", &retune_at, &retune_hz) != 2) { fprintf(stderr, "--retune S:HZ\n"); return 2; }
        }
        else if (a == "--stop") stop_at = atof(argv[++i]);
        else if (a == "--station") {
            double la = 0, lo = 0;
            if (sscanf(argv[++i], "%lf,%lf", &la, &lo) == 2) dig::set_station_location(true, la, lo);
        }
        else if (a == "--opt") {
            std::string kv = argv[++i];
            size_t eq = kv.find('=');
            opt.plugin[kv.substr(0, eq)] = eq == std::string::npos ? "" : kv.substr(eq + 1);
        }
    }
    dig::PluginRegistry::instance().scan();
    dig::DigitalDecoder dec(false);
    dec.set_options(opt);
    dec.set_trunk_handler([](const std::string& plugin, trunk::PluginEvent&& e) { print_trunk(plugin, e); });
    // the input's RF at time t (--retune), the decoder stopped after --stop
    bool stopped = false;
    auto rf_at = [&](double t) {
        if (stop_at >= 0 && t >= stop_at && !stopped) {
            stopped = true;
            dec.set_mode(dig::Mode::OFF);
        }
        return retune_at >= 0 && t >= retune_at ? retune_hz : rf_hz;
    };
    dig::Mode m;
    std::string plugin;
    if (!dig::parse_mode_string(mode, &m, &plugin)) { fprintf(stderr, "unknown mode %s\n", mode.c_str()); return 2; }
    dec.set_mode(m, plugin);
    uint64_t seq = 0;
    // the audio thread's role: pull 48 kHz voice in step with the input,
    // written to --voice at once (16-bit mono; the header's sizes at the end)
    FILE* vf = nullptr;
    auto wav_header = [&](uint32_t n) {
        uint32_t rate = 48000, br = rate * 2, sz = 36 + n * 2;
        uint16_t one = 1, bps = 16, ba = 2;
        uint32_t fmtlen = 16;
        fwrite("RIFF", 1, 4, vf); fwrite(&sz, 4, 1, vf); fwrite("WAVEfmt ", 1, 8, vf); fwrite(&fmtlen, 4, 1, vf);
        fwrite(&one, 2, 1, vf); fwrite(&one, 2, 1, vf); fwrite(&rate, 4, 1, vf); fwrite(&br, 4, 1, vf);
        fwrite(&ba, 2, 1, vf); fwrite(&bps, 2, 1, vf); fwrite("data", 1, 4, vf); uint32_t dl = n * 2; fwrite(&dl, 4, 1, vf);
    };
    if (!voice_out.empty()) {
        vf = fopen(voice_out.c_str(), "wb");
        if (!vf) { fprintf(stderr, "cannot write %s\n", voice_out.c_str()); return 1; }
        wav_header(0);
    }
    std::vector<float> voice;
    std::vector<int16_t> vq;
    uint64_t voice_n = 0, voice_nz = 0;
    double voice_due = 0;
    auto pull = [&](double secs) {
        if (voice_out.empty()) return;
        voice_due += secs * 48000;
        size_t n = static_cast<size_t>(voice_due);
        voice_due -= n;
        voice.resize(n);
        vq.resize(n);
        dec.pull_voice(voice.data(), n);
        for (size_t k = 0; k < n; k++) {
            vq[k] = (int16_t)std::lround(std::clamp(voice[k], -1.0f, 1.0f) * 32767);
            voice_nz += vq[k] != 0;
        }
        fwrite(vq.data(), 2, n, vf);
        voice_n += n;
    };
    if (!voice_out.empty()) { float z[8]; dec.pull_voice(z, 0); }
    auto flush_events = [&]() {
        std::string s = dec.status_json(seq, 1000);
        // print events crudely
        size_t e = s.find("\"events\":[");
        size_t p = e + 10;
        while ((p = s.find("\"m\":\"", p)) != std::string::npos) {
            size_t q = s.find("\"}", p);
            size_t pp = s.rfind("\"p\":\"", p);
            std::cout << "  [" << s.substr(pp + 5, s.find('"', pp + 5) - pp - 5) << "] " << s.substr(p + 5, q - p - 5) << "\n";
            p = q;
        }
        seq = dec.last_event_seq();
    };
    if (fmt == "dis") {
        std::ifstream f(file, std::ios::binary);
        std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
        std::vector<int16_t> s(raw.size() / 2);
        memcpy(s.data(), raw.data(), s.size() * 2);
        // DSDcc .dis: scale to ~ Hz (rms of a 4FSK/GMSK signal ~ 1.5 kHz)
        double m = 0, e = 0;
        for (auto v : s) m += v;
        m /= s.size();
        for (auto v : s) e += (v - m) * (v - m);
        float sd = std::sqrt(e / s.size());
        float k = 1500.0f / (sd > 0 ? sd : 1);
        std::vector<float> hz(s.size());
        for (size_t i = 0; i < s.size(); i++) hz[i] = float(s[i] - m) * k * (conj ? -1 : 1);
        // FM-modulate it again: the plugins take complex baseband
        std::vector<cf> x(hz.size());
        double ph = 0;
        for (size_t i = 0; i < hz.size(); i++) {
            ph = std::remainder(ph + 2 * M_PI * hz[i] / 48000.0, 2 * M_PI);
            x[i] = std::polar(1.0f, float(ph));
        }
        for (size_t i = 0; i < x.size() && i < seconds * 48000; i += 480) {
            dec.process(x.data() + i, std::min<size_t>(480, x.size() - i), 48000, rf_at(i / 48000.0));
            pull(0.01);
            flush_events();
        }
    } else if (fmt == "u8mc") {
        // every antenna, 10 ms blocks at the VFO rate (= fs), streamed
        if (channels < 2 || channels > 8 || fs <= 0) { fprintf(stderr, "u8mc needs --channels 2..8 and --fs\n"); return 1; }
        FILE* f = fopen(file.c_str(), "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", file.c_str()); return 1; }
        const size_t blk = static_cast<size_t>(fs / 100), m = static_cast<size_t>(channels);
        std::vector<unsigned char> b(blk * m * 2);
        std::vector<cf> x0(blk);
        std::vector<std::complex<int16_t>> mc(blk * m);
        size_t done = 0;
        for (size_t got; done < seconds * fs && (got = fread(b.data(), m * 2, blk, f)) > 0; done += got) {
            for (size_t i = 0; i < got; i++)
                for (size_t k = 0; k < m; k++) {
                    const float re = (b[(i * m + k) * 2] - 127.5f) / 127.5f, im = (b[(i * m + k) * 2 + 1] - 127.5f) / 127.5f;
                    if (k == 0) x0[i] = cf(re, im);
                    mc[i * m + k] = {static_cast<int16_t>(std::lround(re * dig::DigitalDecoder::MC_SCALE)),
                                     static_cast<int16_t>(std::lround(im * dig::DigitalDecoder::MC_SCALE))};
                }
            dec.process(x0.data(), got, fs, rf_at(done / fs), dig::DigitalDecoder::NO_POS, mc.data(), channels);
            pull(0.01);
            flush_events();
        }
        fclose(f);
        fprintf(stderr, "%zu samples x %d antennas at %.0f Hz\n", done, channels, fs);
    } else {
        // u8 / wav / cf32 streamed: read a chunk, conjugate, mix, resample
        // (the resampler keeps its state: the chunks give exactly the samples
        // of one pass over the whole file), hand on 10 ms blocks at the VFO rate
        Input in;
        if (!in.open(file)) { fprintf(stderr, "cannot open %s\n", file.c_str()); return 1; }
        uint64_t off = 0;
        size_t nx = 0, bpf = 8;   // samples, bytes per sample
        WavInfo wi;
        if (fmt != "wav" && !(fs > 0)) { fprintf(stderr, "%s needs --fs\n", fmt.c_str()); return 1; }
        if (fmt == "u8") {
            nx = std::min<size_t>(static_cast<size_t>(in.size / 2), size_t(seconds * fs));
            bpf = 2;
        } else if (fmt == "wav") {
            if (!wav_info(in, wi)) { fprintf(stderr, "bad wav\n"); return 1; }
            fs = wi.rate;
            off = wi.off;
            nx = wi.frames;
            bpf = static_cast<size_t>(wi.bps * wi.ch);
        } else if (fmt == "cf32") {
            nx = static_cast<size_t>(in.size / 8);
        }
        fprintf(stderr, "%zu samples at %.0f Hz\n", nx, fs);
        double ph = 0, w = -2 * M_PI * offset / fs;
        const bool resamp = std::fabs(vfo_rate - fs) > 1;
        msresamp_crcf r = resamp ? msresamp_crcf_create(vfo_rate / fs, 60) : nullptr;
        const size_t CH = 1 << 16, blk = vfo_rate / 100;
        std::vector<unsigned char> raw(CH * bpf);
        std::vector<cf> x(CH), yo(static_cast<size_t>(CH * (vfo_rate / fs)) + 1000), y;
        size_t i = 0, y0 = 0;   // the next block's first sample; y[0]'s index
        auto block = [&](size_t n) {
            dec.process(y.data() + (i - y0), n, vfo_rate, rf_at(i / vfo_rate));
            pull(0.01);
            flush_events();
            i += blk;
        };
        for (size_t done = 0; done < nx && i < seconds * vfo_rate;) {
            const size_t n = std::min(CH, nx - done);
            if (in.read_at(off + static_cast<uint64_t>(done) * bpf, raw.data(), n * bpf) < n * bpf) break;
            for (size_t k = 0; k < n; k++) {
                const unsigned char* b = raw.data() + k * bpf;
                if (fmt == "u8") {
                    x[k] = cf((b[0] - 127.5f) / 127.5f, (b[1] - 127.5f) / 127.5f);
                } else if (fmt == "wav") {
                    float v[2];
                    for (int c = 0; c < 2; c++) {
                        const unsigned char* q = b + c * wi.bps;
                        int32_t s = wi.bps == 2 ? int16_t(q[0] | q[1] << 8) : (int32_t(q[0] << 8 | q[1] << 16 | q[2] << 24) >> 8);
                        v[c] = s / (wi.bps == 2 ? 32768.0f : 8388608.0f);
                    }
                    x[k] = cf(v[0], v[1]);
                } else {
                    memcpy(&x[k], b, 8);
                }
            }
            done += n;
            if (conj) for (size_t k = 0; k < n; k++) x[k] = std::conj(x[k]);
            // mix + resample to the VFO rate (what the decimator would deliver)
            for (size_t k = 0; k < n; k++) { x[k] *= std::polar(1.0f, float(ph)); ph = std::remainder(ph + w, 2 * M_PI); }
            y.erase(y.begin(), y.begin() + static_cast<std::ptrdiff_t>(i - y0));
            y0 = i;
            if (resamp) {
                unsigned int ny = 0;
                msresamp_crcf_execute(r, x.data(), n, yo.data(), &ny);
                y.insert(y.end(), yo.begin(), yo.begin() + ny);
            } else {
                y.insert(y.end(), x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n));
            }
            while (y.size() - (i - y0) >= blk && i < seconds * vfo_rate) block(blk);
        }
        // the last, shorter block
        while (i - y0 < y.size() && i < seconds * vfo_rate) block(std::min(blk, y.size() - (i - y0)));
        if (r) msresamp_crcf_destroy(r);
    }
    dec.drain();
    pull(0.5);
    flush_events();
    if (vf) {
        const uint32_t n = static_cast<uint32_t>(voice_n);
        fseeko(vf, 0, SEEK_SET);
        wav_header(n);
        fclose(vf);
        fprintf(stderr, "voice: %.1f s written, %.1f s non-silent\n", n / 48000.0, voice_nz / 48000.0);
    }
    std::string s = dec.status_json(dec.last_event_seq(), 0, true);
    std::cout << s << "\n";
    std::string pts, keys;
    dec.report().map_json(0, 0, pts, keys);
    if (!pts.empty()) std::cout << "map: [" << pts << "]\n";
}
