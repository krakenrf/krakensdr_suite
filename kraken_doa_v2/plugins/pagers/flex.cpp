// FLEX pager receiver - see flex.hpp for the format and its sources.
#include "flex.hpp"

#include "bch.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace pagers {

// sync 1 "A" words (FLEX patent drawing; first sent = MSB here, LSB-first
// there): which speed / modulation the frame's data portion uses
const FlexRx::Mode FlexRx::MODES[4] = {{"1600/2", 0x78F35939u, 1600, 2, 1},
                                       {"3200/2", 0x84E75939u, 3200, 2, 2},
                                       {"3200/4", 0x4F975939u, 1600, 4, 2},
                                       {"6400/4", 0x215F5939u, 3200, 4, 4}};

namespace {

constexpr uint64_t SYNC1_B = 0x5555;     // 16 bits between A and A inverted
constexpr uint32_t SYNC2_C = 0xED84;     // sync 2's "C", first sent = MSB
constexpr int SYNC1_MAX_ERRORS = 5;      // of its first 64 bits
constexpr double FRAG_KEEP_S = 270;      // a fragment comes at least every 4 minutes
constexpr double GROUP_KEEP_S = 240;

// address word ranges (MC68175 data sheet, table A-20)
bool long1(uint32_t w) { return w >= 0x000001 && w <= 0x008000; }
bool long2(uint32_t w) { return w >= 0x1F7FFF && w <= 0x1FFFFE; }
bool long3(uint32_t w) { return w >= 0x1E0001 && w <= 0x1E8000; }
bool long4(uint32_t w) { return w >= 0x1E8001 && w <= 0x1F0000; }
constexpr uint32_t TEMP0 = 0x1F7800;     // the 16 temporary (group) addresses

// the 4-bit check of the frame information, block information and vector
// words: the information bits' nibbles (+ the 21st bit) add up to 0xF
bool sum_ok(uint32_t w) {
    unsigned s = (w & 0xF) + ((w >> 4) & 0xF) + ((w >> 8) & 0xF) + ((w >> 12) & 0xF) + ((w >> 16) & 0xF) +
                 ((w >> 20) & 1);
    return (s & 0xF) == 0xF;
}
// the message checks add a word's information bits in groups of 8, 8 and 5
unsigned group_sum(uint32_t w) { return (w & 0xFF) + ((w >> 8) & 0xFF) + ((w >> 16) & 0x1F); }

// numeric characters (MC68175 table A-14; 0xA = spare, 0xC = space / fill)
char numeric_char(unsigned d) { return "0123456789*U -]["[d & 0xF]; }

std::string capcode_text(uint64_t c, bool is_long) {
    char b[24];
    if (!is_long) snprintf(b, sizeof b, "%07llu", static_cast<unsigned long long>(c));
    else if (c < 1000000000ull) snprintf(b, sizeof b, "%09llu", static_cast<unsigned long long>(c));
    else snprintf(b, sizeof b, "%010llu", static_cast<unsigned long long>(c));
    return b;
}

// a long address: its CAPCODE (data sheet "CAPCODE to binary conversion",
// inverted); 0 = a set without a published conversion (2-4)
uint64_t long_capcode(uint32_t w1, uint32_t w2) {
    if (long1(w1) && long2(w2)) return 2068480ull + w1 + 32768ull * (0x1FFFFFu - w2);
    if (long1(w1) && (long3(w2) || long4(w2))) return 2068480ull + w1 + 32768ull * (w2 - 1933312u);
    if (long2(w1) && long3(w2)) return 2068479ull + (w1 - 2064383u) + 32768ull * (w2 - 1867776u);
    return 0;
}

const char* short_kind(uint32_t w) {
    if (w >= 0x1F2800 && w <= 0x1F67FF) return " (info service)";
    if (w >= 0x1F6800 && w <= 0x1F77FF) return " (network)";
    if (w >= TEMP0 && w <= TEMP0 + 15) return " (group)";
    if (w >= 0x1F7810 && w <= 0x1F781F) return " (operator)";
    if ((w >= 0x1F0001 && w <= 0x1F27FF) || (w >= 0x1F7820 && w <= 0x1F7FFE)) return " (reserved)";
    return "";
}

int next_fragment(int f) { return f == 2 ? 0 : f + 1; }   // 3 (the first), 0, 1, 2, 0, 1, 2 ...

// time zones of the system message block information word (table A-19), minutes
constexpr int ZONE_RESERVED = INT_MIN;
const int ZONE_MIN[32] = {0,    60,   120,  180,  240,  300,  360,  420,  480,  540,  600,
                          660,  720,  210,  270,  330,  ZONE_RESERVED, 345, 390, 570, -210, -660,
                          -600, -540, -480, -420, -360, -300, -240, -180, -120, -60};

const char* phase_letter(int mode, int p) {
    static const char* L[4][4] = {{"A"}, {"A", "C"}, {"A", "C"}, {"A", "B", "C", "D"}};
    return L[mode][p];
}

}  // namespace

// --- the discriminator, smoothed, by sample number --------------------------------

FlexRx::Smoothed::Smoothed(int len) : delay_((len - 1) / 2.0), y_(RING, 0.0f) { box_.set_taps(kp::fir_boxcar(len)); }

float FlexRx::Smoothed::push(float x) {
    const float y = box_.push(x);
    y_[n_ % RING] = y;
    n_++;
    return y;
}

float FlexRx::Smoothed::at(double pos) const {
    const double idx = pos + delay_;   // the output centred on input sample pos
    if (idx < 0) return 0;
    const uint64_t i0 = static_cast<uint64_t>(idx);
    if (i0 + 1 >= n_ || i0 + RING <= n_) return i0 < n_ ? y_[i0 % RING] : 0.0f;
    const float f = static_cast<float>(idx - static_cast<double>(i0));
    return y_[i0 % RING] * (1 - f) + y_[(i0 + 1) % RING] * f;
}

