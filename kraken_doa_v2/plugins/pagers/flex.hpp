// FLEX pagers (Motorola FLEX): 1600 / 3200 / 6400 bit/s, 2- and 4-level FSK
// (+-4.8 kHz, 4-level inner +-1.6 kHz) - all four speeds, any frame.
//
// A frame (1.875 s, 128 a cycle, cycle 0-14) = sync 1 at 1600 bit/s 2-level
// (32 bits 1010, the 32-bit "A" word naming the frame's speed, 16 bits B,
// the A word inverted) + the frame information word (cycle, frame), sync 2
// at the frame's speed (bit sync, C, both inverted - 25 ms), then 11 blocks
// of 160 ms: per PHASE (1600 bit/s A; 3200 A + C; 6400 A + B + C + D) 8
// codewords bit-interleaved. A phase's 88 codewords: block information word
// (where the address and vector fields start), addresses (short = 1 word,
// long = 2), one vector word per address word (message type, start, length),
// the message words. Codeword = BCH(31,21) + parity, LSB first (bch.hpp).
//
// Sources: the frame and message formats, capcode ranges / conversions and
// checks of Motorola's MC68175 FLEX decoder data sheet (appendix A / B); the
// sync patterns from the FLEX patent's drawing as published by Jelmer Bruijn
// ("Motorola FLEX / P2000 decoding"); bit order, symbol mapping and the
// fragment / message checks confirmed on a real 6400/4 recording.
#pragma once

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "pages.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pagers {

class FlexRx {
public:
    FlexRx(kp::Host& host, Pages& pages, double fs);
    // one discriminator sample (Hz, + = above the carrier) at Host::time() t
    void push(float disc_hz, double t);
    void reset();

private:
    // Sync 1 is looked for every quarter bit on the smoothed discriminator (and,
    // once a frame was read, where the next one must begin); everything after
    // it is sampled on the symbol grid sync 1 gives (one transmitter clock for
    // the whole frame), its fraction refined per block on the eye opening - a
    // loop following every symbol lost whole blocks after a fade.
    // the discriminator after a half-symbol boxcar, kept by sample number
    class Smoothed {
    public:
        explicit Smoothed(int len);
        float push(float x);   // the newest output
        float at(double pos) const;   // linear interpolation, pos = the input sample whose time it is
        double delay() const { return delay_; }
        void reset();
        static constexpr uint64_t RING = 1u << 16;   // 1.7 s

    private:
        kp::Fir<float> box_;
        double delay_;
        std::vector<float> y_;
        uint64_t n_ = 0;
    };

    struct Mode {
        const char* name;
        uint32_t a;     // sync 1 A word, first sent = MSB
        int baud;       // data symbol rate
        int levels;     // 2 / 4
        int phases;     // 1 / 2 / 4
    };
    static const Mode MODES[4];

    // one phase of a frame: 88 words (info bits) + whether each decoded
    struct Phase {
        uint32_t w[88] = {};
        bool ok[88] = {};
        uint8_t fixed[88] = {};   // bits the BCH decoder corrected
    };
    // a message under assembly from its fragments
    struct Frag {
        std::string head;        // event text up to the message kind
        std::string kind;        // "alpha" / "secure" / "binary"
        std::string text;
        int next_f = 0;          // the fragment number expected next
        int sig = -1;            // the first fragment's signature (alpha)
        unsigned sum7 = 0;       // its characters' sum (the signature check)
        bool damaged = false;
        double t = 0;            // last fragment's time
        std::string sender;      // Host::message's sender
    };
    // a temporary (group) address being set up by short instructions
    struct Group {
        std::vector<std::string> members;
        int frame = -1;
        double t = 0;
        bool announced = false;
    };
    // what a page's vector + message words say
    struct Msg {
        std::string kind;        // "alpha", "numeric", "tone only", ...
        std::string text;
        bool damaged = false;
    };

    // HUNT: sync 1 anywhere; NEXT: checked where the hunt found it, or where the
    // frame just read says the next one begins (frames follow with no gap)
    enum class St { HUNT, NEXT, FIW, SYNC2, DATA };
    // sync 1's 80 bits A + B + A inverted at one place, sliced against their mean
    struct SyncFit {
        int mode = -1;
        bool inv = false;
        int dist = 65;          // bits off (of the first 64)
        float mean = 0;
    };

    kp::Host& host_;
    Pages& pages_;
    double fs_;
    Smoothed y16_, y32_;   // half-symbol boxcars for 1600 / 3200 symbols a second
    uint64_t ns_ = 0;      // input samples so far
    double t_now_ = 0;     // the newest one's time
    St st_ = St::HUNT;
    // the frame being received
    int mode_ = 0;
    bool inverted_ = false;
    float centre_ = 0, dev_ = 1;   // sync 1: carrier offset (Hz), outer deviation (Hz)
    double p_sync_ = 0;            // the centre of sync 1's last bit (input sample number)
    double p_next_ = 0;            // ... of the next frame's, by this frame's grid
    double p_data_ = 0;            // where the data symbols' grid starts (sample number)
    double need_ = 0;              // samples the next step waits for
    double sps_ = 24;              // input samples a data symbol
    double frac_ = 0;              // the grid's offset found for the last block (symbols)
    float dc_ = 0, outer_ = 1, inner_ = 1.0f / 3;   // data slicer (sync-1 deviation units)
    int block_ = 0;
    std::array<Phase, 4> ph_{};
    int cycle_ = -1, frame_ = -1;
    // across frames
    std::map<std::pair<uint64_t, int>, Frag> frags_;   // (capcode, message number)
    std::array<Group, 16> groups_{};
    std::string date_, time_, zone_;

    const Smoothed& data_y() const { return MODES[mode_].baud == 3200 ? y32_ : y16_; }
    float norm(float v) const { return (inverted_ ? 1.0f : -1.0f) * (v - centre_) / dev_; }   // > 0 = a "1"
    double t_at(double pos) const { return t_now_ - (static_cast<double>(ns_ - 1) - pos) / fs_; }
    double best_fraction(double first, int n, double lo, double hi, double step) const;
    SyncFit fit_sync(double p) const;
    bool sync_at(double p, double span);
    void read_fiw();
    void do_sync2();
    struct BlockTry;
    BlockTry try_block(double first, double frac) const;
    double timing_error(double first, int n, double frac) const;
    void do_block();
    void end_frame();
    void parse_phase(int p);
    void ext_biw(uint32_t w);
    void page(int p, uint32_t aw, uint32_t aw2, bool is_long, bool priority, bool vec_ok, uint32_t vec,
              bool vec2_ok, uint32_t vec2, const Phase& ph, int vs, int nvec);
    std::vector<std::pair<uint32_t, bool>> message_words(const Phase& ph, int b, int n, bool is_long, bool vec2_ok,
                                                         uint32_t vec2, int vs, int nvec, bool* in_range) const;
    void fragment(const std::string& head, const std::string& sender, uint64_t capcode, int temp, const Msg& m,
                  int C, int F, int N, int sig, unsigned sum7, bool first);
    void emit(const std::string& head, const std::string& sender, const Msg& m, int temp);
    void expire(double now);
    std::string mode_text() const;
};

}  // namespace pagers
