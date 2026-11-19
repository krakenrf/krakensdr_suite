#pragma once
// Programme Associated Data of DAB and DAB+ audio (ETSI EN 300 401 clause
// 7.4): F-PAD, short and variable size X-PAD with contents indicators, X-PAD
// data groups, and the dynamic label (clause 7.4.5.2, ETSI TS 102 980) with
// the character sets of ETSI TS 101 756 table 1 converted to UTF-8.
// Other X-PAD applications (MOT slideshow, DL Plus tags, data group length
// indicators, user applications) are recognised and skipped.

#include <cstddef>
#include <cstdint>
#include <string>

namespace dab::audio {

// The EBU Latin based repertoire (TS 101 756 annex C) code -> Unicode; 0 for
// the reserved codes 0x00, 0x0A, 0x0B, 0x1F (control codes in a label)
extern const uint16_t EBU_LATIN[256];

// A dynamic label (character field bytes of all segments, charset of the
// first segment) as UTF-8. Control codes: 0x0A (preferred line break) and
// 0x0B (end of headline) become a space, 0x1F (preferred word break) and
// other control characters are dropped; leading / trailing spaces trimmed.
std::string dl_to_utf8(const uint8_t* d, size_t n, int charset);

class PadDecoder {
public:
    struct Sink {
        virtual ~Sink() = default;
        // a complete label that differs from the last one ("" after a clear)
        virtual void label(const std::string& utf8) = 0;
    };
    explicit PadDecoder(Sink& s) : sink_(s) {}
    void reset();

    // One PAD field. fpad: its two F-PAD bytes (byte L-1, byte L). xpad: the
    // bytes in front of the F-PAD in transmission order, xpad_avail of them,
    // ending right before the F-PAD (DAB+) or the ScF-CRC (DAB): the X-PAD
    // field occupies the LAST bytes of that range (reversed byte order).
    // exact_len: the X-PAD field length is known (DAB+: the data stream
    // element's length - 2); otherwise it follows from the contents indicators.
    void feed(const uint8_t fpad[2], const uint8_t* xpad, size_t xpad_avail, bool exact_len);
    // the PAD of an audio frame / AU that was lost: breaks continuations
    void lost();

    const std::string& current_label() const { return label_; }

private:
    void subfield(int app, const uint8_t* d, size_t n);
    void dls_group(const uint8_t* d, size_t n);

    Sink& sink_;
    // X-PAD state
    int last_app_ = -1;          // application type of the last data sub-field (-1: none)
    size_t last_len_ = 0;        // length of the last X-PAD field (variable size)
    // DLS X-PAD data group being assembled
    uint8_t dg_[2 + 16 + 2] = {};
    size_t dg_len_ = 0;
    bool dg_active_ = false;
    // DLS message segments
    uint8_t seg_[8][16] = {};
    uint8_t seg_len_[8] = {};
    unsigned seg_have_ = 0;      // bit i: segment i received
    int seg_total_ = 0;          // number of segments, once the last one is seen
    int toggle_ = -1;
    int suppress_toggle_ = -1;   // the message a "clear display" command removed
    int charset_ = 0;
    std::string label_;
};

}   // namespace dab::audio