void FlexRx::Smoothed::reset() {
    box_.reset();
    n_ = 0;
}

FlexRx::FlexRx(kp::Host& host, Pages& pages, double fs)
    : host_(host), pages_(pages), fs_(fs), y16_(std::max(1, static_cast<int>(fs / 1600 / 2))),
      y32_(std::max(1, static_cast<int>(fs / 3200 / 2))) {}

void FlexRx::reset() {
    y16_.reset();
    y32_.reset();
    ns_ = 0;
    st_ = St::HUNT;
    need_ = 0;
    frags_.clear();
    groups_ = {};
}

void FlexRx::push(float disc_hz, double t) {
    t_now_ = t;
    ns_++;
    y16_.push(disc_hz);
    y32_.push(disc_hz);
    // the hunt: every quarter of a 1600 bit/s bit, the 80 bits ending at the
    // newest smoothed sample against sync 1 - no clock to lock first (one
    // would come out of the noise before a transmission, or out of 3200
    // symbols a second, too late for the sync)
    const double bit = fs_ / 1600;
    if (st_ == St::HUNT && ns_ % static_cast<uint64_t>(std::max(1.0, bit / 4)) == 0 && ns_ > 82 * bit) {
        const double p = static_cast<double>(ns_ - 2) - y16_.delay();
        if (fit_sync(p).dist <= SYNC1_MAX_ERRORS) {
            p_next_ = p;   // fitted once its half bit beyond is in
            need_ = p + 0.6 * bit + y16_.delay() + 2;
            st_ = St::NEXT;
        }
    }
    if (static_cast<double>(ns_) < need_) return;
    switch (st_) {
    case St::NEXT:   // a sync 1 where the hunt or the last frame's grid put it
        if (!sync_at(p_next_, 0.5)) st_ = St::HUNT;
        break;
    case St::FIW: read_fiw(); break;
    case St::SYNC2: do_sync2(); break;
    case St::DATA: do_block(); break;
    default: break;
    }
}

// --- sync 1 + the frame information word (always 1600 bit/s, 2-level) ---------


// The 80 bits ending with the bit centred at p (A + B + A inverted), sliced
// against their mean: the speed / polarity they match best and how many bits
// off - of the first 64: an encoder that gets only A, B and the first half of
// A inverted right (multimon-ng's gen-ng: its decoder checks no more) is read
// too
FlexRx::SyncFit FlexRx::fit_sync(double p) const {
    const double bit = fs_ / 1600;
    float v[80];
    double sum = 0;
    for (int k = 0; k < 80; k++) sum += v[k] = y16_.at(p - (79 - k) * bit);
    SyncFit f;
    f.mean = static_cast<float>(sum / 80);
    uint64_t got_hi = 0, got_lo = 0;
    for (int k = 0; k < 80; k++) {
        got_hi = (got_hi << 1) | (got_lo >> 63);
        got_lo = (got_lo << 1) | (v[k] < f.mean ? 1 : 0);
    }
    for (int m = 0; m < 4; m++) {
        const uint64_t a = MODES[m].a;
        const uint64_t p_hi = a >> 16;
        const uint64_t p_lo = ((a & 0xFFFF) << 48) | (SYNC1_B << 32) | (~a & 0xFFFFFFFFull);
        const int d = kp::popcount64((got_hi & 0xFFFF) ^ p_hi) + kp::popcount64((got_lo ^ p_lo) >> 16);
        if (d < f.dist) f.dist = d, f.mode = m, f.inv = false;
        if (64 - d < f.dist) f.dist = 64 - d, f.mode = m, f.inv = true;
    }
    return f;
}

// Sync 1 with its last bit centred within +-span bits of p? Taken at the
// timing that fits it best (a bit sampled where the window's bits stand
// furthest from their mean); then the frame information word follows
bool FlexRx::sync_at(double p, double span) {
    const double bit = fs_ / 1600;
    double best_p = 0, best_e = -1;
    SyncFit best;
    for (double d = -span; d <= span + 1e-9; d += 1.0 / 24) {
        const SyncFit f = fit_sync(p + d * bit);
        if (f.dist > SYNC1_MAX_ERRORS) continue;
        double e = 0;
        for (int k = 0; k < 80; k++) e += std::fabs(y16_.at(p + d * bit - (79 - k) * bit) - f.mean);
        e -= 1e3 * f.dist;   // fewer bits off first
        if (e > best_e) best_e = e, best_p = p + d * bit, best = f;
    }
    if (best.mode < 0) return false;
    mode_ = best.mode;
    inverted_ = best.inv;
    centre_ = best.mean;   // the carrier offset
    double dev = 0;
    for (int k = 0; k < 80; k++) dev += std::fabs(y16_.at(best_p - (79 - k) * bit) - centre_);
    dev_ = std::max(100.0f, static_cast<float>(dev / 80));
    p_sync_ = best_p;
    st_ = St::FIW;
    need_ = p_sync_ + 32.6 * bit + y16_.delay() + 2;
    host_.freq_error(centre_);
    static const bool dump = getenv("FLEX_CW") != nullptr;
    if (dump)
        fprintf(stderr, "FLEX_SYNC %.3f s %s%s, %d of 64 bits off, carrier %+.0f Hz, deviation %.0f Hz\n",
                t_at(p_sync_), MODES[mode_].name, inverted_ ? " inverted" : "", best.dist, centre_, dev_);
    return true;
}

