// Audio-band signalling (sig.hpp).
//
// FFSK bursts: per audio sample two sliding one-bit correlators (complex,
// a continuous local oscillator per tone) for 1200 / 1800 Hz at 1200 bit/s
// and 1200 / 2400 Hz at 2400 bit/s, on a 300 Hz high-passed copy (CTCSS and
// the carrier offset out). The tone soft value (|a| - |b|) / (|a| + |b|) is
// correlated with each sync word at bit spacing for every sample (hard bits,
// few errors allowed); the best sample of a sync = the bit timing.
//   - MDC1200 is XOR-precoded MSK: the tone of a bit says whether it differs
//     from the one before, so tone decisions would turn one error into a
//     wrong rest of the packet. Instead each bit is read COHERENTLY: the
//     phase of the waveform at a bit's start is the previous data bit (pi
//     per 1800 Hz bit - every change), so the correlators' phase in window
//     i + 1 (the window's own tone, the 1800 Hz oscillator's pi per bit
//     taken off) against a reference learnt from the sync's known bits gives
//     data bit i on its own. The convolutional code + CRC check the block.
//   - FleetSync: tone = bit; read coherently (a Viterbi over the phase
//     parity the bits before leave, run_fs) and by tone with a balanced
//     threshold; bit timing tracked on the transitions; 64-bit blocks while
//     they check, a single error among a block's least certain bits
//     corrected.
// Tones: every 5 ms a Hann-windowed FFT of the last 20 ms (512 points): the
// strongest tone 280-3300 Hz and its share of the 250-3400 Hz power, the
// eight DTMF frequencies. Steady single tones become segments; a run of 4+
// segments without pauses is a selcall sequence (each segment the nearest
// tone of a standard within its tolerance, the tone length the standard's);
// a lone steady tone of 0.25 s+ is a tone burst. DTMF: low + high tone
// strongest in their groups, twist, the two holding most of the power, 2+
// frames; digits until 0.5 s without one.

#include "sig.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mdc.hpp"
#include "selcall.hpp"

namespace asig {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr int AMASK = 32767;
const float DTMF_LO[4] = {697, 770, 852, 941};
const float DTMF_HI[4] = {1209, 1336, 1477, 1633};
const char DTMF_KEYS[17] = "123A456B789C*0#D";

// the MDC sync's tone bits (+1 = 1200 Hz, -1 = 1800 Hz = the bit changes);
// the first depends on the preamble's last bit: not used
std::vector<int8_t> mdc_tones() {
    std::vector<int8_t> t(mdc::SYNC_BITS, 0);
    for (int k = 1; k < mdc::SYNC_BITS; k++) {
        const int b = (mdc::SYNC >> (39 - k)) & 1, p = (mdc::SYNC >> (40 - k)) & 1;
        t[k] = b != p ? -1 : 1;
    }
    return t;
}
std::vector<int8_t> fs_tones(uint32_t w) {
    std::vector<int8_t> t(fleetsync::SYNC_BITS);
    for (int k = 0; k < fleetsync::SYNC_BITS; k++) t[k] = ((w >> (31 - k)) & 1) ? 1 : -1;
    return t;
}
}  // namespace

// ---------------------------------------------------------------------------
void Ffsk::init(double fa, double fb, int spb) {
    spb_ = spb;
    pa_.assign(R, cf(0, 0));
    pb_.assign(R, cf(0, 0));
    ca_.assign(R, cf(0, 0));
    cb_.assign(R, cf(0, 0));
    wa_ = std::polar(1.0f, static_cast<float>(-2 * PI * fa / FS_AU));
    wb_ = std::polar(1.0f, static_cast<float>(-2 * PI * fb / FS_AU));
    la_ = lb_ = cf(1, 0);
    sa_ = sb_ = cf(0, 0);
    n_ = 0;
}

void Ffsk::push(float x) {
    const int m = R - 1;
    const cf a = x * la_, b = x * lb_;
    la_ *= wa_;
    lb_ *= wb_;
    if ((n_ & 1023) == 1023) {
        la_ /= std::abs(la_);
        lb_ /= std::abs(lb_);
    }
    sa_ += a - pa_[(n_ - spb_) & m];
    sb_ += b - pb_[(n_ - spb_) & m];
    pa_[n_ & m] = a;
    pb_[n_ & m] = b;
    if ((n_ & 4095) == 4095) {   // the sums' rounding drift
        sa_ = sb_ = cf(0, 0);
        for (int i = 0; i < spb_; i++) {
            sa_ += pa_[(n_ - i) & m];
            sb_ += pb_[(n_ - i) & m];
        }
    }
    ca_[n_ & m] = sa_;
    cb_[n_ & m] = sb_;
    n_++;
}

cf Ffsk::at(const std::vector<cf>& v, double n) const {
    const int64_t i = static_cast<int64_t>(std::floor(n));
    const float f = static_cast<float>(n - static_cast<double>(i));
    const cf x0 = v[i & (R - 1)];
    if (f <= 0) return x0;
    return x0 + f * (v[(i + 1) & (R - 1)] - x0);
}
cf Ffsk::a(double n) const { return at(ca_, n); }
cf Ffsk::b(double n) const { return at(cb_, n); }
float Ffsk::q(double n, float ga, float gb) const {
    const float x = std::abs(a(n)) / ga, y = std::abs(b(n)) / gb;
    return (x - y) / (x + y + 1e-9f);
}

// ---------------------------------------------------------------------------
Sig::Sig() {
    win_.resize(WIN);
    win_sum_ = 0;
    for (int i = 0; i < WIN; i++) {
        win_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * PI * (i + 0.5) / WIN));
        win_sum_ += win_[i];
    }
    fft_in_ = kfft_alloc_real(NFFT);
    fft_out_ = kfft_alloc_complex(NFFT / 2 + 1);
    plan_ = kfft_plan_dft_r2c_1d(NFFT, fft_in_, fft_out_);
    reset();
}

Sig::~Sig() {
    if (plan_) kfft_destroy_plan(plan_);
    if (fft_in_) kfft_free(fft_in_);
    if (fft_out_) kfft_free(fft_out_);
}

