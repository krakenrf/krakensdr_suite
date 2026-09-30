#include "pipeline_control.hpp"
#include "sdr_device.hpp"
#include "sdr_init.hpp"
#include "sdr_pipeline.hpp"
#include "../core/types.hpp"
#include "../core/config.hpp"
#include "../core/utils.hpp"
#include "../core/settings.hpp"
#include "../dsp/compensation.hpp"
#include "../dsp/correlation.hpp"
#include "../net/tcp_data_server.hpp"
#include "../net/rtl_tcp_server.hpp"
#include <rtl-sdr.h>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

std::atomic<bool> pipeline_running{false};
std::atomic<bool> reconfig_in_progress{false};

// Globals owned by main.cpp
extern std::unique_ptr<TcpDataServer> tcp_data_server;
extern std::unique_ptr<RtlTcpServer> rtl_tcp_server;
extern CorrelationResult correlation_result;

namespace {
std::thread drain_thread;
std::thread conversion_thread;
std::vector<std::thread> compensation_threads;
}  // namespace

void start_pipeline_threads() {
    pipeline_running.store(true, std::memory_order_release);
    const int num_elements = std::min(active_num_elements.load(),
                                      static_cast<int>(devices.size()));

    // Per-device USB reader threads. This thread runs librtlsdr's libusb event
    // loop (rtlsdr_read_async dispatches USB completion callbacks here). It is
    // the single most latency-critical thread: if it is preempted longer than
    // the USB ring holds (~218 ms with RTL_USB_BUF_COUNT), the RTL2832 FIFO
    // overflows and samples are silently lost. Give it realtime priority.
    for (int i = 0; i < num_elements; i++) {
        SDRDevice* dev_ptr = devices[i].get();
        if (!dev_ptr || !dev_ptr->dev) continue;
        dev_ptr->running = true;
        dev_ptr->async_exited.store(false, std::memory_order_release);
        const int device_index = i;
        dev_ptr->async_thread = std::thread([dev_ptr, device_index]() {
            char tname[16];
            std::snprintf(tname, sizeof(tname), "rtl-rx-%d", device_index);
            set_thread_realtime(tname, RT_PRIO_USB_READER);
            std::cout << "Starting async read for channel " << device_index
                 << " (Serial: " << dev_ptr->serial_number
                 << ", Physical device: " << dev_ptr->device_id << ")" << std::endl;
            // A stop that lands before this point must not start streaming.
            if (dev_ptr->running) {
                rtlsdr_read_async(dev_ptr->dev, rtlsdr_callback, dev_ptr, RTL_USB_BUF_COUNT, NUM_SAMPLES * 2);
            }
            dev_ptr->async_exited.store(true, std::memory_order_release);
        });
    }

    drain_thread = std::thread([]() { sample_processor(devices); });
    conversion_thread = std::thread([]() {
        conversion_worker(devices, tcp_data_server.get(), rtl_tcp_server.get());
    });

    compensation_threads.clear();
    for (int ch = 0; ch < num_elements; ch++) {
        if (ch != REF_CHANNEL) {
            compensation_threads.emplace_back(
                [ch]() { channel_lag_compensation_processor(ch, correlation_result); });
        }
    }
}