// the frame information word (cycle, frame) right after sync 1, LSB first
void FlexRx::read_fiw() {
    const double bit = fs_ / 1600;
    uint32_t w = 0;
    for (int k = 0; k < 32; k++) w |= (norm(y16_.at(p_sync_ + (k + 1) * bit)) > 0 ? 1u : 0u) << k;
    const int fixed = flex_correct(w);
    if (fixed == 0 || (fixed > 0 && sum_ok(w & 0x1FFFFF))) {
        cycle_ = (w >> 4) & 0xF;
        frame_ = (w >> 8) & 0x7F;
    } else {
        cycle_ = frame_ = -1;   // the speed is known from sync 1 - the frame is still read
    }
    const Mode& m = MODES[mode_];
    sps_ = fs_ / m.baud;
    p_data_ = p_sync_ + 32.5 * bit;   // sync 2 begins where the frame information ends
    // wait for sync 2 + block 0 (+ room to search 2 symbols either way)
    need_ = p_data_ + (m.baud / 40 + m.baud * 0.16 + 3) * sps_ + 8;
    st_ = St::SYNC2;
}


// Where the symbols' centres lie: first + (k + 0.5 + fraction) x sps for k =
// 0 .. n-1 - the fraction in [lo, hi] (symbols) with the widest eye opening.
// Without transitions (idle words are all 0: one frequency) every fraction
// looks the same - then the middle of the range, where the last one was
double FlexRx::best_fraction(double first, int n, double lo, double hi, double step) const {
    const Smoothed& y = data_y();
    double best = 0, best_e = -1, worst_e = 1e300;
    for (double d = lo; d <= hi + 1e-9; d += step) {
        double e = 0;
        for (int k = 0; k < n; k++) e += std::fabs(y.at(first + (k + 0.5 + d) * sps_) - centre_);
        if (e > best_e) best_e = e, best = d;
        worst_e = std::min(worst_e, e);
    }
    return best_e - worst_e > 1e-3 * best_e ? best : 0.5 * (lo + hi);
}

// Sync 2 = bit sync, C, bit sync inverted, C inverted. The grid sync 1 gives is
// fitted to the eye opening of sync 2 + block 0; the C words fix the whole
// symbols, the bit syncs (outer symbols only) the slicer's levels.
void FlexRx::do_sync2() {
    const Mode& m = MODES[mode_];
    const Smoothed& y = data_y();
    const int total = m.baud / 40;            // 25 ms: 40 / 80 symbols
    const int nb = static_cast<int>(m.baud * 0.16);
    const int nc = m.levels == 2 ? 16 : 8;    // C: 16 bits in 16 or 8 symbols
    const int nbs = total / 2 - nc;           // bit sync: 4, 24, 12 or 32 symbols
    float ce[16], ci[16];                     // C and C inverted as the slicer sees them: + = a "1", inner = 1/3
    for (int j = 0; j < nc; j++) {
        if (m.levels == 2) {
            ce[j] = (SYNC2_C >> (15 - j)) & 1 ? 1.0f : -1.0f;
            ci[j] = -ce[j];
        } else {
            const unsigned a = (SYNC2_C >> (15 - 2 * j)) & 1, b = (SYNC2_C >> (14 - 2 * j)) & 1;
            ce[j] = (a ? 1.0f : -1.0f) * (b ? 1.0f / 3 : 1.0f);
            ci[j] = (a ? -1.0f : 1.0f) * (b ? 1.0f : 1.0f / 3);   // a and b complemented
        }
    }
    const double frac = best_fraction(p_data_ - 2 * sps_, total + nb + 4, -0.5, 0.5, 1.0 / 16);
    int shift = 0;
    float best_score = -1e30f;
    for (int s = -2; s <= 2; s++) {
        float sc = 0;
        for (int j = 0; j < nc; j++) {
            sc += ce[j] * norm(y.at(p_data_ + (s + nbs + j + 0.5 + frac) * sps_));
            sc += ci[j] * norm(y.at(p_data_ + (s + 2 * nbs + nc + j + 0.5 + frac) * sps_));
        }
        if (sc > best_score) best_score = sc, shift = s;
    }
    const double s2 = p_data_ + (shift + frac) * sps_;   // sync 2's first symbol begins
    // slicer levels from the two bit syncs (outer, alternating: their mean = the centre)
    double sum = 0, mag = 0;
    const int n = 2 * nbs;
    for (int j = 0; j < nbs; j++)
        for (int part = 0; part < 2; part++) sum += norm(y.at(s2 + (j + part * (nbs + nc) + 0.5) * sps_));
    dc_ = static_cast<float>(sum / n);
    for (int j = 0; j < nbs; j++)
        for (int part = 0; part < 2; part++)
            mag += std::fabs(norm(y.at(s2 + (j + part * (nbs + nc) + 0.5) * sps_)) - dc_);
    outer_ = std::max(0.2f, static_cast<float>(mag / n));
    inner_ = outer_ / 3;
    // the data portion
    p_data_ = s2 + total * sps_;
    frac_ = 0;
    block_ = 0;
    for (auto& p : ph_) p = Phase();
    st_ = St::DATA;
    need_ = p_data_ + (nb + 1) * sps_ + 8;
}

// --- the data portion: 11 blocks of 160 ms -----------------------------------------

// One block sampled at a fraction: its codewords per phase (bit k = the k-th
// received), the slicer levels after it and how many codewords didn't decode
struct FlexRx::BlockTry {
    double frac = 0;
    std::array<std::array<uint32_t, 8>, 4> raw{};
    float outer = 1, inner = 1.0f / 3;
    int bad = 0;
};

