// The audio layer of one DAB sub-channel (see dab_audio.hpp).
//
// DAB (ETSI TS 103 466): one MPEG-1 Layer II frame (48 kHz) per logical
// frame, or one MPEG-2 LSF frame (24 kHz) per two (clause 5.5.2, annex B.1:
// the frame starts in the even logical frame). A frame counts as ok when its
// header is valid for the sub-channel bit rate, its header CRC (annex B.2)
// matches and the ScF-CRC carried at the end of the previous frame (annex
// B.3) matches its scale factors. PAD at the end of every frame (clause 5.5),
// PCM through minimp3.
//
// DAB+ (ETSI TS 102 563): five logical frames make one audio super frame;
// the super frame start is found by the Fire code (clause 5.2) - checked on
// the received bytes and, failing that, after Reed-Solomon correction - then
// RS(120, 110) over the virtual interleaver (clause 6), the super frame
// header, each access unit's CRC, the PAD from the AU's leading
// data_stream_element (clause 5.4), PCM through the user's AAC library
// (audio_aaclib.hpp).

#include "dab_audio.hpp"
#include "audio_aaclib.hpp"
#include "audio_bits.hpp"
#include "audio_fec.hpp"
#include "audio_mp2.hpp"
#include "audio_mpa.hpp"
#include "audio_out.hpp"
#include "audio_pad.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dab {

using namespace audio;

namespace {

constexpr int DAB_SYNC_HOLD = 8;        // audio frames without a good one before "not synced"
constexpr int DABP_SYNC_HOLD = 3;       // super frames whose header fails before re-synchronising
constexpr int SCF_UNTRUSTED_RUN = 25;   // ScF-CRC mismatches in a row (header CRC ok): encoder writes none
constexpr int64_t LOG_GAP_FRAMES = 125; // sync messages at most every 3 s

const char* mode_name(int mode) {
    switch (mode) {
    case 0: return "stereo";
    case 1: return "joint stereo";
    case 2: return "dual channel";
    default: return "mono";
    }
}

// AudioSpecificConfig (ISO/IEC 14496-3 1.6.2.1) of a DAB+ stream: AAC-LC
// with a 960-sample frame (GASpecificConfig frameLengthFlag), SBR / PS
// signalled explicitly and hierarchically (audioObjectType 5 / 29 with the
// extension sampling frequency, then AAC-LC)
size_t make_asc(bool dac48, bool sbr, bool stereo, bool ps, uint8_t* out) {
    auto sfi = [](int rate) { return rate == 48000 ? 3 : rate == 32000 ? 5 : rate == 24000 ? 6 : 8; };
    const int out_rate = dac48 ? 48000 : 32000;
    const int core = sbr ? out_rate / 2 : out_rate;
    uint32_t bits = 0;
    int nb = 0;
    auto put = [&](uint32_t v, int n) { bits = (bits << n) | v; nb += n; };
    if (sbr) {
        put(ps ? 29 : 5, 5);
        put(static_cast<uint32_t>(sfi(core)), 4);
        put(stereo ? 2 : 1, 4);
        put(static_cast<uint32_t>(sfi(out_rate)), 4);
        put(2, 5);
    } else {
        put(2, 5);
        put(static_cast<uint32_t>(sfi(core)), 4);
        put(stereo ? 2 : 1, 4);
    }
    put(1, 1);   // frameLengthFlag: 960
    put(0, 1);   // dependsOnCoreCoder
    put(0, 1);   // extensionFlag
    const int nbytes = (nb + 7) / 8;
    bits <<= (nbytes * 8 - nb);
    for (int i = 0; i < nbytes; i++) out[i] = static_cast<uint8_t>(bits >> (8 * (nbytes - 1 - i)));
    return static_cast<size_t>(nbytes);
}

}   // namespace

struct SubchannelDecoder::Impl : PadDecoder::Sink {
    AudioSink& sink;
    SubchannelConfig cfg;
    AudioStatus st;
    bool valid = false;      // cfg usable
    size_t nbytes = 0;       // logical frame size
    int64_t lf = 0;          // logical frames seen (incl. lost)
    PadDecoder pad{*this};
    Pcm8k out;
    bool audio_on = false;   // PCM was produced for the previous frame (want_audio)
    int64_t last_sync_log = -1000000;
    std::vector<float> mono; // downmix buffer

