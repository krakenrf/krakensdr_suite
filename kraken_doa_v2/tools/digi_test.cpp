// Offline test harness for the digital decoders (not part of kraken_doa).
//   digi_test MODE FORMAT FILE [--fs HZ] [--offset HZ] [--vfo-rate HZ] [--conj] [--verbose] [--seconds S]
// MODE: AUTO P25 DMR TETRA DSTAR
// FORMAT: u8 (rtl_sdr interleaved uint8 IQ, --fs required), wav (PCM IQ, 16/24 bit),
//         dis (S16LE discriminator samples at 48 kHz, DSDcc test files), cf32
#include "digital/digital_decoder.hpp"
#include <liquid/liquid.h>
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

static bool read_wav(const char* fn, std::vector<cf>& out, float& fs) {
    std::ifstream f(fn, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 44 || memcmp(d.data(), "RIFF", 4)) return false;
    size_t p = 12; int ch = 0, bits = 0; uint32_t rate = 0;
    while (p + 8 <= d.size()) {
        uint32_t len; memcpy(&len, &d[p + 4], 4);
        if (!memcmp(&d[p], "fmt ", 4)) { uint16_t c, b; memcpy(&c, &d[p + 10], 2); memcpy(&rate, &d[p + 12], 4); memcpy(&b, &d[p + 22], 2); ch = c; bits = b; }
        else if (!memcmp(&d[p], "data", 4)) {
            size_t n = std::min<size_t>(len, d.size() - p - 8);
            const unsigned char* s = reinterpret_cast<const unsigned char*>(&d[p + 8]);
            int bps = bits / 8;
            size_t frames = n / (bps * ch);
            for (size_t i = 0; i < frames; i++) {
                float v[2];
                for (int c = 0; c < 2; c++) {
                    const unsigned char* q = s + (i * ch + c) * bps;
                    int32_t x = bps == 2 ? int16_t(q[0] | q[1] << 8) : (int32_t(q[0] << 8 | q[1] << 16 | q[2] << 24) >> 8);
                    v[c] = x / (bps == 2 ? 32768.0f : 8388608.0f);
                }
                out.emplace_back(v[0], v[1]);
            }
            fs = rate;
            return ch == 2;
        }
        p += 8 + len + (len & 1);
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: digi_test MODE FORMAT FILE [opts]\n"); return 1; }
    std::string mode = argv[1], fmt = argv[2], file = argv[3];
    float fs = 0, offset = 0, vfo_rate = 48000, seconds = 1e9;
    std::string voice_out;
    bool conj = false;
    dig::Options opt;
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--fs") fs = atof(argv[++i]);
        else if (a == "--offset") offset = atof(argv[++i]);
        else if (a == "--vfo-rate") vfo_rate = atof(argv[++i]);
        else if (a == "--conj") conj = true;
        else if (a == "--verbose") opt.verbose = true;
        else if (a == "--seconds") seconds = atof(argv[++i]);
        else if (a == "--voice") voice_out = argv[++i];
    }
    dig::DigitalDecoder dec(false);
    dec.set_options(opt);
    dec.set_mode(dig::mode_from_string(mode));
    uint64_t seq = 0;
    // the audio thread's role: pull 48 kHz voice in step with the input
    std::vector<float> voice;
    double voice_due = 0;
    auto pull = [&](double secs) {
        if (voice_out.empty()) return;
        voice_due += secs * 48000;
        size_t n = static_cast<size_t>(voice_due);
        voice_due -= n;
        size_t o = voice.size();
        voice.resize(o + n);
        dec.pull_voice(voice.data() + o, n);
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
        for (size_t i = 0; i < hz.size() && i < seconds * 48000; i += 960) {
            dec.process_discriminator(hz.data() + i, std::min<size_t>(960, hz.size() - i));
            pull(0.02);
            flush_events();
        }
    } else {
        std::vector<cf> x;
        if (fmt == "u8") {
            std::ifstream f(file, std::ios::binary);
            std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), {});
            size_t n = std::min<size_t>(b.size() / 2, size_t(seconds * fs));
            x.resize(n);
            for (size_t i = 0; i < n; i++) x[i] = cf((b[2 * i] - 127.5f) / 127.5f, (b[2 * i + 1] - 127.5f) / 127.5f);
        } else if (fmt == "wav") {
            if (!read_wav(file.c_str(), x, fs)) { fprintf(stderr, "bad wav\n"); return 1; }
        } else if (fmt == "cf32") {
            std::ifstream f(file, std::ios::binary);
            std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
            x.resize(b.size() / 8); memcpy(x.data(), b.data(), x.size() * 8);
        }
        if (conj) for (auto& v : x) v = std::conj(v);
        fprintf(stderr, "%zu samples at %.0f Hz\n", x.size(), fs);
        // mix + resample to the VFO rate (what the decimator would deliver)
        double ph = 0, w = -2 * M_PI * offset / fs;
        for (auto& v : x) { v *= std::polar(1.0f, float(ph)); ph = std::remainder(ph + w, 2 * M_PI); }
        std::vector<cf> y(x.size() * (vfo_rate / fs) + 1000);
        unsigned int ny = 0;
        if (std::fabs(vfo_rate - fs) > 1) {
            msresamp_crcf r = msresamp_crcf_create(vfo_rate / fs, 60);
            msresamp_crcf_execute(r, x.data(), x.size(), y.data(), &ny);
            msresamp_crcf_destroy(r);
            y.resize(ny);
        } else y = x;
        size_t blk = vfo_rate / 100;
        for (size_t i = 0; i < y.size() && i < seconds * vfo_rate; i += blk) {
            dec.process(y.data() + i, std::min(blk, y.size() - i), vfo_rate, 100e6);
            pull(0.01);
            flush_events();
        }
    }
    if (!voice_out.empty()) {
        FILE* w = fopen(voice_out.c_str(), "wb");
        uint32_t n = voice.size(), rate = 48000, br = rate * 2, sz = 36 + n * 2;
        uint16_t one = 1, bps = 16, ba = 2;
        uint32_t fmtlen = 16;
        fwrite("RIFF", 1, 4, w); fwrite(&sz, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fmtlen, 4, 1, w);
        fwrite(&one, 2, 1, w); fwrite(&one, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&br, 4, 1, w);
        fwrite(&ba, 2, 1, w); fwrite(&bps, 2, 1, w); fwrite("data", 1, 4, w); uint32_t dl = n * 2; fwrite(&dl, 4, 1, w);
        size_t nz = 0;
        for (float v : voice) { int16_t q = (int16_t)std::lround(std::clamp(v, -1.0f, 1.0f) * 32767); fwrite(&q, 2, 1, w); nz += q != 0; }
        fclose(w);
        fprintf(stderr, "voice: %.1f s written, %.1f s non-silent\n", n / 48000.0, nz / 48000.0);
    }
    std::string s = dec.status_json(dec.last_event_seq(), 0);
    std::cout << s << "\n";
}
