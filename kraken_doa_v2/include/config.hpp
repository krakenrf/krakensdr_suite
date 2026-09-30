#pragma once

#include <cstdint>
#include <vector>

// Network Configuration
constexpr int WEB_PORT = 8080;          // HTTPS WebSocket UI (kraken_doa.html)
constexpr int DOA_HTTP_PORT = 8081;     // Plain HTTP for DOA_value.html (Android app)
constexpr int TCP_DATA_PORT = 8091;
constexpr int TCP_CONTROL_PORT = 8092;
constexpr uint32_t TCP_MAGIC = 0x4D434851; // 'MCHQ'

// GPS / gpsd Configuration
// kraken_doa reads location/heading from gpsd (the standard Linux GPS daemon)
// over its JSON socket. When gpsd is down or no receiver is attached, the GPS
// fields gracefully report 0's / "No GPS". Set GPS_ENABLED=false to skip gpsd.
constexpr bool GPS_ENABLED = true;
constexpr const char* GPSD_HOST = "127.0.0.1";
constexpr int GPSD_PORT = 2947;

// Signal Processing Configuration
constexpr int FFT_SIZE = 16384;
// Compile-time CEILING on wire/processing channels (matches heimdall's
// NUM_DEVICES). The count actually in use is the runtime atomic
// active_num_elements, synced from the 8091 packet header.
constexpr int MAX_CHANNELS = 8;
constexpr float SAMPLE_RATE = 2.4e6f; // 2.4 MSPS
// R820T tuning range (standard hardware). Must match heimdall's
// RTL_TUNER_MIN_HZ / RTL_TUNER_MAX_HZ (heimdall_v2/config.h); FREQ outside it
// is rejected. The Wideband variant uses wb_variant_rf_union_range instead.
constexpr uint64_t RTL_TUNER_MIN_HZ = 24000000ULL;
constexpr uint64_t RTL_TUNER_MAX_HZ = 1766000000ULL;
constexpr int DECIMATION_FACTOR = 10;
// IMPORTANT: If browser uses different rate, change this to match!
// Common rates: 44100 (CD quality), 48000 (professional), 96000 (high-end)
// Check browser console: "AudioContext created with native sample rate: XXXXX Hz"
constexpr float AUDIO_SAMPLE_RATE = 48000.0f;
constexpr int AUDIO_SAMPLES_PER_PACKET = 1920;  // 40ms at 48kHz (optimized for fewer packets)

// KrakenSDR Wideband (downconverter) variant - enabled at runtime with
// --wideband (heimdall must be started with --wideband too). The hardware
// parks the tuners at a fixed, filtered IF and reaches the requested RF by
// programming a shared downconverter LO, placed by the mixer side:
//   high:  LO = IF + RF (heimdall un-mirrors the inverted spectrum at the
//          source), RF 24-4132 MHz
//   low:   LO = IF - RF (upright), RF 24-1183 MHz
//   below: LO = RF - IF (LO under the RF, upright), RF 1353-6668 MHz - the
//          only side reaching past 4.1 GHz
// These must match heimdall_v2/config.h (WB_VARIANT_IF_HZ etc., MixerSide);
// the client uses them for UI limits and the LO readout.
constexpr uint64_t WB_VARIANT_IF_HZ = 1268000000ULL;
constexpr uint64_t WB_LO_MIN_HZ = 85000000ULL;      // moRFeus synthesizer span
constexpr uint64_t WB_LO_MAX_HZ = 5400000000ULL;
constexpr uint64_t WB_RF_MIN_HZ = 24000000ULL;      // front-end floor

enum WbMixerSide : int { WB_SIDE_HIGH = 0, WB_SIDE_LOW = 1, WB_SIDE_BELOW = 2 };

// Wire/UI name, matching heimdall's set_mixer_side values
inline const char* wb_mixer_side_name(int side) {
    return side == WB_SIDE_LOW ? "low" : side == WB_SIDE_BELOW ? "below" : "high";
}

