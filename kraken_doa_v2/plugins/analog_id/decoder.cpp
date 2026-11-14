// Analog FM IDs: what a transmitter sends under / in its analog FM voice to
// say who it is - the sub-audible tone or code (CTCSS / DCS: rx.cpp) and the
// in-band signalling bursts (MDC1200, FleetSync, 5-tone selcall, DTMF, tone
// bursts: sig.cpp). Every transmission becomes a talker over its exact span,
// named by the most specific ID heard in it - the radio's own ID (MDC1200
// unit, FleetSync fleet-unit), else a selcall / DTMF sequence, else the tone /
// code, else a tone burst - so each transmitter (repeater, radio) gets a DoA
// of its own; valid spans drive the Digital squelch, and the "ID / tone
// squelch" option restricts both to chosen tones / IDs. See README.md. The
// plugin was "ctcss" (CTCSS / DCS only) until 2026-10-22: saved VFOs load as
// this one (kraken_doa's renamed_plugin).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "fleetsync.hpp"
#include "mdc.hpp"
#include "rx.hpp"
#include "sig.hpp"

namespace {

using asig::SigEvent;
using ctcss::RxEvent;

// Talker spans / valid spans are reported this far behind the receiver's
// confirmed point: the end of a tone is only known once it has gone (tone
// off 0.12 s, another transmitter capturing the receiver up to ~0.2 s, a
// DCS code's last bits) - samples after it must not reach this tone's DoA
// (nor, with the Digital squelch, the VFO's). The end itself, found back
// in the stream, is exact
constexpr double CONFIRM_LAG = 0.3;
constexpr double END_GUARD = 0.02;
// A transmission is named once it is clear which ID it carries: a radio's
// own ID (MDC1200 / FleetSync) names it at once; otherwise it waits this
// long from its start for a pre-ID (a 5-tone / DTMF ANI is decoded ~1-1.5 s
// after the key-up) ...
constexpr double HOLD_SHORT = 1.6;
// ... or, once IDs were heard on the channel (radios with a post-ID: their
// ID comes at the end), until its end, at most this long - kraken_doa keeps
// unclaimed DoA frames 6 s (talker_doa.cpp FRAME_WAIT_S)
constexpr double HOLD_LONG = 4.2;
// An ended transmission takes IDs decoded up to this long after its end
// (a post-ID's decode: DTMF sequences end 0.5 s after their last digit)
constexpr double LATE_ID = 0.8;

std::string fmt(const char* f, double v) {
    char b[96];
    snprintf(b, sizeof b, f, v);
    return b;
}

std::string hex2(unsigned v) {
    char b[8];
    snprintf(b, sizeof b, "%02X", v & 0xFF);
    return b;
}

std::string dur_text(double s) {
    if (s < 60) return fmt("%.1f s", s);
    char b[48];
    if (s < 3600) snprintf(b, sizeof b, "%d min %02d s", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    else snprintf(b, sizeof b, "%d h %02d min", static_cast<int>(s) / 3600, static_cast<int>(s) / 60 % 60);
    return b;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string squeeze(const std::string& s) {   // upper case, single spaces
    std::string o;
    for (char c : upper(s)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!o.empty() && o.back() != ' ') o += ' ';
        } else {
            o += c;
        }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

// talker id kinds, in naming priority
enum Prio { P_NONE = 0, P_BURST = 1, P_TONE = 2, P_SEQ = 3, P_SENDER = 4 };

class AnalogIdDecoder : public kp::Decoder {
public:
    explicit AnalogIdDecoder(kp::Host& h) : Decoder(h) {
        rx_.dump = getenv("CTCSS_DUMP") != nullptr;
        sig_.dump = getenv("ANALOG_ID_DUMP") != nullptr;
        // offline diagnostics: the 12 kHz voice-band audio the signalling
        // decoders get (float32, Hz of deviation)
        if (const char* f = getenv("ANALOG_ID_AUDIO")) audio_out_ = fopen(f, "wb");
        host.table_columns({"ID / tone", "Type", "Details", "With", "Deviation", "Tx", "Last", "Air time", "Ending"});
        host.table_keep(86400);
    }

    void reset() override {
        anchored_ = false;
        cur_ = Tx{};
        ending_.clear();
        // the host closed the open talkers; the table keeps what was heard
        publish_facts();
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "squelch") {
            sq_tones_.clear();
            sq_ids_.clear();
            // entries split at commas / semicolons; an entry naming an ID kind
            // ("MDC 1029", "FleetSync 100-1001", "CCIR 04221", "DTMF 123",
            // "Burst 1750") is an ID, the rest tones / codes (spaces split
            // them too, as before: "88.5 123.0")
            std::string v = value;
            for (char& c : v)
                if (c == ';') c = ',';
            std::stringstream es(v);
            std::string ent;
            while (std::getline(es, ent, ',')) {
                const std::string e = squeeze(ent);
                if (e.empty()) continue;
                const std::string w = e.substr(0, e.find(' '));
                static const std::set<std::string> kinds = {"MDC", "MDC1200", "FLEETSYNC", "FS", "ZVEI", "DZVEI", "CCIR",
                                                            "EEA", "EIA", "NATEL", "MODAT", "DTMF", "BURST"};
                if (kinds.count(w) && e.find(' ') != std::string::npos) {
                    std::string id = e;
                    if (w == "MDC1200") id = "MDC" + e.substr(w.size());
                    if (w == "FS") id = "FLEETSYNC" + e.substr(w.size());
                    sq_ids_.push_back(id);
                    continue;
                }
                std::stringstream ss(e);
                std::string tok;
                while (ss >> tok) {
                    if (tok == "CTCSS" || tok == "DCS" || tok == "DPL" || tok == "PL" || tok == "HZ") continue;
                    sq_tones_.push_back(tok);
                }
            }
            publish_facts();
        } else if (key == "min_dev") {
            const double d = atof(value.empty() ? "80" : value.c_str());
            rx_.p.min_dev_hz = static_cast<float>(std::clamp(d, 10.0, 2000.0));
        } else if (key == "nonstd") {
            rx_.p.nonstandard = value != "0";
        } else if (key == "ids") {
            ids_mode_ = value == "tone" ? 1 : 0;
        } else if (key == "selcall") {
            sig_.sel_family = value == "auto" ? "" : upper(value);
        }
    }

    void process(const kp::cf* x, size_t n) override {
        if (!anchored_) {
            rx_.reset(host.time());
            sig_.reset();
            anchored_ = true;
        }
        rx_.p.invert = host.rf_inverted();
        rx_.process(x, n);
        if (audio_out_) fwrite(rx_.audio.data(), sizeof(float), rx_.audio.size(), audio_out_);
        sig_.process(rx_.audio.data(), rx_.audio.size());
        rx_.audio.clear();
        for (const RxEvent& e : rx_.events) on_rx(e);
        rx_.events.clear();
        for (const SigEvent& e : sig_.events) on_sig(e);
        sig_.events.clear();
        carrier_watch();
        const double t = host.time();
        update(t);
        if (t - facts_t_ >= 1.0) {
            facts_t_ = t;
            publish_facts();
        }
    }

private:
    ctcss::Rx rx_;
    asig::Sig sig_;
    FILE* audio_out_ = nullptr;
    bool anchored_ = false;
    double facts_t_ = -10;
    int ids_mode_ = 0;                    // 0 = name by IDs first, 1 = by the tone / code only
    bool ids_seen_ = false;               // a radio's ID / selcall / DTMF / burst heard since the start
    std::vector<std::string> sq_tones_;   // squelch: tones / codes (normalised tokens)
    std::vector<std::string> sq_ids_;     // squelch: IDs ("MDC 1029"), upper case
    double last_end_ = -1e9;              // where the last transmission ended
    double last_bare_valid_ = -1e9;

    // ---- one transmission ----
    struct Burst {
        double ta, tb;
        std::string id, text;             // its ID (any kind) and what it said
        bool sender;
    };
    struct Tx {
        bool on = false;
        bool tone_driven = false;         // a CTCSS / DCS tracker ends it; else the carrier watch
        double t0 = 0, t1 = 0;            // start, confirmed up to
        // the tone / code
        std::string tone_id, tone_label;
        RxEvent tone_ev;                  // its START (code, standard, dcs name ...)
        bool has_tone = false;
        // IDs heard in it (the first of each kind)
        std::string ids[5], labels[5];    // by Prio
        double id_t[5] = {};
        std::vector<Burst> bursts;
        // what is reported
        std::string named;                // the talker id now ("" = none yet)
        int named_prio = P_NONE;
        double named_from = 0;            // its span start
        double rep_to = 0;                // talker spans reported up to
        double valid_to = 0;              // valid spans reported up to
        int allowed = -1;                 // squelch: 1 yes, 0 no, -1 not decided yet (an ID may still say)
        // carrier watch (transmissions an ID burst found)
        float cref = 0;
        int64_t cbin = 0;
        int clow = 0;
        // ending
        bool ending = false;
        double t_end = 0;
        std::string why;
    };
    Tx cur_;
    std::vector<Tx> ending_;

    struct Stat {
        std::string type, details, with;
        bool tone = false, dcs = false, standard = true;
        std::string code, other;          // tones: "88.5" / DCS: the other-polarity equivalent
        int tx = 0;
        double last_dur = 0, air = 0, last_t = 0;
        float hz = 0, dev = 0;
        std::string ending;
        bool squelched = false, on = false;
        std::set<std::string> seen_with;
        std::vector<std::pair<std::string, std::string>> extra;   // details lines
    };
    std::map<std::string, Stat> stats_;
    std::string last_line_;
    std::map<std::string, int> counts_;   // decoded messages per kind (the facts)

    // ---- squelch ----
    bool tone_allowed(const RxEvent& e) const {
        for (std::string tok : sq_tones_) {
            if (e.dcs) {
                if (tok.size() >= 2 && tok[0] == 'D' && std::isdigit(static_cast<unsigned char>(tok[1]))) tok.erase(0, 1);
                if (tok.size() == 3) tok += 'N';   // "023" = 023N
                if (tok == e.dcs_name.id() || (!e.dcs_name.other_id().empty() && tok == e.dcs_name.other_id()))
                    return true;
            } else {
                if (tok.empty() || !(std::isdigit(static_cast<unsigned char>(tok[0])))) continue;
                char* end = nullptr;
                const double v = strtod(tok.c_str(), &end);
                if (end && *end == '\0' && std::fabs(v - atof(e.code.c_str())) < 0.06) return true;
            }
        }
        return false;
    }
    bool id_allowed(const std::string& id) const {
        const std::string u = squeeze(id);
        for (const auto& s : sq_ids_)
            if (s == u) return true;
        return false;
    }
    // squelch decision for a transmission: 1 / 0, -1 = an ID may still decide
    int decide_allowed(const Tx& t) const {
        if (sq_tones_.empty() && sq_ids_.empty()) return 1;
        if (t.has_tone && tone_allowed(t.tone_ev)) return 1;
        for (int p = P_BURST; p <= P_SENDER; p++)
            if (p != P_TONE && !t.ids[p].empty() && id_allowed(t.ids[p])) return 1;
        if (sq_ids_.empty()) return t.has_tone ? 0 : -1;
        return -1;
    }

    // ---- valid spans of at most ~2.5 s (the host takes up to 3 s per span)
    void valid_span(double a, double b) {
        while (b - a > 2.5) {
            host.valid(a, a + 2.5);
            a += 2.5;
        }
        if (b > a) host.valid(a, b);
    }

    // ---- the receiver's tone / code ----
    void on_rx(const RxEvent& e) {
        switch (e.type) {
        case RxEvent::START: {
            const bool joins = cur_.on && !cur_.tone_driven && !cur_.ending &&
                               (std::fabs(e.t0 - cur_.t0) < 1.0 || (e.t0 >= cur_.t0 && e.t0 <= cur_.t1 + 0.2));
            if (!joins) {
                if (cur_.on) end_tx(cur_, e.t0, cur_.tone_driven ? "" : "another transmitter");
                open_tx(e.t0);
            }
            Tx& t = cur_;
            t.tone_driven = true;
            t.has_tone = true;
            t.tone_ev = e;
            t.tone_id = e.id;
            t.tone_label = tone_label(e);
            t.t0 = std::min(t.t0, e.t0);
            t.t1 = std::max(t.t1, std::max(e.t0, e.t1));
            Stat& s = stats_[e.id];
            const bool fresh = s.tx == 0;
            s.tone = true;
            s.dcs = e.dcs;
            s.standard = e.standard;
            s.code = e.code;
            s.other = e.dcs ? e.dcs_name.other_id() : "";
            s.type = std::string(e.dcs ? "DCS" : "CTCSS") + (e.standard ? "" : " (non-standard)");
            s.on = true;
            if (!e.dcs) s.hz = e.hz;
            if (s.dev <= 0) s.dev = e.dev_hz;
            if (fresh)
                host.event(std::string("New ") + (e.dcs ? "DCS code " : "CTCSS tone ") + e.code +
                               (e.dcs ? (s.other.empty() ? "" : " (= " + s.other + ")") : " Hz (" + fmt("%.2f Hz", e.hz) + ")") +
                               (e.standard ? "" : ", non-standard") + fmt(", deviation %.0f Hz", e.dev_hz),
                           3600);
            if (host.verbose()) host.event(e.id + " on air", 0);
            break;
        }
        case RxEvent::PROGRESS: {
            if (!cur_.on || !cur_.tone_driven || cur_.tone_id != e.id) break;
            Stat& s = stats_[e.id];
            if (!e.dcs && e.hz > 0) s.hz = e.hz;
            if (e.dev_hz > 0) s.dev = s.dev > 0 ? 0.9f * s.dev + 0.1f * e.dev_hz : e.dev_hz;
            if (!e.dcs) cur_.tone_label = fmt("%.2f Hz", s.hz) + (s.standard ? "" : " · non-standard");
            cur_.t1 = std::max(cur_.t1, e.t1);
            break;
        }
        case RxEvent::END: {
            if (!cur_.on || !cur_.tone_driven || cur_.tone_id != e.id) break;
            end_tx(cur_, std::max(cur_.t0, e.t1), e.why);
            break;
        }
        case RxEvent::NOTE: {
            // the DCS turn-off code after the code ended
            auto it = stats_.find(e.id);
            if (it == stats_.end()) break;
            it->second.ending = e.why;
            update_row(e.id);
            host.event(e.id + ": turn-off code", 2);
            break;
        }
        }
    }

    std::string tone_label(const RxEvent& e) const {
        if (e.dcs) {
            std::string s = e.dcs_name.id();
            if (!e.dcs_name.other_id().empty()) s += " = " + e.dcs_name.other_id();
            if (!e.standard) s += " · non-standard";
            return s;
        }
        return fmt("%.2f Hz", e.hz) + (e.standard ? "" : " · non-standard");
    }

    void open_tx(double t0) {
        cur_ = Tx{};
        cur_.on = true;
        cur_.t0 = std::max(t0, last_end_);
        cur_.t1 = cur_.t0;
        cur_.named_from = cur_.rep_to = cur_.valid_to = cur_.t0;
    }

    // a transmission ended at t_end: it waits LATE_ID for an ID decoded late
    void end_tx(Tx& t, double t_end, const std::string& why) {
        t.ending = true;
        t.t_end = std::max(t.t0, t_end);
        t.why = why;
        last_end_ = std::max(last_end_, t.t_end);
        ending_.push_back(t);
        if (&t == &cur_) cur_ = Tx{};
    }

    // ---- the carrier, for transmissions an ID burst found ----
    void carrier_watch() {
        if (!cur_.on || cur_.tone_driven) return;
        while (cur_.cbin < rx_.carrier_bins()) {
            const float v = rx_.carrier_bin(cur_.cbin);
            const double tb = rx_.carrier_time(cur_.cbin);
            cur_.cbin++;
            if (v < 0) continue;
            if (v < 0.4f * cur_.cref) {
                if (++cur_.clow >= 3) {
                    end_tx(cur_, tb - 0.025, "carrier off");
                    return;
                }
            } else {
                cur_.clow = 0;
                cur_.cref += 0.01f * (v - cur_.cref);
                cur_.t1 = std::max(cur_.t1, tb);
            }
        }
    }

    // ---- the signalling bursts ----
    void on_sig(const SigEvent& e) {
        const double ta = rx_.audio_time(e.ka), tb = rx_.audio_time(e.kb);
        std::string id, label, type, text;
        int prio = P_NONE;
        bool sender = false, emergency = false;
        std::string target;
        std::vector<std::pair<std::string, std::string>> det;
        switch (e.kind) {
        case SigEvent::MDC: {
            mdc::Meaning m = mdc::meaning(e.op, e.arg);
            unsigned unit = e.unit;
            if (e.op == 0x35) {
                // a call: unit = the radio called; the second block names the caller
                m.text = e.dbl_ok ? mdc::call_meaning(e.ex[0], e.ex[1]) : "Call (second block lost)";
                target = "MDC " + mdc::hex4(e.unit);
                if (e.dbl_ok) {
                    unit = static_cast<unsigned>((e.ex[2] << 8) | e.ex[3]);
                    m.sender = true;
                }
            } else if (m.target) {
                target = "MDC " + mdc::hex4(e.unit);
            }
            text = m.text;
            emergency = m.emergency;
            type = "MDC1200";
            if (m.sender) {
                id = "MDC " + mdc::hex4(unit);
                sender = true;
                prio = P_SENDER;
            }
            det = {{"Signalling", "MDC1200 (1200 bit/s MSK)"},
                   {"Last message", m.text + " (op " + hex2(e.op) + " arg " + hex2(e.arg) + ")"},
                   {"Unit ID", mdc::hex4(e.unit)}};
            if (e.dbl)
                det.push_back({"Second block", e.dbl_ok ? hex2(e.ex[0]) + " " + hex2(e.ex[1]) + " " + hex2(e.ex[2]) + " " + hex2(e.ex[3])
                                                        : "lost"});
            if (e.fixed > 0) det.push_back({"Bits corrected", std::to_string(e.fixed)});
            if (host.raw_wanted()) {
                char b[96];
                snprintf(b, sizeof b, "MDC1200 op=%02X arg=%02X unit=%04X", e.op, e.arg, e.unit);
                std::string r = b;
                if (e.dbl_ok) {
                    snprintf(b, sizeof b, " ex=%02X%02X%02X%02X", e.ex[0], e.ex[1], e.ex[2], e.ex[3]);
                    r += b;
                }
                host.raw(r);
            }
            counts_["MDC1200"]++;
            break;
        }
        case SigEvent::FLEETSYNC: {
            const fleetsync::Message& m = e.fs;
            const std::string from = fleetsync::unit_name(m.from_fleet(), m.from_unit());
            const std::string to = fleetsync::unit_name(m.to_fleet(), m.to_unit());
            type = std::string("FleetSync") + (m.fs2 ? " II" : "") + fmt(" (%.0f bit/s)", m.baud);
            if (m.ack()) text = "Acknowledge";
            else if (m.gps()) text = "GPS / data";
            else if (m.status()) text = fmt("Status %.0f", m.status_number());
            else if (m.ani()) text = "ANI";
            else if (m.paging()) text = "Page";
            else text = "type " + hex2(m.type());
            if (m.emergency()) text += ", EMERGENCY";
            if (m.lone_worker()) text += ", lone worker";
            if (m.paging() && text != "Page") text += ", paging";
            if (m.ani()) text += m.eot() ? " (end of transmission)" : " (start of transmission)";
            if (m.to_unit() >= 0 && !(m.to_fleet() == m.from_fleet() && m.to_unit() == m.from_unit()))
                target = "FleetSync " + to;
            emergency = m.emergency();
            const std::string pl = m.payload();
            if (!pl.empty()) text += ": \"" + kp::printable(pl) + "\"";
            if (m.from_unit() >= 0) {
                id = "FleetSync " + from;
                sender = true;
                prio = P_SENDER;
            }
            std::string hex;
            for (uint8_t b : m.bytes) hex += hex2(b);
            det = {{"Signalling", type}, {"Last message", text}, {"From", from}, {"To", to},
                   {"Blocks", std::to_string(m.blocks())}, {"Bytes", hex}};
            if (host.raw_wanted()) host.raw(std::string(m.fs2 ? "FleetSync2 " : "FleetSync ") + fmt("%.0f ", m.baud) + hex);
            counts_["FleetSync"]++;
            break;
        }
        case SigEvent::SELCALL: {
            id = e.family + " " + e.digits;
            type = e.std_name;
            text = fmt("%.0f ms tones", e.tone_ms);
            if (!e.alt.empty()) text += " (or " + e.alt + ")";
            prio = P_SEQ;
            det = {{"Signalling", "5-tone selcall " + e.std_name}, {"Sequence", e.digits}, {"Tone length", fmt("%.0f ms", e.tone_ms)}};
            if (!e.alt.empty())
                det.push_back({"Also reads as", e.alt + " (the same tones: set the Selcall standard option to the one in use)"});
            if (host.raw_wanted()) host.raw("SELCALL " + e.std_name + " " + e.digits);
            counts_["Selcall"]++;
            break;
        }
        case SigEvent::DTMF: {
            id = "DTMF " + e.digits.substr(0, 20);
            type = "DTMF";
            text = std::to_string(e.digits.size()) + " digits";
            prio = P_SEQ;
            det = {{"Signalling", "DTMF"}, {"Digits", e.digits}};
            if (host.raw_wanted()) host.raw("DTMF " + e.digits);
            counts_["DTMF"]++;
            break;
        }
        case SigEvent::TONE: {
            const bool burst = e.std_name == "burst";
            text = fmt("%.0f Hz", e.hz) + fmt(" · %.2f s", e.tone_ms / 1000.0);
            if (burst) {
                id = fmt("Burst %.0f", e.hz);
                type = e.hz == 1750 ? "Tone burst (repeater access)" : "Tone burst";
                prio = P_BURST;
                counts_["Tone burst"]++;
            } else {
                // another steady tone (a test tone ...): logged, names nothing
                host.event(fmt("Steady tone %.1f Hz", e.hz) + fmt(" · %.2f s", e.tone_ms / 1000.0), 5);
                return;
            }
            det = {{"Signalling", type}, {"Frequency", fmt("%.0f Hz", e.hz)}, {"Length", fmt("%.2f s", e.tone_ms / 1000.0)}};
            break;
        }
        }
        if (ids_mode_ == 1) prio = P_NONE;   // option: name by the tone / code only
        if (prio != P_NONE) ids_seen_ = true;
        // which transmission: the open one, one that just ended, else a new
        // one the carrier delimits
        Tx* t = nullptr;
        if (cur_.on && ta >= cur_.t0 - 0.3 && ta <= cur_.t1 + 1.5) t = &cur_;
        for (auto& x : ending_)
            if (!t && ta >= x.t0 - 0.3 && ta <= x.t_end + 0.3) t = &x;
        // another radio's ID in the same carrier (a repeater's output, a capture
        // without a tone change): its own transmission from here
        if (t && prio == P_SENDER && !t->ids[P_SENDER].empty() && t->ids[P_SENDER] != id &&
            ta > t->id_t[P_SENDER] + 0.5 && t == &cur_) {
            end_tx(cur_, ta - 0.05, "another radio");
            t = nullptr;
        }
        if (!t && prio != P_NONE) {
            // found by its ID: from where the carrier came up (if it did, in
            // the 82 s kept) to where it goes away
            const double rise = rx_.carrier_rise(ta, std::max(last_end_, ta - 60.0));
            if (cur_.on) end_tx(cur_, std::isnan(rise) ? ta - 0.05 : rise, "another transmitter");
            open_tx(std::isnan(rise) ? ta - 0.05 : rise);
            cur_.tone_driven = false;
            cur_.cref = rx_.carrier_mean(ta, tb);
            if (!(cur_.cref > 0)) cur_.cref = rx_.carrier_mean(ta - 0.2, tb);
            cur_.cbin = std::max<int64_t>(0, static_cast<int64_t>(std::floor((tb - rx_.carrier_time(0)) / 0.01)));
            cur_.t1 = tb;
            t = &cur_;
        }
        std::string tone_with = t && t->has_tone ? t->tone_id : "";
        if (t) {
            if (prio != P_NONE && t->ids[prio].empty()) {
                t->ids[prio] = id;
                t->labels[prio] = text;
                t->id_t[prio] = ta;
            }
            t->bursts.push_back({ta, tb, id.empty() ? target : id, text, sender});
            t->t1 = std::max(t->t1, tb);
        }
        // the event line
        std::string where;
        if (t && ta - t->t0 < 1.5) where = " (at key-up)";
        std::string line =
            (id.empty() ? (target.empty() ? type : type + " → " + target.substr(target.find(' ') + 1)) : id) + ": " + text + where;
        if (!id.empty() && !target.empty()) line += " → " + target.substr(target.find(' ') + 1);
        if (!tone_with.empty()) line += " · " + tone_with;
        if (emergency) host.event("⚠ EMERGENCY " + line, 60);
        else host.event(line, host.verbose() ? 0.0 : 2.0);
        // the table row of the ID (and of a target-only message's target)
        const std::string key = !id.empty() ? id : target;
        if (!key.empty()) {
            Stat& s = stats_[key];
            const bool fresh = s.type.empty() && s.tx == 0;
            s.type = type;
            s.details = text + where;
            if (!tone_with.empty()) s.seen_with.insert(tone_with);
            if (e.dev_hz > 0) s.dev = e.dev_hz;
            s.extra = det;
            if (id.empty()) s.extra.push_back({"Note", "addressed by another station (a console / another radio sent this)"});
            s.last_t = tb;
            if (t == &cur_ && !id.empty()) s.on = true;
            if (fresh && !id.empty()) {
                const std::string what = e.kind == SigEvent::MDC         ? "MDC1200 unit " + id.substr(4)
                                         : e.kind == SigEvent::FLEETSYNC ? "FleetSync radio " + id.substr(10)
                                         : e.kind == SigEvent::SELCALL   ? "selcall " + type + " " + e.digits
                                         : e.kind == SigEvent::DTMF      ? "DTMF sequence " + e.digits
                                                                         : "tone burst " + text;
                host.event("New " + what + (tone_with.empty() ? "" : " · " + tone_with), 3600);
            }
            update_row(key);
        }
    }

    // ---- naming, talkers and valid spans ----
    int best_prio(const Tx& t) const {
        for (int p = P_SENDER; p >= P_BURST; p--)
            if (p == P_TONE ? t.has_tone : !t.ids[p].empty()) return p;
        return P_NONE;
    }
    std::string name_of(const Tx& t, int p) const { return p == P_TONE ? t.tone_id : t.ids[p]; }
    std::string label_of(const Tx& t, int p) const {
        std::string l = p == P_TONE ? t.tone_label : t.labels[p];
        if (p != P_TONE && t.has_tone) l += " · " + t.tone_id;
        return l.substr(0, 60);
    }

    void report_talker(const Tx& t, double end) {
        kp::Talker k;
        k.id = t.named;
        k.label = label_of(t, t.named_prio);
        k.start_s = t.named_from;
        k.end_s = std::max(t.named_from, end);
        host.talker(k);
    }

    // name / rename the transmission; report its talker span and valid spans
    // up to `to`
    void advance(Tx& t, double to, bool final) {
        const int bp = best_prio(t);
        const double age = (final ? t.t_end : t.t1) - t.t0;
        const double hold = ids_seen_ ? HOLD_LONG : HOLD_SHORT;
        if (t.allowed < 0) t.allowed = decide_allowed(t);
        if (final && t.allowed < 0) t.allowed = 0;
        // named after the hold; once named, only a radio's own ID renames it
        // (a post-ID after the hold: the spans not reported yet go to it - a
        // selcall / DTMF sequence that late is not worth splitting it for)
        const bool can = t.named.empty() ? (bp == P_SENDER || final || age >= hold) : bp == P_SENDER;
        if (bp != P_NONE && bp > t.named_prio && can) {
            if (!t.named.empty()) t.named_from = t.rep_to;
            t.named = name_of(t, bp);
            t.named_prio = bp;
        }
        if (t.allowed == 1 && to > t.valid_to) {
            valid_span(t.valid_to, to);
            t.valid_to = to;
        } else if (t.allowed == 1 && !final && t.t1 - last_bare_valid_ >= 0.05) {
            // nothing confirmed yet (CONFIRM_LAG): the squelch opens now all the same
            host.valid();
            last_bare_valid_ = t.t1;
        }
        if (t.allowed == 1 && !t.named.empty() && (to > t.rep_to || final)) {
            report_talker(t, to);
            t.rep_to = std::max(t.rep_to, to);
        }
    }

    void update(double now) {
        // ended transmissions: final once LATE_ID has passed (or a newer one
        // must report: talkers come in time order)
        const bool cur_named = cur_.on && best_prio(cur_) != P_NONE &&
                               (best_prio(cur_) == P_SENDER || (cur_.t1 - cur_.t0) >= (ids_seen_ ? HOLD_LONG : HOLD_SHORT));
        for (auto it = ending_.begin(); it != ending_.end();) {
            if (now < it->t_end + LATE_ID && !cur_named && it + 1 == ending_.end()) {
                ++it;
                continue;
            }
            finish(*it);
            it = ending_.erase(it);
        }
        if (cur_.on) advance(cur_, std::max(cur_.t0, cur_.t1 - CONFIRM_LAG), false);
    }

    void finish(Tx& t) {
        const double te = std::max(t.t0, t.t_end - END_GUARD);
        advance(t, te, true);
        if (t.allowed == 1 && !t.named.empty()) host.talker_end(te);
        const double dur = std::max(0.0, t.t_end - t.t0);
        // every ID / tone it carried: its row
        std::vector<std::string> keys;
        if (t.has_tone) keys.push_back(t.tone_id);
        for (int p = P_BURST; p <= P_SENDER; p++)
            if (p != P_TONE && !t.ids[p].empty()) keys.push_back(t.ids[p]);
        std::string post;
        for (const auto& b : t.bursts)
            if (b.sender && t.t_end - b.tb < 0.8 && b.ta - t.t0 >= 1.5) post = " · ID at release";
        for (const auto& k : keys) {
            Stat& s = stats_[k];
            s.tx++;
            s.on = false;
            s.last_dur = dur;
            s.air += dur;
            s.last_t = t.t_end;
            s.ending = t.why;
            s.squelched = t.allowed != 1;
            for (const auto& o : keys)
                if (o != k) s.seen_with.insert(o);
            update_row(k);
        }
        if (keys.empty()) return;
        std::string line = (t.named.empty() ? keys.front() : t.named) + " · " + fmt("%.1f s", dur);
        for (const auto& k : keys)
            if (k != t.named) line += " · " + k;
        if (t.has_tone && !t.tone_ev.dcs) line += fmt(" (%.2f Hz)", stats_[t.tone_id].hz);
        line += post;
        if (!t.why.empty()) line += " · " + t.why;
        if (t.allowed != 1) line += " (squelch: ignored)";
        last_line_ = line;
        host.event(line, 0);
        publish_facts();
    }

    void update_row(const std::string& id) {
        Stat& s = stats_[id];
        std::string type = s.type.empty() ? "?" : s.type;
        std::string details = s.details;
        if (s.tone) details = s.dcs ? s.code + (s.other.empty() ? "" : " = " + s.other) : fmt("%.2f Hz", s.hz);
        std::string with;
        int n = 0;
        for (const auto& w : s.seen_with) {
            if (n++ < 3) with += (with.empty() ? "" : ", ") + w;
        }
        if (n > 3) with += fmt(" +%.0f", n - 3);
        const std::string shown = s.tone && !s.dcs ? s.code + " Hz" : id;
        host.table_row(id, {s.tone ? (s.dcs ? "DCS " + s.code : "CTCSS " + shown) : id, type, details, with,
                            s.dev > 0 ? fmt("%.0f Hz", s.dev) : "", std::to_string(s.tx),
                            s.on ? "on air" : (s.tx ? fmt("%.1f s", s.last_dur) : ""), s.tx ? dur_text(s.air) : "",
                            s.on ? "" : s.ending + (s.squelched ? " (squelched)" : "")});
        std::vector<std::pair<std::string, std::string>> d;
        if (s.tone) {
            d.push_back({"Signalling", s.dcs ? "DCS (Golay 23,12 at 134.4 bit/s)" : "CTCSS (sub-audible tone)"});
            if (s.dcs) {
                d.push_back({"Code", s.code + (s.code.back() == 'N' ? " (normal polarity)" : " (inverted)")});
                if (!s.other.empty()) d.push_back({"Same signal as", s.other});
            } else {
                d.push_back({"Measured frequency", fmt("%.2f Hz", s.hz)});
            }
            if (!s.standard) d.push_back({"Standard", "not on the standard lists"});
            d.push_back({"Tone deviation", fmt("%.0f Hz", s.dev)});
        } else {
            d = s.extra;
        }
        if (!with.empty()) d.push_back({"Heard with", with});
        d.push_back({"Transmissions", std::to_string(s.tx)});
        d.push_back({"Air time", dur_text(s.air)});
        if (s.tx && !s.on) d.push_back({"Last transmission", fmt("%.1f s", s.last_dur) + (s.ending.empty() ? "" : ", " + s.ending)});
        if (s.squelched) d.push_back({"Squelch", "not a selected tone / ID: no talker, no valid frames"});
        host.table_detail(id, d);
    }

    void publish_facts() {
        std::string on;
        if (cur_.on) {
            const int bp = best_prio(cur_);
            if (bp != P_NONE) {
                on = name_of(cur_, bp);
                for (int p = P_SENDER; p >= P_BURST; p--)
                    if (p != bp && (p == P_TONE ? cur_.has_tone : !cur_.ids[p].empty())) on += " · " + name_of(cur_, p);
                if (cur_.allowed == 0) on += " - not the squelch's";
            }
        }
        host.fact("On air", on.empty() ? "-" : on);
        host.fact("Last transmission", last_line_);
        std::string heard;
        int n = 0;
        for (const auto& kv : stats_) {
            if (kv.second.tx == 0 && kv.second.tone) continue;
            if (n++ < 10) heard += (heard.empty() ? "" : ", ") + kv.first;
        }
        if (n > 10) heard += ", ...";
        host.fact("IDs / tones heard", n ? std::to_string(n) + ": " + heard : "");
        std::string c;
        for (const auto& kv : counts_) c += (c.empty() ? "" : " · ") + kv.first + " " + std::to_string(kv.second);
        host.fact("Signalling decoded", c.empty() ? "-" : c);
        std::string sq;
        for (const auto& t : sq_tones_) sq += (sq.empty() ? "" : ", ") + t;
        for (const auto& t : sq_ids_) sq += (sq.empty() ? "" : ", ") + t;
        host.fact("ID / tone squelch", sq.empty() ? "off (every tone / ID)" : sq);
    }
};

}  // namespace

KRAKEN_PLUGIN(AnalogIdDecoder,
              {.id = "analog_id",
               .name = "Analog FM IDs (CTCSS / DCS, MDC1200, FleetSync, 5-tone, DTMF, 1750 Hz)",
               .category = "Two-way radio",
               .description = "Who is transmitting on an analog FM channel: the sub-audible tone (CTCSS, measured to "
                              "0.01 Hz, reverse burst) or code (DCS), and the radios' ID bursts - MDC1200 unit IDs, "
                              "FleetSync fleet-unit IDs (1200 / 2400 bit/s), 5-tone selcall (ZVEI, CCIR, EEA, EIA ...), "
                              "DTMF, tone bursts (1750 Hz). Each transmission a talker named by the radio's ID (else its "
                              "tone) with its own DoA; the Digital squelch opens on it (ID / tone squelch: only "
                              "chosen ones). A 12-24 kHz VFO on the channel",
               .version = "2.0",
               .sample_rate = ctcss::FS_IN,
               .min_vfo_rate = 12000,
               .author = "KrakenSDR",
               .options = {{"squelch", "ID / tone squelch", "", "",
                            "Only these CTCSS tones / DCS codes / radio IDs (e.g. 88.5, 023N, MDC 1029, FleetSync "
                            "100-1001, CCIR 12345; several separated by commas; empty = any) open the Digital squelch and "
                            "get a talker - the VFO's audio and DoA follow only those transmitters. Others are still "
                            "listed"},
                           {"ids", "Name transmissions by", "all",
                            "all=Radio IDs first (MDC1200, FleetSync, selcall, DTMF), then the tone|tone=The tone / code "
                            "only",
                            "Which ID a transmission's talker (its own DoA) is named by: the radio's own ID when one "
                            "was decoded, or always the CTCSS tone / DCS code (all users of a tone share one talker)"},
                           {"selcall", "Selcall standard", "auto",
                            "auto=Any (the common reading)|ZVEI=ZVEI-1 / ZVEI-2 / PZVEI|DZVEI=DZVEI / ZVEI-3 / "
                            "PDZVEI|CCIR=CCIR-1 / CCIR-2 / PCCIR|EEA=EEA|EIA=EIA|NATEL=NATEL|MODAT=MODAT",
                            "The 5-tone standard in use here: some sequences read two ways (DZVEI's digits are "
                            "ZVEI's tones moved up one - ZVEI 33344 = DZVEI 41451); auto takes the more common "
                            "standard and lists the other reading"},
                           {"min_dev", "Weakest tone", "80",
                            "40=40 Hz deviation|80=80 Hz deviation|150=150 Hz deviation|250=250 Hz deviation",
                            "The smallest tone deviation taken as a CTCSS tone (transmitters use 300-800 Hz; lower "
                            "finds weak / filtered tones, higher ignores hum)"},
                           {"nonstd", "Non-standard tones", "1", "0=Off|1=On (measured frequency)",
                            "Also report tones off the standard lists, named by their measured frequency"}},
               .talkers = true,
               .squelch_hold_s = 0.35})