void Sig::reset() {
    k_ = 0;
    f12_.init(1200, 1800, 10);
    f24_.init(1200, 2400, 5);
    hp_z_[0] = hp_z_[1] = 0;
    pk_mdc_ = pk_fs12_ = pk_fs24_ = Peak{};
    jobs_.clear();
    last_mdc_k_ = last_fs_k_ = -1e9;
    au_.assign(ARING, 0.0f);
    frame_ = 0;
    seg_on_ = false;
    seq_.clear();
    seq_last_k_ = 0;
    dt_cur_ = -1;
    dt_run_ = dt_off_ = 0;
    dt_in_ = false;
    dt_acc_ = -1;
    dt_str_.clear();
    events.clear();
    // 2nd-order Butterworth high-pass, 300 Hz at 12 kHz (bilinear) for the FFSK path
    const double K = std::tan(PI * 300.0 / FS_AU), nm = 1.0 / (1.0 + std::sqrt(2.0) * K + K * K);
    hp_b_[0] = nm;
    hp_b_[1] = -2 * nm;
    hp_b_[2] = nm;
    hp_a_[1] = 2 * (K * K - 1) * nm;
    hp_a_[2] = (1 - std::sqrt(2.0) * K + K * K) * nm;
}

void Sig::process(const float* x, size_t n) {
    static const std::vector<int8_t> MDC_T = mdc_tones(), FSA = fs_tones(fleetsync::SYNC_A), FSB = fs_tones(fleetsync::SYNC_B);
    for (size_t i = 0; i < n; i++) {
        au_[k_ & AMASK] = x[i];
        const double v = hp_b_[0] * x[i] + hp_z_[0];
        hp_z_[0] = hp_b_[1] * x[i] - hp_a_[1] * v + hp_z_[1];
        hp_z_[1] = hp_b_[2] * x[i] - hp_a_[2] * v;
        const float h = static_cast<float>(v);
        if (want_mdc || want_fs) f12_.push(h);
        if (want_fs) f24_.push(h);
        if (want_mdc) search(f12_, MDC_T, nullptr, 4, pk_mdc_, 0);
        if (want_fs) {
            search(f12_, FSA, &FSB, 2, pk_fs12_, 1);
            search(f24_, FSA, &FSB, 2, pk_fs24_, 2);
        }
        k_++;
        if (k_ % HOP == 0 && k_ >= WIN) frame(k_ - 1);
    }
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        bool done;
        if (it->kind == 0) done = run_mdc(*it);
        else done = run_fs(*it) != 0;
        it = done ? jobs_.erase(it) : std::next(it);
    }
}

// sync search at the newest sample: the tone soft values at bit spacing
// against the pattern(s) (0 = don't care); at most max_err hard bits wrong.
// The best sample within one bit of the first hit is the bit timing
void Sig::search(const Ffsk& f, const std::vector<int8_t>& p1, const std::vector<int8_t>* p2, int max_err, Peak& pk,
                 int kind) {
    const int64_t n = f.end() - 1;
    const int L = f.spb(), S = static_cast<int>(p1.size());
    // pattern p ending at window end m: the hard bits first (early out),
    // then - if close - again with the decision threshold put in the middle
    // of the pattern's own bits (balance(), below)
    auto eval = [&](int64_t m, const std::vector<int8_t>& p, float* score) {
        const int64_t fst = m - static_cast<int64_t>(S - 1) * L;
        if (!f.has(static_cast<double>(fst))) return 999;
        int e = 0;
        for (int k = S - 1; k >= 0; k--) {
            if (!p[k]) continue;
            e += (f.q(static_cast<double>(fst + static_cast<int64_t>(k) * L)) > 0) != (p[k] > 0);
            if (e > max_err + 8) return e;
        }
        const float g = balance(f, static_cast<double>(fst), p);
        e = 0;
        float s = 0;
        for (int k = 0; k < S; k++) {
            if (!p[k]) continue;
            const float v = f.q(static_cast<double>(fst + static_cast<int64_t>(k) * L), 1.0f, 1.0f / g);
            e += (v > 0) != (p[k] > 0);
            s += v * p[k];
        }
        *score = s;
        return e;
    };
    auto best_of = [&](int64_t m, float* score) {
        float s1 = 0, s2 = 0;
        const int e1 = eval(m, p1, &s1), e2 = p2 ? eval(m, *p2, &s2) : 999;
        bool hit = false;
        if (e1 <= max_err) {
            hit = true;
            *score = s1;
        }
        if (e2 <= max_err && (!hit || s2 > *score)) {
            hit = true;
            *score = s2;
        }
        return hit;
    };
    float best = 0;
    const bool hit = best_of(n, &best);
    if (pk.active) {
        pk.age++;
        if (hit && best > pk.best) {
            pk.best = best;
            pk.idx = n;
        }
        if (pk.age >= L) {
            pk.active = false;
            // the window end of the sync's last bit to a fraction of a sample:
            // a parabola through the score next to the best sample
            auto score_at = [&](int64_t m) {
                float s1 = -1e9f, s2 = -1e9f;
                eval(m, p1, &s1);
                if (p2) eval(m, *p2, &s2);
                return std::max(s1, s2);
            };
            const float sm = score_at(pk.idx - 1), sp = score_at(pk.idx + 1);
            const float s0 = pk.best;
            double frac = 0;
            const float den = sm - 2 * s0 + sp;
            if (sm > -1e8f && sp > -1e8f && den < 0) frac = std::clamp(0.5 * (sm - sp) / den, -0.5, 0.5);
            Job j;
            j.kind = kind;
            j.sync_end = pk.idx;
            j.e0 = static_cast<double>(pk.idx) + frac + L;
            if (dump)
                fprintf(stderr, "SIG sync %s at k=%lld score %.1f\n", kind == 0 ? "MDC" : kind == 1 ? "FS1200" : "FS2400",
                        static_cast<long long>(pk.idx), pk.best);
            jobs_.push_back(j);
        }
    } else if (hit) {
        pk.active = true;
        pk.best = best;
        pk.idx = n;
        pk.age = 0;
    }
}

