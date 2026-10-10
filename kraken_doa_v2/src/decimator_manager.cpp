// ============================================
// src/decimator_manager.cpp
// HYBRID PARALLELIZATION: Per-decimator-stage threading
// ============================================

#include "decimator_manager.hpp"
#include "config.hpp"
#include "channel_manager.hpp"
#include "signal_processing/fft_processor.hpp"
#include <atomic>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <thread>
#include <chrono>

DecimatorManager decimator_manager;

extern std::atomic<int> active_channel;

DecimatorManager::DecimatorManager() {}

DecimatorManager::~DecimatorManager() {
    cleanup();
}

void DecimatorManager::initialize(int num_channels, int initial_decimators) {
    std::lock_guard<std::mutex> lock(decimator_mutex);

    decimators.clear();
    next_id = 0;

    // Create initial decimators
    for (int i = 0; i < initial_decimators; ++i) {
        auto instance = std::make_shared<DecimatorInstance>(next_id++);
        instance->decimator->initialize(num_channels);
        instance->decimator->setBandwidthIndex(instance->bandwidth_index);
        std::cout << "Decimator " << instance->id << " initialized: bandwidth_index=" << instance->bandwidth_index
                  << ", decimation=" << instance->decimator->getDecimationFactor()
                  << ", bandwidth=" << instance->decimator->getBandwidthMhz() << " MHz" << std::endl;
        // MUSICProcessor and Beamformer initialize automatically in constructor
        applyBeamformingConfig(instance);
        decimators.push_back(std::move(instance));
    }

    // First decimator feeds FM by default
    fm_decimator_id = 0;

    std::cout << "DecimatorManager initialized with " << initial_decimators
              << " decimator(s) for " << num_channels << " channels" << std::endl;

    // The Beamformer constructors above created FFTW_MEASURE plans that the
    // startup wisdom file may not cover (it is only written once, before any
    // decimator exists). Re-export now so the next boot skips the measurement
    // (which takes many seconds on a busy Pi).
    FFTProcessor::export_wisdom();
}

void DecimatorManager::cleanup() {
    std::lock_guard<std::mutex> lock(decimator_mutex);

    for (auto& instance : decimators) {
        instance->decimator->cleanup();
        // MUSICProcessor cleanup is automatic in destructor
    }

    decimators.clear();
}