    // --- DAB (Layer II) ---
    std::unique_ptr<MpaDecoder> mpa;
    std::vector<float> mpa_pcm;
    std::vector<uint8_t> lsf;      // the first half of a 24 kHz frame
    bool lsf_have = false;
    int64_t lsf_phase = -1;        // logical frame parity of the first halves
    int miss = DAB_SYNC_HOLD;      // audio frames since the last good one
    bool prev_scf_valid = false;   // the ScF-CRC carried by the previous frame applies
    uint8_t prev_scf[4] = {};
    int prev_groups = 0;
    int scf_run = 0;
    bool scf_trusted = true;
    Mp2Header hdr_shown;
    bool hdr_known = false;

    // --- DAB+ ---
    int s = 0;                     // subchannel_index
    std::vector<uint8_t> win;      // the last five logical frames
    unsigned lost_mask = 0;        // bit k: window frame k (0 = oldest) was lost
    int win_fill = 0;
    std::vector<uint8_t> sf;       // super frame being decoded
    uint8_t cw[Rs120::N];
    bool sf_synced = false;
    int64_t sf_phase = 0;
    int sf_miss = 0;
    int last_num_aus = 0;
    int params = -1;               // dac_rate | sbr << 1 | chmode << 2 | ps << 3 | mps << 4
    aac::Decoder aacdec;
    bool aac_tried = false;        // open attempted for `params`
    int au_ms = 0;                 // audio per AU in ms

    explicit Impl(AudioSink& sk) : sink(sk), out(sk) {}

    void label(const std::string& t) override { sink.dynamic_label(t); }

    void log_sync(const char* what) {
        if (lf - last_sync_log < LOG_GAP_FRAMES) return;
        last_sync_log = lf;
        sink.log(what);
    }

    void setup() {
        valid = false;
        nbytes = 0;
        st.can_decode = false;
        st.decoder.clear();
        if (cfg.bitrate_kbps <= 0 || cfg.bitrate_kbps > 384) return;
        nbytes = static_cast<size_t>(cfg.bitrate_kbps) * 3;
        if (cfg.dabplus) {
            if (cfg.bitrate_kbps % 8) return;
            s = cfg.bitrate_kbps / 8;
            win.assign(nbytes * 5, 0);
            sf.assign(nbytes * 5, 0);
            st.decoder = aac::library_status();
            st.can_decode = aac::library_available();
            mono.assign(aac::Decoder::PCM_CAPACITY, 0.0f);
        } else {
            if (!mpa) mpa = std::make_unique<MpaDecoder>();
            lsf.assign(nbytes * 2, 0);
            mpa_pcm.assign(MpaDecoder::MAX_SAMPLES, 0.0f);
            mono.assign(MpaDecoder::MAX_SAMPLES, 0.0f);
            st.decoder = "built in";
            st.can_decode = true;
        }
        valid = true;
    }

    void clear_state() {
        lf = 0;
        pad.reset();
        out.reset();
        audio_on = false;
        st.synced = false;
        st.codec.clear();
        // DAB
        if (mpa) mpa->reset();
        lsf_have = false;
        lsf_phase = -1;
        miss = DAB_SYNC_HOLD;
        prev_scf_valid = false;
        scf_run = 0;
        scf_trusted = true;
        hdr_known = false;
        // DAB+
        std::fill(win.begin(), win.end(), 0);
        lost_mask = 0;
        win_fill = 0;
        sf_synced = false;
        sf_miss = 0;
        last_num_aus = 0;
        params = -1;
        aacdec.close();
        aac_tried = false;
        au_ms = 0;
    }

    // want_audio handling shared by both codecs: (re)start cleanly
    void audio_state(bool want) {
        if (want && !audio_on) {
            out.reset();
            if (mpa) mpa->reset();
            if (aacdec.is_open()) { aacdec.close(); aac_tried = false; }
        }
        if (!want && audio_on) out.reset();
        audio_on = want;
    }

    // ------------------------------------------------------------- DAB --
    void dab_bad(int ms) {
        if (miss < DAB_SYNC_HOLD) {
            st.frames_bad++;
            if (audio_on) out.gap(ms);
            if (++miss == DAB_SYNC_HOLD) {
                st.synced = false;
                out.reset();
                log_sync("DAB audio: frame sync lost");
            }
        }
        pad.lost();
    }

