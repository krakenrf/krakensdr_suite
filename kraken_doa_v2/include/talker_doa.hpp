#pragma once

// DoA per talker (P25 unit ID, ...) of ONE VFO.
//
// The VFO's digital decoder says WHO transmits WHEN (dig::TalkerSpan from
// kp::Host::talker), in the VFO's decimated-stream positions; the VFO's MUSIC
// processor hands over every frame's own covariance together with the stream
// samples it was computed from (MUSICProcessor::setFrameTap). Both count the
// same samples (MultiChannelDecimated::stream_pos), so a frame that lies
// entirely inside one talker's confirmed transmission - GUARD_S inside both
// ends, no other talker overlapping it - holds only that radio's signal:
//   - its covariance is added to the talker's current transmission and MUSIC
//     is re-run on the sum (MUSICProcessor::spectrumFromCovariance: the same
//     steering vectors, source count, half-plane and array offset) - the
//     talker's bearing;
//   - the frame alone is run through MUSIC again and handed to the mobile DF
//     (rdf_mapper.cpp) as a record of that talker, so each radio gets its own
//     heat map while driving.
// Frames that straddle a boundary, mix two radios or fall between
// transmissions are dropped: a radio's bearing never contains another's
// samples. A transmission = one span (one call / PTT); its bearing goes to the
// talker's history when the next one starts.
//
// The talker's ID arrives after its first frames (P25: the unit ID is in the
// first LDU1, decoded ~0.5 s after the call header, plus the decoder's queue
// and pipe), so frames wait up to FRAME_WAIT_S for a span to claim them.
//
// Several talker channels (DMR's two timeslots): a radio on one slot may be
// reported after a frame of the other slot's radio was already inside its
// span, so frames of channels other than 0 are committed only CHANNEL_HOLD_MS
// after MUSIC computed them. A span with the id "?" (a slot in use, its radio
// not named yet) holds no talker: frames touching it go to no one.
//
// Packets (kp::Talker::packet - ADS-B / Mode S messages of 64-120 us, AIS
// messages of 27-133 ms, from many aircraft / ships at once): while a VFO's
// decoder reports packets, the pipeline hands every block to add_block().
// Up to RAW_MAX_RATE it keeps the stream itself for RING_S: a packet's
// covariance comes from exactly its samples, and when the packet names its
// channel (freq_hz, bw_hz - AIS A / B in one VFO) each antenna's samples are
// first mixed to that channel and low-pass filtered, so a ship on the other
// channel at the same moment doesn't leak in. Above it (ADS-B at 2.4 MHz) it
// keeps the covariance of each CHUNK-sample piece instead (the whole band): a
// packet's = the sum over the chunks entirely inside it. Trace-normalised;
// packets that overlap another talker's packet ON THE SAME CHANNEL are dropped.
// A packet talker's covariance is an exponentially weighted sum of its
// packets (time constant: the plugin's avg_s, else PACKET_TAU_S; stream
// time), MUSIC re-run on it at most every PACKET_RECOMPUTE_MS; its
// "transmissions" are packets and hist gets its bearing every
// PACKET_HIST_MS. No mobile DF records (aircraft / ships move and say where
// they are).
//
// Threads: add_frame on the decimation pipeline (under the MUSIC processor's
// lock), add_span on the decoder's worker, update / snapshot on the rdf
// sampler thread. update() never calls into the MUSIC processor while holding
// its own lock (the tap takes them in the other order).

#include <Eigen/Dense>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <complex>

#include "digital/digital_decoder.hpp"
#include "signal_processing/shared_decimator.hpp"

class MUSICProcessor;

namespace tdoa {

constexpr double GUARD_S = 0.04;                  // a frame must lie this far inside its transmission
constexpr double FRAME_WAIT_S = 6.0;              // a frame waits this long for its talker
constexpr double RETUNE_RESET_HZ = 10000;         // MUSIC frequency moved more: another signal, start over
constexpr int64_t ACTIVE_MS = 3000;               // an open span reported within this long = transmitting
constexpr int64_t TALKER_TTL_MS = 30 * 60 * 1000; // a talker not heard for this long is forgotten
constexpr size_t MAX_TALKERS = 200;                // radios + aircraft
constexpr size_t MAX_HIST = 20;                   // earlier transmissions kept per talker
constexpr int64_t CHANNEL_HOLD_MS = 1500;         // channels != 0: a frame is committed this long after it was computed
// packet talkers
constexpr int CHUNK = 16;                         // samples per stored covariance (6.7 us at 2.4 MHz)
constexpr double RING_S = 1.5;                    // stream kept for packets reported late (decoder queue + pipe)
constexpr int MIN_PACKET_CHUNKS = 2;
constexpr int64_t PACKET_HOLD_MS = 200;           // a packet waits this long for an overlapping one
constexpr double PACKET_TAU_S = 1.5;              // a packet's weight halves in ~1 s (aircraft move)
constexpr int64_t PACKET_RECOMPUTE_MS = 1000;
constexpr int64_t PACKET_HIST_MS = 10000;
constexpr int64_t PACKET_TTL_MS = 2 * 60 * 1000;
constexpr int64_t PACKET_IDLE_MS = 5 * 60 * 1000; // no packets for this long: stop keeping the stream
constexpr double RAW_MAX_RATE = 500e3;            // up to this VFO rate the samples themselves are kept
inline bool unnamed_talker(const std::string& id) { return id == "?"; }

// One frame matched to a talker, MUSIC re-run on it alone (a mobile DF record)
struct TalkerFrame {
    std::string id, label;
    int64_t stamp_ms = 0;          // when MUSIC computed it (system clock)
    std::vector<float> spec;       // pseudospectrum, like MUSICProcessor::getPseudospectrum()
    double res = 1;                // degrees per bin
    float conf = 0;
};

// A talker as the UI shows it (snapshot)
struct TalkerInfo {
    std::string id, label, plugin;
    bool active = false;           // transmitting now
    int tx = 0;                    // transmissions heard
    int frames = 0;                // frames used, all transmissions
    int64_t first_ms = 0, last_ms = 0;   // first / last heard (system clock)
    bool packet = false;           // a packet talker (aircraft, ship): tx = packets, no heat map
    double avg_s = 0;              // packets averaged over (s, 0 = PACKET_TAU_S)
    int64_t spec_ms = 0;           // spec computed (system clock)
    // the latest transmission that had frames: MUSIC on its summed covariance
    int tx_frames = 0;
    int64_t tx_start_ms = 0, tx_end_ms = 0;   // its first / last frame
    std::vector<float> spec;       // empty = no bearing yet
    double res = 1;
    float doa = -1, conf = 0;      // peak in degrees as the MUSIC DoA plot shows it (array offset applied)
    struct Hist { int64_t t_ms; float doa, conf; int frames; };
    std::vector<Hist> hist;        // earlier transmissions, oldest first (t = their last frame)
};

class TalkerDoa {
public:
    // on while the VFO's digital decoder is on; off forgets everything
    void set_active(bool on);
    bool active() const { return active_.load(std::memory_order_relaxed); }

