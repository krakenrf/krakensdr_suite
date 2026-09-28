#pragma once

// Beamformer - Coherent multi-channel combining steered by MUSIC DoA
//
// Reconstructed from the shipped fft_viewer binary (symbols + disassembly)
// after the original source files were lost. Interface, defaults and
// algorithm behavior match the original implementation:
//   - DELAY_AND_SUM:       time-domain phase-aligned coherent sum
//   - FREQ_DOMAIN_DAS:     256-pt FFT overlap-add with per-bin steering
//   - MVDR:                minimum variance distortionless response (Eigen LDLT)
//   - SELECTION_DIVERSITY: picks the channel with best FFT-estimated SNR
//
// Array geometry is synced from the MUSIC processor (topology, radius,
// spacing, frequency). Radius/spacing are in millimeters.

#include <atomic>
#include <complex>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

#include <Eigen/Dense>
#include <fftw3.h>

#include "types.hpp"                                // ArrayTopology
#include "signal_processing/shared_decimator.hpp"   // SharedDecimator::MultiChannelDecimated
#include "config.hpp"                               // DOA_NUM_ELEMENTS

enum class BeamformingMode {
    DELAY_AND_SUM = 0,
    MVDR = 1,
    SELECTION_DIVERSITY = 2,
    FREQ_DOMAIN_DAS = 3
};

class Beamformer {
public:
    // Statistics snapshot returned to the UI (see message_builders.cpp)
    struct Stats {
        uint64_t total_blocks_processed = 0;
        uint64_t total_samples_processed = 0;
        float current_steering_angle = 0.0f;
        float estimated_snr_improvement_db = 0.0f;
        BeamformingMode current_mode = BeamformingMode::DELAY_AND_SUM;
        float mvdr_condition_number = 0.0f;
        int selected_channel = 0;          // Selection diversity: active channel
        float selected_channel_snr = 0.0f; // Selection diversity: its SNR (dB)
        float channel_snrs[8] = {};        // Per-channel SNR estimates (dB)
    };

    Beamformer();
    ~Beamformer();

    Beamformer(const Beamformer&) = delete;
    Beamformer& operator=(const Beamformer&) = delete;

    // Enable/disable coherent combining
    void enable();
    void disable();
    bool isEnabled() const;

    // Steering angle in degrees [0, 360), normally driven by MUSIC DoA
    void setSteeringAngle(float angle_deg);
    float getSteeringAngle() const;

    // Array geometry (synced from MUSIC processor each block)
    void setArrayTopology(ArrayTopology topology);
    ArrayTopology getArrayTopology() const;
    void setArrayRadius(float radius_mm);
    float getArrayRadius() const;
    void setElementSpacing(float spacing_mm);
    float getElementSpacing() const;
    void setFrequency(float freq_hz);
    float getFrequency() const;
    void setSampleRate(float rate_hz);
    float getSampleRate() const;

    // Beamforming algorithm selection
    void setBeamformingMode(BeamformingMode mode);
    BeamformingMode getBeamformingMode() const;

    // MVDR tuning
    void setMVDRDiagonalLoading(float alpha);     // clamped to [0.01, 1.0]
    float getMVDRDiagonalLoading() const;
    void setMVDRSnapshotLength(size_t snapshots); // clamped to [64, 4096]
    size_t getMVDRSnapshotLength() const;

    // Main entry point: combine all channels into a single output stream.
    // Returns true if output contains valid beamformed samples.
    bool process(const SharedDecimator::MultiChannelDecimated& input,
                 std::vector<std::complex<float>>& output);

    // Selection diversity: currently selected channel index
    int getSelectedChannel() const;

    Stats getStats() const;
    void resetStats();

private:
    // Compile-time CEILING (sizes the stack pointer arrays); the count
    // actually combined is the runtime num_elements_ below.
    static constexpr int NUM_ELEMENTS = DOA_NUM_ELEMENTS;
    static constexpr int SNR_FFT_SIZE = 256;   // SNR estimation FFT
    static constexpr int FDDAS_FFT_SIZE = 256; // Frequency-domain DAS FFT
    static constexpr int FDDAS_HOP = FDDAS_FFT_SIZE / 2; // 50% overlap

    // Re-sync num_elements_ with the global active_num_elements; when it
    // changed, resizes all per-element state (steering, MVDR, FD-DAS).
    // Caller must hold config_mutex_.
    void syncElementCount();

    // Geometry / steering
    void computeElementPositions();
    void updateSteeringVector();