    int dab_frame(const uint8_t* fr, size_t n, const Mp2Header& h) {
        Mp2SideInfo si;
        parse_mp2_side_info(fr, n, h, si);
        const bool crc_ok = (!si.crc_present || si.crc_ok) && !si.overrun;
        bool scf_ok = true;
        if (crc_ok && prev_scf_valid && prev_groups == si.groups) {
            bool match = std::memcmp(prev_scf, si.scf_crc, static_cast<size_t>(si.groups)) == 0;
            if (match) {
                if (!scf_trusted) sink.log("DAB audio: ScF-CRC present again, checking it");
                scf_trusted = true;
                scf_run = 0;
            } else if (scf_trusted && ++scf_run >= SCF_UNTRUSTED_RUN) {
                scf_trusted = false;
                sink.log("DAB audio: the ScF-CRC never matches - the encoder writes none; not checking it");
            }
            scf_ok = match || !scf_trusted;
        }
        const bool good = crc_ok && scf_ok;
        const int ms = h.mpeg1 ? 24 : 48;
        // the ScF-CRC this frame carries for the next one (its trailer is
        // usable whenever the header and side information are)
        prev_scf_valid = crc_ok;
        if (crc_ok) {
            mp2_carried_scf_crc(fr, n, si.groups, prev_scf);
            prev_groups = si.groups;
        }
        if (!good) {
            dab_bad(ms);
            return 0;
        }
        if (miss >= DAB_SYNC_HOLD) log_sync("DAB audio: frame sync");
        miss = 0;
        st.synced = true;
        st.frames_ok++;
        // (encoders switch between stereo and joint stereo frame by frame:
        // the text says joint stereo once it has been seen)
        const bool st_js = hdr_known && hdr_shown.mode == 1 && h.mode == 0;
        if (!hdr_known || (h.mode != hdr_shown.mode && !st_js) || h.mpeg1 != hdr_shown.mpeg1 || h.bitrate_kbps != hdr_shown.bitrate_kbps) {
            hdr_known = true;
            hdr_shown = h;
            st.codec = std::string(h.mpeg1 ? "MPEG-1 Layer II, 48 kHz, " : "MPEG-2 Layer II, 24 kHz, ") + mode_name(h.mode) +
                       ", " + std::to_string(h.bitrate_kbps) + " kbit/s";
            sink.log("DAB audio: " + st.codec);
        }
        // PAD: F-PAD = the last two bytes, the X-PAD in front of the ScF-CRC
        const size_t tail = 2 + static_cast<size_t>(h.scf_crcs);
        const size_t head = h.protection ? 6 : 4;
        if (n > tail + head) pad.feed(fr + n - 2, fr + head, n - tail - head, false);
        if (audio_on) {
            int ch = 0, rate = 0;
            int ns = mpa->decode(fr, n, mpa_pcm.data(), &ch, &rate);
            if (ns <= 0) {
                out.gap(ms);
            } else {
                const float* p = mpa_pcm.data();
                if (ch == 1) {
                    out.push(p, static_cast<size_t>(ns), rate);
                } else {
                    // dual channel: the first programme; else (L + R) / 2
                    const bool dual = h.mode == 2;
                    for (int i = 0; i < ns; i++) mono[static_cast<size_t>(i)] = dual ? p[2 * i] : 0.5f * (p[2 * i] + p[2 * i + 1]);
                    out.push(mono.data(), static_cast<size_t>(ns), rate);
                }
            }
        }
        return 1;
    }

    int dab_push(const uint8_t* d) {
        Mp2Header h;
        const bool hdr = parse_mp2_header(d, h) && h.bitrate_kbps == cfg.bitrate_kbps;
        const int64_t me = lf++;
        if (lsf_have) {                      // second half of a 24 kHz frame
            lsf_have = false;
            std::memcpy(lsf.data() + nbytes, d, nbytes);
            Mp2Header h0;
            parse_mp2_header(lsf.data(), h0);
            return dab_frame(lsf.data(), nbytes * 2, h0);
        }
        if (hdr && h.mpeg1) return dab_frame(d, nbytes, h);
        if (hdr && !h.mpeg1) {               // first half of a 24 kHz frame
            std::memcpy(lsf.data(), d, nbytes);
            lsf_have = true;
            lsf_phase = me & 1;
            return 0;
        }
        // no frame starts here: bad when one was expected (48 kHz: every
        // logical frame; 24 kHz: every other one)
        prev_scf_valid = false;
        if (!hdr_known || hdr_shown.mpeg1 || (me & 1) == lsf_phase) dab_bad(hdr_known && !hdr_shown.mpeg1 ? 48 : 24);
        return 0;
    }

    void dab_lost() {
        const int64_t me = lf++;
        prev_scf_valid = false;
        if (lsf_have) {
            lsf_have = false;
            dab_bad(48);
            return;
        }
        if (!hdr_known || hdr_shown.mpeg1 || (me & 1) == lsf_phase) dab_bad(hdr_known && !hdr_shown.mpeg1 ? 48 : 24);
    }

    // ------------------------------------------------------------ DAB+ --
    bool fire_ok(const uint8_t* a) const {
        return fire_code(a + 2, 9) == ((static_cast<uint16_t>(a[0]) << 8) | a[1]);
    }

