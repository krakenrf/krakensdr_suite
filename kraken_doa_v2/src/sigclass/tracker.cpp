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
// apart. A channel (>= min_channel_hz) holding most of a wider track's energy
// (the wider one <= core_energy_ratio x its energy) is that track's core and
// stays its own - the wider one is its skirt, a receiver plateau or a key-up
// transient - and is not a piece of it either (not between two bursts: pieces of
// one pulsed signal); a burst seen once does not take a continuous channel that
// lasts (the transient rule drops it once the channel has twice its looks); a
// faint continuous pedestal (< pedestal_db) does not take a signal >=
// pedestal_contrast_db stronger; a lasting narrow track absorbed into a wide one
// seen for the first time gives it its looks.

#include "sigclass/sigclass.hpp"

#include <algorithm>
#include <cmath>

namespace sigclass {

Tracker::Tracker(const TrackerConfig& cfg) : cfg_(cfg) {}

void Tracker::clear() {
    tracks_.clear();
    ev_.clear();
}

bool Tracker::erase(int id) {
    for (size_t i = 0; i < tracks_.size(); i++)
        if (tracks_[i].id == id) {
            tracks_.erase(tracks_.begin() + long(i));
            ev_.erase(ev_.begin() + long(i));
            return true;
        }
    return false;
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

// a detection's energy over the floor in one snapshot: mean excess x width (the SNR of a
// bursty detection is its level while on: x duty)
double energy_of(const Detection& d) {
    const double snr = std::clamp(double(d.snr_db), -20.0, 120.0);
    return std::pow(10.0, 0.1 * snr) * std::max(d.bw_hz, 1.0) * (d.bursty ? std::clamp(double(d.duty), 0.0, 1.0) : 1.0);
}

}  // namespace

const std::vector<Track>& Tracker::update(const std::vector<Scored>& results, double centre_hz, double t_s) {
    expire(t_s);
    for (Track& t : tracks_) t.updated = false;
    for (Evidence& e : ev_) e.look_snr -= 1.0f;   // (its strongest look, forgotten at 1 dB a snapshot)
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
                     !(tr.fills_band && !det.bursty) &&
                     // (a channel holding most of the wider track's energy is that track's
                     // core, not its piece: it is matched on its own)
                     !(!tr.fills_band && !(det.bursty && tr.bursty) && w >= cfg_.min_channel_hz && 2.0 * w <= std::max(tw, e.hi - e.lo) &&
                       e.energy <= cfg_.core_energy_ratio * energy_of(det))) {
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
            Evidence& pe = ev_[size_t(det_trk[d])];
            pe.look_snr = std::max(pe.look_snr, s.det.snr_db);
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
        ev.energy = energy_of(s.det);
        ev.look_snr = std::max(ev.look_snr, s.det.snr_db);
        ev.det_bw = s.det.bw_hz;
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
                        } else {
                            // a channel holding most of the wide one's energy is its core: the
                            // wide one is the channel's skirt, its receiver's plateau or a key-up
                            // transient around it, not the signal - the channel stays its own
                            // (not when both are bursts: then they are pieces of one pulsed signal
                            // - DME, radar - found at two widths)
                            if (!B.fills_band && !(A.bursty && B.bursty) && EA.det_bw >= cfg_.min_channel_hz &&
                                EB.energy <= cfg_.core_energy_ratio * EA.energy) continue;
                            // a burst seen once does not take a continuous channel that lasts: wait
                            // until it repeats (else the transient rule above drops it later); two
                            // bursts are pieces of one pulsed signal
                            if (!B.fills_band && B.bursty && !A.bursty && B.hits < 2 && A.hits >= 2) continue;
                            // a faint continuous pedestal (under 3 dB of mean excess over its band:
                            // a receiver hump, a noise skirt) is not the band of a strong signal
                            // standing on it (>= 10 dB more): that one stays its own (a modulated
                            // signal's own sidebands around its carrier are far denser)
                            if (!B.fills_band && !B.bursty && B.snr_db < cfg_.pedestal_db &&
                                std::max(A.snr_db, EA.look_snr) >= B.snr_db + cfg_.pedestal_contrast_db) continue;
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
                    } else if (keep_i == j && keep.hits <= 1 && !keep.bursty && !keep.fills_band && gone.hits > keep.hits) {
                        // a lasting narrow track absorbed into a wide one seen for the first
                        // time (the same signal measured wider, a hump over it): its looks
                        // stay - the evidence per view weighted by the looks, its hits
                        Evidence& K = ev_[keep_i];
                        const Evidence& G = ev_[drop];
                        for (int v = 0; v < NUM_VIEWS; v++) {
                            if (G.logp[v].empty()) continue;
                            if (K.logp[v].size() != G.logp[v].size() || K.view_hits[v] <= 0) {
                                K.logp[v] = G.logp[v];
                            } else {
                                const double wk = K.view_hits[v], wg = G.view_hits[v];
                                for (size_t c = 0; c < K.logp[v].size(); c++)
                                    K.logp[v][c] = float((wk * K.logp[v][c] + wg * G.logp[v][c]) / std::max(wk + wg, 1.0));
                            }
                            K.view_hits[v] += G.view_hits[v];
                        }
                        keep.hits = gone.hits;
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
