#pragma once
#include <vector>
#include <array>
#include <atomic>
#include <complex>
#include <mutex>
#include <memory>
#include <chrono>
#include <thread>
#include <Eigen/Dense>
#include <Eigen/Eigenvalues>
#include "types.hpp"
#include "signal_processing/shared_decimator.hpp"
#include "config.hpp"

struct MUSICConfig {
    size_t snapshot_length = 256;           // Samples per snapshot
    size_t num_snapshots = 32;              // Number of snapshots for correlation matrix
    size_t overlap_samples = 64;            // Overlap between snapshots (for continuity)
    size_t min_snapshots = 16;              // Minimum snapshots before processing
};

class MUSICProcessor {
public:
    MUSICProcessor();
    ~MUSICProcessor();
    
    // Configuration
    void setConfig(const MUSICConfig& config);
    MUSICConfig getConfig() const;
    
    void setFrequencyWithOffset(float base_freq_hz, float offset_hz);
    float getEffectiveFrequency() const { return current_frequency; }

    // Process pre-decimated complex IQ data with optimized accumulation.
    // The data is only READ (copied into the internal accumulator), so the
    // const& form is the real implementation; the rvalue overload just
    // delegates - neither copies the input.
    bool processDecimatedIQ(const SharedDecimator::MultiChannelDecimated& decimated_data);
    bool processDecimatedIQ(SharedDecimator::MultiChannelDecimated&& decimated_data);

    
    Eigen::VectorXd getPseudospectrum() const;

    // Get peak DoA angle in degrees [0, 360). Sub-grid accuracy via a 3-point
    // parabolic fit on the log-pseudospectrum around the maximum, so the
    // readout is finer than the angular grid resolution (~0.1deg-class at the
    // default 1deg grid). Returns -1 if no valid DoA available.
    float getPeakAngle() const;

    // Get peak DoA angle with confidence (peak-to-average ratio)
    // Returns interpolated angle in degrees [0, 360) and confidence (0.0-1.0)
    std::pair<float, float> getPeakAngleWithConfidence() const;

    // Get eigenvalue ratio for signal detection (squelch)
    // Returns λ1 / mean(λ2..λN) - high ratio indicates coherent signal present
    // Typical values: ~1 for noise only, >2-3 for weak signal, >10 for strong signal
    float getEigenvalueRatio() const;

    // Peak-held eigenvalue ratio: the maximum over the last EIGEN_PEAK_HOLD_MS.
    // A bursty transmission spikes the ratio for a fraction of a second - far
    // shorter than the UI status period - so the instantaneous readout misses
    // it; the held peak is what to set a squelch threshold against.
    float getEigenvalueRatioPeak() const;
    static constexpr int EIGEN_PEAK_HOLD_MS = 5000;

    // Eigenvalue-squelch publish gate. The FFT squelch freezes the DoA output
    // by skipping MUSIC entirely while closed; the eigenvalue squelch cannot
    // do that (the ratio it thresholds IS computed by MUSIC), so instead the
    // frame's ratio is always computed but the pseudospectrum/peak/stamp are
    // NOT updated while the gate is closed - the published DoA freezes at the
    // last open frame. The gate is hysteretic (opens at ratio >= threshold,
    // closes below threshold * EIGEN_GATE_HYSTERESIS) so a ratio hovering at
    // the threshold cannot alternate publish/freeze every frame.
    // threshold <= 0 disarms (e.g. auto mode still learning).
    // Lock-free; safe to call from the pipeline before each frame.
    void setSquelchGate(bool enabled, float threshold);

    // Hysteretic gate state of the most recently computed frame (true =
    // squelch closed / frame suppressed). The pipeline reports this as the
    // decimator's squelch_open so UI/audio agree with the freeze.
    bool isSquelchGateClosed() const {
        return gate_closed_state_.load(std::memory_order_relaxed);
    }

