#pragma once

// 4FSK sync receiver for P25 Phase 1 (C4FM) and DMR, 4800 symbols/s on a
// 48 kHz FM discriminator (dig_fsk4.cpp). It finds the sync words and hands
// each frame / burst to a Fsk4Sink - the P25 plugin and the DMR plugin each
// enable only their own protocol.

#include <deque>
#include <vector>

#include "dig_common.hpp"

namespace dig {

constexpr int P25_FRAME_DIBITS = 864;   // longest P25 frame (LDU)

// DMR sync kinds (values = DmrProto::SyncType)
enum class DmrSync : int { NONE = 0, BS_VOICE, BS_DATA, MS_VOICE, MS_DATA, TS1_VOICE, TS1_DATA, TS2_VOICE, TS2_DATA };

class Fsk4Sink {
public:
    virtual ~Fsk4Sink() = default;
    // P25 frame whose sync ends at s.sync_end (P25_FRAME_DIBITS available)
    virtual bool p25_frame(const SymSrc&) { return false; }
    // DMR burst; sync = NONE for an expected voice burst B..F at a position
    // extrapolated from the last sync. *period_syms = burst repeat period
    virtual bool dmr_burst(const SymSrc&, DmrSync, int* period_syms) { (void)period_syms; return false; }
    virtual void reset() {}
};

class Fsk4Receiver {
public:
    Fsk4Receiver(Fsk4Sink& sink, bool p25, bool dmr);
    // discriminator output in Hz, 48 kHz (10 samples/symbol)
    void process(const float* d, size_t n);
    void reset();
private:
    Fsk4Sink& sink_;
    bool en_p25_ = true, en_dmr_ = true;
    SampleBuf bp_, bd_;             // P25 (boxcar) / DMR (RRC) filtered (bd_ only with DMR on)
    std::vector<float> rrc_taps_;
    std::vector<float> rrc_x_;      // the RRC's input: the last taps - 1 clipped samples, then the block's
    std::vector<float> rrc_y_;      // ... its output for the block
    float box_hist_[10] = {0};
    float box_sum_ = 0;
    int box_pos_ = 0;
    struct Pattern { std::vector<float> t; Mode proto; int id; };
    std::vector<Pattern> pats_p25_, pats_dmr_;
    struct Peak { bool active = false; float best = 0; int64_t idx = 0; int pat = 0; int age = 0; };
    Peak pk_p25_, pk_dmr_;
    struct Job { Mode proto; SymSrc src; int need_k; DmrSync sync; bool on_grid; };
    std::deque<Job> jobs_;
    int64_t dmr_expect_ = -1;       // sync_end of the next expected DMR burst
    int dmr_expect_left_ = 0;
    SymSrc dmr_last_src_;
    int dmr_period_ = 144;
    bool dmr_confirmed_ = false;    // the followed burst sequence's last bursts checked out
    // the sync correlation of samples [n0, n0 + cnt) of b, every pattern (block: vectorises across
    // samples; each one's sums in the same order as one at a time)
    struct Corr {
        int64_t n0 = 0;
        size_t skip = 0, cnt = 0;   // the first `skip` samples have no full sync window yet
        std::vector<float> e, c;    // per sample: energy; correlation with pattern p at c[p * cnt + j]
    };
    Corr corr_p25_, corr_dmr_;
    std::vector<float> m_;          // correlate_block: per sample, its window's mean
    void correlate_block(const SampleBuf& b, const std::vector<Pattern>& pats, int64_t n0, size_t cnt, Corr& out);
    // the peak search at sample j of the block (on_sync for a finished peak)
    void peak(const Corr& cr, size_t j, size_t np, Peak& pk, Mode proto);
    void on_sync(Mode proto, int pat, int64_t sync_end, float corr_sign);
    void run_jobs();
};

}  // namespace dig