FlexRx::BlockTry FlexRx::try_block(double first, double frac) const {
    const Mode& m = MODES[mode_];
    const Smoothed& y = data_y();
    const int nb = static_cast<int>(m.baud * 0.16);   // 256 / 512 symbols
    BlockTry bt;
    bt.frac = frac;
    bt.outer = outer_;
    bt.inner = inner_;
    std::array<int, 4> pos{};
    auto put = [&](int p, uint32_t bit) {
        const int j = pos[p]++;
        if (j < 256) bt.raw[p][j % 8] |= bit << (j / 8);   // 8 words bit-interleaved
    };
    for (int k = 0; k < nb; k++) {
        const float u = norm(y.at(first + (k + 0.5 + frac) * sps_)) - dc_;
        const uint32_t a = u > 0 ? 1 : 0;   // a "1" = the lower frequency (normal polarity)
        uint32_t b = 0;                     // 4-level: 1 = an inner symbol (+-1.6 kHz)
        if (m.levels == 4) {
            const float mag = std::fabs(u);
            b = mag < 0.5f * (bt.outer + bt.inner) ? 1 : 0;
            if (b) bt.inner += 0.02f * (mag - bt.inner);
            else bt.outer += 0.02f * (mag - bt.outer);
            bt.inner = std::clamp(bt.inner, 0.15f * bt.outer, 0.6f * bt.outer);
        }
        const bool odd = k & 1;
        switch (mode_) {
        case 0: put(0, a); break;                    // 1600/2: phase A
        case 1: put(odd ? 1 : 0, a); break;          // 3200/2: A and C alternately
        case 2: put(0, a), put(1, b); break;         // 3200/4: A and C in one symbol
        default:                                     // 6400/4: A + B, then C + D
            if (!odd) put(0, a), put(1, b);
            else put(2, a), put(3, b);
            break;
        }
    }
    for (int p = 0; p < m.phases; p++)
        for (int w = 0; w < 8; w++) {
            uint32_t x = bt.raw[p][w];
            if (flex_correct(x) < 0) bt.bad++;
        }
    return bt;
}

// How late a block sampled at a fraction is (symbols, + = later than the
// signal): at every change of the decided level the sample half way between
// the two symbols should be the levels' midpoint - how far it is past it, as
// a share of the step, times the step's duration. Symbols that don't change
// carry no timing (idle words are all 0: one frequency) - the eye opening
// over a mostly idle block let the fraction wander by +-0.4 symbol.
double FlexRx::timing_error(double first, int n, double frac) const {
    constexpr double STEP = 0.6;   // a level change's duration in symbols (half-symbol boxcar + the transmitter's filter)
    const Mode& m = MODES[mode_];
    const Smoothed& y = data_y();
    const float thr = 0.5f * (outer_ + inner_);
    auto level = [&](float u) {
        const float l = m.levels == 4 && std::fabs(u) < thr ? inner_ : outer_;
        return u > 0 ? l : -l;
    };
    double sum = 0;
    int cnt = 0;
    float last = level(norm(y.at(first + (0.5 + frac) * sps_)) - dc_);
    for (int k = 1; k < n; k++) {
        const float l = level(norm(y.at(first + (k + 0.5 + frac) * sps_)) - dc_);
        if (l != last) {
            const float mid = norm(y.at(first + (k + frac) * sps_)) - dc_;
            sum += std::clamp((mid - 0.5f * (l + last)) / (l - last), -0.5f, 0.5f);
            cnt++;
        }
        last = l;
    }
    return cnt < 8 ? 0.0 : sum / cnt * STEP;
}

void FlexRx::do_block() {
    const Mode& m = MODES[mode_];
    const int nb = static_cast<int>(m.baud * 0.16);
    const double first = p_data_ + block_ * nb * sps_;
    // The transmitter's clock against ours: the fraction drifts by at most a
    // few hundredths of a symbol a block. A fade or a burst moves the timing
    // anywhere - a fraction is only followed from a block that decoded, and a
    // block that didn't is tried at the fractions around the last good one
    const double prev = frac_;
    BlockTry best = try_block(first, prev - std::clamp(timing_error(first, nb, prev), -0.15, 0.15));
    for (double d : {0.0, -0.05, 0.05, -0.1, 0.1, -0.15, 0.15}) {
        if (best.bad == 0) break;
        BlockTry t = try_block(first, prev + d);
        if (t.bad < best.bad) best = t;
    }
    if (best.bad <= m.phases) frac_ = best.frac;
    outer_ = best.outer;
    inner_ = best.inner;
    static const bool dump = getenv("FLEX_CW") != nullptr;   // offline diagnosis: every codeword
    for (int p = 0; p < m.phases; p++)
        for (int w = 0; w < 8; w++) {
            uint32_t x = best.raw[p][w];
            const int n = flex_correct(x);
            const int idx = block_ * 8 + w;
            ph_[p].ok[idx] = n >= 0;
            ph_[p].fixed[idx] = static_cast<uint8_t>(std::max(0, n));
            ph_[p].w[idx] = n >= 0 ? x & 0x1FFFFF : 0;
            if (dump)
                fprintf(stderr, "FLEX_CW %s frame %d block %d %s word %2d %08X -> %s %06X frac %+.3f\n", m.name, frame_,
                        block_, phase_letter(mode_, p), idx, best.raw[p][w], n < 0 ? "BAD" : (n ? "fixed" : "ok"),
                        ph_[p].w[idx], best.frac);
        }
    // a block of good codewords (<= 1 lost per phase): FLEX, these samples
    // (Host::valid - the Digital squelch); block 0 with the frame's syncs
    if (best.bad <= m.phases)
        host_.valid(t_at(block_ == 0 ? p_sync_ - 112 * fs_ / 1600 : first + best.frac * sps_),
                    t_at(first + (nb + best.frac) * sps_));
    if (++block_ == 11) {
        end_frame();
        return;
    }
    need_ = first + (2 * nb + 1 + frac_ + 0.3) * sps_ + 8;
}