// The decision threshold between the two tones: one-bit correlators part
// them only so far (1200 / 1800 Hz: the other correlator sees 0.64 of a
// tone; 1200 / 2400 Hz at 2400 bit/s: 0.64 too), and a receiver's audio
// chain (de-emphasis, filters) leaves that crosstalk unequal - live MDC
// audio: 0.75 on 1200 Hz bits, 0.51 on 1800 Hz bits. A bit is read right
// for any weight g on |b| between r_b (|a| / |b| on b-bits) and 1 / r_a
// (|b| / |a| on a-bits); g = sqrt(r_b / r_a) is their (geometric) middle.
// (Scaling each tone by its own level - tried first - lands outside that
// range when the levels differ: 0 of the live sync's bits right)
float Sig::balance(const Ffsk& f, double first, const std::vector<int8_t>& p) const {
    float ra[64], rb[64];
    int na = 0, nb = 0;
    for (size_t k = 0; k < p.size() && k < 64; k++) {
        if (!p[k]) continue;
        const double at = first + static_cast<double>(k * static_cast<size_t>(f.spb()));
        const float a = std::abs(f.a(at)), b = std::abs(f.b(at));
        if (!(a > 0) || !(b > 0)) continue;
        if (p[k] > 0) ra[na++] = b / a;
        else rb[nb++] = a / b;
    }
    if (na < 3 || nb < 3) return 1.0f;
    std::nth_element(ra, ra + na / 2, ra + na);
    std::nth_element(rb, rb + nb / 2, rb + nb);
    return std::clamp(std::sqrt(rb[nb / 2] / ra[na / 2]), 0.25f, 4.0f);
}

// where a burst began: the sync's first bit, and before it the preamble
// (MDC: steady 1800 Hz; FleetSync: alternating tones) - at most 0.5 s back
double Sig::sync_start(const Ffsk& f, double e0, int sync_bits, int pre_tone) const {
    const int L = f.spb();
    double s = e0 - (sync_bits + 1) * L;   // the window end before the sync's first bit
    for (int i = 0; i < static_cast<int>(0.5 * FS_AU / L); i++) {
        const double at = s - L;
        if (!f.has(at)) break;
        const float v = f.q(at);
        if (pre_tone != 0 ? v * pre_tone < 0.3f : std::fabs(v) < 0.3f) break;
        s = at;
    }
    return s - L + 1;
}

float Sig::deviation(double ka, double kb) const {
    const int64_t a = std::max<int64_t>(static_cast<int64_t>(ka), k_ - ARING + 1), b = std::min<int64_t>(static_cast<int64_t>(kb), k_ - 1);
    if (b <= a) return 0;
    double m = 0, s2 = 0;
    for (int64_t k = a; k <= b; k++) m += au_[k & AMASK];
    m /= static_cast<double>(b - a + 1);
    for (int64_t k = a; k <= b; k++) {
        const double d = au_[k & AMASK] - m;
        s2 += d * d;
    }
    return static_cast<float>(std::sqrt(2.0 * s2 / static_cast<double>(b - a + 1)));
}

// MDC1200: once the samples of a whole packet are there
bool Sig::run_mdc(Job& j) {
    const Ffsk& f = f12_;
    const int L = f.spb();
    const double e0 = j.e0;
    auto e = [&](int i) { return e0 + static_cast<double>(i) * L; };   // window end of bit i (data bits i >= 0)
    if (!f.has(e(mdc::BLOCK_BITS))) {
        // not all there yet (or gone: too old)
        return !f.has(e(-mdc::SYNC_BITS));
    }
    if (std::fabs(static_cast<double>(j.sync_end) - last_mdc_k_) < 20.0 * L) return true;   // the same burst again
    // the sync's known bits: the phase reference per tone
    const uint64_t S = mdc::SYNC;
    auto sb = [&](int k) { return k < 0 ? 1 : static_cast<int>((S >> (39 - k)) & 1); };
    auto sgn18 = [](int i) { return (i & 1) ? -1.0f : 1.0f; };   // the 1800 Hz oscillator turns pi per bit
    cf A12(0, 0), A18(0, 0);
    for (int k = 1; k < mdc::SYNC_BITS; k++) {
        const int i = k - mdc::SYNC_BITS;
        const float u = sb(k - 1) ? -1.0f : 1.0f;
        if (sb(k) == sb(k - 1)) A12 += u * f.a(e(i));
        else A18 += u * sgn18(i) * f.b(e(i));
    }
    if (std::abs(A12) <= 0 || std::abs(A18) <= 0) return true;
    // data bit i from the phase at the start of bit i + 1; the decision
    // keeps the references following (decision directed)
    auto read = [&](int i0, int n, bool last_by_tone, uint8_t* bits) {
        int prev = sb(mdc::SYNC_BITS - 1);
        for (int i = i0; i < i0 + n; i++) {
            int b;
            if (i == i0 + n - 1 && last_by_tone) {
                b = prev ^ (f.q(e(i)) < 0 ? 1 : 0);
            } else {
                const cf ca = f.a(e(i + 1)), cb = f.b(e(i + 1));
                const float v = (ca * std::conj(A12 / std::abs(A12))).real() +
                                sgn18(i + 1) * (cb * std::conj(A18 / std::abs(A18))).real();
                b = v < 0 ? 1 : 0;
                const float u = b ? -1.0f : 1.0f;
                if (std::abs(ca) > std::abs(cb)) A12 = 0.92f * A12 + u * ca;
                else A18 = 0.92f * A18 + u * sgn18(i + 1) * cb;
            }
            bits[i - i0] = static_cast<uint8_t>(b);
            prev = b;
        }
    };
    uint8_t bits[mdc::BLOCK_BITS];
    // the first block's last bit: its phase window may be the second block's
    // first bit (double packet) or voice / silence - read by its tone first
    read(0, mdc::BLOCK_BITS, true, bits);
    uint8_t d[7];
    int fixed = mdc::decode_block(bits, d);
    if (fixed < 0) {
        if (dump) fprintf(stderr, "SIG MDC block CRC wrong at k=%lld\n", static_cast<long long>(j.sync_end));
        return true;
    }
    SigEvent ev;
    ev.kind = SigEvent::MDC;
    ev.op = d[0];
    ev.arg = d[1];
    ev.unit = static_cast<uint16_t>((d[2] << 8) | d[3]);
    ev.fixed = fixed;
    ev.dbl = mdc::is_double(d[0]);
    int nbits = mdc::BLOCK_BITS;
    if (ev.dbl) {
        if (!f.has(e(2 * mdc::BLOCK_BITS))) {
            if (f.has(e(-mdc::SYNC_BITS))) return false;   // wait for the second block
        } else {
            uint8_t bits2[mdc::BLOCK_BITS], d2[7];
            read(mdc::BLOCK_BITS, mdc::BLOCK_BITS, true, bits2);
            const int f2 = mdc::decode_block(bits2, d2);
            if (f2 >= 0) {
                ev.dbl_ok = true;
                ev.fixed += f2;
                for (int i = 0; i < 4; i++) ev.ex[i] = d2[i];
                nbits = 2 * mdc::BLOCK_BITS;
            }
        }
    }
    ev.ka = sync_start(f, e0, mdc::SYNC_BITS, -1);
    ev.kb = e(nbits - 1);
    ev.dev_hz = deviation(e0 - mdc::SYNC_BITS * L, ev.kb);
    last_mdc_k_ = static_cast<double>(j.sync_end);
    if (dump)
        fprintf(stderr, "SIG MDC op %02X arg %02X unit %04X%s fixed %d k %.0f-%.0f\n", ev.op, ev.arg, ev.unit,
                ev.dbl ? (ev.dbl_ok ? " (double)" : " (double, 2nd block lost)") : "", ev.fixed, ev.ka, ev.kb);
    events.push_back(ev);
    return true;
}