void stop_pipeline_threads() {
    pipeline_running.store(false, std::memory_order_release);

    // Cancel the async reads first: stops USB streaming and returns each
    // reader thread out of rtlsdr_read_async so the joins below terminate.
    // rtlsdr_cancel_async only acts on a RUNNING stream, so a cancel that
    // lands while a reader is still starting up (thread created, not yet in
    // rtlsdr_read_async) is lost and the join would hang forever. Keep
    // re-sending it until every reader thread reports it has exited.
    for (auto& device : devices) {
        if (device) device->running = false;
    }
    const auto cancel_start = std::chrono::steady_clock::now();
    bool warned = false;
    for (;;) {
        bool all_exited = true;
        for (auto& device : devices) {
            if (!device || !device->async_thread.joinable() ||
                device->async_exited.load(std::memory_order_acquire)) continue;
            all_exited = false;
            if (device->dev) rtlsdr_cancel_async(device->dev);
        }
        if (all_exited) break;
        if (!warned && std::chrono::steady_clock::now() - cancel_start > std::chrono::seconds(2)) {
            std::cerr << "Pipeline stop: a USB reader has not exited after 2 s - still cancelling" << std::endl;
            warned = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto& device : devices) {
        if (device && device->async_thread.joinable()) device->async_thread.join();
    }

    // Drain/conversion block at most 100 ms in their queue waits and re-check
    // pipeline_running; compensation ticks every 20-50 ms.
    if (drain_thread.joinable()) drain_thread.join();
    if (conversion_thread.joinable()) conversion_thread.join();
    for (auto& t : compensation_threads) {
        if (t.joinable()) t.join();
    }
    compensation_threads.clear();

    // Nothing touches the queues now; drop everything in flight so no stale
    // set (possibly with a different channel count) survives into the restart.
    clear_l1_buffer();
    clear_l2_buffer();
}

bool reconfigure_num_elements(int new_n, std::string& err) {
    const int max_n = static_cast<int>(expected_serials.size());
    if (new_n < 2 || new_n > max_n) {
        err = "num_elements must be between 2 and " + std::to_string(max_n);
        return false;
    }
    const int old_n = active_num_elements.load();
    // Pipeline down = an earlier reconfiguration failed AND its rollback
    // failed: every handle is closed. Any count, including the current one,
    // is then a retry that must reopen the devices, not a no-op.
    const bool pipeline_down = !pipeline_running.load(std::memory_order_acquire);
    if (new_n == old_n && !pipeline_down) return true;

    if (recovery_in_progress.load(std::memory_order_acquire)) {
        err = "recalibration in progress - retry once it completes";
        return false;
    }
    if (discrete_scanner.enabled.load() ||
        operating_mode.load() == OperatingMode::WIDEBAND_SCAN) {
        err = "disable the scanner / wideband scan first";
        return false;
    }

    bool expected = false;
    if (!reconfig_in_progress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        err = "a reconfiguration is already in progress";
        return false;
    }

    if (pipeline_down) {
        std::cerr << "Reconfiguring: devices are closed after a failed reconfiguration - reopening "
                  << new_n << " elements, full recalibration follows" << std::endl;
    } else {
        std::cerr << "Reconfiguring from " << old_n << " to " << new_n
                  << " elements: stopping pipeline, reopening devices, full recalibration follows" << std::endl;
    }

    bool ok;
    {
        // Hold settings_mutex across the close/reopen so a concurrent retune
        // or gain change can't touch handles we are freeing. Command handlers
        // additionally refuse work while reconfig_in_progress is set.
        std::lock_guard<std::mutex> lock(settings_mutex);

        stop_pipeline_threads();

        // Every other handle user takes device_io_mutex, so nothing can touch
        // a handle while it is closed or being reopened. Taken only AFTER the
        // pipeline threads are joined (one may be waiting on it).
        {
            std::lock_guard<std::recursive_mutex> dev_lock(device_io_mutex);
            close_active_devices(devices);

            active_num_elements.store(new_n);
            ok = open_active_devices(devices);
            if (!ok && new_n == old_n) {
                // A retry of the current count: nothing different to roll back to.
                err = "failed to open devices for " + std::to_string(new_n) +
                      " elements - check USB connections and retry";
                reconfig_in_progress.store(false, std::memory_order_release);
                return false;
            }
            if (!ok) {
                // Roll back to the previous count (open_active_devices cleaned up
                // its partial handles). If even that fails, stay stopped rather
                // than run half-open.
                std::cerr << "Reconfiguration: failed to open " << new_n
                          << " devices, rolling back to " << old_n << std::endl;
                active_num_elements.store(old_n);
                if (!open_active_devices(devices)) {
                    err = "failed to open devices for " + std::to_string(new_n) +
                          " elements AND rollback to " + std::to_string(old_n) +
                          " failed - check USB connections, then select an element count "
                          "again to retry (or restart)";
                    reconfig_in_progress.store(false, std::memory_order_release);
                    return false;
                }
                err = "failed to open " + std::to_string(new_n) +
                      " devices (rolled back to " + std::to_string(old_n) + ")";
            }
        }

        // Rebuild the per-channel result state for the (possibly rolled-back)
        // element count BEFORE data flows again. Stale map entries for
        // now-inactive channels would otherwise leak into the correlation
        // message - its status array is sized num_elements*4, so a stale
        // higher channel writes past the end (heap corruption, observed as
        // "double free or corruption" on the first 5->4 reconfiguration) -
        // and would gate the phase machine on a channel that no longer
        // produces data. initialize_correlation_result() itself takes no
        // lock (startup has no threads); here the web timer's 2 Hz
        // build_correlation_message reads these fields under data_mutex, so
        // hold it across the clear+reinit.
        {
            std::lock_guard<std::mutex> rlock(correlation_result.data_mutex);
            correlation_result.lags.clear();
            correlation_result.phases.clear();
            correlation_result.amplitudes.clear();
            correlation_result.channel_states.clear();
            correlation_result.channel_zero_counts.clear();
            correlation_result.data_ready = false;
            initialize_correlation_result(correlation_result);
        }

        start_pipeline_threads();
    }

    // The RTL-TCP source channel may now be out of range.
    if (rtl_tcp_channel.load() >= active_num_elements.load()) {
        if (rtl_tcp_server) rtl_tcp_server->set_source_channel(0);
        else rtl_tcp_channel.store(0);
    }

    // Everything reopened: run the full startup-equivalent recalibration
    // through the coherence-recovery path (noise source on, lag+phase from
    // scratch). Clear any coherence event latched during the teardown first
    // so the watchdog runs a single recovery.
    // --kerberos: an automatic noise-cal is never allowed (antennas are
    // connected) - park in the uncalibrated idle state instead; the user
    // recalibrates manually with the antennas disconnected.
    coherence_lost.store(false, std::memory_order_release);
    if (kerberos_manual_cal_only()) {
        kerberos_enter_uncalibrated("element count changed");
    } else {
        force_recalibration.store(true, std::memory_order_release);
    }

    if (ok) settings::save();  // remember the new element count across restarts

    reconfig_in_progress.store(false, std::memory_order_release);
    return ok;
}