    // External publish hold, OR'd with the eigen gate: while set, frames are
    // computed (ratio/steering inputs stay fresh) but never published. Used
    // for the FFT-method + beamforming case, where MUSIC must keep running
    // for steering but a closed squelch still has to freeze the displayed DoA.
    void setPublishHold(bool hold) {
        publish_hold_.store(hold, std::memory_order_relaxed);
    }

    // Monotonic-per-frame stamp (system_clock ms) set whenever a NEW pseudospectrum
    // is published. Used by the DoA logger to dedup: the same frame is never logged
    // twice, and sub-frame logging intervals collapse to the real data rate.
    int64_t getResultStampMs() const { return result_stamp_ms_.load(std::memory_order_relaxed); }
    // True from a frequency change (centre retune or VFO move) until the next
    // published frame: the pseudospectrum still holds the OLD frequency's
    // bearing. Publishers of records (web mapper, DOA_value page, recorder)
    // skip it - they used to send the old bearing labelled with the new
    // frequency for the ~4 s retune hold.
    bool isResultStale() const { return result_stale_.load(std::memory_order_relaxed); }

    void setArrayTopology(ArrayTopology topology);
    ArrayTopology getArrayTopology() const;
    
    void setArrayRadius(float radius_mm);
    float getArrayRadius() const;
    
    void setElementSpacing(float spacing_mm);
    float getElementSpacing() const;

    void setFrequency(float freq_hz);

    // Angular grid: fixed at 1 degree / 360 bins. Sub-degree bearing accuracy
    // comes from the parabolic peak interpolation (see getPeakAngle()), not
    // from a finer grid, so the grid is not runtime-configurable.
    float getAngularResolution() const;
    int getNumAngles() const;

    // Number of expected signal sources (MUSIC signal-subspace dimension).
    // Clamped at compute time to [1, M-1] where M is the element count.
    void setNumSignalSources(int n);
    int getNumSignalSources() const;

    // ULA forward/backward output mode (resolves the linear-array front/back
    // ambiguity by truncating the pseudospectrum to one half-plane).
    void setULAOutputMode(ULAOutputMode mode);
    ULAOutputMode getULAOutputMode() const;

    // Array orientation offset (degrees). Added to the reported DoA so the
    // pseudospectrum/peak align with the physical array mounting / true north.
    void setArrayOffset(float degrees);
    float getArrayOffset() const;

    // Forward-backward averaging: R_fb = (R + J*conj(R)*J)/2. Doubles the
    // effective snapshot support and partially decorrelates coherent multipath.
    // Only valid for a centro-symmetric array, so it is applied at compute time
    // ONLY when topology == ULA (on a UCA, conjugating the covariance maps
    // arrivals to their antipodes and would create 180-degree ghosts).
    void setFBAveragingEnabled(bool enabled);
    bool isFBAveragingEnabled() const;

    // Temporal covariance smoothing across frames:
    //   R_avg = (1 - alpha) * R_avg + alpha * R_frame
    // alpha is the weight of the newest frame; 1.0 disables averaging.
    // The average is reset on frequency/config/topology changes.
    void setCovarianceAveragingAlpha(float alpha);
    float getCovarianceAveragingAlpha() const;

    // Automatic source-count estimation from the covariance eigenvalues.
    // When enabled, the signal-subspace dimension is estimated every frame and
    // the manual num_signal_sources value is ignored. An eigenvalue counts as
    // a source only when BOTH hold:
    //   1. it stands AUTO_SOURCES_THRESHOLD_DB above the running mean of the
    //      noise eigenvalues below it (separates it from the noise floor), AND
    //   2. it is within AUTO_SOURCES_REL_THRESHOLD_DB of the DOMINANT
    //      eigenvalue. Residual-calibration / dispersion / multipath leakage
    //      scatters a strong signal's power into secondary eigenvectors that
    //      scale with SIGNAL power - at high SNR they clear any noise-relative
    //      test, but they sit far below the dominant eigenvalue, which is what
    //      this criterion rejects.
    // The adopted count also has frame hysteresis (AUTO_SOURCES_STABLE_FRAMES)
    // so borderline eigenvalues can't flip it every update.
    // (Information criteria like MDL/AIC are deliberately NOT used: at N~8k
    // samples they flag the sub-dB gain/noise asymmetries of real tuners as
    // "sources" and saturate at M-1.)
    // getEstimatedNumSources() reports the latest adopted estimate (0 = no
    // source detected - the subspace still keeps one dimension; -1 = auto off
    // / no estimate yet).
    void setAutoNumSources(bool enabled);
    bool isAutoNumSources() const;
    int getEstimatedNumSources() const;