int DecimatorManager::addDecimator() {
    // Read BEFORE decimator_mutex: ChannelManager::update_channel_info holds
    // channel_info_mutex while it walks the decimators (decimator_mutex), so
    // taking them in the other order here could deadlock the uWS loop and the
    // data receiver (a FREQ replay followed by DECIMATORS:, or ADD_DECIMATOR
    // during a retune)
    const float tuner_rf = ChannelManager::get_frequency(active_channel.load(std::memory_order_relaxed));
    std::lock_guard<std::mutex> lock(decimator_mutex);

    if (decimators.size() >= MAX_DECIMATORS) {
        std::cerr << "Maximum number of decimators (" << MAX_DECIMATORS << ") reached" << std::endl;
        return -1;
    }

    // Find the smallest available ID by checking which IDs are in use
    int new_id = 0;
    bool found = false;
    for (int candidate_id = 0; candidate_id < MAX_DECIMATORS; ++candidate_id) {
        bool id_in_use = false;
        for (const auto& dec : decimators) {
            if (dec->id == candidate_id) {
                id_in_use = true;
                break;
            }
        }
        if (!id_in_use) {
            new_id = candidate_id;
            found = true;
            break;
        }
    }

    if (!found) {
        std::cerr << "Could not find available decimator ID" << std::endl;
        return -1;
    }

    auto instance = std::make_shared<DecimatorInstance>(new_id);

    // Initialize with current number of channels (assume same as first decimator)
    if (!decimators.empty()) {
        instance->decimator->initialize(MAX_CHANNELS);

        // Copy settings from the first decimator to maintain consistency
        const auto& reference = decimators[0];
        if (reference) {
            // Copy bandwidth setting
            instance->bandwidth_index = reference->bandwidth_index.load();
            instance->decimator->setBandwidthIndex(reference->bandwidth_index);

            // DO NOT copy frequency offset - new decimators start at 0 Hz (center)
            // User will position them via UI
            // instance->frequency_offset_hz stays at default 0.0f

            if (reference->music_processor) {
                instance->music_processor->setArrayTopology(reference->music_processor->getArrayTopology());
                instance->music_processor->setArrayRadius(reference->music_processor->getArrayRadius());
                instance->music_processor->setElementSpacing(reference->music_processor->getElementSpacing());
                instance->music_processor->setNumSignalSources(reference->music_processor->getNumSignalSources());
                instance->music_processor->setAutoNumSources(reference->music_processor->isAutoNumSources());
                instance->music_processor->setULAOutputMode(reference->music_processor->getULAOutputMode());
                instance->music_processor->setCustomOutputMode(reference->music_processor->getCustomOutputMode());
                instance->music_processor->setArrayOffset(reference->music_processor->getArrayOffset());
                instance->music_processor->setFBAveragingEnabled(reference->music_processor->isFBAveragingEnabled());
                instance->music_processor->setCovarianceAveragingAlpha(reference->music_processor->getCovarianceAveragingAlpha());
                // Custom element positions, snapshot config and elevation grid
                // too: they are array-wide settings, and the settings replay
                // (CUSTOM_POSITIONS / MUSIC_NUM_SNAPSHOTS / ... before
                // DECIMATORS:) only reaches the VFOs that exist at that moment -
                // VFOs added afterwards came up with the default 50 mm UCA
                // positions under CUSTOM topology (wrong bearings).
                instance->music_processor->setConfig(reference->music_processor->getConfig());
                instance->music_processor->setElevationResolution(reference->music_processor->getElevationResolution());
                if (reference->music_processor->hasValidCustomPositions()) {
                    instance->music_processor->setCustomPositions(reference->music_processor->getCustomPositions(),
                                                                  reference->music_processor->getCustomPositionsCount());
                }

                // Frequency: the tuner RF at THIS VFO's offset (0 Hz), not the
                // reference VFO's effective frequency (RF + its offset)
                if (tuner_rf > 0) {
                    instance->music_processor->setFrequency(tuner_rf);
                }
            }
        }
    }

    // Inherit the current beamforming settings (mode/MVDR/enabled).
    applyBeamformingConfig(instance);

    float new_offset = instance->frequency_offset_hz;
    int new_bw_idx = instance->bandwidth_index;

    decimators.push_back(std::move(instance));

    std::cout << "Added new decimator with ID " << new_id
              << " (offset=" << (new_offset / 1000.0f) << " kHz, "
              << "bw_idx=" << new_bw_idx << ")" << std::endl;
    return new_id;
}

bool DecimatorManager::removeDecimator(int id) {
    // The instance is shared_ptr-owned: erasing it here only drops the
    // manager's reference. Any pipeline task / scanner / HTTP handler still
    // holding a reference keeps it alive until it finishes, so no settling
    // delay is needed and no use-after-free is possible.
    std::shared_ptr<DecimatorInstance> removed;
    {
        std::lock_guard<std::mutex> lock(decimator_mutex);

        // Don't remove if it's the last decimator
        if (decimators.size() <= 1) {
            std::cerr << "Cannot remove the last decimator" << std::endl;
            return false;
        }

        auto it = std::find_if(decimators.begin(), decimators.end(),
            [id](const std::shared_ptr<DecimatorInstance>& inst) {
                return inst->id == id;
            });

        if (it == decimators.end()) {
            return false;
        }

        // Mark as being deleted so other threads skip it for new work
        (*it)->being_deleted = true;

        // If this was the FM source, switch to first available
        if (fm_decimator_id == id && !decimators.empty()) {
            for (const auto& dec : decimators) {
                if (dec->id != id) {
                    fm_decimator_id = dec->id;
                    std::cout << "FM source switched to decimator " << dec->id << std::endl;
                    break;
                }
            }
        }

        removed = std::move(*it);
        decimators.erase(it);
    }

    // Destruction (possibly deferred to the last holder) happens outside the
    // lock; SharedDecimator/MUSICProcessor clean up in their destructors.
    removed.reset();
    recountDigital();
    std::cout << "Removed decimator with ID " << id << std::endl;
    return true;
}