    // MUSICProcessor::FrameTap
    void add_frame(const Eigen::MatrixXcd& R, uint64_t a, uint64_t b, float rate_hz, double freq_hz, float eig_ratio);
    // DigitalDecoder talker handler
    void add_span(const dig::TalkerSpan& s);
    // Pipeline, after MUSIC (same gate: no calibration / retune hold), while
    // wants_blocks(): the stream packets are cut from
    bool wants_blocks() const { return want_blocks_.load(std::memory_order_relaxed); }
    void add_block(const SharedDecimator::MultiChannelDecimated& d);

    void reset();

    // Matches the waiting frames to talkers and re-runs MUSIC (mp) on what
    // changed; the frames matched since the last call go to *out
    void update(const MUSICProcessor& mp, std::vector<TalkerFrame>* out);
    std::vector<TalkerInfo> snapshot() const;

private:
    std::atomic<bool> active_{false};
    mutable std::mutex mu_;

    struct Frame {
        Eigen::MatrixXcd R;
        uint64_t a, b;
        float rate;
        double freq;
        int64_t stamp_ms;
    };
    std::deque<Frame> frames_;

    struct Span {
        std::string plugin, id, label;
        uint64_t start, end;
        bool closed;
        int channel;
        uint64_t seq;              // transmission number (unique)
        int64_t updated_ms;
    };
    std::deque<Span> spans_;

    struct Talker {
        TalkerInfo info;
        uint64_t seq = 0;          // the transmission R sums
        Eigen::MatrixXcd R;        // sum of n * R_frame over it
        double w = 0;              // sum of n
        bool dirty = false;
        int64_t open_ms = 0;       // its open span was last reported (for active)
        // packet talkers: R / w decay with stream time
        double last_s = 0;         // stream time of the latest packet
        int64_t computed_ms = 0, hist_ms = 0;
    };
    std::map<std::string, Talker> talkers_;

    // packets waiting PACKET_HOLD_MS for an overlapping one (then committed)
    struct Packet {
        std::string plugin, id, label;
        uint64_t a, b;
        float rate;
        double freq, bw, avg;      // channel (NAN = whole VFO), averaging
        Eigen::MatrixXcd R;        // trace-normalised
        int64_t stamp_ms;
        bool bad = false;          // overlaps another talker's packet
    };
    std::deque<Packet> packets_;
    std::atomic<bool> want_blocks_{false};
    int64_t last_packet_ms_ = 0;
    void add_packet(const dig::TalkerSpan& s);
    void commit_packet_locked(Packet& p, int64_t now);

    // per-chunk covariances (upper triangle, row-major) of the stream
    mutable std::mutex ring_mu_;
    int ring_m_ = 0, ring_tri_ = 0;
    float ring_rate_ = 0;
    size_t ring_cap_ = 0;
    std::vector<std::complex<float>> ring_;
    std::vector<uint64_t> ring_tag_;       // chunk number + 1 held by the slot (0 = none)
    std::vector<std::complex<float>> part_;   // the chunk being summed
    uint64_t part_chunk_ = ~0ull;
    int part_n_ = 0;
    uint64_t ring_next_ = ~0ull;           // stream position the next block should start at
    // the stream itself (rate <= RAW_MAX_RATE): sample p of antenna k at
    // raw_[(p % raw_cap_) * ring_m_ + k], held for raw_lo_ <= p < raw_hi_
    bool raw_mode_ = false;
    std::vector<std::complex<float>> raw_;
    size_t raw_cap_ = 0;
    uint64_t raw_lo_ = 0, raw_hi_ = 0;
    void free_ring();
    // false: not all of [a, b) is held. freq / bw: the channel to filter out
    // (raw mode only; NAN = the whole VFO)
    bool packet_cov(uint64_t a, uint64_t b, double freq, double bw, Eigen::MatrixXcd* R) const;
    bool packet_cov_raw(uint64_t a, uint64_t b, double freq, double bw, Eigen::MatrixXcd* R) const;

    uint64_t next_seq_ = 1;
    double freq_ = 0;              // MUSIC frequency of the frames (0 = none yet)
    int64_t recompute_ms_ = 0;

    void reset_locked();
    void prune_locked(int64_t now);
};

}  // namespace tdoa