    // Custom array positions (for CUSTOM topology). The array is sized to the
    // DOA_NUM_ELEMENTS ceiling; the first getNumElements() entries are used.
    void setCustomPositions(const std::array<ElementPosition, DOA_NUM_ELEMENTS>& positions);
    std::array<ElementPosition, DOA_NUM_ELEMENTS> getCustomPositions() const;
    bool hasValidCustomPositions() const { return custom_positions_valid_; }

    // Runtime element count M currently in use (follows the global
    // active_num_elements, itself synced from the server's packet header).
    int getNumElements() const { return num_elements_; }

    // 2D MUSIC support (elevation estimation for 3D arrays)
    bool is3DArray() const { return is_3d_array_; }
    Eigen::VectorXd getElevationPseudospectrum() const;
    std::pair<int, int> getPeakAzimuthElevation() const;  // Returns (azimuth_deg, elevation_deg)
    int getNumElevationAngles() const { return num_elevation_angles_; }

    // Elevation resolution control (0.5 to 5.0 degrees)
    void setElevationResolution(float resolution_degrees);
    float getElevationResolution() const { return elevation_resolution_; }

    void enable();
    void disable();
    bool isEnabled() const;
    
    size_t getBlocksProcessed() const;
    size_t getTotalSamplesProcessed() const;
    
    // Control methods
    void resetAccumulator();
    
    // Performance monitoring
    struct ProcessingStats {
        size_t blocks_processed = 0;
        size_t snapshots_processed = 0;
        double buffer_fill_ratio = 0.0;
        double last_processing_time_ms = 0.0;      // Last MUSIC algorithm execution time
        double avg_processing_time_ms = 0.0;       // Average processing time
        size_t timing_samples = 0;                 // Number of timing samples collected
    };
    
    ProcessingStats getStats() const;

private:
    // Drop everything accumulated so far (samples + pending snapshots) so the
    // next frame is built only from contiguous new samples. Caller holds
    // config_mutex_ (takes accumulator_.buffer_mutex itself).
    void clearAccumulatorLocked();
    // Input continuity tracking for processDecimatedIQ: a gap longer than
    // MAX_INPUT_GAP (MUSIC skipped while the squelch is closed, calibrating or
    // retuning; dropped packets) or a sample-rate change (bandwidth switch)
    // clears the accumulator, so a frame never mixes pre- and post-gap data.
    static constexpr std::chrono::milliseconds MAX_INPUT_GAP{200};
    // Narrow-bandwidth frame cap (extractSnapshotsOptimized): a frame never
    // waits for more than this much signal, nor uses fewer snapshots than this
    static constexpr double MAX_FRAME_SECONDS = 1.0;
    static constexpr size_t MIN_NARROW_SNAPSHOTS = 4;
    std::chrono::steady_clock::time_point last_input_time_{};
    float last_input_rate_hz_ = 0.0f;
    // Configuration
    MUSICConfig config_;

    // Runtime element count M (2..DOA_NUM_ELEMENTS ceiling). Initialized from
    // active_num_elements in the constructor and re-synced at the start of
    // every processing pass (syncElementCount). Written only under
    // config_mutex_.
    int num_elements_ = DOA_DEFAULT_ELEMENTS;

    ArrayTopology current_topology;
    float array_radius_mm = 50.0f;
    float element_spacing_mm = 30.0f;
    float current_frequency = 100e6f;
    std::atomic<bool> processing_enabled{true};