inline void wb_variant_rf_range(int side, uint64_t& min_hz, uint64_t& max_hz) {
    if (side == WB_SIDE_LOW) {
        // LO = IF - RF >= LO_MIN caps the top.
        min_hz = WB_RF_MIN_HZ;
        max_hz = WB_VARIANT_IF_HZ - WB_LO_MIN_HZ;
    } else if (side == WB_SIDE_BELOW) {
        // LO = RF - IF: the whole LO span shifted up by the IF.
        min_hz = WB_VARIANT_IF_HZ + WB_LO_MIN_HZ;
        max_hz = WB_VARIANT_IF_HZ + WB_LO_MAX_HZ;
    } else {
        // LO = IF + RF <= LO_MAX caps the top; the floor is the front end.
        min_hz = WB_RF_MIN_HZ;
        max_hz = WB_LO_MAX_HZ - WB_VARIANT_IF_HZ;
    }
}

// RF span reachable on SOME side (union of the three): the tuning limits now
// that the side auto-switches when the current one can't reach the RF.
inline void wb_variant_rf_union_range(uint64_t& min_hz, uint64_t& max_hz) {
    min_hz = WB_RF_MIN_HZ;
    max_hz = WB_VARIANT_IF_HZ + WB_LO_MAX_HZ;
}

// Sanity ceiling for a continuous-scanner wideband range: the highest RF any
// supported hardware reaches (the Wideband variant's union span).
constexpr float MAX_WIDEBAND_SCAN_MHZ = (WB_VARIANT_IF_HZ + WB_LO_MAX_HZ) / 1e6f;

inline bool wb_side_valid(int side, uint64_t rf_hz) {
    uint64_t min_hz, max_hz;
    wb_variant_rf_range(side, min_hz, max_hz);
    return rf_hz >= min_hz && rf_hz <= max_hz;
}

// Preferred side when the current one can't reach the RF: high covers
// everything up to its ceiling (low's range is a strict subset of high's),
// below covers the rest. Must match heimdall's downconverter_auto_side().
inline int wb_auto_side(uint64_t rf_hz) {
    return rf_hz <= WB_LO_MAX_HZ - WB_VARIANT_IF_HZ ? WB_SIDE_HIGH : WB_SIDE_BELOW;
}

// Antenna ring auto-selection: the Wideband hardware's three concentric
// rings are picked by the tuned RF (heimdall throws the RF switches on its
// own retunes with the same rule - wb_ring_for_rf must match its
// WB_RING_*_MIN_HZ constants). The fixed radii feed the "Wideband" DoA
// topology (UCA math, radius follows the active ring).
constexpr uint64_t WB_RING_CENTER_MIN_HZ = 1000000000ULL;  // outer ring below 1 GHz
constexpr uint64_t WB_RING_INNER_MIN_HZ = 2500000000ULL;   // center below 2.5 GHz, inner above
constexpr float WB_RING_RADIUS_MM[3] = {127.5f, 51.0f, 20.4f};  // outer, center, inner
constexpr const char* WB_RING_NAMES[3] = {"Outer", "Center", "Inner"};

inline int wb_ring_for_rf(uint64_t rf_hz) {
    return rf_hz < WB_RING_CENTER_MIN_HZ ? 0 : rf_hz < WB_RING_INNER_MIN_HZ ? 1 : 2;
}

// MUSIC DoA Configuration
// DOA_NUM_ELEMENTS is the compile-time CEILING on antenna elements (sizes
// fixed arrays like custom positions and per-channel pointer tables). The
// element count actually processed is the runtime atomic active_num_elements
// (synced from the server's packet header; KrakenSDR = 5, KerberosSDR = 4).
constexpr int DOA_NUM_ELEMENTS = 8;
// Pre-connect default for active_num_elements before the first packet header
// arrives (stock KrakenSDR).
constexpr int DOA_DEFAULT_ELEMENTS = 5;
constexpr int DOA_BLOCK_SIZE = 256;          // Samples per processing block
constexpr int DOA_NUM_SNAPSHOTS = 64;        // Number of snapshots for covariance
constexpr int DOA_ANGULAR_RESOLUTION = 1;    // Degrees
constexpr int DOA_NUM_ANGLES = 360;          // Full circle scan