// FleetSync: block by block as the samples come; 1 = finished.
// Each block is read twice. By tone (|a| against |b|, balanced): the
// non-coherent reading. And COHERENTLY: both modems keep the phase
// continuous, so the phase of a bit's window follows from the bits before
// it - a tone that turns pi per bit (1800 Hz at 1200 bit/s, 1200 Hz at
// 2400 bit/s) flips the phase of everything after it, and so does the
// correlator oscillator that turns pi per bit. A 2-state Viterbi over that
// parity, each tone's expected correlator output learnt from the sync's
// known bits, is the matched filter. At 2400 bit/s (5 samples a bit) the
// tone reading took the last 2400 Hz bit before a 1200 Hz run for a 1200
// Hz one (the correlators' crosstalk): a real recording read 3-4 bits of
// its block wrong at 4 of 5 sampling phases. The coherent word goes first
int Sig::run_fs(Job& j) {
    const Ffsk& f = j.kind == 1 ? f12_ : f24_;
    const int L = f.spb();
    // each tone's phase step per bit in units of pi (mod 2): tone a = bit 1
    const int step_a = j.kind == 1 ? 0 : 1, step_b = j.kind == 1 ? 1 : 0;
    auto sgn = [](int64_t v) { return (v & 1) ? -1.0f : 1.0f; };
    if (j.block == 0 && j.msg.bytes.empty() && j.fs2_n == 0) {
        if (!f.has(j.e0 - (fleetsync::SYNC_BITS + 1) * L)) return 1;
        if (std::fabs(static_cast<double>(j.sync_end) - last_fs_k_) < 10.0 * L) return 1;   // the same burst again
        if (j.nref == 0) {
            // the tones' decision threshold from the sync's preamble half
            // (0xAAAA, the same in both sync words)
            static const std::vector<int8_t> pre = [] {
                std::vector<int8_t> p(fleetsync::SYNC_BITS, 0);
                for (int k = 0; k < 16; k++) p[k] = (k & 1) ? -1 : 1;
                return p;
            }();
            const double s0 = j.e0 - static_cast<double>(fleetsync::SYNC_BITS) * L;   // the sync's first window end
            j.ga = 1.0f;
            j.gb = 1.0f / balance(f, s0, pre);
            j.ka = sync_start(f, j.e0, fleetsync::SYNC_BITS, 0);
            j.msg.baud = j.kind == 1 ? 1200 : 2400;
            // which sync word, then each tone's reference from its bits
            int ea = 0, eb = 0;
            for (int k = 0; k < fleetsync::SYNC_BITS; k++) {
                const bool one = f.q(s0 + static_cast<double>(k * L), j.ga, j.gb) > 0;
                ea += one != (((fleetsync::SYNC_A >> (31 - k)) & 1) != 0);
                eb += one != (((fleetsync::SYNC_B >> (31 - k)) & 1) != 0);
            }
            const uint32_t sw = ea <= eb ? fleetsync::SYNC_A : fleetsync::SYNC_B;
            cf ra(0, 0), rb(0, 0);
            int na = 0, nb = 0, par = 0;
            for (int k = 0; k < fleetsync::SYNC_BITS; k++) {
                const bool one = ((sw >> (31 - k)) & 1) != 0;
                const double at = s0 + static_cast<double>(k * L);
                const int st = one ? step_a : step_b;
                const float s = sgn(par + st * k);
                if (one) {
                    ra += s * f.a(at);
                    na++;
                } else {
                    rb += s * f.b(at);
                    nb++;
                }
                par ^= st;
            }
            if (na > 0) j.ra = ra / static_cast<float>(na);
            if (nb > 0) j.rb = rb / static_cast<float>(nb);
            j.par = par;
            j.bit = fleetsync::SYNC_BITS;
            j.nref = 1;
        }
    }
    auto finish = [&]() {
        if (j.msg.bytes.empty()) return 1;
        // the sender is a radio: fleet 100-349, unit 1000-4999 (a byte of
        // 0xFF / 12 bits of 0xFFF = 354 / 5094 is a steady tone read as a
        // block - voice did that once in 50 min)
        if (j.msg.from_fleet() > 349 || j.msg.from_unit() > 4999) {
            if (dump)
                fprintf(stderr, "SIG FleetSync dropped: no radio %d-%d\n", j.msg.from_fleet(), j.msg.from_unit());
            last_fs_k_ = static_cast<double>(j.sync_end);
            return 1;
        }
        SigEvent ev;
        ev.kind = SigEvent::FLEETSYNC;
        ev.fs = j.msg;
        ev.ka = j.ka;
        ev.kb = j.e0 + static_cast<double>(j.block * 64 - 1) * L;
        ev.dev_hz = deviation(j.ka, ev.kb);
        last_fs_k_ = static_cast<double>(j.sync_end);
        if (dump)
            fprintf(stderr, "SIG FleetSync %d bit/s%s %d blocks from %d-%d to %d-%d type %02X flags %02X\n", j.msg.baud,
                    j.msg.fs2 ? " (II)" : "", j.msg.blocks(), j.msg.from_fleet(), j.msg.from_unit(), j.msg.to_fleet(),
                    j.msg.to_unit(), j.msg.type(), j.msg.flags());
        events.push_back(ev);
        return 1;
    };
    while (j.block < 64) {
        const double first = j.e0 + static_cast<double>(j.block * 64) * L;
        if (!f.has(first + 64.0 * L)) return f.has(first) ? 0 : finish();
        uint64_t w = 0;
        float rel[64];
        cf ca[64], cb[64];
        double tau = 0;   // the bit timing, tracked on the tone transitions
        float prev = 0;
        for (int k = 0; k < 64; k++) {
            const double at = first + static_cast<double>(k) * L + tau;
            const float v = f.q(at, j.ga, j.gb);
            ca[k] = f.a(at);
            cb[k] = f.b(at);
            if (k > 0 && (v > 0) != (prev > 0)) {
                // the crossing should be half a bit before this window end
                const double a0 = at - L;
                double zc = -1;
                float q0 = prev;
                for (int s = 1; s <= L; s++) {
                    const float q1 = f.q(a0 + s, j.ga, j.gb);
                    if ((q1 > 0) != (q0 > 0)) {
                        zc = a0 + s - 1 + q0 / (q0 - q1);
                        break;
                    }
                    q0 = q1;
                }
                if (zc >= 0) {
                    const double err = zc - (a0 + 0.5 * L);
                    if (std::fabs(err) < 0.45 * L) tau += 0.1 * err;
                }
            }
            prev = v;
            w = (w << 1) | (v > 0 ? 1u : 0u);
            rel[k] = std::fabs(v);
        }
        j.e0 += tau;   // the drift so far, for the next block
        // coherent: Viterbi over the phase parity (branch metric = the
        // matched filter's 2 Re(c r*) - |r|^2 of the tone and phase)
        uint64_t wc = 0;
        {
            const float Ea = std::norm(j.ra), Eb = std::norm(j.rb);
            float pm[2] = {-1e30f, -1e30f};
            pm[j.par] = 0;
            pm[j.par ^ 1] = -(Ea + Eb);   // the parity carried over may be wrong: possible, not free
            uint8_t from[64][2] = {}, tone[64][2] = {};
            for (int k = 0; k < 64; k++) {
                const int64_t i = j.bit + k;
                float nm[2] = {-1e30f, -1e30f};
                for (int p = 0; p < 2; p++) {
                    if (pm[p] < -1e29f) continue;
                    for (int t = 0; t < 2; t++) {   // t 1 = tone a = bit 1
                        const int st = t ? step_a : step_b;
                        const cf c = t ? ca[k] : cb[k], r = t ? j.ra : j.rb;
                        const float m = pm[p] + 2 * sgn(p + st * i) * (c * std::conj(r)).real() - (t ? Ea : Eb);
                        const int q = p ^ st;
                        if (m > nm[q]) {
                            nm[q] = m;
                            from[k][q] = static_cast<uint8_t>(p);
                            tone[k][q] = static_cast<uint8_t>(t);
                        }
                    }
                }
                pm[0] = nm[0];
                pm[1] = nm[1];
            }
            int st = pm[1] > pm[0] ? 1 : 0;
            for (int k = 63; k >= 0; k--) {
                wc |= static_cast<uint64_t>(tone[k][st]) << (63 - k);
                st = from[k][st];
            }
        }
        // one error among the 4 least certain bits (by tone) - not in a
        // nearly constant word: a steady tone after a chance sync reads as
        // all 1s, and one of the 2016 words with two 0s checks
        int idx[64];
        for (int k = 0; k < 64; k++) idx[k] = k;
        std::partial_sort(idx, idx + 4, idx + 64, [&](int a, int b) { return rel[a] < rel[b]; });
        auto check1 = [&](uint64_t& x) {
            if (fleetsync::block_ok(x)) return true;
            const int ones = std::popcount(x);
            if (ones < 6 || ones > 58) return false;
            for (int t = 0; t < 4; t++) {
                const uint64_t xx = x ^ (1ULL << (63 - idx[t]));
                if (fleetsync::block_ok(xx)) {
                    x = xx;
                    return true;
                }
            }
            return false;
        };
        bool ok = false;
        uint64_t used = wc;
        if (j.fs2_n == 0) {
            uint64_t x = wc;
            if (fleetsync::block_ok(x)) ok = true;
            else if ((x = w, fleetsync::block_ok(x))) ok = true;
            else if ((x = wc, check1(x))) ok = true;
            else if ((x = w, check1(x))) ok = true;
            if (ok) used = x;
        }
        // the phase parity and the references follow the bits taken
        auto follow = [&](uint64_t x, bool learn) {
            int p = j.par;
            for (int k = 0; k < 64; k++) {
                const bool one = ((x >> (63 - k)) & 1) != 0;
                const int st = one ? step_a : step_b;
                if (learn) {
                    const float s = sgn(p + st * (j.bit + k));
                    if (one) j.ra = 0.95f * j.ra + 0.05f * s * ca[k];
                    else j.rb = 0.95f * j.rb + 0.05f * s * cb[k];
                }
                p ^= st;
            }
            j.par = p;
            j.bit += 64;
        };
        if (dump)
            fprintf(stderr, "SIG FleetSync block %d coherent %016llx tone %016llx%s\n", j.block,
                    static_cast<unsigned long long>(wc), static_cast<unsigned long long>(w), ok ? " ok" : "");
        if (ok) {
            follow(used, true);
            fleetsync::append_data(used, j.msg.bytes);
            j.block++;
            continue;
        }
        // FleetSync II: every 64 bits on air are four 16-bit fields, each a
        // BCH(15,7) codeword carrying one nibble (bits 11..8); the fields go
        // on air in pairs, the second of each pair first (air order: nibble
        // 1, 0, 3, 2). The 4 nibbles make one 16-bit quarter of a FleetSync
        // block, 4 such words the whole block (first quarter = bits 63..48).
        // The coherent word, else the tone word
        auto quarter_of = [](uint64_t x, uint16_t* q) {
            uint32_t v = 0;
            for (int nibble = 0; nibble < 4; nibble++) {
                const int field = nibble ^ 1;   // its place in air order
                const int cw = fleetsync::bch15_repair(static_cast<uint32_t>(x >> (48 - 16 * field)) & 0xFFFF);
                if (cw < 0) return false;
                v = (v << 4) | ((static_cast<uint32_t>(cw) >> 8) & 0xF);
            }
            *q = static_cast<uint16_t>(v);
            return true;
        };
        uint16_t q = 0;
        if (quarter_of(wc, &q)) used = wc;
        else if (quarter_of(w, &q)) used = w;
        else return finish();
        follow(used, false);
        j.fs2_q[j.fs2_n++] = q;
        if (j.fs2_n == 4) {
            uint64_t blk = 0;
            for (const uint16_t part : j.fs2_q) blk = (blk << 16) | part;
            j.fs2_n = 0;
            if (!fleetsync::block_ok(blk)) return finish();
            fleetsync::append_data(blk, j.msg.bytes);
            j.msg.fs2 = true;
        }
        j.block++;
    }
    return finish();
}