    // Stamp (system_clock ms) of the most recently published pseudospectrum.
    // See getResultStampMs().
    std::atomic<int64_t> result_stamp_ms_{0};
    std::atomic<bool> result_stale_{false};   // see isResultStale()

    // Serializes the processing path against config setters (which resize
    // steering_vectors/pseudospectrum) and against result getters reading
    // pseudospectrum from other threads. Public API boundary only — private
    // helpers assume the caller holds it.
    mutable std::mutex config_mutex_;

    // Angular grid (fixed: 1 degree, 360 bins; peak readout is interpolated)
    float angular_resolution_ = 1.0f;
    int num_angles_ = 360;

    // Expected number of signal sources (signal-subspace dimension). The noise
    // subspace used by MUSIC is (M - num_signal_sources_) dimensional.
    int num_signal_sources_ = 1;

    // ULA front/back output handling (applied only when topology == ULA)
    ULAOutputMode ula_output_mode_ = ULAOutputMode::BOTH;

    // Array orientation offset in degrees (rotates the reported spectrum/peak)
    float array_offset_deg_ = 0.0f;
    // Sub-grid part of the offset that the whole-bin spectrum rotation can't
    // represent (|x| <= resolution/2); added back to the interpolated peak.
    float offset_residual_deg_ = 0.0f;

    // Forward-backward averaging (applied only when topology == ULA)
    bool fb_averaging_enabled_ = false;

    // Temporal covariance smoothing (weight of the newest frame; 1.0 = off)
    float covariance_alpha_ = 1.0f;
    Eigen::MatrixXcd covariance_avg_;      // Running average of trace-normalized R
    bool covariance_avg_valid_ = false;

    // Automatic source-count estimation (eigenvalue-dominance threshold)
    bool auto_num_sources_ = false;
    std::atomic<int> estimated_num_sources_{-1};   // Latest adopted estimate (-1 = none)

    // Hysteresis state for the auto estimate (guarded by config_mutex_):
    // a raw per-frame estimate must repeat AUTO_SOURCES_STABLE_FRAMES times
    // before being adopted. -1 = no adopted value yet (first frame adopts).
    int auto_k_current_ = -1;
    int auto_k_candidate_ = -1;
    int auto_k_streak_ = 0;

    // Restart the hysteresis (call when the underlying statistics change:
    // retune, topology/config change, averaging change, auto toggled).
    void resetAutoSourceTracking() {
        auto_k_current_ = -1;
        auto_k_candidate_ = -1;
        auto_k_streak_ = 0;
    }

    float DOA_inter_elem_space = 0.5f;

    // Custom element positions (for CUSTOM topology)
    std::array<ElementPosition, DOA_NUM_ELEMENTS> custom_positions_;
    bool custom_positions_valid_ = false;

    // 2D MUSIC support (elevation estimation for 3D arrays)
    bool is_3d_array_ = false;                 // True when any Z coordinate != 0
    int num_elevation_angles_ = 181;           // -90 to +90 degrees at 1 degree resolution
    float elevation_resolution_ = 1.0f;        // Degrees per elevation step

    // 2D steering vectors: M x (num_azimuth * num_elevation)
    Eigen::MatrixXcd steering_vectors_2d_;
    bool steering_vectors_2d_valid_ = false;

    // Separate spectrums for 2D MUSIC
    Eigen::VectorXd azimuth_pseudospectrum_;    // Marginalized over elevation
    Eigen::VectorXd elevation_pseudospectrum_;  // Marginalized over azimuth

    // 2D peak detection results
    int peak_azimuth_ = 0;     // Azimuth peak index (0 to num_angles_-1)
    int peak_elevation_ = 90;  // Elevation peak index (0=nadir, 90=horizon, 180=zenith for 1 deg res)

    // Optimized circular buffer accumulator
    struct OptimizedAccumulator {
        Eigen::MatrixXcd buffer;                     // Pre-allocated circular buffer
        size_t write_index = 0;                      // Current write position
        size_t samples_available = 0;                // Available samples for processing
        size_t buffer_size_mask = 0;                 // Mask for fast modulo (size - 1)
        size_t buffer_size = 0;                      // Actual buffer size (power of 2)
        std::mutex buffer_mutex;                     // Thread safety
        size_t total_samples = 0;                    // Total processed samples
        size_t global_sample_index = 0;              // Global sample counter
        
