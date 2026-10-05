#pragma once

#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include "signal_processing/shared_decimator.hpp"
#include "signal_processing/music_processor.hpp"
#include "signal_processing/fm_demodulator.hpp"
#include "signal_processing/beamformer.hpp"
#include "signal_processing/beamformed_fft.hpp"
#include "digital/digital_decoder.hpp"

class DecimatorManager {
public:
    struct DecimatorInstance {
        int id;
        std::unique_ptr<SharedDecimator> decimator;
        std::unique_ptr<MUSICProcessor> music_processor;
        // Per-decimator coherent beamformer (steered by this decimator's DoA)
        // and the beamformed-FFT slice it produces for the UI overlay/squelch.
        std::unique_ptr<Beamformer> beamformer;
        BeamformedFFTData beamformed_fft;
        // Tuning fields are written on the uWS loop (VFO commands, scanner)
        // while the decimation pass / FM path / status builders read them on
        // other threads - atomic, like the squelch fields below (they were
        // plain fields: a data race, i.e. undefined behaviour).
        std::atomic<float> frequency_offset_hz;
        std::atomic<int> bandwidth_index;
        std::atomic<bool> enabled;
        std::atomic<bool> being_deleted{false};
        std::atomic<int> wideband_tuner_channel;  // Which tuner channel to use in wideband mode (0-4)
        std::atomic<DemodulatorMode> demod_mode;  // Per-decimator demodulator mode

        // Per-decimator squelch settings
        // Default squelch level is 15dB above normalized noise floor (0dB)
        std::atomic<bool> squelch_enabled{false};
        std::atomic<float> squelch_level{15.0f};
        std::atomic<bool> squelch_open{false};  // True = signal present, False = squelched
        // Method: 0 = FFT peak vs squelch_level (dB), 1 = EIGENVALUE
        // (λ1/mean(λ2..N) from this decimator's MUSIC vs squelch_eigen_threshold),
        // 2 = EIGENVALUE_AUTO (self-learned threshold, resets on VFO moves)
        std::atomic<int> squelch_method{0};
        std::atomic<float> squelch_eigen_threshold{2.0f};

        // EIGENVALUE_AUTO state (written by the pipeline task, read by the FM
        // audio path / status builder). floor tracks the ratio's noise floor;
        // threshold = max(floor * margin, minimum). 0 = not learned yet (the
        // squelch stays open until the first frames teach the floor).
        std::atomic<float> auto_eigen_floor{0.0f};
        std::atomic<float> auto_eigen_threshold{0.0f};
        std::atomic<float> auto_eigen_freq{0.0f};   // VFO freq the floor was learned at
        std::atomic<int64_t> auto_eigen_reset_ms{0}; // steady_clock ms of the last reset

        // Digital voice/data decoder (P25 / DMR / TETRA / D-STAR, see
        // digital/digital_decoder.hpp). Created on first use and kept while
        // the VFO lives (its worker thread idles when the mode is OFF). Set on
        // the uWS loop, read by the pipeline: guarded by digital_mu.
        mutable std::mutex digital_mu;
        std::shared_ptr<dig::DigitalDecoder> digital;
        std::atomic<int> digital_mode{0};   // dig::Mode, mirrors digital->mode()
        std::shared_ptr<dig::DigitalDecoder> getDigital() const {
            std::lock_guard<std::mutex> lk(digital_mu);
            return digital;
        }

        DecimatorInstance(int _id)
            : id(_id)
            , decimator(std::make_unique<SharedDecimator>())
            , music_processor(std::make_unique<MUSICProcessor>())
            , beamformer(std::make_unique<Beamformer>())
            , frequency_offset_hz(0.0f)
            , bandwidth_index(DEFAULT_BANDWIDTH_INDEX)
            , enabled(true)
            , wideband_tuner_channel(0)
            , demod_mode(DemodulatorMode::WBFM) {}
    };

private:
    // shared_ptr ownership: pipeline tasks, the scanner and the DoA HTTP
    // handler hold references across lock releases, so removal must not
    // destroy an instance while a holder is still using it. removeDecimator
    // just erases from this vector; the instance dies with its last holder.
    std::vector<std::shared_ptr<DecimatorInstance>> decimators;
    mutable std::mutex decimator_mutex;
    std::atomic<int> next_id{0};
    std::atomic<int> fm_decimator_id{0}; // Which decimator feeds the FM demodulator
    std::atomic<double> last_process_ms_{0.0}; // Duration of the last decimation pass (see getLastProcessMs)
    std::atomic<int> digital_active_{0};      // VFOs with a digital decoder on
    void recountDigital();
    static constexpr int MAX_DECIMATORS = 16;