std::shared_ptr<DecimatorManager::DecimatorInstance> DecimatorManager::getDecimator(int id) {
    std::lock_guard<std::mutex> lock(decimator_mutex);

    auto it = std::find_if(decimators.begin(), decimators.end(),
        [id](const std::shared_ptr<DecimatorInstance>& inst) {
            return inst->id == id;
        });

    return (it != decimators.end()) ? *it : nullptr;
}

std::shared_ptr<const DecimatorManager::DecimatorInstance> DecimatorManager::getDecimator(int id) const {
    std::lock_guard<std::mutex> lock(decimator_mutex);

    auto it = std::find_if(decimators.begin(), decimators.end(),
        [id](const std::shared_ptr<DecimatorInstance>& inst) {
            return inst->id == id;
        });

    return (it != decimators.end()) ? *it : nullptr;
}

std::vector<std::shared_ptr<DecimatorManager::DecimatorInstance>> DecimatorManager::getAllDecimators() {
    std::lock_guard<std::mutex> lock(decimator_mutex);
    return decimators;
}

size_t DecimatorManager::getDecimatorCount() const {
    std::lock_guard<std::mutex> lock(decimator_mutex);
    return decimators.size();
}

bool DecimatorManager::setFrequencyOffset(int id, float offset_hz) {
    auto decimator = getDecimator(id);
    if (decimator) {
        // Log offset changes
        if (fabs(decimator->frequency_offset_hz - offset_hz) > 0.1f) {
            std::cout << "Decimator " << id << " offset changed: "
                      << (decimator->frequency_offset_hz / 1000.0f) << " kHz → "
                      << (offset_hz / 1000.0f) << " kHz" << std::endl;
        }

        // Store the offset as-is for display/reference
        decimator->frequency_offset_hz = offset_hz;

        decimator->decimator->setFrequencyOffset(offset_hz);

        // Update MUSIC processor frequency: tuner RF frequency plus this
        // decimator's offset within the captured bandwidth
        float rf_frequency = ChannelManager::get_frequency(active_channel.load(std::memory_order_relaxed));
        decimator->music_processor->setFrequency(rf_frequency + offset_hz);

        return true;
    }
    return false;
}

bool DecimatorManager::setBandwidthIndex(int id, int index) {
    if (index < 0 || index >= NUM_BANDWIDTH_OPTIONS) {
        std::cerr << "DecimatorManager: rejecting out-of-range bandwidth index "
                  << index << std::endl;
        return false;
    }
    auto decimator = getDecimator(id);
    if (decimator) {
        decimator->bandwidth_index = index;
        decimator->decimator->setBandwidthIndex(index);
        return true;
    }
    return false;
}

bool DecimatorManager::setEnabled(int id, bool enabled) {
    auto decimator = getDecimator(id);
    if (decimator) {
        decimator->enabled = enabled;
        if (enabled) {
            decimator->music_processor->enable();
        } else {
            decimator->music_processor->disable();
        }
        return true;
    }
    return false;
}

bool DecimatorManager::setDemodMode(int id, DemodulatorMode mode) {
    auto decimator = getDecimator(id);
    if (decimator) {
        decimator->demod_mode = mode;
        std::cout << "Decimator " << id << " demod mode set to " << demodModeToString(mode) << std::endl;
        return true;
    }
    return false;
}