        // Helper function to round up to next power of 2
        static size_t nextPowerOf2(size_t n) {
            if (n == 0) return 1;
            n--;
            n |= n >> 1;
            n |= n >> 2;
            n |= n >> 4;
            n |= n >> 8;
            n |= n >> 16;
            if (sizeof(size_t) == 8) {
                n |= n >> 32;
            }
            return n + 1;
        }
        
        void resize(size_t requested_size, int num_channels) {
            // Round up to next power of 2 for efficient masking
            buffer_size = nextPowerOf2(requested_size);
            buffer_size_mask = buffer_size - 1;
            buffer.resize(num_channels, buffer_size);
            buffer.setZero();
            write_index = 0;
            samples_available = 0;
        }
    } accumulator_;

    
    // Snapshot management
    struct SnapshotManager {
        std::vector<Eigen::MatrixXcd> snapshots;     // Current snapshot collection
        bool snapshots_ready = false;
    } snapshot_mgr_;
    
    // Pre-allocated working matrices to avoid repeated allocations
    Eigen::MatrixXcd snapshot_working_matrix_;       // Working space for snapshot extraction
    Eigen::MatrixXcd correlation_matrix_;            // Pre-allocated correlation matrix
    Eigen::MatrixXcd combined_snapshots_;            // Combined snapshot data
    Eigen::MatrixXcd EH_S_working_;                  // Pre-allocated for noise subspace projection

    // Pre-allocated eigensolver to avoid repeated construction
    std::unique_ptr<Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd>> eigensolver_;
    bool eigensolver_initialized_ = false;
    
    // Steering vectors
    Eigen::MatrixXcd steering_vectors;
    bool steering_vectors_valid = false;
    
    // Output spectrum
    Eigen::VectorXd pseudospectrum;

    // Eigenvalue ratio for signal detection (computed during MUSIC processing)
    // λ1 / mean(λ2..λN) - high ratio indicates coherent signal
    std::atomic<float> eigenvalue_ratio_{1.0f};

    // Peak hold of the ratio (see getEigenvalueRatioPeak)
    std::atomic<float> eigenvalue_ratio_peak_{1.0f};
    std::atomic<int64_t> eigenvalue_peak_stamp_ms_{0};

    // Store a freshly computed ratio and maintain the held peak
    void storeEigenvalueRatio(float ratio);

    // Eigenvalue-squelch publish gate (see setSquelchGate)
    std::atomic<bool> squelch_gate_enabled_{false};
    std::atomic<float> squelch_gate_threshold_{0.0f};
    std::atomic<bool> gate_closed_state_{false};
    std::atomic<bool> publish_hold_{false};

    // Hysteresis: once open, close only below threshold * this factor (~1 dB)
    static constexpr float EIGEN_GATE_HYSTERESIS = 0.8f;

    // Whether the current frame updated the published pseudospectrum (false
    // when the squelch gate suppressed it). Written and read on the processing
    // path under config_mutex_.
    bool frame_published_ = true;

    // Evaluate the gate for this frame's just-stored ratio, with hysteresis,
    // and remember the state. Called once per computed frame.
    bool updateSquelchGate() {
        if (!squelch_gate_enabled_.load(std::memory_order_relaxed)) {
            gate_closed_state_.store(false, std::memory_order_relaxed);
            return false;
        }
        const float thr = squelch_gate_threshold_.load(std::memory_order_relaxed);
        if (thr <= 0.0f) {
            gate_closed_state_.store(false, std::memory_order_relaxed);
            return false;
        }
        const float ratio = eigenvalue_ratio_.load(std::memory_order_relaxed);
        bool closed = gate_closed_state_.load(std::memory_order_relaxed);
        if (closed) {
            if (ratio >= thr) closed = false;                        // reopen at full threshold
        } else {
            if (ratio < thr * EIGEN_GATE_HYSTERESIS) closed = true;  // close ~1 dB below
        }
        gate_closed_state_.store(closed, std::memory_order_relaxed);
        return closed;
    }