void FlexRx::end_frame() {
    // a FLEX transmitter sends frames back to back: the next sync 1 begins
    // where this frame's last block ends - looked for there first
    const double bit = fs_ / 1600;
    p_next_ = p_data_ + (11 * MODES[mode_].baud * 0.16 + frac_) * sps_ + 111.5 * bit;
    need_ = p_next_ + 0.6 * bit + y16_.delay() + 2;
    st_ = St::NEXT;
    char b[128];
    if (cycle_ >= 0)
        snprintf(b, sizeof b, "%s, cycle %d frame %d%s, deviation %.1f kHz", mode_text().c_str(), cycle_, frame_,
                 inverted_ ? ", inverted" : "", dev_ / 1000.0f);
    else
        snprintf(b, sizeof b, "%s%s, deviation %.1f kHz", mode_text().c_str(), inverted_ ? ", inverted" : "",
                 dev_ / 1000.0f);
    host_.fact("FLEX", b);
    for (int p = 0; p < MODES[mode_].phases; p++) parse_phase(p);
    // group calls set up in this frame
    for (int a = 0; a < 16; a++) {
        Group& g = groups_[a];
        if (g.members.empty() || g.announced) continue;
        g.announced = true;
        std::string list;
        for (size_t i = 0; i < g.members.size() && i < 12; i++) list += (i ? " " : "") + g.members[i];
        if (g.members.size() > 12) list += " +" + std::to_string(g.members.size() - 12);
        host_.event("FLEX " + mode_text() + " group call set up: capcode " + capcode_text(TEMP0 + a - 32768, false) +
                        " (group) for " + std::to_string(g.members.size()) + " pager" +
                        (g.members.size() == 1 ? "" : "s") + " in frame " + std::to_string(g.frame) + ": " + list,
                    10.0);
    }
    expire(host_.time());
}

std::string FlexRx::mode_text() const { return MODES[mode_].name; }

// --- a phase: block information, addresses, vectors, messages -----------------------

void FlexRx::parse_phase(int p) {
    const Phase& ph = ph_[p];
    // In a noisy block, noise "decodes" too: a random word passes the BCH
    // decoder about one time in three after a correction, and 4 bit errors
    // turn a word into another one undetected (even parity again). Message
    // words have their check characters, the block information and vector
    // words a 4-bit check, addresses none: at 2 dB a real page landed on a
    // capcode that wasn't called (its address word 4 bits off, "corrected").
    // So the words that place a page (block information, addresses, vectors)
    // are taken with fewer corrections the noisier their block - its bit
    // errors estimated from the corrections + 3 for each word lost
    bool sure[88];
    for (int b = 0; b < 11; b++) {
        int errs = 0;
        for (int w = 0; w < 8; w++) errs += ph.ok[b * 8 + w] ? ph.fixed[b * 8 + w] : 3;
        const int most = errs >= 12 ? 0 : errs >= 4 ? 1 : 2;
        for (int w = 0; w < 8; w++) sure[b * 8 + w] = ph.ok[b * 8 + w] && ph.fixed[b * 8 + w] <= most;
    }
    // the 4-bit checks are there to catch a word "corrected" into another: a
    // word that needed no correction is taken without (multimon-ng's gen-ng
    // leaves the block information word's 0)
    auto checked = [&](int i) { return sure[i] && (ph.fixed[i] == 0 || sum_ok(ph.w[i])); };
    if (!checked(0)) return;   // without it nothing can be found
    const uint32_t biw = ph.w[0];
    const int prio = (biw >> 4) & 0xF;           // priority addresses come first
    const int eob = (biw >> 8) & 3;              // block information words after this one
    const int vs = (biw >> 10) & 0x3F;           // the vector field's first word
    const int ai = eob + 1;                      // the address field's first word
    if (vs < ai) return;
    const int nvec = vs - ai;                    // a vector word per address word
    if (vs + nvec > 88) return;
    for (int e = 1; e <= eob; e++)
        if (checked(e)) ext_biw(ph.w[e]);
    int naddr = 0;
    for (int i = ai; i < vs;) {
        const int vi = vs + (i - ai);
        if (!sure[i]) {   // a lost address word: its page is lost
            naddr++;
            i++;
            continue;
        }
        const uint32_t aw = ph.w[i];
        if (aw == 0 || aw == 0x1FFFFF) {   // idle
            i++;
            continue;
        }
        const bool next_ok = i + 1 < vs && sure[i + 1];
        const uint32_t aw2 = next_ok ? ph.w[i + 1] : 0;
        const bool priority = naddr++ < prio;
        if (long1(aw) || (long2(aw) && next_ok && (long3(aw2) || long4(aw2)))) {
            // a long address: two address words, two vector words (the second
            // the message's first word, with its check)
            if (next_ok)
                page(p, aw, aw2, true, priority, checked(vi), ph.w[vi], ph.ok[vi + 1], ph.w[vi + 1], ph, vs, nvec);
            i += 2;
        } else if (long2(aw) || long3(aw) || long4(aw)) {
            i++;   // a long address's second word, its first one lost
        } else {
            page(p, aw, 0, false, priority, checked(vi), ph.w[vi], false, 0, ph, vs, nvec);
            i++;
        }
    }
}

