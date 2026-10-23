#pragma once
// RDS / RBDS group decoding (EN 50067:1998, IEC 62106-2, NRSC-4-B): the
// station's identity and everything its groups carry, per programme
// identification code (PI). GroupDecoder::handle() takes each group the
// BlockSync assembled; data from a corrected block is used only once a
// second copy confirms it (a miscorrected block must not put a wrong
// character / time / frequency on screen).

#include "rds_block.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace rds {

// Text made of segments (PS, RadioText, PTYN, Long PS, eRT, EON PS):
// per character its value and a pending copy (from a corrected block)
class SegText {
public:
    explicit SegText(int n = 8) { resize(n); }
    void resize(int n) {
        if (static_cast<int>(v_.size()) == n) return;
        v_.assign(static_cast<size_t>(n), -1);
        p_.assign(static_cast<size_t>(n), -1);
    }
    void clear() {
        std::fill(v_.begin(), v_.end(), -1);
        std::fill(p_.begin(), p_.end(), -1);
    }
    int size() const { return static_cast<int>(v_.size()); }
    // one character; trusted = from an uncorrected block. true when the
    // position holds c afterwards (set or confirmed)
    bool put(int pos, uint8_t c, bool trusted) {
        if (pos < 0 || pos >= size()) return false;
        const size_t i = static_cast<size_t>(pos);
        if (trusted || v_[i] == c || p_[i] == c) {
            v_[i] = c;
            p_[i] = -1;
            return true;
        }
        p_[i] = c;
        return false;
    }
    int at(int pos) const { return pos >= 0 && pos < size() ? v_[static_cast<size_t>(pos)] : -1; }
    // every character before pos known
    bool known_to(int pos) const {
        for (int i = 0; i < pos && i < size(); i++)
            if (v_[static_cast<size_t>(i)] < 0) return false;
        return true;
    }
    // the position of the first 0x0D (end of text), else -1
    int terminator() const {
        for (int i = 0; i < size(); i++)
            if (v_[static_cast<size_t>(i)] == 0x0D) return i;
        return -1;
    }
    std::vector<uint8_t> bytes(int n) const {
        std::vector<uint8_t> b;
        for (int i = 0; i < n && i < size(); i++) b.push_back(static_cast<uint8_t>(std::max<int16_t>(v_[static_cast<size_t>(i)], 0x20)));
        return b;
    }
private:
    std::vector<int16_t> v_, p_;
};

// RDS character set -> UTF-8 (control codes out, line breaks to spaces)
std::string rds_text(const std::vector<uint8_t>& b);
// UTF-8 / UCS-2 bytes from eRT / Long PS -> printable UTF-8
std::string utf8_text(const std::vector<uint8_t>& b);
std::string ucs2_text(const std::vector<uint8_t>& b);
std::string trim(const std::string& s);

struct RtPlusTag {
    int type = 0;
    std::string text;
};

struct OtherNetwork {   // EON (group 14): another programme of the same broadcaster
    uint16_t pi = 0;
    SegText ps{8};
    std::string ps_str;
    std::set<int> af;   // AF codes
    int pty = -1;
    int tp = -1, ta = -1;
    double last = 0;
};

