#pragma once

#include <atomic>
#include <vector>
#include <array>
#include <mutex>
#include <condition_variable>
#include <complex>
#include <memory>
#include <chrono>
#include <cmath>
#include <fftw3.h>
#include "App.h"
#include "types.hpp"
#include "config.hpp"
#include "concurrentqueue.h"

// Forward declarations
class SystemStats;
class FMDemodulatorRobust;
class MUSICProcessor;
class Beamformer;
class TCPClient;
class SharedDecimator;
class DecimatorManager;
class RawDataBuffer;
class ScannerManager;
class ContinuousScanner;
class GpsdClient;
class StationInfo;

// Global state declarations
extern std::atomic<bool> running;
extern std::mutex fft_mutex;
extern std::vector<std::vector<float>> fft_magnitudes;
extern std::vector<std::vector<float>> fft_averaged;
extern std::atomic<float> averaging_alpha;
extern std::atomic<int> num_channels;
extern std::atomic<int> active_num_elements;  // Synced from server (set via -n flag on heimdall)

// KerberosSDR support mode, synced from the high bits of the packet header's
// phase-state field (heimdall started with --kerberos). Calibration is manual
// there: the UI shows uncalibrated/stale warnings built from these plus the
// masked phase state.
extern std::atomic<bool> server_kerberos_mode;
extern std::atomic<bool> server_cal_stale;
extern std::atomic<int> active_channel;

// Per-channel frequency/gain state lives in ChannelManager (channel_manager.hpp)

extern std::atomic<bool> data_ready;
extern std::atomic<bool> fm_enabled;
extern std::atomic<bool> doa_enabled;
extern std::atomic<bool> beamforming_enabled;

// Currently-connected browser WebSocket clients (live count for the status
// dashboard): incremented on open, decremented on close.
extern std::atomic<int> ws_client_count;

// Beamformed FFT data is now per-decimator (DecimatorInstance::beamformed_fft);
// each decimator runs its own beamformer steered to its own DoA.

// Operating mode (top-bar Mode selector, OPERATING_MODE:): coherent (both
// false), wideband scan (tuners spread, stitched spectrum) or independent
// (every tuner its own receiver, one spectrum per tuner). Outside coherent
// DoA / beamforming are parked and each VFO decimates its own tuner
// (DecimatorInstance::tuner_channel).
extern std::atomic<bool> wideband_mode_enabled;
extern std::atomic<bool> independent_mode_enabled;
inline bool multi_tuner_mode() {
    return wideband_mode_enabled.load(std::memory_order_relaxed) ||
           independent_mode_enabled.load(std::memory_order_relaxed);
}
// 0 coherent, 1 wideband, 2 independent
inline int operating_mode_index() {
    return wideband_mode_enabled.load(std::memory_order_relaxed) ? 1
         : independent_mode_enabled.load(std::memory_order_relaxed) ? 2 : 0;
}
extern std::atomic<bool> doa_enabled_before_wideband;  // DoA state before leaving coherent mode
// Independent mode: each tuner's requested frequency (Hz, 0 = none) and gain
// (dB, -1 = auto, -999 = none) - persisted as TUNERS:, re-sent to heimdall on
// entering the mode
extern std::atomic<double> indep_tuner_freq_hz[8];
extern std::atomic<float> indep_tuner_gain_db[8];
extern std::atomic<bool> fm_enabled_before_scanner;     // Store FM state before discrete scanner
extern std::atomic<uint32_t> fft_reset_generation;      // Generation counter for FFT reset (scanner retune)
extern std::atomic<uint32_t> wideband_last_reset_gen;   // Track wideband FFT reset generation
extern std::atomic<bool> fft_data_valid;                // False after reset until first valid FFT frame
// Per-channel frequency from server packet metadata, Hz. uint64_t: in the
// wideband (downconverter) variant this carries the true RF, which reaches
// ~6.7 GHz on low-side injection - past uint32's ~4.3 GHz cap.
extern std::array<std::atomic<uint64_t>, MAX_CHANNELS> tuner_frequencies;
// Independent mode: tuner ch's frequency in Hz - the client's own request
// when the stream agrees with it (the header's float32 is only ~64 Hz exact
// near 1 GHz), else the header's value
inline uint64_t exact_tuner_hz(int ch) {
    if (ch < 0 || ch >= MAX_CHANNELS) return 0;
    const double hdr = static_cast<double>(tuner_frequencies[ch].load(std::memory_order_relaxed));
    const double req = indep_tuner_freq_hz[ch].load(std::memory_order_relaxed);
    return static_cast<uint64_t>(std::llround((req > 0 && std::abs(req - hdr) < 500.0) ? req : hdr));
}

