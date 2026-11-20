// Signal classifier: the tracker - one track per signal across snapshots.
// A detection joins the track whose band overlaps it most (overlap / union
// >= min_iou, or centres within freq_tol_hz for narrow signals of similar
// width), one detection per track, best pairs first; a detection that lies
// mostly inside a track's band without matching it (a piece of the signal:
// one FSK tone, a burst's lobe, a narrower measurement of the same signal)
// keeps that track alive but changes neither its band nor its class evidence.
// Frequencies are absolute (capture centre + offset), so a retune that keeps
// the signal in band keeps its track. Class evidence per view is a
// mean of log-probabilities (of the first 1 / alpha looks, then exponentially
// weighted, alpha 0.1); a track's probabilities
// are its softmax for the view most of its detections had. Tracks unseen for
// expire_s go.
//
// One signal = one track: after each update, a track lying mostly
// (merge_overlap of its width) inside another is merged into it - into the
// wider one when that is at least twice as wide (unless the narrow one is a
// strong continuous signal of its own: >= 6 dB over the wide one, or the wide
// one fills the band - a carrier on a DAB block, which the detector reports
// apart on purpose), else into the one with more hits (two transmitters
// taking turns on one channel). Neighbouring channels barely overlap and stay
// apart.

#include "sigclass/sigclass.hpp"

#include <algorithm>
#include <cmath>

