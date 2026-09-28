/*
 * Heimdall Minimal Configuration
 * Optimized implementation with int16_t IQ samples and ARM NEON
 * Edit these values to customize system behavior
 */

#ifndef CONFIG_H
#define CONFIG_H

// ========================================================================
// OPTIMIZATION SETTINGS
// ========================================================================

// The implementation uses optimized int16_t IQ samples for:
// - 50% memory bandwidth reduction vs float complex
// - ARM NEON vectorization (8x int16 vs 4x float32 per instruction)
// - Better cache utilization and power efficiency
// - Automatic fallback to scalar code on non-ARM platforms

// ========================================================================
// RTL-SDR CONFIGURATION
// ========================================================================

// Maximum number of RTL-SDR devices (compile-time CEILING - sizes the static
// per-channel state). The number actually opened/used at runtime is
// active_num_elements: the -n flag or the web UI's element-count selector,
// bounded by this ceiling AND by the length of the expected-serials list
// (--serials flag, default "1000".."1007" in main.cpp).
#define NUM_DEVICES 8

// Reference channel (0-based index)
#define REF_CHANNEL 0

// RF Settings
#define CENTER_FREQ     100000000   // 100 MHz center frequency
#define SAMPLE_RATE     2400000     // 2.4 MSPS sample rate
#define GAIN            496         // 49.6 dB gain (value * 10)

// ========================================================================
// KRAKENSDR WIDEBAND (DOWNCONVERTER) VARIANT
// ========================================================================
// The KrakenSDR Wideband hardware variant places a mixer in front of every
// tuner, all driven by ONE shared LO (an on-board synthesizer speaking the
// moRFeus USB HID protocol, VID/PID 10c4:eac9, custom "WB fw" firmware). The
// tuners sit at a fixed IF (the hardware IF filter is centred there) and
// retuning means reprogramming the LO, which the mixer side places around
// the IF: LO = IF + RF (high side, spectrum inverted), LO = IF - RF (low
// side, upright) or LO = RF - IF (below - LO under the RF, upright; the only
// side reaching past the synthesizer ceiling, RF up to IF + LO_MAX =
// 6668 MHz). Activated at runtime with --wideband; these constants only take
// effect in that mode.

#define WB_VARIANT_IF_HZ    1268000000ULL  // fixed IF the tuners are parked at
#define WB_LO_MIN_HZ        85000000ULL    // synthesizer LO limits (moRFeus-class)
#define WB_LO_MAX_HZ        5400000000ULL
#define WB_LO_CURRENT       3              // default LO output drive current (0-7),
                                           // runtime-adjustable from the client UI
#define WB_RF_MIN_HZ        24000000ULL    // lowest RF the front end usefully passes

// Antenna ring auto-selection: the three concentric rings are frequency-
// scaled copies of the array, picked by the tuned RF (outer below CENTER_MIN,
// center below INNER_MIN, inner above). Must match the client's constants.
#define WB_RING_CENTER_MIN_HZ 1000000000ULL   // outer ring below 1 GHz
#define WB_RING_INNER_MIN_HZ  2500000000ULL   // center ring below 2.5 GHz, inner above

// Sample Processing
#define NUM_SAMPLES     16384       // Samples per FFT (power of 2)
#define CORRELATION_SIZE (NUM_SAMPLES * 2)  // Zero-padded correlation size

// Buffer Management
#define BUFFER_SIZE     100          // Number of sample sets to buffer (L1 depth cap)

// L2-raw staging depth cap (#4). The conversion worker drains this queue IN
// ORDER (every set must reach the TCP streamers), so a deep queue would add
// real latency to the lag loop. Keep it shallow: it only smooths conversion
// jitter; under sustained overload whole aligned sets drop here (coherence-safe)
// and latency stays bounded (~L2_RAW_MAX * NUM_SAMPLES / SAMPLE_RATE seconds).
// The per-device sample pool is sized to cover L1 + L2-raw in-flight buffers.
#define L2_RAW_MAX      16

