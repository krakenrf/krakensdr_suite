#pragma once

// ContinuousScanner - continuous signal scanner with multi-decimator tracking
//
// Scans the spectrum for signals above squelch, locks up to three signals onto
// available decimators (FM audio follows the first lock), dwells on them until
// they fade or the dwell time expires, and remembers recent peaks so they are
// not immediately re-acquired. In wideband mode it builds a band plan covering
// [start, end] in 2.4 MHz tuner steps (edge-clipped) and retunes the SDR center
// frequency through the Heimdall server as it cycles bands.
//
// Reconstructed from the fft_viewer binary committed at 30f205f (the original
// source was never committed to git).

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class ContinuousScanner {
public:
    enum ScanState : int {
        IDLE     = 0,
        SCANNING = 1,
        DWELLING = 2,
        RETUNING = 3
    };

    static constexpr int MAX_TRACKED_SIGNALS = 3;

    // One signal locked onto a decimator
    struct TrackedSignal {
        int   decimator_id = -1;
        float freq_hz      = 0.0f;
        float offset_hz    = 0.0f;
        float signal_db    = 0.0f;
        std::chrono::steady_clock::time_point lock_time{};
        int   signal_index = -1;
        bool  active       = false;
    };

    ContinuousScanner() = default;
    ~ContinuousScanner();

    bool start();
    bool stop();
    bool isRunning() const { return running_.load(); }

    void setDwellTime(int dwell_ms);
    void setSquelchLevel(float squelch_db);
    void setDecayTime(int decay_ms);
    void setShowSquelch(bool show);

    void setWidebandScanRange(float start_hz, float end_hz);
    void setWidebandScanEnabled(bool enable);

    std::string getStatusJson() const;

private:
    void scannerThread();

    // FFT snapshot and signal detection
    void refreshFFTData();
    std::vector<std::pair<float, float>> findAllSignals();
    float getSignalStrength(float freq_hz);

    // Width of one "signal" for detection/matching purposes: the user-selected
    // decimator bandwidth (first enabled decimator), 10 kHz minimum.
    float getScanBandwidthHz();

    // True while the server is running coherent calibration (noise source on
    // or phase compensation not yet settled) - FFT data is unusable then.
    bool isCalibrationActive();

    // Peak memory (recently seen signals, keyed by freq in kHz)
    void updatePeakMemory(const std::vector<std::pair<float, float>>& signals);
    std::vector<std::pair<float, float>> getSignalsFromMemory();
    bool isSignalInMemory(float freq_hz);

    // Decimator tracking
    std::vector<int> getAvailableDecimators();
    void tuneTo(int decimator_id, float freq_hz, float signal_db, int signal_index);
    void updateFMToSignal();
    void clearBandState();

    // Wideband band plan
    void buildBandPlan();
    void initiateRetune(int band_index);
    void advanceToNextBand();

    // Settings
    std::atomic<int>   dwell_time_ms_{2000};
    std::atomic<float> squelch_level_db_{15.0f};
    std::atomic<bool>  show_squelch_{false};
    std::atomic<int>   decay_time_ms_{5000};

    // Scan state
    std::atomic<int>  state_{IDLE};
    std::atomic<bool> running_{false};
    std::mutex lifecycle_mutex_;   // serializes start() / stop()

    // Signals detected in the current band
    std::vector<std::pair<float, float>> detected_signals_;  // (freq_hz, power_db)
    int current_signal_index_ = 0;
    mutable std::mutex signals_mutex_;

    // Signals locked onto decimators
    TrackedSignal tracked_signals_[MAX_TRACKED_SIGNALS];
    int num_tracked_ = 0;
    mutable std::mutex tracked_mutex_;

    // Snapshot of the active channel's averaged FFT (noise floor subtracted)
    std::vector<float> current_fft_;
    float fft_center_freq_hz_  = 0.0f;
    float fft_sample_rate_hz_  = 0.0f;
    std::chrono::steady_clock::time_point last_fft_time_{};
    mutable std::mutex fft_data_mutex_;

    // Peak memory: freq (kHz) -> last seen time
    std::map<int, std::chrono::steady_clock::time_point> peak_memory_;
    mutable std::mutex memory_mutex_;

    // Wideband scan
    std::atomic<bool>  wideband_scan_enabled_{false};
    std::atomic<float> wideband_start_hz_{0.0f};
    std::atomic<float> wideband_end_hz_{0.0f};
    std::vector<float> band_centers_;
    int current_band_index_ = 0;
    int num_bands_ = 0;
    mutable std::mutex band_plan_mutex_;

    // Retune/calibration tracking
    std::chrono::steady_clock::time_point retune_start_time_{};
    std::chrono::steady_clock::time_point calibration_done_time_{};
    bool calibration_started_ = false;
    bool calibration_done_    = false;

    std::thread scanner_thread_;
};
