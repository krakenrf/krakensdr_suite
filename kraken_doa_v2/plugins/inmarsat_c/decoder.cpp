// Inmarsat-C (STD-C) TDM decoder: the NCS common channel and LES TDM
// channels on the L-band downlink (~1530-1545 MHz) - 1200 symbols/s BPSK,
// rate-1/2 K=7 code, 8.64 s interleaved frames, packets with a 16-bit sum
// check; EGC (SafetyNET / FleetNET) messages reassembled from their packets.
// See README.md (what is decoded, sources, measurements).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "demod.hpp"
#include "frame.hpp"
#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "packets.hpp"

namespace {

constexpr int FS = 9600;   // 8 samples per symbol
constexpr int RING = 2 * inmc::FRAME_SYMS;
constexpr float SYNC_SEARCH = 0.50f;   // unique-word correlation to take a frame when not locked
constexpr float SYNC_LOCKED = 0.25f;   // ... at the expected place, when locked
constexpr int SYNC_WINDOW = 4;         // +- symbols around the expected place
constexpr double PARTIAL_S = 180;      // an EGC message with no new packet for this long is reported as it is
constexpr double KEEP_S = 48 * 3600;   // repeat suppression memory (repeats come up to 24 h apart, twice)

std::string one_line(const std::string& s, size_t max) {
    std::string o;
    for (char c : s) {
        if (c == '\n') {
            if (!o.empty() && o.back() != ' ') o += " / ";
        } else {
            o += c;
        }
        if (o.size() >= max) {
            o += "...";
            break;
        }
    }
    return o;
}

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string fmti(const char* f, unsigned v) {
    char b[128];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

class InmarsatC : public kp::Decoder {
public:
    explicit InmarsatC(kp::Host& h) : Decoder(h), demod_(FS) {
        ring_.assign(RING, 0.0f);
        pos_.assign(RING, 0.0);
        host.table_columns({"Service", "Address", "LES", "Msg", "Packets", "State", "Text"});
        host.table_keep(24 * 3600);
    }

    void reset() override {
        demod_.reset();
        sync_reset();
        egc_.clear();
        last_rf_ok_ = false;
        locked_reported_ = false;
        type_counts_.clear();
        les_tdm_.clear();
        frames_ok_ = frames_bad_ = packets_ok_ = packets_lost_ = 0;
        sym_err_ = sym_tot_ = 0;
        egc_msgs_ = egc_complete_ = egc_repeats_ = 0;
        base_time_ = 0;
        n_total_ = 0;
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "repeats") show_repeats_ = value == "1";
    }

    void process(const kp::cf* x, size_t n) override {
        base_time_ = host.time();
        base_index_ = n_total_;
        syms_.clear();
        const bool was = demod_.tracking();
        demod_.process(x, n, syms_);
        n_total_ += static_cast<int64_t>(n);
        if (was && !demod_.tracking()) {
            host.log("carrier lost - searching again");
            sync_reset();
        }
        for (const inmc::Symbol& s : syms_) push_symbol(s);
        if (!demod_.tracking() && sym_count_ > 0) sync_reset();
        // partial EGC messages whose packets stopped coming
        const double now = host.time();
        if (now - last_housekeeping_ > 10) {
            last_housekeeping_ = now;
            for (inmc::EgcMessage* m : egc_.stale(now, PARTIAL_S)) report_egc(*m, false);
            egc_.prune(now, KEEP_S);
        }
    }

private:
    // ---- symbols -> frames -------------------------------------------------------
    double time_of(double input_index) const { return base_time_ + (input_index - static_cast<double>(base_index_)) / FS; }

    void sync_reset() {
        sym_count_ = 0;
        locked_ = false;
        pending_ = -1;
        misses_ = 0;
    }

    float uw_corr(int64_t s) const {
        float c = 0, e = 0;
        for (int j = 0; j < inmc::ROWS; j++) {
            const int64_t k = s + static_cast<int64_t>(j) * inmc::COLS;
            const float a = ring_[k % RING], b = ring_[(k + 1) % RING];
            c += inmc::uw_sign(j) * (a + b);
            e += a * a + b * b;
        }
        return e > 0 ? c / std::sqrt(128.0f * e) : 0.0f;
    }

    void push_symbol(const inmc::Symbol& s) {
        ring_[sym_count_ % RING] = s.soft;
        pos_[sym_count_ % RING] = s.pos;
        sym_count_++;
        if (sym_count_ < inmc::FRAME_SYMS) return;
        const int64_t start = sym_count_ - inmc::FRAME_SYMS;   // a frame ending with this symbol
        if (!locked_) {
            const float c = uw_corr(start);
            if (std::fabs(c) > SYNC_SEARCH && (pending_ < 0 || std::fabs(c) > std::fabs(pending_c_))) {
                pending_ = start;
                pending_c_ = c;
            }
            if (pending_ >= 0 && start >= pending_ + SYNC_WINDOW) {
                decode_at(pending_, pending_c_);
                locked_ = true;
                expected_ = pending_ + inmc::FRAME_SYMS;
                pending_ = -1;
                misses_ = 0;
            }
            return;
        }
        if (start < expected_ + SYNC_WINDOW) return;
        // the window around the expected start is complete
        int64_t best = expected_;
        float bc = 0;
        for (int64_t s2 = expected_ - SYNC_WINDOW; s2 <= expected_ + SYNC_WINDOW; s2++) {
            if (s2 < 0 || s2 + inmc::FRAME_SYMS > sym_count_ || sym_count_ - s2 > RING) continue;
            const float c = uw_corr(s2);
            if (std::fabs(c) > std::fabs(bc)) bc = c, best = s2;
        }
        if (std::fabs(bc) >= SYNC_LOCKED) {
            decode_at(best, bc);
            expected_ = best + inmc::FRAME_SYMS;
            misses_ = 0;
        } else {
            expected_ += inmc::FRAME_SYMS;
            if (++misses_ >= 2) {
                locked_ = false;
                host.log("unique word lost - searching");
            }
        }
    }

    void decode_at(int64_t start, float corr) {
        std::vector<float> soft(inmc::FRAME_SYMS);
        const float pol = corr < 0 ? -1.0f : 1.0f;
        for (int k = 0; k < inmc::FRAME_SYMS; k++) soft[k] = pol * ring_[(start + k) % RING];
        const inmc::FrameResult fr = inmc::decode_frame(soft.data());
        const double t0 = time_of(pos_[start % RING]) - 0.5 / inmc::SYMBOL_RATE;
        const double t1 = time_of(pos_[(start + inmc::FRAME_SYMS - 1) % RING]) + 0.5 / inmc::SYMBOL_RATE;
        frame(fr, t0, t1, corr);
    }

    // ---- frames -> packets -------------------------------------------------------
    void frame(const inmc::FrameResult& fr, double t0, double t1, float corr) {
        const inmc::WalkResult w = inmc::walk_frame(fr.bytes.data(), inmc::INFO_BYTES);
        if (w.packets.empty()) {
            frames_bad_++;
            host.log("frame at " + fmt("%.1f", t0) + " s: unique word " + fmt("%.2f", corr) +
                     ", no packet passed its sum check");
            update_counters();
            return;
        }
        frames_ok_++;
        sym_err_ += fr.symbol_errors;
        sym_tot_ += inmc::DATA_SYMS;
        if (w.bad) {
            packets_lost_++;
            char b[64];
            std::snprintf(b, sizeof b, "type %02X at byte %d", w.bad_type, w.bad_offset);
            host.log(std::string("sum check failed: ") + b + " - rest of the frame skipped");
        }
        // the Digital squelch / DoA get the frame's samples (in pieces under 3 s)
        const int pieces = 3;
        for (int i = 0; i < pieces; i++)
            host.valid(t0 + (t1 - t0) * i / pieces, t0 + (t1 - t0) * (i + 1) / pieces);
        const double fe = demod_.carrier_hz();
        host.freq_error(static_cast<float>(fe));
        host.fact("Carrier", fmt("%+.1f Hz", fe) + ", Es/N0 " + fmt("%.1f dB", demod_.esn0_db()));
        have_origin_ = false;
        for (const inmc::Packet& p : w.packets) packet(fr.bytes.data() + p.offset, p.len, t0, t1);
        satellite_talker(t0, t1);
        update_counters();
    }

    void update_counters() {
        char b[160];
        std::snprintf(b, sizeof b, "%lld decoded, %lld failed; channel symbol errors corrected %.2f %%",
                      static_cast<long long>(frames_ok_), static_cast<long long>(frames_bad_),
                      sym_tot_ ? 100.0 * sym_err_ / sym_tot_ : 0.0);
        host.fact("Frames", b);
        std::string t;
        for (const auto& [ty, n] : type_counts_) {
            std::snprintf(b, sizeof b, "%s%02X x%lld", t.empty() ? "" : ", ", ty, static_cast<long long>(n));
            t += b;
        }
        std::snprintf(b, sizeof b, "%lld passed their sum check%s", static_cast<long long>(packets_ok_),
                      packets_lost_ ? fmt(", %.0f frame(s) cut short by a failed one", packets_lost_).c_str() : "");
        host.fact("Packets", b);
        if (!t.empty()) host.fact("Packet types", t);
    }

    void packet(const uint8_t* p, int n, double t0, double t1) {
        packets_ok_++;
        type_counts_[p[0]]++;
        if (host.raw_wanted()) host.raw(inmc::hex(p, n));
        switch (p[0]) {
        case 0x7D: bulletin_board(p, n); break;
        case 0x6C: sig_descriptor(p, n); break;
        case 0x81: {
            inmc::LesTdmRef r;
            if (inmc::parse_0x81(p, n, r)) {
                note_les_tdm(r.les, r.tdm_code);
                if (host.verbose()) {
                    char b[160];
                    std::snprintf(b, sizeof b, "81: MES %06X, LES %s, its TDM %s", r.mes, inmc::station_id(r.les).c_str(),
                                  inmc::freq_text(r.tdm_code).c_str());
                    host.event(b, 30);
                }
            }
            break;
        }
        case 0x92: {
            std::vector<inmc::LesListEntry> v;
            if (inmc::parse_0x92(p, n, v))
                for (const auto& e : v) note_les_tdm(e.les, e.tdm_code);
            break;
        }
        case 0xB1:
        case 0xB2: egc_packet(p, n, t0, t1); break;
        default:
            if (host.verbose()) {
                char b[64];
                std::snprintf(b, sizeof b, "packet %02X (%d bytes): ", p[0], n);
                host.event(b + inmc::hex(p, std::min(n, 24)) + (n > 24 ? " ..." : ""), 30);
            }
        }
    }

    void bulletin_board(const uint8_t* p, int n) {
        inmc::BulletinBoard bb;
        if (!inmc::parse_bulletin_board(p, n, bb)) return;
        const bool ncs = inmc::station_is_ncs(bb.origin);
        const std::string who = std::string(ncs ? "NCS common channel, NCS " : "LES TDM, LES ") +
                                inmc::station_id(bb.origin) + " (" + inmc::region_name(inmc::station_region(bb.origin)) + ")";
        host.fact("Channel", who);
        char b[200];
        std::snprintf(b, sizeof b, "%d (%s)", bb.frame_number, inmc::frame_time(bb.frame_number).c_str());
        host.fact("Frame number", b);
        std::snprintf(b, sizeof b, "network version %d, status %02X, services %04X, randomising interval %d, fields %02X %02X %02X",
                      bb.network_version, bb.status, bb.services, bb.random_interval, bb.b4, bb.b5, bb.b6);
        host.fact("Bulletin board", b);
        if (!locked_reported_ || who != channel_) {
            locked_reported_ = true;
            channel_ = who;
            host.event("Receiving " + who + fmt(", carrier %+.0f Hz", demod_.carrier_hz()), 60);
        }
        if (!std::isnan(host.rf_hz())) last_rf_ok_ = true;
        origin_ = bb.origin;
        have_origin_ = true;
    }

    // The RF transmitter of a TDM is the ocean region's satellite (the NCS /
    // LES only feeds it): one talker per ocean region. Each decoded frame
    // gives 8 packets of 0.24 s spread over it (the carrier's own 3 kHz
    // filtered out of the VFO), averaged over 60 s - a fixed transmitter.
    void satellite_talker(double t0, double t1) {
        if (!have_origin_) return;
        const std::string id = std::string("Inmarsat ") + inmc::region_name(inmc::station_region(origin_));
        const std::string label = std::string(inmc::station_is_ncs(origin_) ? "NCS " : "LES ") + inmc::station_id(origin_);
        constexpr int K = 8;
        for (int k = 0; k < K; k++) {
            kp::Talker t;
            t.id = id;
            t.label = label;
            t.packet = true;
            t.start_s = t0 + (t1 - t0) * (k + 0.5) / K - 0.12;
            t.end_s = t.start_s + 0.24;
            t.freq_hz = demod_.carrier_hz();   // where the carrier is in the input (the host AFC moves it to 0)
            t.bw_hz = 3000;
            t.avg_s = 60;
            t.keep_s = 1800;
            host.talker(t);
        }
    }

    void sig_descriptor(const uint8_t* p, int n) {
        inmc::SigChannelDescriptor sd;
        if (!inmc::parse_sig_descriptor(p, n, sd)) return;
        char b[200];
        std::snprintf(b, sizeof b,
                      "uplink channel code %d (%.4f MHz if the uplink raster is 1611.5 MHz + code x 2.5 kHz), flags %02X, "
                      "28 slot markers: %d / %d / %d / %d (states 0/1/2/3)",
                      sd.uplink_code, 1611.5 + sd.uplink_code * 0.0025, sd.flags, sd.slots[0], sd.slots[1], sd.slots[2],
                      sd.slots[3]);
        host.fact("Signalling channel", b);
    }

    void note_les_tdm(uint8_t les, int code) {
        if (les_tdm_[les] == code) return;
        les_tdm_[les] = code;
        std::string t;
        for (const auto& [id, c] : les_tdm_) {
            char b[48];
            std::snprintf(b, sizeof b, "%s%s %.4f", t.empty() ? "" : ", ", inmc::station_id(id).c_str(), inmc::downlink_mhz(c));
            if (t.size() + std::strlen(b) > 390) break;
            t += b;
        }
        host.fact("LES TDMs (MHz)", t);
    }

    // ---- EGC -----------------------------------------------------------------------
    void egc_packet(const uint8_t* p, int n, double t0, double t1) {
        (void)t1;
        inmc::EgcHeader h;
        if (!inmc::parse_egc_header(p, n, h)) {
            egc_bad_header_++;
            host.log("EGC packet with a header that fails its own sum check");
            return;
        }
        bool fresh = false;
        inmc::EgcMessage* m = egc_.add(h, p + h.header_len, n - h.header_len - 2, t0, &fresh);
        if (m->receptions == 1) egc_msgs_++;
        if (!fresh) {
            egc_repeats_++;
            update_egc_fact();
            if (show_repeats_ && m->reported_complete && h.packet_seq == m->last_seq && h.type == 0xB2)
                host.event("EGC repeat: " + summary(*m), 0);
            return;
        }
        update_row(*m);
        if (m->complete() && !m->reported_complete) report_egc(*m, true);
        update_egc_fact();
    }

    std::string summary(const inmc::EgcMessage& m) const {
        const char* name = inmc::egc_service_name(m.service);
        char b[160];
        std::snprintf(b, sizeof b, "%s %s, %s, LES %s, msg %d", inmc::egc_service_family(m.service),
                      name ? name : fmti("service %02X", m.service).c_str(), inmc::egc_address_text(m.service, m.address).c_str(),
                      inmc::station_id(m.les).c_str(), m.msn);
        return b;
    }

    void update_row(const inmc::EgcMessage& m) {
        const char* name = inmc::egc_service_name(m.service);
        int perr = 0;
        const std::string text = m.text(&perr);
        const std::vector<int> miss = m.missing();
        std::string state = m.complete() ? "complete" : (m.last_seq ? "missing packets" : "receiving");
        if (perr) state += fmt(", %.0f parity errors", perr);
        char pk[32];
        std::snprintf(pk, sizeof pk, "%d%s", static_cast<int>(m.have.size()), m.last_seq ? fmt(" / %.0f", m.last_seq).c_str() : "");
        host.table_row(m.key, {name ? name : fmti("%02X", m.service), inmc::egc_address_text(m.service, m.address),
                               inmc::station_id(m.les), std::to_string(m.msn), pk, state,
                               kp::printable(one_line(text, 80))});
        std::vector<std::pair<std::string, std::string>> d;
        d.push_back({"Service", fmti("%02X ", m.service) + (name ? name : "") + " (" + inmc::egc_service_family(m.service) + ")"});
        d.push_back({"Address", inmc::egc_address_text(m.service, m.address)});
        d.push_back({"LES", inmc::station_id(m.les) + " (" + inmc::region_name(inmc::station_region(m.les)) + ")"});
        d.push_back({"Message sequence number", std::to_string(m.msn)});
        d.push_back({"Flags", fmti("%02X (bit 7 = more packets follow; the priority's coding is not decoded)", m.flags)});
        d.push_back({"Presentation", fmti("%02X ", m.presentation) + inmc::presentation_name(m.presentation)});
        std::string ms;
        for (int s : miss) ms += (ms.empty() ? "" : ", ") + std::to_string(s);
        if (!ms.empty()) d.push_back({"Missing packets", ms});
        d.push_back({"Bytes", std::to_string(m.body_bytes())});
        // the text, line by line (<= 400 characters a line, <= ~190 lines)
        size_t a = 0;
        int lines = 0;
        while (a < text.size() && lines < 190) {
            size_t e = text.find('\n', a);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(a, std::min<size_t>(e - a, 400));
            if (!line.empty()) {
                d.push_back({lines ? "" : "Text", kp::printable(line)});
                lines++;
            }
            a = e + 1;
        }
        host.table_detail(m.key, d);
    }

    void report_egc(inmc::EgcMessage& m, bool complete) {
        int perr = 0;
        const std::string text = m.text(&perr);
        std::string ev = (m.service == 0x14 ? "⚠ " : "") + std::string("EGC ") + summary(m);
        char b[96];
        std::snprintf(b, sizeof b, " (%d packets, %d bytes%s%s): ", static_cast<int>(m.have.size()), m.body_bytes(),
                      complete ? "" : ", INCOMPLETE", perr ? fmt(", %.0f parity errors", perr).c_str() : "");
        ev += b;
        ev += kp::printable(one_line(text, 240));
        host.event(ev, 24 * 3600);
        m.reported = true;
        if (complete) {
            m.reported_complete = true;
            egc_complete_++;
        }
        update_row(m);
        update_egc_fact();
    }

    void update_egc_fact() {
        char b[200];
        std::snprintf(b, sizeof b, "%lld messages (%lld complete), %lld repeated packets suppressed%s",
                      static_cast<long long>(egc_msgs_), static_cast<long long>(egc_complete_),
                      static_cast<long long>(egc_repeats_),
                      egc_bad_header_ ? fmt(", %.0f headers failed their sum check", egc_bad_header_).c_str() : "");
        host.fact("EGC", b);
    }

    inmc::BpskDemod demod_;
    std::vector<inmc::Symbol> syms_;
    std::vector<float> ring_;
    std::vector<double> pos_;
    int64_t sym_count_ = 0;
    bool locked_ = false;
    int64_t pending_ = -1, expected_ = 0;
    float pending_c_ = 0;
    int misses_ = 0;
    double base_time_ = 0;
    int64_t base_index_ = 0, n_total_ = 0;
    double last_housekeeping_ = 0;

    inmc::EgcAssembler egc_;
    std::map<uint8_t, long long> type_counts_;
    std::map<uint8_t, int> les_tdm_;
    long long frames_ok_ = 0, frames_bad_ = 0, packets_ok_ = 0, packets_lost_ = 0;
    long long sym_err_ = 0, sym_tot_ = 0;
    long long egc_msgs_ = 0, egc_complete_ = 0, egc_repeats_ = 0, egc_bad_header_ = 0;
    bool show_repeats_ = false;
    bool locked_reported_ = false, last_rf_ok_ = false;
    uint8_t origin_ = 0;
    bool have_origin_ = false;
    std::string channel_;
};

KRAKEN_PLUGIN(InmarsatC, {.id = "inmarsat_c",
                          .name = "Inmarsat-C (STD-C) EGC / TDM",
                          .category = "Marine & distress",
                          .description = "Inmarsat-C NCS common channel and LES TDMs on the L-band downlink (1530-1545 MHz, "
                                         "1200 symbols/s BPSK): bulletin board, network / station identity, packets by type, "
                                         "EGC SafetyNET / FleetNET messages reassembled with repeats suppressed. A patch "
                                         "antenna + LNA (bias tee) is enough; carrier found within +-3.6 kHz",
                          .version = "1.0",
                          .sample_rate = FS,
                          .min_vfo_rate = FS,
                          .author = "KrakenRF",
                          .options = {{"repeats", "Repeated EGC messages", "0", "0=Suppress|1=Show again",
                                       "EGC messages are broadcast several times (minutes to 24 h apart): each one is "
                                       "shown once; Show again adds an event for each repeat"}},
                          .talkers = true,
                          .squelch_hold_s = 9.5})