// ---------------------------------------------------------------------------
// tones
double Sig::Seg::dur_s() const {
    // a frame passes as a tone while the tone fills most of its Hann window:
    // the frames span the tone minus about half a window
    return (static_cast<double>(k1 - k0) + 0.5 * WIN) / FS_AU;
}

// DTMF "talk-off": voice at a pitch f0 can hold two harmonics on a DTMF
// pair (1209 / 697 ~ 7 / 4: f0 = 174 Hz puts its 4th and 7th harmonics on
// "1"). Both tones multiples of one f0 of 70-350 Hz and the harmonics next
// to them there too = voice; a DTMF pair has nothing at those
bool Sig::voice_family(const float* P, float flo, float fhi, double pair) const {
    constexpr double BIN = FS_AU / NFFT;
    auto lobe = [&](double hz) {
        // (frame() fills P over 250-3400 Hz +- 4 bins)
        const int c = static_cast<int>(std::lround(hz / BIN));
        if (c - 2 < static_cast<int>(std::ceil(250 / BIN)) - 4 || c + 2 > static_cast<int>(3400 / BIN) + 4) return 0.0;
        double s = 0;
        for (int b = c - 2; b <= c + 2; b++) s += P[b];
        return s;
    };
    for (int k = 2; k <= 10; k++) {
        const double f0 = flo / k;
        if (f0 < 70 || f0 > 350) continue;
        const double m = fhi / f0;
        const int mi = static_cast<int>(std::lround(m));
        if (std::fabs(m - mi) > 0.012 * mi) continue;
        // the neighbours of both tones in that harmonic series - those outside
        // the tones' own main lobes (+-100 Hz with the 20 ms Hann window)
        double nb = 0;
        int n = 0;
        for (int h : {k - 1, k + 1, mi - 1, mi + 1}) {
            const double fh = h * f0;
            if (h < 2 || h == k || h == mi || std::fabs(fh - flo) < 130 || std::fabs(fh - fhi) < 130) continue;
            nb += lobe(fh);
            n++;
        }
        if (n && nb >= 0.03 * pair) return true;
    }
    // (the DTMF receivers' second-harmonic check doesn't work here: twice a
    // low tone lands inside a high tone's lobe - 2 x 697 = 1394 next to 1336
    // / 1477 Hz - and real "2" / "3" / "6" / "B" / "C" were refused)
    return false;
}