    // RS-correct sf in place over the virtual interleaver (code word i =
    // bytes i, i + s, i + 2s, ...); counts the corrected / uncorrectable words
    void rs_correct(int& corrected, int& failed) {
        corrected = failed = 0;
        for (int i = 0; i < s; i++) {
            for (int j = 0; j < Rs120::N; j++) cw[j] = sf[static_cast<size_t>(i + j * s)];
            int r = Rs120::decode(cw);
            if (r > 0) {
                for (int j = 0; j < Rs120::N; j++) sf[static_cast<size_t>(i + j * s)] = cw[j];
                corrected++;
            } else if (r < 0) {
                failed++;
            }
        }
    }
    void rs_count(int corrected, int failed) {
        st.rs_corrected += static_cast<uint64_t>(corrected);
        st.rs_failed += static_cast<uint64_t>(failed);
    }

    // a super frame whose header could not be used
    void sf_bad() {
        if (!sf_synced) return;
        st.frames_bad += static_cast<uint64_t>(last_num_aus);
        pad.lost();
        if (audio_on) {
            for (int k = 0; k < last_num_aus; k++) conceal_au();
        }
        if (++sf_miss >= DABP_SYNC_HOLD) {
            sf_synced = false;
            st.synced = false;
            out.reset();
            log_sync("DAB+ audio: super frame sync lost");
        }
    }

    void conceal_au() {
        if (aacdec.is_open()) {
            int ns = aacdec.decode(nullptr, 0);
            if (ns > 0) { aac_pcm(ns); return; }
        }
        out.gap(au_ms > 0 ? au_ms : 40);
    }

    void aac_pcm(int ns) {
        const int ch = aacdec.channels();
        const int16_t* p = aacdec.pcm();
        constexpr float k = 1.0f / 32768.0f;
        if (ch == 1) {
            for (int i = 0; i < ns; i++) mono[static_cast<size_t>(i)] = p[i] * k;
        } else {
            for (int i = 0; i < ns; i++) mono[static_cast<size_t>(i)] = 0.5f * k * (p[i * ch] + p[i * ch + 1]);
        }
        if (!out.push(mono.data(), static_cast<size_t>(ns), aacdec.sample_rate())) out.gap(au_ms);
    }

    void set_params(int p) {
        if (p == params) return;
        params = p;
        aacdec.close();
        aac_tried = false;
        const bool dac48 = p & 1, sbr = p & 2, stereo = p & 4, ps = (p & 8) && sbr && !stereo;
        const int mps = (p >> 4) & 7;
        std::string c;
        if (sbr && ps) c = "HE-AAC v2 (SBR + PS)";
        else if (sbr) c = "HE-AAC (SBR)";
        else c = "AAC-LC";
        c += dac48 ? ", 48 kHz, " : ", 32 kHz, ";
        c += (stereo || ps) ? "stereo" : "mono";
        if (mps) c += ", MPEG Surround (core decoded)";
        st.codec = c;
        sink.log("DAB+ audio: " + c);
    }

    void open_aac() {
        if (aac_tried) return;
        aac_tried = true;
        if (!aac::library_available()) return;
        uint8_t asc[8];
        const int p = params;
        size_t n = make_asc(p & 1, p & 2, p & 4, (p & 8) && (p & 2) && !(p & 4), asc);
        // a mono core (mono, or parametric stereo): the library's mono output,
        // which skips the PS synthesis; a stereo core is downmixed here -
        // the library's own stereo -> mono downmix is L + R and clips
        if (!aacdec.open(asc, n, (p & 4) ? 2 : 1)) sink.log("DAB+ audio: the AAC library refused the stream configuration");
    }