    // Beamforming config shared by every decimator's beamformer. Stored here so
    // newly-created decimators inherit the current settings (apply via
    // applyBeamformingConfig). The master enable gate is the global
    // beamforming_enabled atomic; bf_enabled_ mirrors it for inheritance.
    std::atomic<bool> bf_enabled_{false};
    std::atomic<int> bf_mode_{0};                 // BeamformingMode
    std::atomic<float> bf_mvdr_loading_{0.5f};
    std::atomic<int> bf_mvdr_snapshots_{256};
    void applyBeamformingConfig(const std::shared_ptr<DecimatorInstance>& inst);

public:
    DecimatorManager();
    ~DecimatorManager();

    // Initialize with default number of decimators
    void initialize(int num_channels, int initial_decimators = 1);
    void cleanup();

    // Add/remove decimators dynamically
    int addDecimator();
    bool removeDecimator(int id);

    // Get decimator by ID (shared_ptr keeps it alive past removeDecimator)
    std::shared_ptr<DecimatorInstance> getDecimator(int id);
    std::shared_ptr<const DecimatorInstance> getDecimator(int id) const;

    // Get all decimators
    std::vector<std::shared_ptr<DecimatorInstance>> getAllDecimators();
    size_t getDecimatorCount() const;

    // Wall-clock duration of the most recent decimation pass, in
    // milliseconds. Surfaced live on the status dashboard (replaces the old
    // periodic "PERFORMANCE WARNING: parallel decimation..." log line).
    double getLastProcessMs() const { return last_process_ms_.load(std::memory_order_relaxed); }
    void setLastProcessMs(double ms) { last_process_ms_.store(ms, std::memory_order_relaxed); }

    // Set which decimator feeds the FM demodulator
    void setFMDecimatorId(int id) { fm_decimator_id = id; }
    int getFMDecimatorId() const { return fm_decimator_id.load(); }

    // Beamforming config - applied to every decimator's beamformer and
    // remembered so new decimators inherit it. Called from the control handler.
    void setBeamformingEnabledAll(bool enabled);
    void setBeamformingModeAll(BeamformingMode mode);
    void setMVDRDiagonalLoadingAll(float alpha);
    void setMVDRSnapshotLengthAll(size_t snapshots);

    // Update decimator parameters
    bool setFrequencyOffset(int id, float offset_hz);
    bool setBandwidthIndex(int id, int index);
    bool setEnabled(int id, bool enabled);
    bool setDemodMode(int id, DemodulatorMode mode);
    // Digital decoder of a VFO: mode OFF/AUTO/P25/DMR/TETRA/DSTAR and options
    // plugin = plugin id for dig::Mode::PLUGIN
    bool setDigitalMode(int id, dig::Mode mode, const std::string& plugin = "");
    bool setDigitalOptions(int id, const dig::Options& opts);
    bool anyDigitalActive() const { return digital_active_.load(std::memory_order_relaxed) > 0; }

    // Squelch settings
    bool setSquelchEnabled(int id, bool enabled);
    bool setSquelchLevel(int id, float level_db);
    bool setSquelchMethod(int id, int method);            // 0 = FFT, 1 = EIGENVALUE, 2 = EIGENVALUE_AUTO
    bool setSquelchEigenThreshold(int id, float threshold);

