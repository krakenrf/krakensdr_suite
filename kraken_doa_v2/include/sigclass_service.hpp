#pragma once

// Signal ID (docs/dev/signal-classifier.md): every signal in the live
// spectrum labelled with its type by the signal classifier core
// (include/sigclass/sigclass.hpp) on 0.5 s snapshots of ONE tuner's raw
// 8-bit IQ.
//
// A worker thread (started from main, idle while the feature is off) loops:
// pick a tuner and its exact centre (coherent: the CH selector's antenna at
// coherent_tuner_hz; wideband scan / independent / generic RTL-SDRs: the
// bands in turn - tuners on one frequency share a look, a band's first look
// comes first -, exact_tuner_hz; sigclass_rules.hpp), arm a capture, wait
// for it, analyze, update the band's tracker (absolute Hz), publish
// {"sigclass":{...}} to the pages; before the next look it rests to keep
// analysis within CPU_SHARE of the wall time.
//
// The tap: the data receiver thread hands every packet to on_packet() while
// a capture is armed (armed(): one atomic load per packet otherwise). It
// copies one channel's bytes into a buffer sized once until
// sigclass::NOMINAL_SNAPSHOT_SAMPLES are in, then waits TRAIL_PACKETS more
// clean packets (heimdall's noise flag trails the samples) before handing
// the snapshot over. A stream gap, a noise-source packet, a retuning packet,
// a change of that channel's frequency or of the channel count aborts it.
//
// Read-only: nothing is ever sent to heimdall; during a debug / recording
// playback the snapshots are the playback's packets (the tap sits in the
// packet parser both sources go through). Only the on / off setting is
// persisted (SIGCLASS:, the settings store - sandboxed during a playback).

#include <atomic>
#include <cstdint>
#include <string>

namespace sigsvc {

// SIGCLASS:0|1 (uWS loop thread). The model (sigclass.model in the working
// directory, or $KRAKEN_SIGCLASS_MODEL) is loaded by the worker on the first
// enable; a model that fails to load makes the feature unavailable until
// the next enable (one line to stderr, no retries meanwhile).
void set_enabled(bool on);
bool enabled();

// {"sigclass":{enabled, available, reason, parked, analyze_ms, period_ms,
// duty, snapshots, tracks:[...]}} - the current state (ages computed now);
// sent to every page on each change (at most ~2 a second) and to a page
// that connects (get_connect_sync_messages)
std::string state_json();

// main(): the worker thread (idle until enabled) / its end (joined)
void start();
void stop();

// --- the tap (data receiver thread) ---
extern std::atomic<bool> g_armed;
inline bool armed() { return g_armed.load(std::memory_order_relaxed); }
// iq: the packet's channel-major interleaved uint8 I/Q (channels x samples x
// 2 bytes); freq_hz: each channel's header frequency; gap: bytes were lost
// before this packet (a resync, a reconnect, a source switch). Never blocks
// on the worker, never allocates.
void on_packet(const uint8_t* iq, uint32_t channels, uint32_t samples, const float* freq_hz, bool noise,
               bool retuning, bool gap);

}  // namespace sigsvc