// Bandwidth Configuration Structure
struct BandwidthOption {
    int decimation_factor;
    float bandwidth_mhz;
    const char* display_name;
    
    constexpr BandwidthOption(int dec, float bw, const char* name) 
        : decimation_factor(dec), bandwidth_mhz(bw), display_name(name) {}
};

// Available bandwidth options (no resampling, integer decimation only)
constexpr BandwidthOption BANDWIDTH_OPTIONS[] = {
    {1, 2.400f, "2.4 MHz (No decimation)"},
    {2, 1.200f, "1.2 MHz"},
    {3, 0.800f, "800 kHz"},
    {4, 0.600f, "600 kHz"},
    {5, 0.480f, "480 kHz"},
    {6, 0.400f, "400 kHz"},
    {8, 0.300f, "300 kHz"},
    {10, 0.240f, "240 kHz"},
    {12, 0.200f, "200 kHz"},
    {15, 0.160f, "160 kHz"},
    {16, 0.150f, "150 kHz"},
    {20, 0.120f, "120 kHz"},
    {24, 0.100f, "100 kHz"},
    {30, 0.080f, "80 kHz"},
    {40, 0.060f, "60 kHz"},
    {48, 0.050f, "50 kHz"},
    {60, 0.040f, "40 kHz"},
    {80, 0.030f, "30 kHz"},
    {100, 0.024f, "24 kHz"},
    {120, 0.020f, "20 kHz"},
    {150, 0.016f, "16 kHz"},
    {200, 0.012f, "12 kHz"},
    {240, 0.010f, "10 kHz"},
    {300, 0.008f, "8 kHz"},
    {400, 0.006f, "6 kHz"},
    {480, 0.005f, "5 kHz"},
    {600, 0.004f, "4 kHz"},
    {800, 0.003f, "3 kHz"},
    {1200, 0.002f, "2 kHz"},
    {2400, 0.001f, "1 kHz"}
};

constexpr int NUM_BANDWIDTH_OPTIONS = sizeof(BANDWIDTH_OPTIONS) / sizeof(BANDWIDTH_OPTIONS[0]);

// Default index - single bandwidth setting for both FM and MUSIC
constexpr int DEFAULT_BANDWIDTH_INDEX = 7;  // 240 kHz (decimation 10)

// Computed constants
constexpr float AUDIO_PACKET_TIME_MS = (AUDIO_SAMPLES_PER_PACKET * 1000.0f) / AUDIO_SAMPLE_RATE;
constexpr int AUDIO_RING_BUFFER_SIZE = AUDIO_SAMPLE_RATE * 1; // 1 second
constexpr int INITIAL_FILL_TARGET = AUDIO_SAMPLE_RATE / 10;   // 0.1 seconds

// File Configuration
constexpr const char* HTML_FILE = "kraken_doa.html";
constexpr const char* SSL_CERT_FILE = "server.crt";
constexpr const char* SSL_KEY_FILE = "server.key";

// API authentication: optional shared token for the WebSocket API (browser +
// native apps). Loaded at startup from the KRAKEN_API_TOKEN env var, falling
// back to this file. Empty/absent => fail-open (no auth required, as before).
constexpr const char* AUTH_TOKEN_FILE = "api_token";

// Utility functions
inline int find_bandwidth_index(int decimation_factor) {
    for (int i = 0; i < NUM_BANDWIDTH_OPTIONS; i++) {
        if (BANDWIDTH_OPTIONS[i].decimation_factor == decimation_factor) {
            return i;
        }
    }
    return DEFAULT_BANDWIDTH_INDEX; // fallback
}

inline const BandwidthOption& get_bandwidth_option(int index) {
    if (index >= 0 && index < NUM_BANDWIDTH_OPTIONS) {
        return BANDWIDTH_OPTIONS[index];
    }
    return BANDWIDTH_OPTIONS[DEFAULT_BANDWIDTH_INDEX];
}