// The time / date block information words (table A-16 - A-18): the local time
// at the start of frame 0 of this cycle
void FlexRx::ext_biw(uint32_t w) {
    const int f = (w >> 4) & 7;
    const unsigned s = (w >> 7) & 0x3FFF;
    char b[64];
    if (f == 1) {   // month / day / year (1994 + Y, modulo 32 years)
        const int Y = s & 0x1F, d = (s >> 5) & 0x1F, m = (s >> 10) & 0xF;
        if (m < 1 || m > 12 || d < 1) return;
        const time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        int year = 1994 + Y;
        while (year + 16 < 1900 + tmv.tm_year) year += 32;
        snprintf(b, sizeof b, "%04d-%02d-%02d", year, m, d);
        date_ = b;
    } else if (f == 2) {   // seconds (in 7.5 s) / minute / hour
        const int H = s & 0x1F, M = (s >> 5) & 0x3F, S = (s >> 11) & 7;
        if (H > 23 || M > 59) return;
        snprintf(b, sizeof b, "%02d:%02d:%04.1f", H, M, S * 7.5);
        time_ = b;
    } else if (f == 5 && (s & 0xE) == 0x4) {   // system message: time zone, daylight saving
        const int z = (s >> 4) & 0x1F, L = (s >> 9) & 1;
        const int mins = ZONE_MIN[z];
        if (mins == ZONE_RESERVED) return;
        snprintf(b, sizeof b, "GMT%c%02d%02d%s", mins < 0 ? '-' : '+', std::abs(mins) / 60, std::abs(mins) % 60,
                 L ? "" : ", daylight saving");
        zone_ = b;
    } else {
        return;
    }
    if (date_.empty() && time_.empty()) return;
    std::string t = date_;
    if (!time_.empty()) t += (t.empty() ? "" : " ") + time_;
    if (!zone_.empty()) t += " " + zone_;
    host_.fact("FLEX time", t + " (frame 0 of the cycle)");
}

// A page's message words: the second vector word first for a long address,
// then the message field's. Each with whether it decoded.
std::vector<std::pair<uint32_t, bool>> FlexRx::message_words(const Phase& ph, int b, int n, bool is_long,
                                                             bool vec2_ok, uint32_t vec2, int vs, int nvec,
                                                             bool* in_range) const {
    std::vector<std::pair<uint32_t, bool>> out;
    *in_range = false;
    if (n < 1) return out;
    int from = b, count = n;
    if (is_long) {
        out.emplace_back(vec2, vec2_ok);
        count = n - 1;
    }
    if (count > 0 && (from < vs + nvec || from + count > 88)) return out;
    for (int k = 0; k < count; k++) out.emplace_back(ph.w[from + k], ph.ok[from + k]);
    *in_range = true;
    return out;
}

