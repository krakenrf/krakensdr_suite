#pragma once
// Region / vendor profiles of the wildlife plugin (option `profile`).
//
// The default, "global", assumes NO country: a collar is found anywhere in
// VHF / UHF, named by its frequency, and its modes come from its OWN
// behaviour (the baseline rate it was first heard at, then sustained rate
// changes - see decoder.cpp classify()). A profile only ADDS: a channel
// scheme, a table of absolute rates, what a sustained rise means for that
// region's common vendors, a data-stream scheme and band hints. It never
// removes the generic detection. Sources per profile: README.md.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wl {

// A band used for wildlife tracking somewhere (MHz). `region` = the
// profile id that names it ("" = several / worldwide).
struct BandHint {
    double lo, hi;
    const char* region;
    const char* label;
};

// Wildlife tracking allocations / common usage worldwide - only hints for
// the "Band" fact and a collar's details: collars are detected anywhere.
inline const std::vector<BandHint>& band_hints() {
    static const std::vector<BandHint> b = {
        {40.66, 40.70, "na", "animal / ocean buoy tracking (US, FCC 47 CFR 90.248)"},
        {48.0, 49.0, "au", "aquatic animal tracking (Australia, ACMA LIPD class licence)"},
        {142.93, 142.99, "jp", "animal detection notification system (Japan, MIC)"},
        {146.93, 146.99, "jp", "animal detection notification system (Japan, MIC)"},
        {148.0, 152.0, "na", "wildlife collars (North America: common agency use; South Africa: ICASA wildlife telemetry)"},
        {150.05, 150.24, "eu", "animal radio tagging (Germany, BNetzA Vfg 125/2023)"},
        {150.095, 150.105, "motus", "Motus 150.100 MHz (Europe)"},
        {151.495, 151.505, "motus", "Motus 151.500 MHz (Australia)"},
        {155.4, 155.525, "eu", "tracking collars, mainly hunting dogs (Finland, Traficom)"},
        {159.0, 161.0, "na", "wildlife collars (North America: common agency use)"},
        {160.1, 160.6, "nz", "wildlife tracking (NZ general user licence, channels 0-47)"},
        {160.6, 161.11, "nz", "wildlife tracking (NZ, DOC sub-licence, channels 48-99)"},
        {164.0, 168.0, "na", "wildlife collars (North America: common agency use)"},
        {166.375, 166.385, "motus", "Motus 166.380 MHz (Americas)"},
        {173.2, 173.35, "eu", "telemetry used for tags (UK)"},
        {173.7, 174.0, "eu", "telemetry incl. airborne bird tracking (UK, 1999 exemption)"},
        {216.0, 220.0, "na", "animal / ocean buoy tracking (US, FCC 47 CFR 90.248)"},
    };
    return b;
}

// A pulse rate with a fixed meaning in a profile's convention.
struct RateEntry {
    double ppm, tol;
    const char* mode;
    bool mortality;
};

struct Profile {
    const char* id;
    const char* name;
    std::vector<RateEntry> rates;   // absolute rates (NZ DOC); empty = none
    const char* first_mode;         // a collar's mode at its first rate (no table entry)
    // a sustained rise of x1.8-2.25 over the collar's baseline means
    // MORTALITY in this profile's vendor convention; else (and for other
    // rises x1.4-3.1) "possible mortality"
    bool doubling_is_mortality;
    bool wildtech;                  // Wildtech pulse-count data streams (time since death, Chick / Egg Timer)
    bool nz_channels;               // NZ DOC / Telonics TR4 channel numbers
    bool motus;                     // Motus frequencies named as channels
    const char* convention;         // shown in a collar's details
};

inline const std::vector<Profile>& profiles() {
    static const std::vector<Profile> p = {
        {"global", "Global (no country)", {}, "baseline", false, false, false, false,
         "modes from the collar's own rate: its first steady rate = baseline; a rise x1.4-3 held 2 min = possible "
         "mortality (most mortality sensors raise the rate - Kenward: it doubles after ~4 h still), back and forth "
         "between known rates = an activity sensor"},
        {"na", "North America", {}, "baseline", true, false, false, false,
         "a doubling over its resting rate = MORTALITY (Kenward / Lotek: the rate rises after hours still; Telonics: "
         "the mortality time is set per collar); activity collars switch between rates (USFS collar: 40 resting / 60 "
         "active / 80 mortality)"},
        {"eu", "Europe", {}, "baseline", true, false, false, false,
         "a doubling over its resting rate = MORTALITY (Kenward's convention: doubles after ~4 h still)"},
        {"au", "Australia", {}, "baseline", true, false, false, false,
         "a doubling over its resting rate = MORTALITY (Kenward / Lotek convention); VHF tags need an apparatus licence "
         "here (the LIPD class licence only has 48-49 MHz aquatic tracking)"},
        {"motus", "Motus (coded tags)", {}, "baseline", false, false, false, true,
         "Motus: Lotek coded tags (4 pulses, the gaps = the Lotek ID, + the burst interval = the Motus tag) on "
         "166.380 / 150.100 / 151.500 MHz"},
        {"nz", "New Zealand (DOC / Wildtech)",
         {{30, 3, "normal", false}, {48, 4, "incubating", false}, {60, 5, "MORTALITY", true}, {80, 6, "MORTALITY", true}},
         "normal", true, true, true, false,
         "DOC / Wildtech: 30 ppm normal, 48 incubating (Egg / Chick Timer), mortality = double (60; kakapo 80) after "
         "24 h without movement; Wildtech data streams"},
    };
    return p;
}

inline const Profile& profile_by_id(const std::string& id) {
    for (const auto& p : profiles())
        if (id == p.id) return p;
    return profiles().front();   // "" / unknown / the old "change" -> global
}

// NZ wildlife channels: 160.120 + 0.010 n MHz (Telonics TR4 channels 0-47
// general use, 48-99 special licence - DOC Kiwi Best Practice Manual 6.4)
inline int nz_channel(double mhz) {
    if (!std::isfinite(mhz) || mhz < 160.115 || mhz > 161.115) return -1;
    const long n = std::lround((mhz - 160.120) / 0.010);
    if (n < 0 || n > 99 || std::fabs(mhz - (160.120 + 0.010 * n)) > 0.0025) return -1;
    return static_cast<int>(n);
}

// the profile's channel name of a carrier ("" = none)
inline std::string channel_label(const Profile& p, double mhz) {
    if (!std::isfinite(mhz)) return "";
    if (p.nz_channels) {
        const int n = nz_channel(mhz);
        if (n >= 0) return "NZ ch " + std::to_string(n);
    }
    if (p.motus)
        for (double f : {166.380, 150.100, 151.500})
            if (std::fabs(mhz - f) <= 0.005) {
                char b[32];
                snprintf(b, sizeof b, "Motus %.3f", f);
                return b;
            }
    return "";
}

// the hint for a carrier (one frequency: lo = hi) or the VFO's span; the
// profile's own region first. "" = no known wildlife band there
inline std::string band_label(double lo, double hi, const Profile& p) {
    std::string own, other;
    for (const auto& b : band_hints()) {
        if (hi < b.lo || lo > b.hi) continue;
        char r[160];
        snprintf(r, sizeof r, "%g-%g MHz %s", b.lo, b.hi, b.label);
        std::string& s = std::string(p.id) != "global" && std::string(p.id) == b.region ? own : other;
        if (s.find(r) == std::string::npos) s += std::string(s.empty() ? "" : "; ") + r;
    }
    if (!own.empty()) return own + (other.empty() ? "" : "; also " + other);
    return other;
}

}  // namespace wl