// KrakenSDR Wideband (downconverter) variant - distinct from the wideband
// SCAN mode above (tuner spread), which is unavailable in this variant.
// Enabled by the --wideband flag; heimdall must be started with --wideband
// too. The UI then offers RF entry across the downconverter range plus the
// mixing-side selector; heimdall owns the LO programming and reports the true
// RF in packet metadata, so the DSP paths here need no orientation changes.
extern std::atomic<bool> wb_variant_enabled;
extern std::atomic<int> wb_variant_mixer_side;  // WbMixerSide: 0 high, 1 low, 2 below
extern std::atomic<int> wb_variant_array;       // antenna ring: 0 = outer, 1 = center, 2 = inner
                                                // (auto-selected from the tuned frequency)
extern std::atomic<int> wb_variant_lo_current;  // LO output drive current 0-7
// "Wideband" DoA topology active: UCA math with the radius auto-set from the
// active antenna ring (WB_RING_RADIUS_MM) instead of the user radius setting
extern std::atomic<bool> wb_topology_active;

// UCA element ordering: antenna rings are wired CLOCKWISE (ANT0 on +x, ANT1
// clockwise from it) on both the standard KrakenSDR arrays and the Wideband
// boards. UCA geometry multiplies the element angle by this sign, a mirror
// about +x (ANT0 stays put, the rest reverse order), so DoA output stays
// unit-circle CCW while the elements run clockwise. Single choke point: flip
// to +1.0 for a counter-clockwise array.
inline double uca_angle_sign() {
    return -1.0;
}
extern std::vector<float> wideband_fft_magnitudes;  // Stitched wideband FFT
extern std::vector<float> wideband_fft_averaged;    // Averaged wideband FFT
extern std::mutex wideband_fft_mutex;
extern std::atomic<float> wideband_reference_noise_floor;  // Reference noise floor for normalization

extern TCPClient data_client, control_client;

// Decimator manager for handling multiple decimators dynamically
extern DecimatorManager decimator_manager;

// FFT work queue
extern moodycamel::ConcurrentQueue<FFTWorkItem> fft_work_queue;
extern std::mutex fft_queue_mutex;
extern std::condition_variable fft_work_available;

extern SystemStats global_stats;
extern FMDemodulatorRobust fm_demod;
extern ScannerManager scanner_manager;
extern GpsdClient gps_client;
extern StationInfo station_info;
extern ContinuousScanner continuous_scanner;

// FFT management - per-channel contexts defined in fft_processor.hpp
// Old single-plan globals removed - now using channel_fft_contexts array
// for true parallel FFT processing across channels

// Dynamic FFT settings
extern std::atomic<int> current_fft_size;
extern std::atomic<int> current_fft_decimation;

// FFT resize synchronization
// When resizing, we must drain all in-flight FFT operations before destroying contexts
extern std::atomic<bool> fft_resize_in_progress;       // True while resize is happening
extern std::atomic<int> fft_workers_active;            // Count of workers currently in FFT execution
extern std::mutex fft_resize_mutex;                    // Protects resize operation
extern std::condition_variable fft_resize_complete;    // Signaled when all workers drain

// Squelch settings (method/threshold live per decimator - see DecimatorInstance)
extern std::atomic<bool> squelch_enabled;
extern std::atomic<float> squelch_level_db;
extern std::atomic<bool> squelch_open;

// Manual steering angle override for beamforming diagnostics
extern std::atomic<bool> manual_steering_enabled;         // When true, use manual angle instead of DoA
extern std::atomic<float> manual_steering_angle;          // Manual steering angle in degrees (0-360)

// Persistent data buffer (stores complex<float> to avoid wasteful conversions)
extern std::vector<std::complex<float>> persistent_data_buffer;
extern size_t persistent_buffer_size;
extern std::mutex persistent_buffer_mutex;
// Bumped each time the write position wraps to 0. FFT work items carry the
// generation they were written under; a worker that observes a different
// generation after reading discards the (possibly overwritten) frame.
extern std::atomic<uint32_t> persistent_buffer_generation;

// uWS globals
extern uWS::Loop* loop;
extern struct us_timer_t* broadcast_timer;
extern struct us_timer_t* audio_timer;
extern struct us_timer_t* doa_timer;
extern uWS::SSLApp* global_ssl_app;

// C timer functions
extern "C" {
    struct us_timer_t* us_create_timer(struct us_loop_t* loop, int fallthrough, unsigned int ext_size);
    void us_timer_set(struct us_timer_t* timer, void (*cb)(struct us_timer_t* t), int ms, int repeat_ms);
    void us_timer_close(struct us_timer_t* timer);
    void us_wakeup_loop(struct us_loop_t* loop);
}