#pragma once
// Inmarsat-C TDM packets: framing, checksum, the packet types whose layout
// is known (published facts + measurement on a real NCS common channel), and
// EGC message reassembly. No Host calls here - decoder.cpp reports.
//
// Packet framing (measured; every packet of the recording passes):
//   type byte T: T < 0x80       -> length = (T & 0x0F) + 1 (type = T >> 4)
//                0x80 <= T < 0xC0 -> length = next byte + 2
//                T >= 0xC0       -> length = next two bytes + 3 (unverified:
//                                   no such packet seen; the checksum guards it)
//   the last two bytes are a 16-bit sum check: a Fletcher sum modulo 256
//   over the whole packet, both running sums zero.
//   A zero type byte = idle fill to the end of the frame.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace inmc {

bool fletcher_ok(const uint8_t* p, int n);
// The two check bytes that make p[0..n-1] + {c0, c1} sum to zero (generator / tests)
void fletcher_make(const uint8_t* p, int n, uint8_t* c0, uint8_t* c1);
int packet_length(const uint8_t* p, int avail);   // -1 = not computable

struct Packet {
    int offset = 0, len = 0;
    uint8_t type = 0;
};
struct WalkResult {
    std::vector<Packet> packets;   // all passed their checksum
    bool bad = false;              // a packet failed: the rest of the frame was skipped
    uint8_t bad_type = 0;
    int bad_offset = -1;
    int idle_from = -1;            // first idle byte
};
WalkResult walk_frame(const uint8_t* info, int n = 639);

// Station identity byte: 2-bit ocean region + 6-bit number; NCS x44..x63,
// LES x00..x43 (SDM Vol. 3 Ch. 3 7).
inline int station_region(uint8_t id) { return id >> 6; }
inline int station_number(uint8_t id) { return id & 63; }
inline bool station_is_ncs(uint8_t id) { return (id & 63) >= 44; }
std::string station_id(uint8_t id);           // "144"
const char* region_name(int region);         // "AOR-E"
// Channel code -> downlink frequency: 1510 MHz + code x 2.5 kHz (fits the
// published NCS channels: 11080 = 1537.700 AOR-W, 12580 = 1541.450 AOR-E / POR,
// 10840 = 1537.100 IOR)
inline double downlink_mhz(int code) { return 1510.0 + code * 0.0025; }
std::string freq_text(int code);              // "11080 (1537.700 MHz)"

struct BulletinBoard {
    int network_version = 0;
    int frame_number = 0;          // 0..9999 frames since 00:00 UTC (8.64 s each)
    uint8_t b4 = 0, b5 = 0, b6 = 0;   // signalling-channel count / channel-type fields (raw)
    uint8_t origin = 0;            // the NCS / LES whose TDM this is
    uint8_t status = 0;
    uint16_t services = 0;
    uint8_t random_interval = 0;
};
bool parse_bulletin_board(const uint8_t* p, int n, BulletinBoard& bb);
std::string frame_time(int frame_number);    // "14:22:07 UTC"

struct SigChannelDescriptor {
    uint8_t flags = 0;
    int uplink_code = 0;           // the signalling channel's frequency code
    int slots[4] = {0, 0, 0, 0};   // 28 slot-state markers, counted by value 0..3
};
bool parse_sig_descriptor(const uint8_t* p, int n, SigChannelDescriptor& sd);

// Type 0x81 (measured): MES id (3 bytes), LES id, that LES's TDM channel code,
// a 16-bit number that counts up per LES; the rest not decoded.
struct LesTdmRef {
    uint32_t mes = 0;
    uint8_t les = 0;
    int tdm_code = 0;
    int counter = 0;
};
bool parse_0x81(const uint8_t* p, int n, LesTdmRef& r);
// Station lists (measured on the AOR-E 2008 recording and the POR NCS off air
// in 2026, every entry checked by tuning to it): type 0x92 = type, length,
// an MES id (3 bytes), 2 bytes, network version, entry count; type 0xAB
// (broadcast) = type, length, network version, entry count. Then `count`
// 6-byte entries {station id (an LES or an NCS), 3 status bytes, TDM channel
// code}. The network version equals the bulletin board's.
struct LesListEntry {
    uint8_t les = 0;
    uint32_t status = 0;
    int tdm_code = 0;
};
bool parse_station_list(const uint8_t* p, int n, std::vector<LesListEntry>& out, int* network_version);

