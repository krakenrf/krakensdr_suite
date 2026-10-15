// CTCSS / DCS tones (analog FM): the sub-audible tone or digital code a
// transmitter sends under its voice - its ID on a shared channel. Every
// transmission becomes a talker "CTCSS 88.5" / "DCS 023N" over its exact
// span, so each transmitter (repeater, user) gets a DoA of its own; valid
// spans drive the Digital squelch, and the "Tone squelch" option restricts
// both to one tone / code. See README.md; the receiver is rx.cpp.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "kraken_dsp.hpp"
#include "kraken_plugin.hpp"
#include "rx.hpp"

namespace {

using ctcss::RxEvent;

// Talker spans / valid spans are reported this far behind the receiver's
// confirmed point: the end of a tone is only known once it has gone (tone
// off 0.12 s, another transmitter capturing the receiver up to ~0.2 s, a
// DCS code's last bits) - samples after it must not reach this tone's DoA
// (nor, with the Digital squelch, the VFO's). The end itself, found back
// in the stream, is exact
constexpr double CONFIRM_LAG = 0.3;
constexpr double END_GUARD = 0.02;

std::string fmt(const char* f, double v) {
    char b[64];
    snprintf(b, sizeof b, f, v);
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

class CtcssDecoder : public kp::Decoder {
public:
    explicit CtcssDecoder(kp::Host& h) : Decoder(h) {
        rx_.dump = getenv("CTCSS_DUMP") != nullptr;
        host.table_columns({"Tone / code", "Type", "Measured", "Deviation", "Tx", "Last", "Air time", "Ending"});
        host.table_keep(86400);
    }

    void reset() override {
        anchored_ = false;
        for (auto& o : open_) o = Open{};
        // the host closed the open talkers; the table keeps what was heard
        publish_facts(true);
    }

    void option(const std::string& key, const std::string& value) override {
        if (key == "squelch") {
            squelch_.clear();
            std::string v = upper(value);
            for (char& c : v)
                if (c == ',' || c == ';') c = ' ';
            std::stringstream ss(v);
            std::string tok;
            while (ss >> tok) {
                if (tok == "CTCSS" || tok == "DCS" || tok == "DPL" || tok == "PL" || tok == "HZ") continue;
                squelch_.push_back(tok);
            }
            publish_facts(true);
        } else if (key == "min_dev") {
            const double d = atof(value.empty() ? "80" : value.c_str());
            rx_.p.min_dev_hz = static_cast<float>(std::clamp(d, 10.0, 2000.0));
        } else if (key == "nonstd") {
            rx_.p.nonstandard = value != "0";
        }
    }

    void process(const kp::cf* x, size_t n) override {
        if (!anchored_) {
            rx_.reset(host.time());
            anchored_ = true;
        }
        rx_.p.invert = host.rf_inverted();
        rx_.process(x, n);
        for (const RxEvent& e : rx_.events) handle(e);
        rx_.events.clear();
        const double t = host.time();
        if (t - facts_t_ >= 1.0) {
            facts_t_ = t;
            publish_facts(false);
        }
    }

private:
    ctcss::Rx rx_;
    bool anchored_ = false;
    double facts_t_ = -10;
    std::vector<std::string> squelch_;   // tone squelch: tones / codes (normalised tokens), empty = any

    struct Open {
        bool on = false;
        std::string id, label;
        double t0 = 0, t1 = 0, valid_to = 0;
        bool allowed = true;
    };
    Open open_[2];   // [0] CTCSS, [1] DCS
    struct Stat {
        bool dcs = false, standard = true;
        std::string code, other;   // DCS: the equivalent read the other way round (047I for 023N)
        int tx = 0;
        double last_dur = 0, air = 0, last_t = 0, first_t = 0;
        float hz = 0, dev = 0;
        std::string ending;
        bool squelched = false;
        bool on = false;
    };
    std::map<std::string, Stat> stats_;
    std::string last_line_;

    // does the tone squelch let this tone / code through?
    bool allowed(const RxEvent& e) const {
        if (squelch_.empty()) return true;
        for (std::string tok : squelch_) {
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

    std::string label_of(const RxEvent& e) const {
        if (e.dcs) {
            std::string s = e.dcs_name.id();
            if (!e.dcs_name.other_id().empty()) s += " = " + e.dcs_name.other_id();
            if (!e.standard) s += " · non-standard";
            return s;
        }
        return fmt("%.2f Hz", e.hz) + (e.standard ? "" : " · non-standard");
    }

    // valid spans of at most ~2.5 s (the host takes up to 3 s per span)
    void valid_span(double a, double b) {
        while (b - a > 2.5) {
            host.valid(a, a + 2.5);
            a += 2.5;
        }
        if (b > a) host.valid(a, b);
    }

    void handle(const RxEvent& e) {
        Open& o = open_[e.dcs ? 1 : 0];
        switch (e.type) {
        case RxEvent::START: {
            if (o.on) close(o, e.t0, "");
            o = Open{};
            o.on = true;
            o.id = e.id;
            o.label = label_of(e);
            o.t0 = e.t0;
            o.t1 = std::max(e.t0, e.t1);
            o.allowed = allowed(e);
            Stat& s = stats_[e.id];
            const bool fresh = s.tx == 0;
            s.dcs = e.dcs;
            s.standard = e.standard;
            s.code = e.code;
            s.other = e.dcs ? e.dcs_name.other_id() : "";
            s.tx++;
            s.on = true;
            s.squelched = !o.allowed;
            if (fresh) {
                s.first_t = e.t0;
                host.event(std::string("New ") + (e.dcs ? "DCS code " : "CTCSS tone ") + e.code +
                               (e.dcs ? (s.other.empty() ? "" : " (= " + s.other + ")") : " Hz (" + fmt("%.2f Hz", e.hz) + ")") +
                               (e.standard ? "" : ", non-standard") + fmt(", deviation %.0f Hz", e.dev_hz),
                           3600);
            }
            if (s.dev <= 0) s.dev = e.dev_hz;
            if (!e.dcs) s.hz = e.hz;
            if (host.verbose()) host.event(e.id + " on air" + (o.allowed ? "" : " (tone squelch: ignored)"), 0);
            if (o.allowed) {
                // spans CONFIRM_LAG behind what the receiver has seen (below);
                // the squelch opens now all the same
                o.valid_to = std::max(o.t0, o.t1 - CONFIRM_LAG);
                report_talker(o, o.valid_to);
                if (o.valid_to > o.t0) valid_span(o.t0, o.valid_to);
                else host.valid();
            }
            update_row(e.id);
            publish_facts(true);
            break;
        }
        case RxEvent::PROGRESS: {
            if (!o.on || o.id != e.id) break;
            Stat& s = stats_[e.id];
            if (!e.dcs && e.hz > 0) s.hz = e.hz;
            if (e.dev_hz > 0) s.dev = s.dev > 0 ? 0.9f * s.dev + 0.1f * e.dev_hz : e.dev_hz;
            o.label = e.dcs ? o.label : fmt("%.2f Hz", s.hz) + (s.standard ? "" : " · non-standard");
            if (e.t1 <= o.t1) break;
            o.t1 = e.t1;
            if (o.allowed) {
                const double to = o.t1 - CONFIRM_LAG;
                if (to > o.valid_to) {
                    valid_span(o.valid_to, to);
                    o.valid_to = to;
                    report_talker(o, to);
                } else {
                    host.valid();
                }
            }
            break;
        }
        case RxEvent::END: {
            if (!o.on || o.id != e.id) break;
            close(o, std::max(o.t0, e.t1), e.why);
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

    void report_talker(const Open& o, double end) {
        kp::Talker t;
        t.id = o.id;
        t.label = o.label;
        t.start_s = o.t0;
        t.end_s = std::max(o.t0, end);
        host.talker(t);
    }

    void close(Open& o, double t_end, const std::string& why) {
        if (o.allowed) {
            // the end found back in the stream (to a few ms): up to END_GUARD
            // before it - what follows may be another transmitter
            const double te = std::max(o.t0, t_end - END_GUARD);
            if (te > o.valid_to) valid_span(o.valid_to, te);
            report_talker(o, te);
            host.talker_end(te);
        }
        Stat& s = stats_[o.id];
        s.on = false;
        s.last_dur = std::max(0.0, t_end - o.t0);
        s.air += s.last_dur;
        s.last_t = t_end;
        s.ending = why;
        std::string line = o.id + (s.dcs ? "" : " Hz") + " · " + fmt("%.1f s", s.last_dur);
        if (!s.dcs) line += fmt(" · %.2f Hz", s.hz);
        if (!why.empty()) line += " · " + why;
        if (!o.allowed) line += " (tone squelch: ignored)";
        last_line_ = line;
        host.event(line, 0);
        update_row(o.id);
        o.on = false;
        publish_facts(true);
    }

    void update_row(const std::string& id) {
        const Stat& s = stats_[id];
        std::string type = s.dcs ? "DCS" : "CTCSS";
        if (!s.standard) type += " (non-standard)";
        const std::string meas = s.dcs ? s.code + (s.other.empty() ? "" : " = " + s.other) : fmt("%.2f Hz", s.hz);
        host.table_row(id, {s.code + (s.dcs ? "" : " Hz"), type, meas, s.dev > 0 ? fmt("%.0f Hz", s.dev) : "",
                            std::to_string(s.tx), s.on ? "on air" : fmt("%.1f s", s.last_dur), dur_text(s.air),
                            s.on ? "" : s.ending + (s.squelched ? " (squelched)" : "")});
        std::vector<std::pair<std::string, std::string>> d;
        d.push_back({"Signalling", s.dcs ? "DCS (Golay 23,12 at 134.4 bit/s)" : "CTCSS (sub-audible tone)"});
        if (s.dcs) {
            d.push_back({"Code", s.code + (s.code.back() == 'N' ? " (normal polarity)" : " (inverted)")});
            if (!s.other.empty()) d.push_back({"Same signal as", s.other});
        } else {
            d.push_back({"Measured frequency", fmt("%.2f Hz", s.hz)});
        }
        if (!s.standard) d.push_back({"Standard", "not on the standard lists"});
        d.push_back({"Tone deviation", fmt("%.0f Hz", s.dev)});
        d.push_back({"Transmissions", std::to_string(s.tx)});
        d.push_back({"Air time", dur_text(s.air)});
        if (!s.on) d.push_back({"Last transmission", fmt("%.1f s", s.last_dur) + (s.ending.empty() ? "" : ", " + s.ending)});
        if (s.squelched) d.push_back({"Tone squelch", "not the selected tone / code: no talker, no valid frames"});
        host.table_detail(id, d);
    }

    void publish_facts(bool force) {
        (void)force;
        std::string on;
        for (const Open& o : open_)
            if (o.on) {
                const Stat& s = stats_[o.id];
                if (!on.empty()) on += ", ";
                on += o.id + (s.dcs ? (s.other.empty() ? "" : " (= " + s.other + ")") : fmt(" Hz (%.2f Hz)", s.hz)) +
                      fmt(", deviation %.0f Hz", s.dev) + (o.allowed ? "" : " - not the tone squelch's");
            }
        host.fact("On air", on.empty() ? "-" : on);
        host.fact("Last transmission", last_line_);
        std::string heard;
        int n = 0;
        for (const auto& kv : stats_) {
            if (n++ < 10) heard += (heard.empty() ? "" : ", ") + kv.first;
        }
        if (n > 10) heard += ", ...";
        host.fact("Tones / codes heard", n ? std::to_string(n) + ": " + heard : "");
        std::string sq;
        for (const auto& t : squelch_) sq += (sq.empty() ? "" : ", ") + t;
        host.fact("Tone squelch", sq.empty() ? "off (every tone / code)" : sq);
    }
};

}  // namespace

KRAKEN_PLUGIN(CtcssDecoder,
              {.id = "ctcss",
               .name = "CTCSS / DCS tones (analog FM)",
               .description = "The sub-audible tone (CTCSS 67-254 Hz, measured to 0.01 Hz, reverse burst) or digital "
                              "code (DCS: 23-bit Golay at 134.4 bit/s, normal / inverted, turn-off code) under an "
                              "analog FM transmission - each tone / code a talker with its own DoA; the Digital "
                              "squelch opens on it (Tone squelch: only one). A 12-24 kHz VFO on the channel",
               .version = "1.0",
               .sample_rate = ctcss::FS_IN,
               .min_vfo_rate = 12000,
               .author = "KrakenSDR",
               .options = {{"squelch", "Tone squelch", "", "",
                            "Only this CTCSS tone / DCS code (e.g. 88.5, 023N or D023N; several separated by commas; "
                            "empty = any) opens the Digital squelch and gets a talker - the VFO's audio and DoA follow "
                            "only that transmitter. Other tones are still listed"},
                           {"min_dev", "Weakest tone", "80",
                            "40=40 Hz deviation|80=80 Hz deviation|150=150 Hz deviation|250=250 Hz deviation",
                            "The smallest tone deviation taken as a CTCSS tone (transmitters use 300-800 Hz; lower "
                            "finds weak / filtered tones, higher ignores hum)"},
                           {"nonstd", "Non-standard tones", "1", "0=Off|1=On (measured frequency)",
                            "Also report tones off the standard lists, named by their measured frequency"}},
               .talkers = true,
               .squelch_hold_s = 0.35})