void DecimatorManager::recountDigital() {
    std::lock_guard<std::mutex> lock(decimator_mutex);
    int n = 0;
    for (const auto& inst : decimators)
        if (inst->digital_mode.load(std::memory_order_relaxed) != 0) n++;
    digital_active_.store(n, std::memory_order_relaxed);
}

bool DecimatorManager::setDigitalMode(int id, dig::Mode mode, const std::string& plugin) {
    auto inst = getDecimator(id);
    if (!inst) return false;
    {
        std::lock_guard<std::mutex> lk(inst->digital_mu);
        if (mode != dig::Mode::OFF) ensureDigital(*inst);
        if (inst->digital) inst->digital->set_mode(mode, plugin);
        inst->digital_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
        // DoA per talker while a decoder can report talkers
        inst->talker_doa->set_active(mode != dig::Mode::OFF);
    }
    recountDigital();
    return true;
}

bool DecimatorManager::setDigitalOptions(int id, const dig::Options& opts) {
    auto inst = getDecimator(id);
    if (!inst) return false;
    std::lock_guard<std::mutex> lk(inst->digital_mu);
    ensureDigital(*inst);
    inst->digital->set_options(opts);
    return true;
}

void DecimatorManager::ensureDigital(DecimatorInstance& inst) {
    if (inst.digital) return;
    inst.digital = std::make_shared<dig::DigitalDecoder>();
    inst.digital->set_vfo(inst.id);
    // who transmits when -> the per-talker DoA (holds the TalkerDoa, not the
    // instance: the handler runs on the decoder's own thread)
    auto td = inst.talker_doa;
    inst.digital->set_talker_handler([td](const dig::TalkerSpan& s) { td->add_span(s); });
}

bool DecimatorManager::setSquelchEnabled(int id, bool enabled) {
    auto decimator = getDecimator(id);
    if (decimator) {
        decimator->squelch_enabled.store(enabled, std::memory_order_relaxed);
        std::cout << "Decimator " << id << " squelch " << (enabled ? "enabled" : "disabled") << std::endl;
        return true;
    }
    return false;
}

bool DecimatorManager::setSquelchLevel(int id, float level_db) {
    auto decimator = getDecimator(id);
    if (decimator) {
        decimator->squelch_level.store(level_db, std::memory_order_relaxed);
        std::cout << "Decimator " << id << " squelch level set to " << level_db << " dB" << std::endl;
        return true;
    }
    return false;
}

bool DecimatorManager::setSquelchMethod(int id, int method) {
    auto decimator = getDecimator(id);
    if (decimator) {
        method = method == static_cast<int>(SquelchMethod::DIGITAL) ? method : static_cast<int>(SquelchMethod::FFT);
        decimator->squelch_method.store(method, std::memory_order_relaxed);
        std::cout << "Decimator " << id << " squelch method set to "
                  << (method == static_cast<int>(SquelchMethod::DIGITAL) ? "DIGITAL" : "FFT") << std::endl;
        return true;
    }
    return false;
}

SquelchMethod DecimatorManager::effectiveSquelchMethod(const DecimatorInstance& inst) {
    if (inst.squelch_method.load(std::memory_order_relaxed) == static_cast<int>(SquelchMethod::DIGITAL) &&
        inst.digital_mode.load(std::memory_order_relaxed) != 0)
        return SquelchMethod::DIGITAL;
    return SquelchMethod::FFT;
}

bool DecimatorManager::digitalSquelchOpen(const DecimatorInstance& inst) {
    auto dd = inst.getDigital();
    return dd && dd->frames_within(DIGITAL_SQUELCH_HOLD_MS);
}

bool DecimatorManager::setSquelchOpen(int id, bool open) {
    auto decimator = getDecimator(id);
    if (decimator) {
        decimator->squelch_open.store(open, std::memory_order_relaxed);
        return true;
    }
    return false;
}

// ============================================
// Beamforming config (applied to every decimator's beamformer)
// ============================================