// C5: tighter L2-raw staging cap used ONLY while a coherent calibration is active
// (phase not yet CONVERGED). Bounds the conversion-feedback latency the lag servo /
// phase cal see if the conversion worker saturates during cal (16 sets ~109 ms ->
// 4 sets ~27 ms @ 2.4 MSPS = fresher data into the loops). Restored to L2_RAW_MAX
// once CONVERGED for the steady-state overflow cushion. The buffer pools stay sized
// at L2_RAW_MAX (over-provisioned), so a smaller RUNTIME cap only drops the same
// coherence-safe whole aligned sets sooner. Zero effect when conversion keeps up.
#define L2_RAW_CAL_MAX  4

// ========================================================================
// COMMUNICATION CONFIGURATION
// ========================================================================

// Web interface port (HTTP + WebSocket)
#define WEB_PORT        8070

// ========================================================================
// PERFORMANCE TUNING
// ========================================================================

// Realtime (SCHED_RR) priorities for the time-critical ingest threads. These
// resist CFS preemption under load (the cause of USB FIFO overflow). Applied
// best-effort: if the process can't get realtime scheduling it runs at normal
// priority. USB readers > sample drain so reads always win a core. The
// conversion worker deliberately runs at NORMAL priority (it can fall behind
// safely, and at RT its malloc/mutex use would priority-invert against the web/
// FFTW threads), so it has no RT priority here.
#define RT_PRIO_USB_READER      20  // per-device librtlsdr async reader threads
#define RT_PRIO_SAMPLE_DRAIN    15  // L1 -> L2-raw aligned-collection thread

// librtlsdr async USB transfer buffers (buf_num passed to rtlsdr_read_async).
// 0 = library default (15 x 32 KB ~= 102 ms cushion @ 2.4 MSPS). A larger ring
// widens the window the reader thread may stall before the RTL2832 FIFO drops
// samples; 32 ~= 218 ms at the cost of harmless latency and ~1 MB/device.
#define RTL_USB_BUF_COUNT       32

// Bound on consecutive per-device L1 read timeouts in the sample drain before
// declaring coherence lost. At 100 ms/timeout this is ~3 s of one device being
// starved while the others overflow - a stuck/unplugged dongle, not a hiccup.
#define STUCK_DEVICE_MAX_TIMEOUTS 30

// Low-rate streaming coherence monitor (#5). Post-calibration the server
// auto-disables FFT and measures no lag, so a silent USB-level drop (RTL2832
// FIFO overflow - invisible to callback counting) would go unnoticed until the
// DoA output quietly went wrong. When enabled, a ~2 Hz differential cross-
// correlation check on the already-compensated stream flags such a slip and
// triggers coherence recovery. DEFAULT OFF: it is a heuristic whose false
// positives would trigger a disruptive (~30-60 s, noise-source) recalibration,
// so it must be validated and its thresholds tuned on real hardware before being
// trusted in production. The application-level drop path (L1 overflow / pool
// exhaustion -> coherence_lost) is always on and covers the high-CPU-load case.
#define ENABLE_COHERENCE_MONITOR 0

// Periodic calibration check. Once calibration has converged, the system
// re-verifies lag/phase coherence every PERIODIC_RECAL_DEFAULT_MINUTES: it
// briefly engages the noise source (if the user did not already have it on),
// measures the residual lag/phase the eigen monitor reports, then restores
// whatever the user had on. If the averaged residual on any channel exceeds the
// thresholds below the calibration has drifted, so a full lag+phase
// recalibration is triggered (same path as a coherence-loss recovery). The user
// can also force an immediate recalibration from the web UI at any time. These
// are only DEFAULTS - the enable flag and period are runtime-configurable from
// the web UI and persisted in heimdall_settings.conf.
#define PERIODIC_RECAL_DEFAULT_ENABLED  1      // default ON
#define PERIODIC_RECAL_DEFAULT_MINUTES  5      // default 5 minute check period
// Thresholds for declaring the calibration "bad". Set well above the converged
// noise floor (lag locks to +-0.15 samples, phase to +-1 deg) so normal jitter
// never triggers a disruptive (~30-60 s, noise-source) recalibration.
#define PERIODIC_RECAL_LAG_THRESHOLD    1.0f   // avg |residual lag| in samples above this == bad
#define PERIODIC_RECAL_PHASE_THRESHOLD  10.0f  // avg |residual phase| in degrees above this == bad