struct Station {
    uint16_t pi = 0;
    double first = 0, last = 0;
    uint64_t groups = 0, clean_groups = 0;
    uint64_t type_count[32] = {};   // group type * 2 + version
    // basic
    int pty = -1, tp = -1, ta = -1, ms = -1;
    int di = 0, di_known = 0;       // decoder identification bits d0..d3 / which are known
    SegText ps{8};
    unsigned ps_mask = 0;           // segments of the current PS cycle
    std::string ps_str;             // the last complete PS
    std::vector<std::pair<double, std::string>> ps_hist;   // recent PS changes (dynamic PS)
    bool dynamic_ps = false;
    // RadioText (2A 64 / 2B 32 characters)
    SegText rt{64};
    int rt_ab = -1;
    bool rt_b = false;              // version B (32 characters)
    unsigned rt_mask = 0;
    std::string rt_str;
    // PTYN, Long PS (15A), eRT
    SegText ptyn{8};
    int ptyn_ab = -1;
    unsigned ptyn_mask = 0;
    std::string ptyn_str;
    SegText lps{32};
    unsigned lps_mask = 0;
    std::string lps_str;
    SegText ert{128};
    uint32_t ert_mask = 0;
    bool ert_utf8 = true;
    std::string ert_str;
    // AF codes heard (0A block C), LF / MF in kHz
    std::set<int> af;
    std::set<int> af_lfmf;
    // slow labelling codes (1A)
    int ecc = -1, lang = -1, tmc_id = -1, ews_id = -1, la = -1;
    uint16_t pin = 0;
    // clock time (4A)
    std::string ct;                 // "2026-10-22 03:04 UTC"
    std::string ct_local;
    double ct_t = -1e9;
    int64_t ct_minutes = -1;        // UTC minutes since MJD 0, for the confirmation
    // ODAs: group type (number*2 + version) -> AID
    std::map<int, uint16_t> oda;
    std::map<uint16_t, uint16_t> oda_msg;   // AID -> its 3A message bits
    // RT+
    int rtp_toggle = -1, rtp_running = -1;
    std::vector<RtPlusTag> rtp;     // the latest tags with their text
    // TMC
    uint64_t tmc_groups = 0;
    std::string tmc_info;
    // DAB cross-reference
    std::string dab;
    // EON
    std::map<uint16_t, OtherNetwork> eon;
    // other group counts
    uint64_t ews_groups = 0, paging_groups = 0, tdc_groups = 0, ih_groups = 0;
    // pending copies (from corrected blocks) of the single values
    int p_tp = -2, p_pty = -2, p_ta = -2, p_ms = -2, p_ecc = -2, p_lang = -2, p_tmc = -2, p_ews = -2;
    uint16_t p_pin = 0xFFFF;
    int64_t p_ct = -1;
    double p_ct_t = 0;
    bool lfmf_next = false;   // the AF code after 250 is an LF / MF frequency
};

class GroupDecoder {
public:
    // PTY table: 0 = auto (RBDS in the Americas' RBDS countries: from the
    // ECC, else from the receiver's location), 1 = RDS, 2 = RBDS
    void set_pty_mode(int m) { pty_mode_ = m; }
    void set_location(bool have, double lat, double lon) {
        have_loc_ = have;
        lat_ = lat;
        lon_ = lon;
    }
    void reset() {
        st_.clear();
        cur_ = 0;
        cand_ = 0;
        cand_n_ = 0;
        cand_clean_ = false;
        news_.clear();
        last_ours_ = -1e9;
    }

    // one group (t = host time of its end). Returns the PI the group was
    // decoded for (0 = not used: no / an unconfirmed PI)
    uint16_t handle(const Group& g, double t);

    uint16_t current() const { return cur_; }
    const Station* station(uint16_t pi) const {
        auto it = st_.find(pi);
        return it == st_.end() ? nullptr : &it->second;
    }
    const std::map<uint16_t, Station>& stations() const { return st_; }
    bool rbds(const Station& s) const;
    // events since the last call (text, dedup seconds)
    std::vector<std::pair<std::string, double>> take_news() { return std::move(news_); }

    // descriptions
    std::string pty_text(const Station& s, int pty) const;
    std::string country(const Station& s) const;     // "New Zealand" / "" (unknown)
    std::string callsign(const Station& s) const;    // RBDS only
    static std::string pi_hex(uint16_t pi);
    static std::string af_mhz(int code);             // "98.5"
    static std::string area(uint16_t pi);            // RDS coverage area from the PI
    std::string pin_text(uint16_t pin) const;

private:
    std::map<uint16_t, Station> st_;
    uint16_t cur_ = 0, cand_ = 0;
    int cand_n_ = 0;
    bool cand_clean_ = false;
    double last_ours_ = -1e9;
    int pty_mode_ = 0;
    bool have_loc_ = false;
    double lat_ = 0, lon_ = 0;
    std::vector<std::pair<std::string, double>> news_;

    void news(const std::string& s, double dedup = 2.0) { news_.emplace_back(s, dedup); }
    Station& sta(uint16_t pi, double t);

    void basic(Station& s, const Group& g, double t);
    void g0(Station& s, const Group& g, bool ver_b, double t);
    void g1(Station& s, const Group& g, bool ver_b);
    void g2(Station& s, const Group& g, bool ver_b);
    void g3a(Station& s, const Group& g);
    void g4a(Station& s, const Group& g, double t);
    void g10a(Station& s, const Group& g);
    void g14(Station& s, const Group& g, bool ver_b, double t);
    void g15a(Station& s, const Group& g);
    void g15b(Station& s, const Group& g);
    void oda_group(Station& s, const Group& g, uint16_t aid);
    void rtplus(Station& s, const Group& g);
    void ert_group(Station& s, const Group& g);
    void tmc_group(Station& s, const Group& g);
    void dab_group(Station& s, const Group& g);
    void finish_ps(Station& s, double t);
    void finish_rt(Station& s);
};

}  // namespace rds
