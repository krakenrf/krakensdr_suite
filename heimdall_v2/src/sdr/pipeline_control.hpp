#pragma once

#include <atomic>
#include <string>

// Lifecycle control for the device-touching pipeline threads (per-device USB
// readers, sample drain, conversion worker, per-channel lag compensation),
// plus the runtime element-count reconfiguration built on top of it.
//
// The global `devices` vector is STRUCTURALLY IMMUTABLE after startup: it
// holds one SDRDevice object per expected serial for the life of the process,
// and only the first active_num_elements entries have an open librtlsdr
// handle (device->dev != nullptr). Reconfiguration closes/reopens handles
// inside the existing objects - the vector is never resized, so the lock-free
// readers (status dashboard, TCP status builders, correlation) stay safe.

// False while the pipeline threads are stopped (element-count reconfiguration
// in progress). Checked by the drain / conversion / lag-compensation loops
// alongside global_running; start_pipeline_threads() sets it, and
// stop_pipeline_threads() clears it before joining.
extern std::atomic<bool> pipeline_running;

// True for the whole of reconfigure_num_elements(). Device-touching command
// handlers (web/TCP) refuse work, and the coherence watchdog + periodic
// calibration monitor hold off, while this is set.
extern std::atomic<bool> reconfig_in_progress;

// Start the per-device USB reader threads plus drain / conversion /
// lag-compensation threads for the first active_num_elements devices.
void start_pipeline_threads();

// Stop and join every thread started by start_pipeline_threads(), then flush
// L1 / L2-raw / L2 so no stale sets survive into the next configuration.
void stop_pipeline_threads();

// Save an explicit element-count choice (web UI / control port) without
// reconfiguring - for a count that is already the live one.
void remember_num_elements(int n);

// Runtime element-count change: stop the pipeline, close every device handle,
// reopen the first `new_n`, restart the pipeline and arm a full
// recalibration (via the coherence-recovery path). On an open failure it
// rolls back to the previous count. Returns false with `err` set if the
// request is invalid, the system is busy (recovery / scanner / wideband scan
// active), or the devices could not be reopened.
bool reconfigure_num_elements(int new_n, std::string& err);