    // Statistics and optimization
    mutable std::mutex stats_mutex_;
    ProcessingStats stats_;
    size_t stats_update_counter_ = 0;                // Reduce stats update frequency
    
    // Timing measurement
    mutable std::chrono::steady_clock::time_point last_timing_start_;
    void startTiming();
    void endTiming();
    
    // Re-sync num_elements_ with the global active_num_elements; when it
    // changed, resizes all per-element state and regenerates steering vectors.
    // Caller must hold config_mutex_.
    void syncElementCount();
    void detect3DArray();  // caller holds config_mutex_

    // Private methods - Optimized accumulation (read-only input)
    void addToAccumulatorOptimized(const SharedDecimator::MultiChannelDecimated& decimated_data);
    bool extractSnapshotsOptimized();
    void getSnapshotWindowOptimized(size_t start_offset, Eigen::MatrixXcd& output);
    
    // Private methods - Optimized MUSIC processing with timing
    void computeCorrelationMatrixOptimized(const Eigen::MatrixXcd& X);
    void computeMUSICSpectrumOptimized();
    bool processSnapshotCollectionOptimized();

    // Eigenvalue-dominance estimate of the source count from ascending
    // eigenvalues: grow the noise set from the smallest eigenvalue upward and
    // count everything > AUTO_SOURCES_THRESHOLD_DB above its running mean.
    int estimateNumSources(const Eigen::VectorXd& eigenvalues) const;

    // How far (dB) an eigenvalue must stand above the noise-eigenvalue mean to
    // count as a source. 6 dB ~= per-channel SNR of -2 dB with 5 elements -
    // weaker sources barely perturb the subspace and are better treated as
    // noise. Well above real-hardware eigenvalue spread (sub-dB).
    static constexpr float AUTO_SOURCES_THRESHOLD_DB = 6.0f;

    // How close (dB) an eigenvalue must be to the dominant one to count as a
    // source. Leakage from residual calibration error / band dispersion /
    // partially-decorrelated multipath typically rides 15-25 dB below the
    // signal; a genuine co-channel emitter worth resolving is closer than
    // this. Raise toward 15 to catch weaker second transmitters, lower toward
    // 10 if a dynamic multipath environment still over-counts.
    static constexpr float AUTO_SOURCES_REL_THRESHOLD_DB = 12.0f;

    // Consecutive frames a new estimate must persist before being adopted
    // (borderline eigenvalues otherwise flip the count frame to frame).
    static constexpr int AUTO_SOURCES_STABLE_FRAMES = 3;

    // Signal-subspace dimension for this frame (auto-estimated or the manual
    // expectation), clamped to [1, M-1]. Caller must have run eigensolver_.
    int resolveSignalDimension(int M);

    // Sub-grid peak refinement: 3-point parabolic fit on ln(pseudospectrum)
    // around max_idx with circular indexing. Returns the interpolated angle in
    // degrees [0, 360). Falls back to the grid angle at a masked-bin edge
    // (ULA half-plane zeros). Caller must hold config_mutex_.
    float interpolatePeakAngle(Eigen::Index max_idx, double max_val) const;

    void updateSteeringVectors();

    // Apply ULA forward/backward truncation and array-offset rotation to the
    // final azimuth `pseudospectrum`. Caller must hold config_mutex_.
    void applyOutputTransforms();

    // 2D MUSIC private methods
    void updateSteeringVectors2D();           // Compute 2D steering vectors for 3D array
    void computeMUSICSpectrum2D();            // Compute 2D pseudospectrum
    void marginalizeSpectrums(const Eigen::VectorXd& spectrum_2d);  // Marginalize to 1D spectrums
    void initializeCustomPositionsToUCA();    // Initialize custom positions to UCA equivalent
};