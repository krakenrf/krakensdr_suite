#pragma once

// UAT message contents (RTCA DO-282B layouts, as publicly described):
// ADS-B payloads (header, state vector, mode status, auxiliary state vector)
// and ground uplink payloads (station header, information frames, FIS-B
// APDU headers, DLAC text).

#include <cstdint>
#include <string>
#include <vector>

namespace uat {

// address qualifier (3 bits of the header)
enum Aq { AQ_ADSB_ICAO = 0, AQ_ADSB_SELF = 1, AQ_TISB_ICAO = 2, AQ_TISB_TRACK = 3, AQ_VEHICLE = 4, AQ_BEACON = 5,
          AQ_ADSR = 6, AQ_RESERVED = 7 };
const char* aq_name(int aq);
// sent by the aircraft / vehicle / beacon itself (not a ground station's rebroadcast)
inline bool aq_self(int aq) { return aq == AQ_ADSB_ICAO || aq == AQ_ADSB_SELF || aq == AQ_VEHICLE || aq == AQ_BEACON; }
inline bool aq_tisb(int aq) { return aq == AQ_TISB_ICAO || aq == AQ_TISB_TRACK; }

struct AdsbMsg {
    int type = 0, aq = 0;
    uint32_t addr = 0;
    // state vector
    bool pos_valid = false;
    double lat = 0, lon = 0;
    int alt_kind = 0;            // 0 none, 1 barometric, 2 geometric
    int alt_ft = 0;
    int nic = 0;
    int ag = 0;                  // 0 airborne subsonic, 1 supersonic, 2 on ground, 3 reserved
    bool ns_ok = false, ew_ok = false;
    int ns_kt = 0, ew_kt = 0;
    bool speed_ok = false;
    int speed_kt = 0;
    int track_kind = 0;          // 0 none, 1 true track, 2 magnetic heading, 3 true heading
    double track = 0;
    int vr_kind = 0;             // 0 none, 1 barometric, 2 geometric
    int vrate_fpm = 0;
    bool dims = false;
    double length_m = 0, width_m = 0;
    bool utc = false;
    int site = 0;                // TIS-B site ID (TIS-B messages)
    // mode status
    bool has_ms = false;
    int cat = 0;                 // emitter category 0..39
    std::string callsign;        // trimmed; the flight plan ID (squawk) when !cs_callsign
    bool cs_callsign = true;
    int emergency = 0, version = 0, sil = 0, mso = 0, nacp = 0, nacv = 0, nic_baro = 0;
    bool cdti = false, acas = false, acas_ra = false, ident = false, atc = false, hdg_magnetic = false;
    // auxiliary state vector
    bool has_aux = false;
    int sec_alt_kind = 0, sec_alt_ft = 0;
};
// f = 18 (type 0) or 34 byte payload after FEC. false = not a valid layout
bool decode_adsb(const uint8_t* f, int len, AdsbMsg& m);
std::string category_code(int cat);         // "A1", "B4" ... ("" for 0)
const char* category_name(int cat);
const char* emergency_name(int e);

struct FisbApdu {
    int product = -1;
    bool a = false, g = false, p = false, s = false;
    int t_opt = 0, month = -1, day = -1, hour = -1, minute = -1, second = -1;
    std::vector<uint8_t> data;   // the product's payload
};
struct InfoFrame {
    int type = 0;                // 0 = FIS-B APDU
    std::vector<uint8_t> data;
    bool is_fisb = false;
    FisbApdu fisb;
};
struct UplinkMsg {
    bool pos_valid = false;
    double lat = 0, lon = 0;
    bool utc = false, app_valid = false;
    int slot = 0, site = 0;
    std::vector<InfoFrame> frames;
};
// f = 432 byte payload after FEC
bool decode_uplink(const uint8_t* f, UplinkMsg& m);
const char* product_name(int id);
bool product_is_text(int id);
// one line with every field (UAT_DUMP=1 offline: comparisons with other decoders)
std::string dump(const AdsbMsg& m);
std::string dump(const UplinkMsg& u);
// DLAC 6-bit text (4 characters in 3 bytes), as sent: '\x1e' between
// records, '\x03' at the end, '\n' / tab runs inside
std::string dlac(const uint8_t* d, size_t n);
// the text records of a DLAC text product, white space collapsed
std::vector<std::string> text_records(const uint8_t* d, size_t n);

}  // namespace uat