void FlexRx::page(int p, uint32_t aw, uint32_t aw2, bool is_long, bool priority, bool vec_ok, uint32_t vec,
                  bool vec2_ok, uint32_t vec2, const Phase& ph, int vs, int nvec) {
    const Mode& m = MODES[mode_];
    uint64_t capcode;
    std::string cap;
    int temp = -1;
    if (is_long) {
        capcode = long_capcode(aw, aw2);
        if (capcode) {
            cap = capcode_text(capcode, true);
        } else {
            char b[40];
            snprintf(b, sizeof b, "long %06X %06X", aw, aw2);
            cap = b;
            capcode = (static_cast<uint64_t>(aw) << 21) | aw2;   // a key for the fragments
        }
    } else {
        capcode = aw - 32768;
        cap = capcode_text(capcode, false) + short_kind(aw);
        if (aw >= TEMP0 && aw <= TEMP0 + 15) temp = static_cast<int>(aw - TEMP0);
    }
    const std::string head = "FLEX " + mode_text() + " capcode " + cap;
    const std::string sender = "FLEX capcode " + (is_long && !long_capcode(aw, aw2) ? cap : capcode_text(capcode, is_long));
    const int V = vec_ok ? static_cast<int>((vec >> 4) & 7) : -1;
    std::vector<std::pair<uint32_t, bool>> words;
    bool in_range = true;

    // the decoder data log: frame, phase, address, vector, message words (hex)
    auto log_raw = [&]() {
        if (!host_.raw_wanted()) return;
        char b[96];
        snprintf(b, sizeof b, "FLEX %s %d.%d %s capcode %s%s V%d %06X", m.name, cycle_, frame_, phase_letter(mode_, p),
                 cap.c_str(), priority ? " priority" : "", V, vec);
        std::string r = b;
        for (const auto& w : words) {
            snprintf(b, sizeof b, w.second ? " %06X" : " ------", w.first);
            r += b;
        }
        host_.raw(r);
    };

    if (!vec_ok) {
        log_raw();
        emit(head, sender, {"", "(message lost)", true}, temp);
        return;
    }
    Msg msg;
    switch (V) {
    case 2: {   // short message / tone only: all in the vector word
        const int t = (vec >> 7) & 3;
        const unsigned d = (vec >> 9) & 0xFFF;
        if (t == 0) {
            msg.kind = "numeric";
            for (int k = 0; k < 3; k++) msg.text += numeric_char(d >> (4 * k));
            if (is_long) {   // 5 more in the second vector word
                if (vec2_ok)
                    for (int k = 0; k < 5; k++) msg.text += numeric_char(vec2 >> (4 * k));
                else
                    msg.damaged = true;
            }
            while (!msg.text.empty() && msg.text.back() == ' ') msg.text.pop_back();
        } else if (t == 1 || t == 2) {
            msg.kind = "tone only";
            msg.text = "source " + std::to_string(d & 7);
            if (t == 2) msg.text += ", message " + std::to_string((d >> 3) & 0x3F);
        } else {
            char b[24];
            snprintf(b, sizeof b, "%03X", d);
            msg.kind = "short message (type 3)";
            msg.text = b;
        }
        log_raw();
        emit(head, sender, msg, temp);
        return;
    }
    case 1: {   // short instruction: 000 = a temporary (group) address in a frame
        const int i = (vec >> 7) & 7;
        const unsigned d = (vec >> 10) & 0x7FF;
        log_raw();
        if (i == 0) {
            const int frame = d & 0x7F, a = (d >> 7) & 0xF;
            Group& g = groups_[a];
            const double now = host_.time();
            if (g.frame != frame || now - g.t > GROUP_KEEP_S) g = Group();
            if (std::find(g.members.begin(), g.members.end(), capcode_text(capcode, is_long)) == g.members.end())
                g.members.push_back(capcode_text(capcode, is_long));
            g.frame = frame;
            g.t = now;
        } else if (host_.verbose()) {
            host_.event(head + " instruction " + std::to_string(i), 10.0);
        }
        return;
    }
    case 3:
    case 4:
    case 7: {   // numeric: standard, special format, numbered
        const int b = (vec >> 7) & 0x7F, n = ((vec >> 14) & 7) + 1;
        const unsigned k03 = (vec >> 17) & 0xF;
        words = message_words(ph, b, n, is_long, vec2_ok, vec2, vs, nvec, &in_range);
        log_raw();
        if (!in_range || words.empty()) {
            emit(head, sender, {"", "(message lost)", true}, temp);
            return;
        }
        // 6-bit check: K0-K3 in the vector, K4 K5 the first message bits
        unsigned sum = 0;
        bool all_ok = true;
        for (size_t k = 0; k < words.size(); k++) {
            sum += group_sum(k == 0 ? words[k].first & ~3u : words[k].first);
            all_ok = all_ok && words[k].second;
        }
        sum &= 0xFF;
        sum = (sum & 0x3F) + (sum >> 6);
        const unsigned K = k03 | ((words[0].first & 3) << 4);
        msg.kind = "numeric";
        msg.damaged = !all_ok || ((~sum) & 0x3F) != K;
        // the digits, 4 bits each LSB first, after the check (and a numbered
        // message's number / flags)
        const int skip = V == 7 ? 10 : 2;
        const int nbits = static_cast<int>(words.size()) * 21;
        for (int pos = skip; pos + 4 <= nbits; pos += 4) {
            unsigned dgt = 0;
            bool lost = false;
            for (int k = 0; k < 4; k++) {
                const auto& w = words[(pos + k) / 21];
                lost = lost || !w.second;
                dgt |= ((w.first >> ((pos + k) % 21)) & 1) << k;
            }
            msg.text += lost ? '?' : numeric_char(dgt);
        }
        while (!msg.text.empty() && msg.text.back() == ' ') msg.text.pop_back();
        if (V == 7 && words[0].second && ((words[0].first >> 8) & 1))   // numbered + retrieval: the number
            msg.text += " (#" + std::to_string((words[0].first >> 2) & 0x3F) + ")";
        emit(head, sender, msg, temp);
        return;
    }
    default: {   // 0 secure, 5 alphanumeric, 6 hex / binary - may come in fragments
        const int b = (vec >> 7) & 0x7F, n = (vec >> 14) & 0x7F;
        words = message_words(ph, b, n, is_long, vec2_ok, vec2, vs, nvec, &in_range);
        log_raw();
        if (!in_range || words.size() < 2) {
            emit(head, sender, {"", "(message lost)", true}, temp);
            return;
        }
        const uint32_t hdr = words[0].first;
        int C, F, N;
        unsigned kmask;
        if (V == 6) C = (hdr >> 12) & 1, F = (hdr >> 13) & 3, N = (hdr >> 15) & 0x3F, kmask = 0xFFF;
        else C = (hdr >> 10) & 1, F = (hdr >> 11) & 3, N = (hdr >> 13) & 0x3F, kmask = 0x3FF;
        if (!words[0].second) C = 0, F = 3, N = -1;   // header lost: taken as a whole message
        // fragment check: the words' group sum (the check field as 0), complemented
        unsigned sum = 0;
        bool all_ok = true;
        for (size_t k = 0; k < words.size(); k++) {
            sum += group_sum(k == 0 ? words[k].first & ~kmask : words[k].first);
            all_ok = all_ok && words[k].second;
        }
        const bool first = F == 3;
        msg.damaged = !all_ok || ((~sum) & kmask) != (hdr & kmask);
        int sig = -1;
        unsigned sum7 = 0;
        if (V == 6) {
            msg.kind = "binary";
            // first fragment: word 2 = retrieval, mail drop, display direction,
            // header flag, blocking length (bits per character, 1 = binary), signature
            const size_t from = first ? 2 : 1;
            const int blk = first && words[1].second ? static_cast<int>((words[1].first >> 4) & 0xF) : 1;
            const int cs = blk == 0 ? 16 : blk;
            std::vector<uint8_t> bits;
            for (size_t k = from; k < words.size(); k++)
                for (int j = 0; j < 21; j++) bits.push_back((words[k].first >> j) & 1);
            const size_t full = bits.size();
            // the fill: the opposite of the last data bit to the word's end
            if (!bits.empty()) {
                const uint8_t fill = bits.back();
                while (!bits.empty() && bits.back() == fill) bits.pop_back();
            }
            size_t len = bits.size();
            if (cs > 1 && full % cs == 0 && len % cs != 0) len = full;   // no fill: the data filled the words
            len -= len % std::max(1, cs);
            if (cs == 7 || cs == 8) {
                std::string t;
                for (size_t pos = 0; pos + cs <= len; pos += cs) {
                    unsigned c = 0;
                    for (int j = 0; j < cs; j++) c |= bits[pos + j] << j;
                    t += static_cast<char>(c);
                }
                msg.text = kp::printable(t);
            } else {
                static const char* hex = "0123456789ABCDEF";
                for (size_t pos = 0; pos + 4 <= len; pos += 4)
                    msg.text += hex[bits[pos] | bits[pos + 1] << 1 | bits[pos + 2] << 2 | bits[pos + 3] << 3];
            }
        } else {
            msg.kind = V == 5 ? "alpha" : "secure";
            // 7-bit characters, 3 a word; an alphanumeric first fragment starts
            // with the message's signature
            std::string raw;
            std::vector<size_t> lost_at;   // a lost word's place in raw
            for (size_t k = 1; k < words.size(); k++) {
                if (!words[k].second) {
                    lost_at.push_back(raw.size());
                    continue;
                }
                for (int s = 0; s < 21; s += 7) {
                    const char c = static_cast<char>((words[k].first >> s) & 0x7F);
                    if (k == 1 && s == 0 && first && V == 5) sig = c;
                    else raw += c;
                }
            }
            // fill: NULs end a fragment, ETX / NUL the message
            while (!raw.empty() && (raw.back() == 0 || (C == 0 && raw.back() == 3))) raw.pop_back();
            for (unsigned char c : raw) sum7 += c;
            size_t from = 0;
            for (size_t at : lost_at) {
                at = std::min(at, raw.size());
                msg.text += kp::printable(raw.substr(from, at - from)) + "\xE2\x80\xA6";   // ...
                from = at;
            }
            if (from < raw.size()) msg.text += kp::printable(raw.substr(from));
        }
        fragment(head, sender, capcode, temp, msg, C, F, N, sig, sum7, first);
        return;
    }
    }
}