    // EIGENVALUE_AUTO: feed one fresh ratio measurement (per MUSIC frame) and
    // get back the current self-learned threshold. Tracks the ratio's noise
    // floor and resets when the VFO frequency moves. Called from the pipeline
    // task; the FM audio path reads inst.auto_eigen_threshold directly.
    static constexpr float AUTO_EIGEN_MARGIN = 2.5f;         // threshold = floor x margin
    static constexpr float AUTO_EIGEN_MIN_THRESHOLD = 1.3f;  // never below noise-ratio jitter
    static constexpr float AUTO_EIGEN_FLOOR_ALPHA = 0.05f;   // upward EMA rate on noise frames
    // Disambiguation between "VFO landed on a continuous signal" (squelch must
    // open) and "coherent elevated noise floor" (spurs/sub-floor carriers can
    // hold the idle λ at 4-6 on some frequencies; squelch must learn it and
    // stay closed): the λ statistic alone cannot tell them apart - both are
    // sustained rank-1 coherent power. The FFT can: a real transmission shows
    // an in-band peak above the (0 dB-normalized) FFT noise floor, coherent
    // sub-floor junk does not. So floor LEARNING is allowed only while the
    // in-band FFT peak is below this level; while a visible signal is present
    // the floor is left untouched (an unlearned floor keeps threshold = 0 =
    // squelch open). Landing on a broadcast therefore opens by default, and
    // the true idle floor is learned from the first quiet moment.
    // 7 dB: a visible transmission freezes learning. Must sit ABOVE the
    // averaged-FFT in-band noise-peak statistics (~2-4 dB over the normalized
    // floor) so pure/junk floors keep learning, and BELOW moderate signals
    // (a marginal broadcast peaks 10-13 dB here - a 12 dB bar straddled it,
    // and momentary dips let the learner seed from the signal and squelch it).
    static constexpr float AUTO_EIGEN_FFT_SIGNAL_DB = 7.0f;
    // Skip +-N FFT bins around the spectrum center in the learn-gate peak
    // search so a residual DC spike can't freeze learning forever at offset 0.
    static constexpr int AUTO_EIGEN_FFT_DC_EXCLUDE_BINS = 3;
    // No learning for this long after a reset (VFO move / mode entry): the
    // first blocks after a retune or decimator rebuild are settling transients
    // whose ratio must not seed the floor.
    static constexpr int AUTO_EIGEN_SETTLE_MS = 2000;
    // allow_learn = false freezes seeding and upward drift of the floor (the
    // fast attack downward always applies); pass it from the FFT signal check.
    static float updateAutoEigenThreshold(DecimatorInstance& inst, float ratio, float vfo_freq_hz,
                                          bool allow_learn);
    bool setSquelchOpen(int id, bool open);

    // Helper to convert demod mode to/from string
    static std::string demodModeToString(DemodulatorMode mode);
    static DemodulatorMode stringToDemodMode(const std::string& str);

    // Per-decimator result of one pipeline pass (data_receiver.cpp)
    struct ProcessResult {
        int decimator_id;
        SharedDecimator::MultiChannelDecimated decimated_data;
        bool is_fm_source;
        // Kept alive for the post-barrier beamforming pass (steering + FFT).
        std::shared_ptr<DecimatorInstance> instance;
        // Beamformed combined stream for this decimator (DAS/MVDR/etc).
        std::vector<std::complex<float>> beamformed_samples;
        bool have_beamformed = false;
        bool beamformer_ran = false;   // beamformer invoked for this block (output may be empty while FD-DAS buffers)
    };

    // Get decimator info for UI
    struct DecimatorInfo {
        int id;
        float frequency_offset_hz;
        int bandwidth_index;
        float bandwidth_mhz;
        bool enabled;
        bool is_fm_source;
        DemodulatorMode demod_mode;
        bool squelch_enabled;
        float squelch_level;
        bool squelch_open;
        int squelch_method;            // 0 = FFT, 1 = EIGENVALUE
        float squelch_eigen_threshold;
        int digital_mode;              // dig::Mode
        dig::Options digital_opts;
        std::string digital_plugin;    // plugin id (Mode::PLUGIN)
    };

    std::vector<DecimatorInfo> getDecimatorInfoList() const;
};

// Global instance
extern DecimatorManager decimator_manager;