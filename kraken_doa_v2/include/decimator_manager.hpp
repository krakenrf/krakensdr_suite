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
#include "talker_doa.hpp"

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
        std::atomic<int> tuner_channel;  // Tuner this VFO decimates in wideband / independent mode
        std::atomic<DemodulatorMode> demod_mode;  // Per-decimator demodulator mode

        // Per-decimator squelch settings
        // Default squelch level is 15dB above normalized noise floor (0dB)
        std::atomic<bool> squelch_enabled{false};
        std::atomic<float> squelch_level{15.0f};
        std::atomic<bool> squelch_open{false};  // True = signal present, False = squelched
        // Method (SquelchMethod as int): 0 = FFT peak vs squelch_level (dB),
        // 3 = DIGITAL (the VFO's decoder; only in effect while it is on -
        // effectiveSquelchMethod)
        std::atomic<int> squelch_method{0};

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

        // Position of the next decimated sample in this VFO's stream
        // (MultiChannelDecimated::stream_pos; the pipeline advances it)
        std::atomic<uint64_t> stream_pos{0};
        // DoA per talker (talker_doa.hpp): this VFO's MUSIC frames (frame tap)
        // cut by its digital decoder's talker spans. Never replaced.
        std::shared_ptr<tdoa::TalkerDoa> talker_doa;

        DecimatorInstance(int _id)
            : id(_id)
            , decimator(std::make_unique<SharedDecimator>())
            , music_processor(std::make_unique<MUSICProcessor>())
            , beamformer(std::make_unique<Beamformer>())
            , frequency_offset_hz(0.0f)
            , bandwidth_index(DEFAULT_BANDWIDTH_INDEX)
            , enabled(true)
            , tuner_channel(0)
            , demod_mode(DemodulatorMode::WBFM) {
            talker_doa = std::make_shared<tdoa::TalkerDoa>();
            auto td = talker_doa;
            music_processor->setFrameTap(
                [td](const Eigen::MatrixXcd& R, uint64_t a, uint64_t b, float rate, double freq, float ratio) {
                    if (td->active()) td->add_frame(R, a, b, rate, freq, ratio);
                });
        }
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
    // the VFO's decoder, created on first use (caller holds digital_mu)
    static void ensureDigital(DecimatorInstance& inst);
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
    bool setSquelchMethod(int id, int method);            // 0 = FFT, 3 = DIGITAL (anything else = FFT)

    // The Digital squelch: open while the VFO's decoder reports valid frames
    // (and this long after the last one), closed otherwise; the DoA then
    // comes from exactly the confirmed samples (talker_doa.cpp)
    static constexpr int64_t DIGITAL_SQUELCH_HOLD_MS = 1500;
    // The method in effect: DIGITAL only while the VFO's digital decoder is on
    static SquelchMethod effectiveSquelchMethod(const DecimatorInstance& inst);
    static bool digitalSquelchOpen(const DecimatorInstance& inst);

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
        int squelch_method;            // 0 = FFT, 3 = DIGITAL
        int digital_mode;              // dig::Mode
        dig::Options digital_opts;
        std::string digital_plugin;    // plugin id (Mode::PLUGIN)
    };

    std::vector<DecimatorInfo> getDecimatorInfoList() const;
};

// Global instance
extern DecimatorManager decimator_manager;