    int process_sf(bool already_corrected) {
        if (!already_corrected) {
            std::memcpy(sf.data(), win.data(), sf.size());
            int c, f;
            rs_correct(c, f);
            rs_count(c, f);
        }
        const uint8_t* a = sf.data();
        if (!fire_ok(a)) { sf_bad(); return 0; }
        const int dac = (a[2] >> 6) & 1, sbr = (a[2] >> 5) & 1, chm = (a[2] >> 4) & 1, ps = (a[2] >> 3) & 1, mps = a[2] & 7;
        const int num_aus = dac ? (sbr ? 3 : 6) : (sbr ? 2 : 4);
        static constexpr int FIRST[7] = {0, 0, 5, 6, 8, 0, 11};
        const int size = s * 110;
        int start[7];
        start[0] = FIRST[num_aus];
        {
            BitReader br(a + 3, 8);
            for (int k = 1; k < num_aus; k++) start[k] = static_cast<int>(br.get(12));
        }
        start[num_aus] = size;
        for (int k = 0; k < num_aus; k++) {
            if (start[k + 1] - start[k] < 3) {   // an AU of at least one byte + its CRC
                sf_bad();
                return 0;
            }
        }
        sf_miss = 0;
        if (!sf_synced) log_sync("DAB+ audio: super frame sync");
        sf_synced = true;
        st.synced = true;
        last_num_aus = num_aus;
        au_ms = 120 / num_aus;
        set_params(dac | sbr << 1 | chm << 2 | ps << 3 | mps << 4);
        if (audio_on) open_aac();
        const size_t fb = nbytes;
        int good = 0;
        for (int k = 0; k < num_aus; k++) {
            const size_t b = static_cast<size_t>(start[k]), e = static_cast<size_t>(start[k + 1]);
            const size_t len = e - b - 2;
            bool ok = crc16_ccitt(a + b, len) == ((static_cast<uint16_t>(a[e - 2]) << 8) | a[e - 1]);
            // never trust an AU that overlaps a lost logical frame (zeros)
            for (size_t f = b / fb; ok && f <= (e - 1) / fb && f < 5; f++)
                if (lost_mask & (1u << f)) ok = false;
            if (!ok) {
                st.frames_bad++;
                pad.lost();
                if (audio_on) conceal_au();
                continue;
            }
            st.frames_ok++;
            good++;
            const uint8_t* au = a + b;
            // PAD: a leading data_stream_element (id_syn_ele 4)
            if (len >= 2 && (au[0] >> 5) == 4) {
                size_t cnt = au[1], off = 2;
                if (cnt == 255) {
                    if (len >= 3) { cnt += au[2]; off = 3; }
                    else cnt = 0;
                }
                if (cnt >= 2 && off + cnt <= len) pad.feed(au + off + cnt - 2, au + off, cnt - 2, true);
            }
            if (audio_on) {
                int ns = aacdec.is_open() ? aacdec.decode(au, len) : 0;
                if (ns > 0) aac_pcm(ns);
                else if (aacdec.is_open()) conceal_au();
            }
        }
        return good;
    }

    int dabp_push(const uint8_t* d, bool lost) {
        const size_t n = nbytes;
        std::memmove(win.data(), win.data() + n, n * 4);
        if (lost) std::memset(win.data() + n * 4, 0, n);
        else std::memcpy(win.data() + n * 4, d, n);
        lost_mask = (lost_mask >> 1) | (lost ? 16u : 0u);
        const int64_t me = lf++;
        if (win_fill < 5) win_fill++;
        if (win_fill < 5) return 0;
        const int64_t first = me - 4;   // logical frame index of the window's first frame
        if (sf_synced) {
            if ((first - sf_phase) % 5 != 0) return 0;
            return process_sf(false);
        }
        if (lost_mask) return 0;
        // search: the Fire code on the received bytes, else after RS correction
        // (a false match on garbage then fails the header checks or the AU CRCs)
        sf_phase = first;
        sf_miss = 0;
        if (fire_ok(win.data())) return process_sf(false);
        std::memcpy(sf.data(), win.data(), sf.size());
        int c, f;
        rs_correct(c, f);
        if (!fire_ok(sf.data())) return 0;
        rs_count(c, f);
        return process_sf(true);
    }
};

SubchannelDecoder::SubchannelDecoder(AudioSink& sink) : impl_(std::make_unique<Impl>(sink)) {}
SubchannelDecoder::~SubchannelDecoder() = default;

void SubchannelDecoder::configure(const SubchannelConfig& cfg) {
    Impl& m = *impl_;
    if (cfg == m.cfg && m.valid) return;
    m.cfg = cfg;
    m.st = AudioStatus{};
    m.setup();
    m.clear_state();
}

void SubchannelDecoder::reset() {
    impl_->clear_state();
}

int SubchannelDecoder::push_frame(const uint8_t* data, size_t n, bool want_audio) {
    Impl& m = *impl_;
    if (!m.valid || !data || n != m.nbytes) return 0;
    m.audio_state(want_audio && m.st.can_decode);
    return m.cfg.dabplus ? m.dabp_push(data, false) : m.dab_push(data);
}

void SubchannelDecoder::push_lost() {
    Impl& m = *impl_;
    if (!m.valid) return;
    if (m.cfg.dabplus) m.dabp_push(nullptr, true);
    else m.dab_lost();
}

const AudioStatus& SubchannelDecoder::status() const { return impl_->st; }

std::string aac_library_status(bool* ok) {
    if (ok) *ok = aac::library_available();
    return aac::library_status();
}

}   // namespace dab