void DecimatorManager::applyBeamformingConfig(const std::shared_ptr<DecimatorInstance>& inst) {
    if (!inst || !inst->beamformer) return;
    inst->beamformer->setBeamformingMode(static_cast<BeamformingMode>(bf_mode_.load()));
    inst->beamformer->setMVDRDiagonalLoading(bf_mvdr_loading_.load());
    inst->beamformer->setMVDRSnapshotLength(static_cast<size_t>(bf_mvdr_snapshots_.load()));
    if (bf_enabled_.load()) {
        inst->beamformer->enable();
    } else {
        inst->beamformer->disable();
    }
}

void DecimatorManager::setBeamformingEnabledAll(bool enabled) {
    bf_enabled_.store(enabled);
    for (const auto& inst : getAllDecimators()) {
        if (!inst || !inst->beamformer) continue;
        if (enabled) inst->beamformer->enable();
        else         inst->beamformer->disable();
    }
}

void DecimatorManager::setBeamformingModeAll(BeamformingMode mode) {
    bf_mode_.store(static_cast<int>(mode));
    for (const auto& inst : getAllDecimators()) {
        if (inst && inst->beamformer) inst->beamformer->setBeamformingMode(mode);
    }
}

void DecimatorManager::setMVDRDiagonalLoadingAll(float alpha) {
    bf_mvdr_loading_.store(alpha);
    for (const auto& inst : getAllDecimators()) {
        if (inst && inst->beamformer) inst->beamformer->setMVDRDiagonalLoading(alpha);
    }
}

void DecimatorManager::setMVDRSnapshotLengthAll(size_t snapshots) {
    bf_mvdr_snapshots_.store(static_cast<int>(snapshots));
    for (const auto& inst : getAllDecimators()) {
        if (inst && inst->beamformer) inst->beamformer->setMVDRSnapshotLength(snapshots);
    }
}

std::string DecimatorManager::demodModeToString(DemodulatorMode mode) {
    switch (mode) {
        case DemodulatorMode::WBFM: return "WBFM";
        case DemodulatorMode::NBFM: return "NBFM";
        case DemodulatorMode::AM:   return "AM";
        case DemodulatorMode::DIGITAL: return "DIGITAL";
        default: return "WBFM";
    }
}

DemodulatorMode DecimatorManager::stringToDemodMode(const std::string& str) {
    if (str == "NBFM" || str == "nbfm") return DemodulatorMode::NBFM;
    if (str == "AM" || str == "am") return DemodulatorMode::AM;
    if (str == "DIGITAL" || str == "digital") return DemodulatorMode::DIGITAL;
    return DemodulatorMode::WBFM;  // Default
}

std::vector<DecimatorManager::DecimatorInfo> DecimatorManager::getDecimatorInfoList() const {
    std::lock_guard<std::mutex> lock(decimator_mutex);

    std::vector<DecimatorInfo> info_list;
    int fm_id = fm_decimator_id.load();

    for (const auto& inst : decimators) {
        DecimatorInfo info;
        info.id = inst->id;
        info.frequency_offset_hz = inst->frequency_offset_hz;
        info.bandwidth_index = inst->bandwidth_index;
        info.bandwidth_mhz = inst->decimator->getBandwidthMhz();
        info.enabled = inst->enabled;
        info.is_fm_source = (inst->id == fm_id);
        info.demod_mode = inst->demod_mode;
        info.squelch_enabled = inst->squelch_enabled.load(std::memory_order_relaxed);
        info.squelch_level = inst->squelch_level.load(std::memory_order_relaxed);
        info.squelch_open = inst->squelch_open.load(std::memory_order_relaxed);
        info.squelch_method = inst->squelch_method.load(std::memory_order_relaxed);
        info.digital_mode = inst->digital_mode.load(std::memory_order_relaxed);
        auto dd = inst->getDigital();
        info.digital_opts = dd ? dd->options() : dig::Options();
        info.digital_plugin = dd ? dd->plugin_id() : std::string();

        info_list.push_back(info);
    }

    return info_list;
}