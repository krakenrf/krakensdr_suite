// Voice codecs of the digital decoders - see dig_vocoder.hpp.
//
// The IMBE decoder below is a port of mbelib's IMBE 7200x4400 path
// (imbe7200x4400.c, ecc.c, mbelib.c): Copyright (C) 2010 mbelib Author
// (GPG 0xEA5EFE2C), ISC licence (full notice in mbe_tables.hpp). Changes:
// C++, a per-instance random generator instead of rand() (several decoders
// run on their own threads), float output scaled to +-1.

#include "dig_vocoder.hpp"
#include "mbe_tables.hpp"

#include <dlfcn.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace dig {

using namespace mbe_tables;

namespace {

constexpr float OUT_SCALE = 7.0f / 32768.0f;   // mbelib's short gain, then to +-1

void move_parms(const MbeParms& cur, MbeParms& prev) {
    prev.w0 = cur.w0;
    prev.L = cur.L;
    prev.K = cur.K;
    prev.gamma = cur.gamma;
    prev.repeat = cur.repeat;
    for (int l = 0; l <= 56; l++) {
        prev.Ml[l] = cur.Ml[l];
        prev.Vl[l] = cur.Vl[l];
        prev.log2Ml[l] = cur.log2Ml[l];
        prev.PHIl[l] = cur.PHIl[l];
        prev.PSIl[l] = cur.PSIl[l];
    }
}

void spectral_amp_enhance(MbeParms& mp) {
    float Rm0 = 0, Rm1 = 0, Wl[57];
    for (int l = 1; l <= mp.L; l++) {
        Rm0 += mp.Ml[l] * mp.Ml[l];
        Rm1 += mp.Ml[l] * mp.Ml[l] * std::cos(mp.w0 * l);
    }
    float R2m0 = Rm0 * Rm0, R2m1 = Rm1 * Rm1;
    for (int l = 1; l <= mp.L; l++) {
        if (mp.Ml[l] == 0) continue;
        float den = mp.w0 * Rm0 * (R2m0 - R2m1);
        if (den <= 0) continue;
        Wl[l] = std::sqrt(mp.Ml[l]) *
                std::pow((0.96f * static_cast<float>(M_PI) * ((R2m0 + R2m1) - 2 * Rm0 * Rm1 * std::cos(mp.w0 * l))) / den, 0.25f);
        if (8 * l <= mp.L) {
        } else if (Wl[l] > 1.2f) mp.Ml[l] *= 1.2f;
        else if (Wl[l] < 0.5f) mp.Ml[l] *= 0.5f;
        else mp.Ml[l] *= Wl[l];
    }
    float sum = 0;
    for (int l = 1; l <= mp.L; l++) sum += mp.Ml[l] * mp.Ml[l];
    float gamma = sum == 0 ? 1.0f : std::sqrt(Rm0 / sum);
    for (int l = 1; l <= mp.L; l++) mp.Ml[l] *= gamma;
}

// Golay(23,12) on in[22..0] (in[22] first), mbelib layout; returns errors
int golay2312(const char* in, char* out) {
    long block = 0;
    for (int i = 22; i >= 0; i--) block = (block << 1) + in[i];
    int ecc = 0;
    long mask = 0x400000L;
    for (int i = 0; i < 12; i++) {
        if (block & mask) ecc ^= golayGenerator[i];
        mask >>= 1;
    }
    int syndrome = ecc ^ static_cast<int>(block & 0x7FF);
    long data = (block >> 11) ^ golayMatrix[syndrome];
    for (int i = 22; i >= 11; i--) {
        out[i] = static_cast<char>((data & 2048) >> 11);
        data <<= 1;
    }
    for (int i = 10; i >= 0; i--) out[i] = in[i];
    int errs = 0;
    for (int i = 22; i >= 11; i--) errs += out[i] != in[i];
    return errs;
}

int hamming1511(const char* in, char* out) {
    int block = 0;
    for (int i = 14; i >= 0; i--) block = (block << 1) | in[i];
    int syndrome = 0;
    for (int i = 0; i < 4; i++) {
        syndrome = (syndrome << 1) | (__builtin_popcount(block & hammingGenerator[i]) & 1);
    }
    int errs = 0;
    if (syndrome > 0) {
        errs++;
        block ^= hammingMatrix[syndrome];
    }
    for (int i = 14; i >= 0; i--) {
        out[i] = static_cast<char>((block & 0x4000) >> 14);
        block <<= 1;
    }
    return errs;
}

int bits_lsb_last(const char* b, int from_hi, int to_lo) {   // b[from_hi..to_lo] MSB first
    int v = 0;
    for (int i = from_hi; i >= to_lo; i--) v = (v << 1) | b[i];
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// IMBE
// ---------------------------------------------------------------------------
void ImbeDecoder::reset() {
    prev_ = MbeParms{};
    prev_.w0 = 0.09378f;
    prev_.L = 30;
    prev_.K = 10;
    for (int l = 0; l <= 56; l++) prev_.PSIl[l] = static_cast<float>(M_PI) / 2;
    move_parms(prev_, cur_);
    move_parms(prev_, prev_enh_);
}

int ImbeDecoder::decode_params(const char* d) {
    cur_.repeat = prev_.repeat;
    int b0 = (d[0] << 7) | (d[1] << 6) | (d[2] << 5) | (d[3] << 4) | (d[4] << 3) | (d[5] << 2) | (d[85] << 1) | d[86];
    if (b0 > 207) return 1;   // silence / invalid pitch
    cur_.w0 = 4 * static_cast<float>(M_PI) / (b0 + 39.5f);
    int L = static_cast<int>(0.9254 * static_cast<int>((M_PI / cur_.w0) + 0.25));
    if (L > 56 || L < 9) return 1;
    cur_.L = L;
    const int L9 = L - 9;
    int K = L < 37 ? (L + 2) / 3 : 12;
    cur_.K = K;

    char bb[58][12] = {{0}};
    for (int i = 6; i < 85; i++) bb[bo[L9][i - 6][0]][bo[L9][i - 6][1]] = d[i];

    // voiced/unvoiced per band of 3 harmonics
    for (int i = 1, j = 1, k = K - 1; i <= L; i++) {
        cur_.Vl[i] = bb[1][k];
        if (j == 3) { j = 1; if (k > 0) k--; } else j++;
    }
    float Gm[7], Ri[7], Cik[7][11] = {{0}};
    Gm[1] = B2[bits_lsb_last(bb[2], 5, 0)];
    for (int i = 2; i < 7; i++) {
        int nb = static_cast<int>(ba[L9][i - 2][0]);
        int bm = nb ? bits_lsb_last(bb[i + 1], nb - 1, 0) : 0;
        Gm[i] = ba[L9][i - 2][1] * (bm - std::pow(2.0f, nb - 1) + 0.5f);
    }
    for (int i = 1; i <= 6; i++) {
        float sum = 0;
        for (int m = 1; m <= 6; m++)
            sum += (m == 1 ? 1 : 2) * Gm[m] * std::cos(static_cast<float>(M_PI) * (m - 1) * (i - 0.5f) / 6);
        Ri[i] = sum;
    }
    int m = 8;
    for (int i = 1; i <= 6; i++) {
        Cik[i][1] = Ri[i];
        for (int k = 2; k <= ImbeJi[L9][i - 1]; k++, m++) {
            int Bm = hoba[L9][m - 8];
            if (Bm == 0) { Cik[i][k] = 0; continue; }
            int bm = bits_lsb_last(bb[m], Bm - 1, 0);
            Cik[i][k] = quantstep[Bm - 1] * standdev[k - 2] * (bm - std::pow(2.0f, Bm - 1) + 0.5f);
        }
    }
    float Tl[57];
    for (int i = 1, l = 1; i <= 6; i++) {
        int ji = ImbeJi[L9][i - 1];
        for (int j = 1; j <= ji; j++, l++) {
            float sum = 0;
            for (int k = 1; k <= ji; k++)
                sum += (k == 1 ? 1 : 2) * Cik[i][k] * std::cos(static_cast<float>(M_PI) * (k - 1) * (j - 0.5f) / ji);
            Tl[l] = sum;
        }
    }
    float rho = cur_.L <= 15 ? 0.4f : (cur_.L <= 24 ? 0.03f * cur_.L - 0.05f : 0.7f);
    if (cur_.L > prev_.L) {
        for (int l = prev_.L + 1; l <= cur_.L; l++) {
            prev_.Ml[l] = prev_.Ml[prev_.L];
            prev_.log2Ml[l] = prev_.log2Ml[prev_.L];
        }
    }
    float flokl[57], deltal[57], sum77 = 0;
    int intkl[57];
    for (int l = 1; l <= cur_.L; l++) {
        flokl[l] = static_cast<float>(prev_.L) / cur_.L * l;
        intkl[l] = static_cast<int>(flokl[l]);
        deltal[l] = flokl[l] - intkl[l];
        sum77 += (1 - deltal[l]) * prev_.log2Ml[intkl[l]] + deltal[l] * prev_.log2Ml[std::min(intkl[l] + 1, 56)];
    }
    sum77 *= rho / cur_.L;
    for (int l = 1; l <= cur_.L; l++) {
        float c1 = rho * (1 - deltal[l]) * prev_.log2Ml[intkl[l]];
        float c2 = rho * deltal[l] * prev_.log2Ml[std::min(intkl[l] + 1, 56)];
        cur_.log2Ml[l] = Tl[l] + c1 + c2 - sum77;
        cur_.Ml[l] = std::pow(2.0f, cur_.log2Ml[l]);
    }
    return 0;
}

void ImbeDecoder::synthesize(float* out, MbeParms& cur, MbeParms& prev) {
    const int N = 160;
    const int uvq = 3;
    const float uvthreshold = 2700.0f * static_cast<float>(M_PI) / 4000.0f;
    const float uvsine = 1.3591409f * static_cast<float>(M_E);
    const float uvrand = 2.0f;
    const float qfactor = std::log(static_cast<float>(uvq)) / uvq;
    const float uvstep = 1.0f / uvq;
    const float uvoffset = uvstep * (uvq - 1) / 2;
    auto rphase = [this] { return rnd() * 2 * static_cast<float>(M_PI) - static_cast<float>(M_PI); };

    int numUv = 0;
    for (int l = 1; l <= cur.L; l++) numUv += cur.Vl[l] == 0;
    const float cw0 = cur.w0, pw0 = prev.w0;
    for (int n = 0; n < N; n++) out[n] = 0;
    int maxl;
    if (cur.L > prev.L) {
        maxl = cur.L;
        for (int l = prev.L + 1; l <= maxl; l++) { prev.Ml[l] = 0; prev.Vl[l] = 1; }
    } else {
        maxl = prev.L;
        for (int l = cur.L + 1; l <= maxl; l++) { cur.Ml[l] = 0; cur.Vl[l] = 1; }
    }
    for (int l = 1; l <= 56; l++) {
        cur.PSIl[l] = prev.PSIl[l] + (pw0 + cw0) * (static_cast<float>(l * N) / 2);
        cur.PHIl[l] = l <= cur.L / 4 ? cur.PSIl[l] : cur.PSIl[l] + numUv * rphase() / cur.L;
    }
    float rp[64], rp2[64];
    for (int l = 1; l <= maxl; l++) {
        const float cw0l = cw0 * l, pw0l = pw0 * l;
        auto multisine = [&](float w, float wl, int n, const float* ph) {
            float c = 0;
            for (int i = 0; i < uvq; i++) {
                c += std::cos(w * n * (l + i * uvstep - uvoffset) + ph[i]);
                if (wl > uvthreshold) c += (wl - uvthreshold) * uvrand * rnd();
            }
            return c;
        };
        if (cur.Vl[l] == 0 && prev.Vl[l] == 1) {
            for (int i = 0; i < uvq; i++) rp[i] = rphase();
            for (int n = 0; n < N; n++) {
                float C1 = Ws[n + N] * prev.Ml[l] * std::cos(pw0l * n + prev.PHIl[l]);
                float C3 = multisine(cw0, cw0l, n, rp) * uvsine * Ws[n] * cur.Ml[l] * qfactor;
                out[n] += C1 + C3;
            }
        } else if (cur.Vl[l] == 1 && prev.Vl[l] == 0) {
            for (int i = 0; i < uvq; i++) rp[i] = rphase();
            for (int n = 0; n < N; n++) {
                float C1 = Ws[n] * cur.Ml[l] * std::cos(cw0l * (n - N) + cur.PHIl[l]);
                float C3 = multisine(pw0, pw0l, n, rp) * uvsine * Ws[n + N] * prev.Ml[l] * qfactor;
                out[n] += C1 + C3;
            }
        } else if (cur.Vl[l] == 1 || prev.Vl[l] == 1) {
            for (int n = 0; n < N; n++) {
                float C1 = Ws[n + N] * prev.Ml[l] * std::cos(pw0l * n + prev.PHIl[l]);
                float C2 = Ws[n] * cur.Ml[l] * std::cos(cw0l * (n - N) + cur.PHIl[l]);
                out[n] += C1 + C2;
            }
        } else {
            for (int i = 0; i < uvq; i++) rp[i] = rphase();
            for (int i = 0; i < uvq; i++) rp2[i] = rphase();
            for (int n = 0; n < N; n++) {
                float C3 = multisine(pw0, pw0l, n, rp) * uvsine * Ws[n + N] * prev.Ml[l] * qfactor;
                float C4 = multisine(cw0, cw0l, n, rp2) * uvsine * Ws[n] * cur.Ml[l] * qfactor;
                out[n] += C3 + C4;
            }
        }
    }
}

int ImbeDecoder::decode(char fr[8][23], float* out) {
    // C0: Golay(23,12); its 12 data bits seed the PN that whitens c1..c6
    char tmp[23];
    int errs = golay2312(fr[0], tmp);
    std::memcpy(fr[0], tmp, 23);
    int seed = bits_lsb_last(fr[0], 22, 11);
    uint16_t pr[115];
    pr[0] = static_cast<uint16_t>(16 * seed);
    for (int i = 1; i < 115; i++) pr[i] = static_cast<uint16_t>((173 * pr[i - 1] + 13849) % 65536);
    for (int i = 1; i < 115; i++) pr[i] = pr[i] / 32768;
    int k = 1;
    for (int i = 1; i < 4; i++)
        for (int j = 22; j >= 0; j--) fr[i][j] ^= static_cast<char>(pr[k++]);
    for (int i = 4; i < 7; i++)
        for (int j = 14; j >= 0; j--) fr[i][j] ^= static_cast<char>(pr[k++]);
    // FEC -> 88 data bits
    char d[88], *p = d;
    for (int i = 0; i < 4; i++) {
        if (i > 0) {
            errs += golay2312(fr[i], tmp);
            for (int j = 22; j > 10; j--) *p++ = tmp[j];
        } else {
            for (int j = 22; j > 10; j--) *p++ = fr[i][j];
        }
    }
    for (int i = 4; i < 7; i++) {
        errs += hamming1511(fr[i], tmp);
        for (int j = 14; j >= 4; j--) *p++ = tmp[j];
    }
    for (int j = 6; j >= 0; j--) *p++ = fr[7][j];

    int bad = decode_params(d);
    bool use = !(bad || errs > 5);
    if (!use) {
        // repeat the previous frame (up to 3 times), then mute
        cur_ = prev_;
        cur_.repeat++;
    } else {
        cur_.repeat = 0;
    }
    if (cur_.repeat <= 3) {
        move_parms(cur_, prev_);
        spectral_amp_enhance(cur_);
        synthesize(out, cur_, prev_enh_);
        move_parms(cur_, prev_enh_);
        for (int n = 0; n < 160; n++) out[n] = std::clamp(out[n] * OUT_SCALE, -1.0f, 1.0f);
    } else {
        for (int n = 0; n < 160; n++) out[n] = 0;
        reset();
    }
    return use ? errs : -1;
}

// ---------------------------------------------------------------------------
// mbelib (user-installed) for AMBE / AMBE+2
// ---------------------------------------------------------------------------
MbeLib& MbeLib::instance() {
    static MbeLib m;
    return m;
}

MbeLib::MbeLib() {
    std::vector<std::string> names;
    if (const char* e = std::getenv("KRAKEN_MBELIB")) {
        std::string v = e;
        if (v == "none" || v == "off") {   // explicitly disabled
            status_ = "disabled (KRAKEN_MBELIB=" + v + ")";
            return;
        }
        names.push_back(v);
    }
    for (const char* n : {"libmbe.so.1", "libmbe.so", "/usr/local/lib/libmbe.so.1", "/usr/local/lib/libmbe.so"})
        names.push_back(n);
    void* h = nullptr;
    std::string used;
    for (const auto& n : names) {
        h = dlopen(n.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h) { used = n; break; }
    }
    if (!h) {
        status_ = "not installed (see README: Digital voice codecs)";
        return;
    }
    init_ = reinterpret_cast<decltype(init_)>(dlsym(h, "mbe_initMbeParms"));
    ambe2450_ = reinterpret_cast<decltype(ambe2450_)>(dlsym(h, "mbe_processAmbe3600x2450Framef"));
    ambe2400_ = reinterpret_cast<decltype(ambe2400_)>(dlsym(h, "mbe_processAmbe3600x2400Framef"));
    auto ver = reinterpret_cast<void (*)(char*)>(dlsym(h, "mbe_printVersion"));
    if (!init_ || !ambe2450_ || !ambe2400_) {
        status_ = used + " lacks the AMBE functions";
        return;
    }
    char v[64] = "?";
    if (ver) ver(v);
    status_ = used + " (mbelib " + v + ")";
    ok_ = true;
}

void MbeLib::init_state(void* state) const {
    if (!ok_) return;
    std::memset(state, 0, STATE_BYTES);
    uint8_t* s = static_cast<uint8_t*>(state);
    init_(s, s + 4096, s + 8192);
}

int MbeLib::decode(bool dstar, char fr[4][24], void* state, float* out) const {
    if (!ok_) return -1;
    uint8_t* s = static_cast<uint8_t*>(state);
    int errs = 0, errs2 = 0;
    char err_str[64];
    char ambe_d[49];
    (dstar ? ambe2400_ : ambe2450_)(out, &errs, &errs2, err_str, fr, ambe_d, s, s + 4096, s + 8192, 3);
    for (int n = 0; n < 160; n++) out[n] = std::clamp(out[n] * OUT_SCALE, -1.0f, 1.0f);
    return errs2;
}

AmbeStream::AmbeStream(bool dstar) : dstar_(dstar), state_(MbeLib::STATE_BYTES) { reset(); }
AmbeStream::~AmbeStream() = default;
void AmbeStream::reset() { MbeLib::instance().init_state(state_.data()); }
int AmbeStream::decode(char fr[4][24], float* out) { return MbeLib::instance().decode(dstar_, fr, state_.data(), out); }

}  // namespace dig

// ---------------------------------------------------------------------------
// TETRA ACELP: the ETSI reference codec programs, user-built (README).
//   us --(690-word blocks)--> cdecoder --(2 x 138 words)--> sdecoder --> PCM
// Both programs take file names; /dev/stdin and /dev/stdout make them
// filters. Their stdio buffering adds ~0.5 s of latency.
// ---------------------------------------------------------------------------
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace dig {

namespace {
// cdecoder / sdecoder locations: $KRAKEN_TETRA_CODEC_DIR/{cdecoder,sdecoder},
// else tetra-cdecoder / tetra-sdecoder on the PATH (README installs those)
bool find_codec(std::string& cdec, std::string& sdec) {
    auto exe = [](const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && (st.st_mode & S_IXUSR); };
    if (const char* d = std::getenv("KRAKEN_TETRA_CODEC_DIR")) {
        cdec = std::string(d) + "/cdecoder";
        sdec = std::string(d) + "/sdecoder";
        if (exe(cdec) && exe(sdec)) return true;
    }
    const char* path = std::getenv("PATH");
    std::string ps = path ? path : "/usr/local/bin:/usr/bin";
    size_t a = 0;
    while (a <= ps.size()) {
        size_t b = ps.find(':', a);
        std::string dir = ps.substr(a, b == std::string::npos ? std::string::npos : b - a);
        if (!dir.empty() && exe(dir + "/tetra-cdecoder") && exe(dir + "/tetra-sdecoder")) {
            cdec = dir + "/tetra-cdecoder";
            sdec = dir + "/tetra-sdecoder";
            return true;
        }
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return false;
}
}  // namespace

bool TetraCodec::available() {
    std::string c, s;
    return find_codec(c, s);
}

std::string TetraCodec::status() {
    std::string c, s;
    return find_codec(c, s) ? c + " + " + s : "not installed (see README: Digital voice codecs)";
}

TetraCodec::TetraCodec() = default;
TetraCodec::~TetraCodec() { stop(); }

bool TetraCodec::start() {
    std::string cdec, sdec;
    if (!find_codec(cdec, sdec)) return false;
    signal(SIGPIPE, SIG_IGN);
    int p1[2], p2[2], p3[2];
    if (pipe2(p1, O_CLOEXEC) || pipe2(p2, O_CLOEXEC) || pipe2(p3, O_CLOEXEC)) return false;
    int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    auto spawn = [&](const std::string& prog, int in, int out, pid_t* pid) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in, 0);
        posix_spawn_file_actions_adddup2(&fa, out, 1);
        posix_spawn_file_actions_adddup2(&fa, devnull, 2);
        char* argv[] = {const_cast<char*>(prog.c_str()), const_cast<char*>("/dev/stdin"),
                        const_cast<char*>("/dev/stdout"), nullptr};
        int rc = posix_spawn(pid, prog.c_str(), &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        return rc == 0;
    };
    pid_t pc = -1, ps = -1;
    bool ok = spawn(cdec, p1[0], p2[1], &pc) && spawn(sdec, p2[0], p3[1], &ps);
    close(p1[0]); close(p2[0]); close(p2[1]); close(p3[1]);
    if (devnull >= 0) close(devnull);
    if (!ok) {
        close(p1[1]); close(p3[0]);
        if (pc > 0) { kill(pc, SIGTERM); waitpid(pc, nullptr, 0); }
        return false;
    }
    fcntl(p1[1], F_SETFL, O_NONBLOCK);
    fcntl(p3[0], F_SETFL, O_NONBLOCK);
    to_cdec_ = p1[1];
    from_sdec_ = p3[0];
    pid_c_ = pc;
    pid_s_ = ps;
    return true;
}

void TetraCodec::stop() {
    if (to_cdec_ >= 0) close(to_cdec_);
    if (from_sdec_ >= 0) close(from_sdec_);
    to_cdec_ = from_sdec_ = -1;
    for (int* p : {&pid_c_, &pid_s_}) {
        if (*p > 0) {
            kill(*p, SIGTERM);
            waitpid(*p, nullptr, 0);
            *p = -1;
        }
    }
    pending_.clear();
    carry_.clear();
}

void TetraCodec::feed_slot(const uint8_t* t4, std::vector<float>& out) {
    out.clear();
    if (to_cdec_ < 0 && !start()) return;
    // the codec's test-frame format: markers 0x6B21.. every 115 words,
    // soft bits +127 = 0 / -127 = 1, 0 = erasure (t4 == nullptr: flush)
    int16_t block[690] = {0};
    for (int i = 0; i < 6; i++) block[115 * i] = static_cast<int16_t>(0x6B21 + i);
    auto sb = [t4](int i) -> int16_t { return t4 ? (t4[i] ? -127 : 127) : 0; };
    for (int i = 0; i < 114; i++) block[1 + i] = sb(i);
    for (int i = 0; i < 114; i++) block[116 + i] = sb(114 + i);
    for (int i = 0; i < 114; i++) block[231 + i] = sb(228 + i);
    for (int i = 0; i < 90; i++) block[346 + i] = sb(342 + i);
    const uint8_t* b = reinterpret_cast<const uint8_t*>(block);
    pending_.insert(pending_.end(), b, b + sizeof block);
    if (pending_.size() > 64 * sizeof block) pending_.erase(pending_.begin(), pending_.end() - 64 * sizeof block);
    while (!pending_.empty()) {
        ssize_t w = write(to_cdec_, pending_.data(), pending_.size());
        if (w > 0) { pending_.erase(pending_.begin(), pending_.begin() + w); continue; }
        if (w < 0 && errno == EAGAIN) break;
        stop();   // codec died
        return;
    }
    uint8_t buf[8192];
    std::vector<uint8_t>& carry = carry_;
    for (;;) {
        ssize_t r = read(from_sdec_, buf, sizeof buf);
        if (r <= 0) break;
        carry.insert(carry.end(), buf, buf + r);
    }
    size_t ns = carry.size() / 2;
    for (size_t i = 0; i < ns; i++) {
        int16_t v;
        std::memcpy(&v, carry.data() + 2 * i, 2);
        out.push_back(v / 32768.0f);
    }
    carry.erase(carry.begin(), carry.begin() + static_cast<long>(ns * 2));
}

}  // namespace dig