void Sig::frame(int64_t k_end) {
    frame_++;
    // the window, its mean (the carrier offset) out
    double mean = 0;
    for (int i = 0; i < WIN; i++) mean += au_[(k_end - WIN + 1 + i) & AMASK];
    mean /= WIN;
    for (int i = 0; i < WIN; i++) fft_in_[i] = static_cast<float>((au_[(k_end - WIN + 1 + i) & AMASK] - mean) * win_[i]);
    // DTMF: the eight frequencies exactly (before the FFT: the input stays)
    float g[8];
    for (int t = 0; t < 8; t++) {
        const double w = 2 * PI * (t < 4 ? DTMF_LO[t] : DTMF_HI[t - 4]) / FS_AU;
        const double c = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < WIN; i++) {
            const double s = fft_in_[i] + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        g[t] = static_cast<float>(s1 * s1 + s2 * s2 - c * s1 * s2);
    }
    std::fill(fft_in_ + WIN, fft_in_ + NFFT, 0.0f);
    kfft_execute(plan_);
    constexpr double BIN = FS_AU / NFFT;
    const int b250 = static_cast<int>(std::ceil(250 / BIN)), b3400 = static_cast<int>(3400 / BIN);
    const int b280 = static_cast<int>(std::ceil(280 / BIN)), b3300 = static_cast<int>(3300 / BIN);
    float P[NFFT / 2 + 1];
    double band = 0;
    int pk = b280;
    for (int b = b250 - 4; b <= b3400 + 4; b++) {
        P[b] = fft_out_[b][0] * fft_out_[b][0] + fft_out_[b][1] * fft_out_[b][1];
        if (b >= b250 && b <= b3400) band += P[b];
        if (b >= b280 && b <= b3300 && P[b] > P[pk]) pk = b;
    }
    Frame fr;
    fr.k_end = k_end;
    if (band > 0) {
        const float l0 = std::log(std::max(P[pk - 1], 1e-20f)), l1 = std::log(std::max(P[pk], 1e-20f)),
                    l2 = std::log(std::max(P[pk + 1], 1e-20f));
        const float den = l0 - 2 * l1 + l2;
        const float dl = std::clamp(den < 0 ? 0.5f * (l0 - l2) / den : 0.0f, -0.5f, 0.5f);
        fr.hz = static_cast<float>((pk + dl) * BIN);
        fr.amp = 2.0f * std::sqrt(std::exp(l1 - 0.25f * (l0 - l2) * dl)) / win_sum_;
        double lobe = 0;
        for (int b = pk - 4; b <= pk + 4; b++) lobe += P[b];
        fr.purity = static_cast<float>(lobe / band);
        // DTMF
        auto best2 = [&](int o, int* i1, float* p1, float* p2) {
            *i1 = 0;
            for (int t = 1; t < 4; t++)
                if (g[o + t] > g[o + *i1]) *i1 = t;
            *p1 = g[o + *i1];
            *p2 = 0;
            for (int t = 0; t < 4; t++)
                if (t != *i1) *p2 = std::max(*p2, g[o + t]);
        };
        int il, ih;
        float l1p, l2p, h1p, h2p;
        best2(0, &il, &l1p, &l2p);
        best2(4, &ih, &h1p, &h2p);
        const float al = 2.0f * std::sqrt(l1p) / win_sum_, ah = 2.0f * std::sqrt(h1p) / win_sum_;
        auto lobe_at = [&](float hz) {
            const int c = static_cast<int>(std::lround(hz / BIN));
            double s = 0;
            for (int b = c - 4; b <= c + 4; b++) s += P[b];
            return s;
        };
        // each tone's own frequency (the FFT's peak near it): DTMF decoders
        // must take +-1.5 % and reject +-3.5 % - a voice harmonic next to a
        // DTMF frequency lands in its Goertzel too
        auto peak_hz = [&](float hz) {
            const int c = static_cast<int>(std::lround(hz / BIN));
            int b = c;
            for (int k = c - 2; k <= c + 2; k++)
                if (P[k] > P[b]) b = k;
            const float m0 = std::log(std::max(P[b - 1], 1e-20f)), m1 = std::log(std::max(P[b], 1e-20f)),
                        m2 = std::log(std::max(P[b + 1], 1e-20f));
            const float dn = m0 - 2 * m1 + m2;
            return static_cast<float>((b + std::clamp(dn < 0 ? 0.5f * (m0 - m2) / dn : 0.0f, -0.5f, 0.5f)) * BIN);
        };
        const double ll = lobe_at(DTMF_LO[il]), lh = lobe_at(DTMF_HI[ih]);
        const float twist = h1p / std::max(l1p, 1e-20f);
        auto near = [](float f, float nom) { return std::fabs(f - nom) <= std::max(0.018f * nom, 12.0f); };
        const float flo = peak_hz(DTMF_LO[il]), fhi = peak_hz(DTMF_HI[ih]);
        // twist: receiver audio (de-emphasised) has the high group 6-10 dB
        // down (sigidwiki's samples), pre-emphasised transmitters the other way
        if (al >= 60 && ah >= 60 && l1p >= 4 * l2p && h1p >= 4 * h2p && twist > 0.05f && twist < 20.0f &&
            ll + lh >= 0.75 * band && ll >= 0.05 * band && lh >= 0.05 * band && near(flo, DTMF_LO[il]) &&
            near(fhi, DTMF_HI[ih]) && !voice_family(P, flo, fhi, ll + lh)) {
            fr.dtmf = il * 4 + ih;
            fr.dtmf_amp = std::max(al, ah);
            fr.dtmf_lo = flo;
            fr.dtmf_hi = fhi;
        }
    }
    if (want_dtmf) dtmf_frame(fr);
    if (want_selcall || want_burst) {
        const bool tone = fr.dtmf < 0 && fr.purity >= 0.6f && fr.amp >= 150;
        if (tone && seg_on_ && std::fabs(fr.hz - seg_.f) <= std::max(0.012 * seg_.f, 10.0)) {
            const double w = static_cast<double>(fr.amp) * fr.amp;
            seg_.f = (seg_.f * seg_.w + fr.hz * w) / (seg_.w + w);
            seg_.w += w;
            seg_.k1 = k_end;
            seg_.frames++;
            seg_.amp = std::max(seg_.amp, fr.amp);
            seg_.fmin = std::min(seg_.fmin, fr.hz);
            seg_.fmax = std::max(seg_.fmax, fr.hz);
        } else {
            seg_close();
            if (tone) {
                seg_on_ = true;
                seg_ = Seg{};
                seg_.f = fr.hz;
                seg_.w = static_cast<double>(fr.amp) * fr.amp;
                seg_.k0 = seg_.k1 = k_end;
                seg_.frames = 1;
                seg_.amp = fr.amp;
                seg_.fmin = seg_.fmax = fr.hz;
            }
        }
        if (!seg_on_ && !seq_.empty() && k_end - seq_last_k_ > static_cast<int64_t>(0.08 * FS_AU)) seq_eval(true);
    }
    if (dump && (fr.purity >= 0.5f || fr.dtmf >= 0))
        fprintf(stderr, "TONE k=%lld %.1f Hz amp %.0f purity %.2f%s\n", static_cast<long long>(k_end), fr.hz, fr.amp,
                fr.purity, fr.dtmf >= 0 ? (std::string(" DTMF ") + DTMF_KEYS[fr.dtmf]).c_str() : "");
}