// Fragmented messages (alphanumeric, secure, binary): the first fragment has
// F = 3, the next 0, 1, 2, 0 ...; C = 0 on the last. Put together per
// address + message number and shown once complete.
void FlexRx::fragment(const std::string& head, const std::string& sender, uint64_t capcode, int temp, const Msg& m,
                      int C, int F, int N, int sig, unsigned sum7, bool first) {
    const auto key = std::make_pair(capcode, N);
    const double now = host_.time();
    auto it = frags_.find(key);
    if (first && C == 0) {   // the whole message
        Msg w = m;
        if (sig >= 0 && !w.damaged && static_cast<int>((~sum7) & 0x7F) != sig) w.damaged = true;
        if (it != frags_.end()) {
            emit(it->second.head, it->second.sender, {it->second.kind, it->second.text + "\xE2\x80\xA6", true}, -1);
            frags_.erase(it);
        }
        emit(head, sender, w, temp);
        return;
    }
    if (first) {   // a fragmented message begins
        if (it != frags_.end())
            emit(it->second.head, it->second.sender, {it->second.kind, it->second.text + "\xE2\x80\xA6", true}, -1);
        Frag f;
        f.head = head;
        f.kind = m.kind;
        f.text = m.text;
        f.next_f = 0;
        f.sig = sig;
        f.sum7 = sum7;
        f.damaged = m.damaged;
        f.t = now;
        f.sender = sender;
        frags_[key] = f;
        return;
    }
    if (it == frags_.end()) {   // its start was missed
        Frag f;
        f.head = head;
        f.kind = m.kind;
        f.text = "\xE2\x80\xA6" + m.text;
        f.damaged = true;
        f.sender = sender;
        it = frags_.emplace(key, f).first;
    } else {
        Frag& f = it->second;
        if (F != f.next_f) {   // a fragment in between was missed
            f.text += "\xE2\x80\xA6";
            f.damaged = true;
        }
        f.text += m.text;
        f.sum7 += sum7;
        f.damaged = f.damaged || m.damaged;
    }
    Frag& f = it->second;
    f.next_f = next_fragment(F);
    f.t = now;
    if (C == 0) {
        bool damaged = f.damaged;
        if (f.sig >= 0 && !damaged && static_cast<int>((~f.sum7) & 0x7F) != f.sig) damaged = true;
        emit(f.head, f.sender, {f.kind, f.text, damaged}, temp);
        frags_.erase(it);
    }
}

void FlexRx::emit(const std::string& head, const std::string& sender, const Msg& m, int temp) {
    std::string h = head;
    if (temp >= 0) {   // a group call: who was told to listen
        Group& g = groups_[temp];
        if (!g.members.empty()) {
            std::string list;
            for (size_t i = 0; i < g.members.size() && i < 12; i++) list += (i ? " " : "") + g.members[i];
            if (g.members.size() > 12) list += " +" + std::to_string(g.members.size() - 12);
            h += " (to " + std::to_string(g.members.size()) + " pager" + (g.members.size() == 1 ? "" : "s") + ": " +
                 list + ")";
            g = Group();
        }
    }
    std::string body;
    if (m.kind.empty()) body = ": " + m.text;
    else if (m.kind == "tone only") body = " tone only (" + m.text + ")";
    else body = " " + m.kind + ": " + m.text;
    if (m.damaged && !m.kind.empty()) body += " [incomplete]";
    host_.event(h + body, 10.0);
    // text pages may hold an incident address (kraken_doa's incident map)
    if (m.kind == "alpha" && !m.text.empty()) host_.message(sender, m.text);
    pages_.page(true, sender, m.kind == "tone only" ? body.substr(1) : body.substr(body.find(':') + 2));
}

void FlexRx::expire(double now) {
    for (auto it = frags_.begin(); it != frags_.end();) {
        if (now - it->second.t > FRAG_KEEP_S) {
            emit(it->second.head, it->second.sender, {it->second.kind, it->second.text + "\xE2\x80\xA6", true}, -1);
            it = frags_.erase(it);
        } else {
            ++it;
        }
    }
    for (Group& g : groups_)
        if (!g.members.empty() && now - g.t > GROUP_KEEP_S) g = Group();
}

}  // namespace pagers