namespace sigclass {

Tracker::Tracker(const TrackerConfig& cfg) : cfg_(cfg) {}

void Tracker::clear() {
    tracks_.clear();
    ev_.clear();
}

const std::vector<Track>& Tracker::expire(double t_s) {
    for (size_t i = 0; i < tracks_.size();) {
        if (t_s - tracks_[i].last_s > cfg_.expire_s) {
            tracks_.erase(tracks_.begin() + long(i));
            ev_.erase(ev_.begin() + long(i));
        } else {
            i++;
        }
    }
    return tracks_;
}

namespace {

// a band-filling detection's band in the tracker: the capture's inside the detector's
// default edge guards (3 % of the nominal rate each side)
constexpr double FILL_HALF_HZ = 0.47 * NOMINAL_FS_HZ;

double overlap_hz(double c1, double w1, double c2, double w2) {
    const double lo = std::max(c1 - 0.5 * w1, c2 - 0.5 * w2);
    const double hi = std::min(c1 + 0.5 * w1, c2 + 0.5 * w2);
    return std::max(0.0, hi - lo);
}

}  // namespace

const std::vector<Track>& Tracker::update(const std::vector<Scored>& results, double centre_hz, double t_s) {
    expire(t_s);
    for (Track& t : tracks_) t.updated = false;
    struct Pair {
        double score;
        size_t det, trk;
        bool inside;   // a piece of the track's signal (no band / evidence update)
    };
    std::vector<Pair> pairs;
    for (size_t d = 0; d < results.size(); d++) {
        const Detection& det = results[d].det;
        const double f = centre_hz + det.offset_hz, w = std::max(det.bw_hz, 1.0);
        for (size_t t = 0; t < tracks_.size(); t++) {
            const Track& tr = tracks_[t];
            const double tw = std::max(tr.bw_hz, 1.0);
            const double inter = overlap_hz(f, w, tr.centre_hz, tw);
            const Evidence& e = ev_[t];
            const double inter_env = overlap_hz(f, w, 0.5 * (e.lo + e.hi), std::max(e.hi - e.lo, 1.0));
            const double uni = std::max(f + 0.5 * w, tr.centre_hz + 0.5 * tw) - std::min(f - 0.5 * w, tr.centre_hz - 0.5 * tw);
            const double iou = uni > 0.0 ? inter / uni : 0.0;
            const double df = std::fabs(f - tr.centre_hz);
            const double ratio = std::max(w, tw) / std::min(w, tw);
            double score = -1.0;
            bool inside = false;
            if (det.fills_band && tr.fills_band && std::fabs(f - tr.centre_hz) < FILL_HALF_HZ) score = 2.0;   // the band-filling signal again
            else if (iou >= cfg_.min_iou) score = 1.0 + iou;
            else if (df <= cfg_.freq_tol_hz && ratio <= 4.0) score = 1.0 - df / (cfg_.freq_tol_hz + 1.0);
            else if (cfg_.merge_overlap > 0.0 && w < std::max(tw, e.hi - e.lo) && inter_env >= cfg_.merge_overlap * w &&
                     !(tr.fills_band && !det.bursty)) {
                score = 0.5 * inter_env / w;
                inside = true;
            }
            if (score > 0.0) pairs.push_back({score, d, t, inside});
        }
    }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.trk != b.trk) return a.trk < b.trk;
        return a.det < b.det;
    });
    std::vector<int> det_trk(results.size(), -1);
    std::vector<uint8_t> det_inside(results.size(), 0);
    std::vector<uint8_t> trk_used(tracks_.size(), 0);
    for (const Pair& p : pairs) {
        if (det_trk[p.det] >= 0 || trk_used[p.trk]) continue;
        det_trk[p.det] = int(p.trk);
        det_inside[p.det] = p.inside ? 1 : 0;
        trk_used[p.trk] = 1;
    }
    const double a = std::clamp(cfg_.alpha, 0.0, 1.0);
    for (size_t d = 0; d < results.size(); d++) {
        const Scored& s = results[d];
        const double f = centre_hz + s.det.offset_hz;
        if (det_trk[d] >= 0 && det_inside[d]) {
            // a piece of a wider track's signal: it is still there
            Track& tr = tracks_[size_t(det_trk[d])];
            tr.last_s = t_s;
            tr.updated = true;
            continue;
        }
        if (det_trk[d] < 0) {
            Track tr;
            tr.id = next_id_++;
            tr.centre_hz = f;
            tr.bw_hz = s.det.bw_hz;
            tr.first_s = tr.last_s = t_s;
            tracks_.push_back(tr);
            Evidence e;
            e.lo = f - 0.5 * s.det.bw_hz;
            e.hi = f + 0.5 * s.det.bw_hz;
            if (s.det.fills_band) {   // it fills the band: its band is the whole band (inside the guards)
                e.lo = centre_hz - FILL_HALF_HZ;
                e.hi = centre_hz + FILL_HALF_HZ;
            }
            ev_.push_back(e);
            det_trk[d] = int(tracks_.size() - 1);
        } else {
            Track& tr = tracks_[size_t(det_trk[d])];
            Evidence& e = ev_[size_t(det_trk[d])];
            tr.centre_hz += a * (f - tr.centre_hz);
            // the band: as wide as its detections at once (an FSK signal seen as one
            // tone, then both), back to a narrower one slowly
            const double dlo = s.det.fills_band ? centre_hz - FILL_HALF_HZ : f - 0.5 * s.det.bw_hz;
            const double dhi = s.det.fills_band ? centre_hz + FILL_HALF_HZ : f + 0.5 * s.det.bw_hz;
            e.lo = dlo < e.lo ? dlo : e.lo + 0.2 * (dlo - e.lo);
            e.hi = dhi > e.hi ? dhi : e.hi + 0.2 * (dhi - e.hi);
            tr.bw_hz = e.hi - e.lo;
            tr.last_s = t_s;
        }
        Track& tr = tracks_[size_t(det_trk[d])];
        Evidence& ev = ev_[size_t(det_trk[d])];
        tr.hits++;
        tr.updated = true;
        tr.snr_db = s.det.snr_db;
        tr.duty = s.det.duty;
        tr.bursty = s.det.bursty;
        tr.fills_band = s.det.fills_band;
        const int v = std::clamp(s.det.view, 0, NUM_VIEWS - 1);
        ev.view_hits[v]++;
        if (!s.probs.empty()) {
            std::vector<float>& lp = ev.logp[v];
            const bool fresh = lp.size() != s.probs.size();
            if (fresh) lp.assign(s.probs.size(), 0.0f);
            // the mean of the first 1 / alpha looks, then an EWMA with weight alpha: a
            // label rests on ~10 looks (with 0.3 the last three decided - a DMR repeater
            // whose two busy slots read as P25 in a few snapshots flipped the label)
            const double an = std::max(a, 1.0 / double(std::max(1, ev.view_hits[v])));
            for (size_t c = 0; c < s.probs.size(); c++) {
                const float l = float(std::log(std::max(double(s.probs[c]), 1e-6)));
                lp[c] = fresh ? l : float((1.0 - an) * lp[c] + an * l);
            }
        }
    }
    // one signal = one track
    if (cfg_.merge_overlap > 0.0) {
        bool merged = true;
        while (merged) {
            merged = false;
            for (size_t i = 0; i < tracks_.size() && !merged; i++) {
                for (size_t j = 0; j < tracks_.size() && !merged; j++) {
                    if (i == j) continue;
                    const Track& A = tracks_[i];   // candidate to be absorbed
                    const Track& B = tracks_[j];
                    const Evidence& EA = ev_[i];
                    const Evidence& EB = ev_[j];
                    const double wa = std::max(EA.hi - EA.lo, 1.0), wb = std::max(EB.hi - EB.lo, 1.0);
                    if (wa > wb) continue;
                    const double inter = std::max(0.0, std::min(EA.hi, EB.hi) - std::max(EA.lo, EB.lo));
                    // (similar widths: overlapping by half the merge fraction is one channel -
                    // two transmitters a few hundred Hz apart taking turns)
                    if (inter < (wb >= 2.0 * wa ? cfg_.merge_overlap : 0.5 * cfg_.merge_overlap) * wa) continue;
                    // drop = the absorbed one, keep = the one that stays
                    size_t drop = i, keep_i = j;
                    if (wb >= 2.0 * wa) {
                        // ... and only into a wide one that lasts (a key-up click's splatter,
                        // one snapshot long, must not swallow the channel it came from)
                        // (the strong-narrow exemption only for a very wide one: a carrier on a
                        // DAB block, not a VOR's subcarrier lines on its 20 kHz channel)
                        // (a band-filling one takes only bursty or faint pieces: a strong
                        // continuous signal on top of DVB-T that lasts stays its own)
                        if (B.fills_band ? !(A.bursty || A.snr_db < 10.0f || 2 * A.hits < B.hits)
                                         : (wb >= 20.0 * wa && wb >= 200e3 && !(A.bursty || A.snr_db <= B.snr_db + 6.0f))) continue;
                        if (2 * B.hits < A.hits) {
                            // the wide one is the transient (a key-up click's splatter over a
                            // channel that lasts): it goes, if it was a burst
                            if (!B.bursty) continue;
                            drop = j;
                            keep_i = i;
                        }
                    } else if (!(A.hits < B.hits || (A.hits == B.hits && A.id > B.id))) {
                        drop = j;   // similar widths: the one with fewer hits goes
                        keep_i = i;
                    }
                    Track& keep = tracks_[keep_i];
                    const Track& gone = tracks_[drop];
                    if (wb < 2.0 * wa) {   // two transmitters on one channel: the channel's band
                        ev_[keep_i].lo = std::min(EB.lo, EA.lo);
                        ev_[keep_i].hi = std::max(EB.hi, EA.hi);
                        keep.bw_hz = ev_[keep_i].hi - ev_[keep_i].lo;
                    }
                    keep.last_s = std::max(keep.last_s, gone.last_s);
                    keep.first_s = std::min(keep.first_s, gone.first_s);
                    keep.updated = keep.updated || gone.updated;
                    tracks_.erase(tracks_.begin() + long(drop));
                    ev_.erase(ev_.begin() + long(drop));
                    merged = true;
                }
            }
        }
    }
    // the reported view and probabilities
    for (size_t t = 0; t < tracks_.size(); t++) {
        Track& tr = tracks_[t];
        const Evidence& ev = ev_[t];
        tr.lo_hz = ev.lo;
        tr.hi_hz = ev.hi;
        tr.bw_hz = ev.hi - ev.lo;
        int bv = 0;
        for (int v = 1; v < NUM_VIEWS; v++)
            if (ev.view_hits[v] > ev.view_hits[bv]) bv = v;
        tr.view = bv;
        const std::vector<float>& lp = ev.logp[bv];
        tr.probs.clear();
        tr.best = -1;
        tr.best_p = 0.0f;
        if (!lp.empty()) {
            float mx = lp[0];
            for (float l : lp) mx = std::max(mx, l);
            double sum = 0.0;
            tr.probs.resize(lp.size());
            for (size_t c = 0; c < lp.size(); c++) {
                tr.probs[c] = float(std::exp(double(lp[c] - mx)));
                sum += tr.probs[c];
            }
            for (size_t c = 0; c < lp.size(); c++) {
                tr.probs[c] = float(tr.probs[c] / sum);
                if (tr.probs[c] > tr.best_p) {
                    tr.best_p = tr.probs[c];
                    tr.best = int(c);
                }
            }
        }
    }
    return tracks_;
}

}  // namespace sigclass