// Processing delays (microseconds)
#define PROCESS_LOOP_DELAY      10000   // Delay in main processing loop

// ========================================================================
// HARDWARE OPTIMIZATION
// ========================================================================

// RTL-SDR device configuration
#define ENABLE_BIAS_TEE         1       // Enable bias tee on all devices
#define AUTO_GAIN_MODE          0       // 0=manual gain, 1=auto gain

// USB configuration hints
#define USB_RESET_ON_INIT       1       // Reset USB endpoint on init

// --kerberos_sw: Raspberry Pi header GPIOs driving the third-party CKOVAL
// antenna switches (BCM numbering; same pins the V1 DAQ firmware used).
// Idle: ANT1=1, ANT2=0 (antenna input 1). Both LOW while the calibration
// noise source is on (antennas disconnected).
#define KERBEROS_SW_GPIO_ANT1   23
#define KERBEROS_SW_GPIO_ANT2   24

// ========================================================================
// FFTW CONFIGURATION
// ========================================================================

// FFTW planner effort (affects startup time vs. runtime performance)
// Options: FFTW_ESTIMATE, FFTW_MEASURE, FFTW_PATIENT, FFTW_EXHAUSTIVE
#define FFTW_PLANNER_EFFORT     FFTW_MEASURE

// ========================================================================
// DEVICE MAPPING
// ========================================================================

// Channel N is the device whose USB serial matches entry N of the expected-
// serials list. Default list (KrakenSDR "1000".."1007") is defined in
// main.cpp (declared in src/core/config.hpp); override at runtime with
// --serials s0,s1,...

// ========================================================================
// VALIDATION MACROS
// ========================================================================

// Compile-time checks
#if NUM_DEVICES < 2
    #error "NUM_DEVICES must be at least 2"
#endif

#if NUM_DEVICES > 32
    #error "NUM_DEVICES cannot exceed 32"
#endif

#if REF_CHANNEL >= NUM_DEVICES
    #error "REF_CHANNEL must be less than NUM_DEVICES"
#endif

#if (NUM_SAMPLES & (NUM_SAMPLES - 1)) != 0
    #error "NUM_SAMPLES must be a power of 2"
#endif

#if SAMPLE_RATE < 225000 || SAMPLE_RATE > 3200000
    #warning "SAMPLE_RATE outside typical RTL-SDR range (225 kHz - 3.2 MHz)"
#endif

#if CENTER_FREQ < 24000000 || CENTER_FREQ > 1766000000
    #warning "CENTER_FREQ outside RTL-SDR range (24 MHz - 1766 MHz)"
#endif

// ========================================================================
// HELPER MACROS
// ========================================================================

// Frequency conversion helpers
#define MHZ(x)  ((x) * 1000000)
#define KHZ(x)  ((x) * 1000)

// Gain conversion (RTL-SDR uses tenths of dB)
#define DB(x)   ((int)((x) * 10))

// Time conversion
#define MS(x)   ((x) * 1000)
#define US(x)   (x)

// ========================================================================
// EXAMPLE CONFIGURATIONS
// ========================================================================

/*
// VHF Air Band Configuration
#define CENTER_FREQ     MHZ(118)
#define SAMPLE_RATE     MHZ(2.4)
#define GAIN            DB(40)

// UHF Ham Band Configuration  
#define CENTER_FREQ     MHZ(432)
#define SAMPLE_RATE     MHZ(2.4)
#define GAIN            DB(45)

// L-Band GPS Configuration
#define CENTER_FREQ     MHZ(1575.42)
#define SAMPLE_RATE     MHZ(2.4)
#define GAIN            DB(40)

// High Performance Configuration (more samples, higher precision)
#define NUM_SAMPLES     32768
#define FFTW_PLANNER_EFFORT FFTW_PATIENT

// Low Resource Configuration (fewer samples, faster processing)
#define NUM_SAMPLES     8192
#define FFTW_PLANNER_EFFORT FFTW_ESTIMATE
*/

#endif // CONFIG_H