void Sig::seg_close() {
    if (!seg_on_) return;
    seg_on_ = false;
    if (seg_.frames < 2) return;
    if (!seq_.empty() && seg_.k0 - seq_.back().k1 > static_cast<int64_t>(0.06 * FS_AU)) seq_eval(true);
    // a steady tone of 0.25 s+ on the burst list: a tone burst (1750 Hz),
    // reported as it ends - not part of a selcall run
    const bool steady = seg_.fmax - seg_.fmin <= std::max(0.008 * seg_.f, 8.0);
    if (steady && seg_.dur_s() >= 0.25) {
        const float b = selcall::burst_tone(static_cast<float>(seg_.f));
        if (want_burst && (b > 0 || seg_.dur_s() >= 0.5)) {   // other steady tones only when long
            SigEvent ev;
            ev.kind = SigEvent::TONE;
            ev.hz = b > 0 ? b : static_cast<float>(seg_.f);
            ev.std_name = b > 0 ? "burst" : "tone";
            ev.tone_ms = static_cast<float>(seg_.dur_s() * 1000);
            ev.ka = static_cast<double>(seg_.k0) - 0.75 * WIN;
            ev.kb = static_cast<double>(seg_.k1) - 0.25 * WIN;
            ev.dev_hz = seg_.amp;
            if (dump) fprintf(stderr, "SIG tone %.1f Hz %.0f ms\n", seg_.f, ev.tone_ms);
            events.push_back(ev);
        }
        if (b > 0) {
            seq_eval(true);   // what came before it is complete
            seq_last_k_ = seg_.k1;
            return;
        }
    }
    seq_.push_back(seg_);
    seq_last_k_ = seg_.k1;
    seq_eval(false);
    if (seq_.size() > 48) seq_.erase(seq_.begin(), seq_.begin() + 16);   // voice going on: the oldest go
}

