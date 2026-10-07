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

#include "digital/digital_decoder.hpp"

class MUSICProcessor;

namespace tdoa {

constexpr double GUARD_S = 0.04;                  // a frame must lie this far inside its transmission
constexpr double FRAME_WAIT_S = 6.0;              // a frame waits this long for its talker
constexpr double RETUNE_RESET_HZ = 10000;         // MUSIC frequency moved more: another signal, start over
constexpr int64_t ACTIVE_MS = 3000;               // an open span reported within this long = transmitting
constexpr int64_t TALKER_TTL_MS = 30 * 60 * 1000; // a talker not heard for this long is forgotten
constexpr size_t MAX_TALKERS = 100;
constexpr size_t MAX_HIST = 20;                   // earlier transmissions kept per talker
constexpr int64_t CHANNEL_HOLD_MS = 1500;         // channels != 0: a frame is committed this long after it was computed
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
    };
    std::map<std::string, Talker> talkers_;

    uint64_t next_seq_ = 1;
    double freq_ = 0;              // MUSIC frequency of the frames (0 = none yet)
    int64_t recompute_ms_ = 0;

    void reset_locked();
    void prune_locked(int64_t now);
};

}  // namespace tdoa