// ---- EGC -------------------------------------------------------------------------
// Header (measured on SafetyNET packets; field order as published for the
// EGC header): type, length, service code, flags (bit 7 = C: more packets
// follow; bits 6-5 = priority 0 routine .. 3 distress - measured off air:
// a shore-to-ship distress relay 3, a MET forecast 1; bits 4-0 not decoded),
// message sequence number (16
// bits), packet sequence number (from 1), presentation, LES id, address
// (as many bytes as the service code's low hex digit), then a sum check of
// its own over all of that (Fletcher mod 256, like a packet's).
struct EgcHeader {
    uint8_t type = 0;
    uint8_t service = 0;
    uint8_t flags = 0;
    bool more = false;             // C bit
    int priority = 0;              // 0 routine, 1 safety, 2 urgency, 3 distress
    int low_bits = 0;              // bits 4-0 of the flags byte (not decoded)
    int msn = 0;
    int packet_seq = 0;
    uint8_t presentation = 0;
    uint8_t les = 0;
    std::vector<uint8_t> address;
    int header_len = 0;            // including the header sum check
    bool address_rule = true;      // address length = the service code's low digit
};
// p = a whole packet that passed its checksum. false: not an EGC header.
bool parse_egc_header(const uint8_t* p, int n, EgcHeader& h);
bool is_egc_type(uint8_t t);       // 0xB1 / 0xB2 (double header parts 1 / 2)
const char* egc_service_name(uint8_t service);
const char* egc_service_family(uint8_t service);   // "SafetyNET", "FleetNET", "System"
std::string egc_address_text(uint8_t service, const std::vector<uint8_t>& a);
const char* presentation_name(uint8_t p);
const char* priority_name(int p);
// Continued packets: a packet that does not fit the frame is sent as type BD
// (its first part) at the end of the frame and BE (the rest) at the start of
// the next one, each with a sum check of its own; the parts' payloads joined
// are the original packet WITHOUT its last two bytes (its sum check) -
// measured off air on 17 of 17 (EGC B2 halves, a station list).

struct EgcMessage {
    std::string key;               // "<LES>-<service hex>-<msn>"
    uint8_t les = 0, service = 0, presentation = 0, flags = 0;
    int msn = 0;
    std::vector<uint8_t> address;
    // parts[seq][0 = header-1 / single, 1 = header-2] = body bytes
    std::map<int, std::vector<std::vector<uint8_t>>> parts;
    std::map<int, std::vector<bool>> have;
    std::map<int, bool> double_header;
    int last_seq = 0;              // the packet with C = 0 (0 = not seen yet)
    double first_t = 0, last_t = 0;
    int receptions = 0;            // packets received (repeats included)
    int repeats = 0;               // packets that were already there
    bool reported = false;         // the event went out
    bool reported_complete = false;
    bool complete() const;
    std::vector<int> missing() const;   // sequence numbers not (fully) received
    int body_bytes() const;
    // Text in the message's presentation; parity errors -> '_' (IEC 61097-4
    // 5.4.10), missing packets -> "[...]". *errors = characters with bad parity
    std::string text(int* errors = nullptr) const;
};

class EgcAssembler {
public:
    // Adds one packet; returns the message, *fresh = the part was new.
    EgcMessage* add(const EgcHeader& h, const uint8_t* body, int blen, double t, bool* fresh);
    // Messages whose last part arrived more than timeout_s ago and that were
    // never reported (partial ones), oldest first.
    std::vector<EgcMessage*> stale(double now, double timeout_s);
    // Forget messages older than keep_s (repeat suppression memory)
    void prune(double now, double keep_s);
    size_t size() const { return msgs_.size(); }
    void clear() { msgs_.clear(); }
private:
    std::map<std::string, EgcMessage> msgs_;
};

std::string ia5_text(const std::vector<uint8_t>& b, int* errors);
std::string ita2_text(const std::vector<uint8_t>& b);
std::string hex(const uint8_t* p, int n);

}  // namespace inmc