// selcall: the best run of 4+ segments one standard reads - every tone of it
// within the standard's tolerance, every length near its tone length (the
// first may be a longer lead-in). Voice right before or after the sequence
// adds segments of its own, which end the run. A run is reported once a
// segment after it broke it, or (all) once the tones have stopped
void Sig::seq_eval(bool all) {
    std::vector<Seg>& s = seq_;
    struct Cand {
        const selcall::Standard* st;
        size_t i0, n;
        std::string sym;
        double ms, score;
    };
    std::vector<Cand> cands;
    if (want_selcall && s.size() >= 4) {
        for (const auto& st : selcall::standards()) {
            if (!sel_family.empty() && sel_family != st.family) continue;
            size_t i = 0;
            while (i < s.size()) {
                std::string sym;
                double ferr = 0;
                std::vector<double> d;
                size_t j = i;
                for (; j < s.size() && j - i < 16; j++) {
                    float e = 0;
                    const char c = selcall::symbol_of(st, static_cast<float>(s[j].f), &e);
                    const double r = s[j].dur_s() * 1000 / st.width_ms;
                    // the same symbol twice in a row is no sequence: every standard
                    // sends a repeated digit as the repeat tone
                    if (!c || r < 0.45 || r > (j == i ? 12.0 : 1.8) || (j > i && c == sym.back())) break;
                    sym += c;
                    ferr += e;
                    if (j > i) d.push_back(r);
                }
                if (j - i >= 4) {
                    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
                    const double r = d[d.size() / 2];
                    if (r >= 0.55 && r <= 1.6) {
                        // letters other than a repeat after a digit: less likely; a
                        // repeat a little less likely than digits only (DZVEI 90210
                        // = ZVEI2 891E9: 970 Hz is ZVEI2's repeat tone)
                        int odd = 0, rep = 0;
                        for (size_t k = 0; k < sym.size(); k++) {
                            if (sym[k] == 'E' && k > 0 && sym[k - 1] <= '9') rep++;
                            else if (sym[k] >= 'A') odd++;
                        }
                        const double sc = std::fabs(std::log(r)) + 50 * ferr / sym.size() + 0.3 * odd + 0.15 * rep -
                                          0.2 * sym.size() + st.prior;
                        cands.push_back({&st, i, j - i, sym, r * st.width_ms, sc});
                    }
                }
                i = std::max(j, i + 1);
            }
        }
    }
    if (!cands.empty()) {
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
        const Cand& c = cands[0];
        if (!all && c.i0 + c.n >= s.size()) return;   // the run may go on
        SigEvent ev;
        ev.kind = SigEvent::SELCALL;
        ev.family = c.st->family;
        // the others reading the same run the same way (same family and
        // symbols, as close a tone length): the name lists them
        ev.std_name = c.st->name;
        for (size_t i = 1; i < cands.size(); i++)
            if (cands[i].i0 == c.i0 && cands[i].n == c.n && cands[i].sym == c.sym &&
                std::string(cands[i].st->family) == ev.family && cands[i].score - c.score < 0.15)
                ev.std_name += std::string("/") + cands[i].st->name;
        auto expand = [](const Cand& x) {
            std::string dig;
            for (size_t i = 0; i < x.sym.size(); i++) {
                // E = repeat the digit before
                if (x.sym[i] == 'E' && i > 0 && x.st->letter[4] > 0 && !dig.empty()) dig += dig.back();
                else dig += x.sym[i];
            }
            return dig;
        };
        const std::string dig = expand(c);
        ev.digits = dig;
        // another family reading the same tones about as well (DZVEI / ZVEI)
        for (size_t i = 1; i < cands.size(); i++)
            if (cands[i].i0 == c.i0 && cands[i].n == c.n && std::string(cands[i].st->family) != ev.family &&
                cands[i].score - c.score < 0.6) {
                ev.alt = std::string(cands[i].st->name) + " " + expand(cands[i]);
                break;
            }
        ev.tone_ms = static_cast<float>(c.ms);
        ev.ka = static_cast<double>(s[c.i0].k0) - 0.75 * WIN;
        ev.kb = static_cast<double>(s[c.i0 + c.n - 1].k1) - 0.25 * WIN;
        float amp = 0;
        for (size_t i = c.i0; i < c.i0 + c.n; i++) amp = std::max(amp, s[i].amp);
        ev.dev_hz = amp;
        if (dump) fprintf(stderr, "SIG selcall %s %s (%.0f ms)\n", ev.std_name.c_str(), dig.c_str(), c.ms);
        events.push_back(ev);
        s.erase(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(c.i0 + c.n));
    }
    if (all) s.clear();
}

void Sig::dtmf_frame(const Frame& f) {
    if (f.dtmf >= 0) {
        // the same digit, its tones where they were (a voice harmonic sliding
        // past a DTMF tone passes for one for a frame or two, but moves)
        auto still = [](float a, float b) { return std::fabs(a - b) <= std::max(0.006f * b, 6.0f); };
        if (f.dtmf == dt_cur_ && still(f.dtmf_lo, dt_lo0_) && still(f.dtmf_hi, dt_hi0_)) {
            dt_run_++;
        } else {
            dt_cur_ = f.dtmf;
            dt_run_ = 1;
            dt_lo0_ = f.dtmf_lo;
            dt_hi0_ = f.dtmf_hi;
        }
        dt_off_ = 0;
        // a digit: 4 frames (~35 ms of tone; the standard's minimum is 40 ms)
        if (dt_run_ >= 4 && (!dt_in_ || dt_acc_ != dt_cur_)) {
            if (dt_str_.empty()) dt_ka_ = static_cast<double>(f.k_end) - WIN - 4 * HOP;
            if (dt_str_.size() < 32) dt_str_ += DTMF_KEYS[dt_cur_];
            dt_in_ = true;
            dt_acc_ = dt_cur_;
            dt_amp_ = std::max(dt_amp_, f.dtmf_amp);
            dt_longest_ = 0;
        }
        if (dt_in_) dt_longest_ = std::max(dt_longest_, dt_run_);
        if (dt_in_) {
            dt_kb_ = static_cast<double>(f.k_end) - 0.25 * WIN;
            dt_last_k_ = f.k_end;
        }
        return;
    }
    if (++dt_off_ >= 2) {
        dt_in_ = false;
        dt_cur_ = -1;
        dt_run_ = 0;
    }
    if (!dt_str_.empty() && f.k_end - dt_last_k_ > static_cast<int64_t>(0.5 * FS_AU)) dtmf_flush();
}

void Sig::dtmf_flush() {
    if (dt_str_.empty()) return;
    // a lone digit only when long (~60 ms of tone): voice holding two
    // harmonics on DTMF tones for a moment makes single digits
    if (dt_str_.size() == 1 && dt_longest_ < 8) {
        if (dump) fprintf(stderr, "SIG DTMF %s alone and short: dropped\n", dt_str_.c_str());
        dt_str_.clear();
        dt_amp_ = 0;
        return;
    }
    SigEvent ev;
    ev.kind = SigEvent::DTMF;
    ev.digits = dt_str_;
    ev.ka = dt_ka_;
    ev.kb = dt_kb_;
    ev.dev_hz = dt_amp_;
    if (dump) fprintf(stderr, "SIG DTMF %s\n", dt_str_.c_str());
    events.push_back(ev);
    dt_str_.clear();
    dt_amp_ = 0;
}

}  // namespace asig