    // Algorithms (fill output; leave empty on failure)
    void delayAndSum(const SharedDecimator::MultiChannelDecimated& input,
                     std::vector<std::complex<float>>& output);
    void freqDomainDAS(const SharedDecimator::MultiChannelDecimated& input,
                       std::vector<std::complex<float>>& output);
    void mvdrBeamform(const SharedDecimator::MultiChannelDecimated& input,
                      std::vector<std::complex<float>>& output);
    void selectionDiversity(const SharedDecimator::MultiChannelDecimated& input,
                            std::vector<std::complex<float>>& output);

    // MVDR internals
    void updateCovarianceMatrix(const SharedDecimator::MultiChannelDecimated& input);
    void computeMVDRWeights();

    // SNR estimation: peak minus 10th-percentile noise floor over a
    // 256-point FFT, in dB
    float computeChannelSNR_FFT(const std::vector<std::complex<float>>& samples);

    // Frequency-domain DAS internals
    void initFreqDomainDAS();
    void cleanupFreqDomainDAS();
    void updateFDDASSteeringPhases();

    // Gather per-channel sample pointers; returns min sample count over all
    // NUM_ELEMENTS channels, or 0 if any channel is missing/empty
    size_t gatherChannels(const SharedDecimator::MultiChannelDecimated& input,
                          const std::complex<float>* (&ptrs)[NUM_ELEMENTS]) const;

    // Serializes process() (data thread) against the mode/MVDR/geometry
    // setters (control thread): mode_, mvdr_* and the covariance accumulator
    // are multi-word state that must not change mid-block.
    mutable std::mutex config_mutex_;

    // --- Array configuration ---
    // Runtime element count M (2..NUM_ELEMENTS ceiling). Initialized from
    // active_num_elements in the constructor, re-synced at the start of every
    // process() call. Written only under config_mutex_.
    int num_elements_ = DOA_DEFAULT_ELEMENTS;

    ArrayTopology topology_ = ArrayTopology::UCA;
    float array_radius_mm_ = 50.0f;
    float element_spacing_mm_ = 30.0f;
    float frequency_hz_ = 100e6f;
    float steering_angle_deg_ = 0.0f;
    std::atomic<bool> enabled_{false};
    BeamformingMode mode_ = BeamformingMode::DELAY_AND_SUM;

    // Steering vector (one complex weight per element)
    Eigen::VectorXcd steering_vector_;
    bool steering_vector_valid_ = false;

    // Element positions in wavelengths
    std::vector<double> element_x_;
    std::vector<double> element_y_;

    // --- MVDR state ---
    float mvdr_diagonal_loading_ = 0.5f;
    size_t mvdr_snapshot_length_ = 256;
    Eigen::MatrixXcd covariance_;        // Smoothed/loaded covariance R
    Eigen::VectorXcd mvdr_weights_;
    bool mvdr_weights_valid_ = false;
    float mvdr_condition_number_ = 0.0f;
    Eigen::MatrixXcd covariance_accum_;  // Snapshot accumulator
    size_t snapshot_count_ = 0;

    // --- Statistics ---
    mutable std::mutex stats_mutex_;
    Stats stats_;
    std::atomic<int> selected_channel_{0};
    float channel_snrs_[8] = {};

    // --- SNR estimation FFT (256-point, plan shared, mutex guarded) ---
    fftwf_plan snr_plan_ = nullptr;
    fftwf_complex* snr_in_ = nullptr;
    fftwf_complex* snr_out_ = nullptr;
    mutable std::mutex snr_mutex_;
    std::vector<float> snr_db_buffer_;
    std::vector<float> snr_sorted_buffer_;

    // SNR improvement update pacing (recomputed every 10th process() call)
    int snr_update_counter_ = 0;
    float last_snr_improvement_db_ = 0.0f;

    float sample_rate_hz_ = 240000.0f;

    // --- Frequency-domain DAS state ---
    mutable std::mutex fdds_mutex_;
    fftwf_plan fdds_fwd_plan_ = nullptr;
    fftwf_plan fdds_inv_plan_ = nullptr;
    fftwf_complex* fdds_fft_in_ = nullptr;
    fftwf_complex* fdds_fft_out_ = nullptr;
    fftwf_complex* fdds_ifft_in_ = nullptr;
    fftwf_complex* fdds_ifft_out_ = nullptr;
    std::vector<float> fdds_window_;     // Hann window
    // Per-channel, per-bin steering phasors [channel][bin]
    std::vector<std::vector<std::complex<float>>> fdds_steering_;
    // Per-channel input history for 50% overlap processing
    std::vector<std::vector<std::complex<float>>> fdds_history_;
    // Overlap-add accumulator carried between blocks
    std::vector<std::complex<float>> fdds_overlap_;
    float fdds_last_angle_ = -1000.0f;   // Sentinel: force initial update
    float fdds_last_freq_ = 0.0f;
};
