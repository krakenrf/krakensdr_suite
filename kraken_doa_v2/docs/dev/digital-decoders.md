# Digital voice / data decoders (engine, protocol receivers, decoder tabs)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Digital voice/data decoders (one per VFO): engine `src/digital/`, decoders `plugins/`:**
- EVERY decoder is a plugin (out-of-process, see *Decoder plugins* below);
  kraken_doa contains no protocol code. Shipped: p25, dmr, tetra, dstar,
  nxdn, ysf, mpt1327, pagers (see *Pagers*; was pocsag), aprs, adsb (see *ADS-B* below), ais (see *AIS*),
  radiosonde (see *Radiosonde*), dme and ils (see *DME / TACAN* and *ILS /
  VOR / marker*), ism (see *ISM devices*), radar (see *Pulse radar*),
  ssr1030 (see *1030 MHz interrogations*), uat (see *UAT*), flarm (see
  *FLARM / OGN*), analog_id (see *Analog FM IDs*; was ctcss), beacon406 (see *406 MHz
  distress beacons*), beacon121 (see *121.5 MHz beacons*), lora (see *LoRa / Meshtastic / LoRaWAN*), wildlife (see
  *Wildlife collars*), rds (see *RDS / RBDS + SCA*), acars (see *ACARS*), vdl2 (see *VDL Mode 2*), dab (see
  *DAB / DAB+*; multichannel, a talker per transmitter by its TII code), dtv and cells
  (ARRAY plugins - see *Digital TV* and *LTE / 5G NR Cell Decoder*)
- Which plugins run in Auto detect is the USER's choice: the "Auto detect"
  tick per plugin in the plugin list (⚙ Setup Settings → Decoders), all on by
  default. WS `PLUGIN_AUTO:id:0|1` -> `PluginRegistry::set_auto()` (bumps the
  registry generation, so decoders in AUTO rebuild their plugin set) and
  records the setting `AUTO_DETECT_OFF:id,id` (settings_store schema, before
  DECIMATORS; replayed through `set_auto_off()`); the plugin list's `auto`
  field is that state. `kp::Info::auto_detect` is deprecated and ignored
- `DigitalDecoder` (digital_decoder.hpp) hangs off a `DecimatorInstance`
  (`digital`, created on first use, `digital_mode` mirrors the mode). The
  decimation pass pushes the VFO's decimated samples (the beamformer output
  when beamforming ran for it, else the listened-to channel - the same stream
  FM audio gets) with `push()`; a worker thread ("digital-dec") per decoder
  drives the plugin processes, so the pipeline never waits. Queue capped at
  1 s (oldest dropped, `dropped` in the status). In FM/digital-only mode (DoA
  and beamforming off) only the VFOs heard (🔊) and the VFOs with a decoder are
  decimated, on one channel (`fm_only` in data_receiver.cpp)
- Modes (`dig::Mode`): OFF, AUTO, PLUGIN (+ plugin id). Wire / snapshot form
  `OFF`, `AUTO`, `PLUGIN:<id>`; `parse_mode_string()` maps the old protocol
  names (P25, DMR, TETRA, DSTAR, D-STAR, NXDN, MPT1327) to their plugins and
  renamed plugins to their new id (`renamed_plugin()`: `sfn` -> `dtv`,
  `ctcss` -> `analog_id`, `pocsag` -> `pagers`; the snapshot's `sfn.*`,
  `ctcss.*`, `pocsag.*` options too), so saved settings and old clients keep
  working
- `Engine` (digital_decoder.cpp): AFC mixer at the VFO rate -> one msresamp
  per distinct plugin `sample_rate` (shared by the plugins at that rate; a
  plugin declaring 0 gets the VFO's rate unresampled - `Runner::vfo_rate`,
  started `--serve --rate HZ`, restarted when the VFO's rate changes) ->
  each plugin process (`Runner`). AUTO runs one Runner per built plugin the
  user left in Auto detect (rebuilt when the registry generation changes)
  minus `manual_only` plugins (`kp::Info::manual_only`, ADS-B - the plugin
  list shows no Auto detect tick for them, `digAutoPlugins()` leaves them
  out), whatever the VFO's width: a plugin whose `min_vfo_rate` is more than
  the VFO's bandwidth runs on an internal stream (below, *Internal
  streams*; 2026-11-20 - until then such a plugin was left out when it
  needed more than 2x the VFO's rate, `AUTO_MAX_RATE_SHORTFALL`, and ran
  starved on upsampled samples within 2x: on a narrow VFO Auto detect could
  never find RDS or analog TV, and TETRA ran on a 12.5 kHz VFO's samples.
  The offline harness, which has no internal streams -
  `DigitalDecoder(threaded, internal_streams = false)` -, keeps that rule);
  detection = majority of `valid()` frames in a 4 s window. Only the "lead"
  plugin (the fixed one, or the detected one) gets VOICE_WANTED and has its
  audio / voice state / carrier offset used. AFC: plugin offset reports are
  averaged and applied at most every 300 ms, ignoring reports from the 300 ms
  after a correction (the pipe latency made an every-report integrator run
  away - TETRA drifted -5 kHz). With no correction (yet) the mixer is a
  plain copy (a sin/cos per sample cost ~5% of a core at 2.4 MHz) and its
  phase goes back to 0; idle, the correction decays to the VFO centre and
  lands on 0 below 0.5 Hz (until 2026-10-16 the phase of the last
  correction was kept for good: once a decoder that reports an offset -
  D-STAR, P25, ... - had run on a VFO, a later multichannel plugin there -
  lora, ism - got one antenna until kraken_doa restarted: a debug playback
  of lora-mesh-five after dstar-two-radios had no separation by code). A VFO
  retune (> 100 Hz) or mode change resets everything (RESET to the plugins,
  facts + map points cleared; `reset_pending_` is consumed by the worker)
- **Internal streams** (2026-11-20; the user: "I often see the error message
  D0: VFO bandwidth 25.5 kHz is too narrow for Auto detect (needs 2000 kHz)
  ... Is there a way to make autodetect work with just the center freq, and
  it will create multiple internal VFO sizes for autodetection of
  decoders?"). Auto detect works from the VFO's centre frequency alone:
  - Per plugin (`Engine::assign_streams`, `Runner::stream_w`): the VFO's own
    stream when the VFO's BANDWIDTH (`push(.., bw_hz)` =
    `MultiChannelDecimated::bandwidth_hz`, not the rate - the page's "too
    narrow" test) holds its `min_vfo_rate`; else an internal stream at
    `efficient_bandwidth_at_least(min_vfo_rate)` (what the page's "Set X kHz"
    sets). Fixed modes, the offline harness (`internal_streams` false), array
    plugins, and the time before the first block: always the VFO's own.
    Grouping: the unmet needs sorted, a need joins the current group while
    its width is <= `AUTO_STREAM_MERGE_RATIO` (2) x the group's narrowest,
    the group's stream = its widest; more than `DigitalDecoder::MAX_STREAMS`
    (4) groups: the two neighbours closest in width merge. Today's ticked
    plugins on a 12.5 kHz VFO: 25 kHz (aprs, v23_telemetry, radiosonde,
    tetra, vdl2), 48 kHz (ils), 150 kHz (rds, every antenna), 2.4 MHz
    (analogtv); on 25.5 kHz: 48 kHz, 150 kHz, 2.4 MHz; from 2 MHz: none
  - Equivalence: an internal stream is what the plugin would get from the
    VFO widened to that width at the same centre - a real `SharedDecimator`
    at that bandwidth and the VFO's offset (read once with the VFO's block),
    decimated in the SAME `decimateVfos` pass (extra `VfoBlock`s), the same
    AFC mixer (per stream, its own phase), the VFO's channel choice (the
    listened-to channel; the VFO's tuner in wideband-scan / independent
    mode; one channel in `fm_only`), every antenna for a `multichannel`
    plugin on it (`StreamWant::mc`) exactly when the VFO's own stream would
    get them (coherent, not `fm_only`, no AFC correction, the plugin at the
    stream's rate). The one exception: never the beamformer output (the
    listened-to channel). A plugin at `sample_rate` 0 runs at the stream's
    rate (`--rate` = the width; moving to another stream restarts it, as "new
    VFO rate" does); the others are resampled from it
  - Not VFOs: no `DecimatorInstance`, never in MUSIC / squelch / the VFO list
    / settings / the per-VFO pipeline loop, nothing drawn. The pipeline
    (`data_receiver.cpp`, `AutoStreamSet` / `AutoStream`, a vector local to
    the decimation thread) creates, reconfigures and frees them - the only
    thread touching them: per block, a VFO in AUTO whose decoder
    `wants_streams()` gets a set (keyed by its instance, weak_ptr-checked);
    `streams_gen()` changed -> `wanted_streams()` read under the decoder's
    `streams_mu_` and the decimators reconciled (a width kept keeps its
    filter state; only a change allocates); a set its VFO no longer wants
    (decoder off / fixed / wide enough / VFO deleted / not decimated) goes in
    the same block ("Auto detect: VFO N internal streams: ..." / "...
    freed" on stdout). The engine publishes the wants from its worker
    (`set_wanted_streams`, like `set_array_wanted`). A VFO not in AUTO, or
    wide enough: one atomic load per block, nothing else
  - Hand-over: the stream blocks go in the VFO's `push` (`AuxStream`, copied
    into the same queue `Block`, so the 1 s cap - now counted in seconds of
    signal, `queued_s_` - bounds them; a 2.4 MHz stream is 19 MB a second).
    An internal stream is continuous whatever happens to the VFO's block:
    when the beamformer holds the VFO's samples back - FD-DAS's 256-point
    frames (hop 128) are longer than the ~85 samples a 12.5 kHz VFO gets per
    16384-sample block, so it returns nothing on about one block in three,
    and more in the next - the block goes as a `push` with no VFO samples
    (n = 0) and its internal streams; the engine then feeds only the
    internal streams' plugins and leaves the VFO stream's AFC phase,
    resamplers and positions alone. Such a block's length in the queue is
    its internal streams' (every block has the same), so the 1 s cap stays
    right; positions come from the decimated block's `stream_pos`, never
    from the beamformer. (The first version skipped the internal blocks
    with a withheld VFO block, as the brief said: TETRA on a 12.5 kHz VFO
    with FD-DAS gave 0 frames - the orchestrator's review, 2026-11-20.) A
    wanted stream a block lacks (the pipeline makes it from the next one)
    gives its plugins nothing for that block - never upsampled VFO samples
  - Positions: the VFO block starts at `stream_pos`; an internal sample j of
    the block is VFO position `stream_pos + (D_vfo - D_stream) x vfo_rate + j
    x vfo_rate / stream_rate` (`AuxStream::pos`, the `run()` step) - D = the
    cascade's group delay (`SharedDecimator::groupDelaySeconds`: each
    linear-phase stage's (taps - 1) / 2 of its input; 12.5 kHz 0.79 ms = 10
    VFO samples, 25 kHz 0.45, 25.5 kHz 0.40, 48 kHz 0.23, 150 kHz 0.07, 2.4
    MHz 0 - the wider stream sees a signal earlier; ignoring the first
    output's phase in the block costs < 1 VFO sample). `pos_map` takes
    fractional positions, so talker spans,
    `valid(start, end)` (Digital squelch) and packets land in the VFO's
    stream and per-talker DoA works from the VFO's MUSIC frames as before
    (only the VFO's band of the signal: widening gives DoA the whole one)
  - Protocol (compatible both ways): status `plugins[].stream_hz` (absent =
    the VFO's own); `rate_ok` / `min_rate` count only the plugins on the
    VFO's own stream; capability marker `"auto_streams":true` next to the
    plugin list (`AiManager::plugins_message`, every page gets it at
    connect and after a rescan) - an older page ignores both. Page
    (`digAutoStreams`): no "too narrow" warning in Auto detect, nothing
    widens a VFO for it (Demod -> Digital, `digFixBandwidthFor`,
    `digMinRateK(mode, id)` = 0 while searching); locked onto a plugin that
    needs more than the VFO has: a `.dig-note` "D0: RDS decoded from a 150
    kHz stream" + "Set 150 kHz" (title: what widening adds), and a "Stream"
    row in its facts; fixed modes and an older receiver (no marker): as
    before. `sf_scanner`'s `decoder_need_hz` gives Auto detect no minimum
    (was 24 kHz)
  - Tests (2026-11-20, x86_64 TEST builds of the change and of the tree
    before it, machine load 15-30; scratch impl/: `tpb.py` + `batch.sh` -
    debug playback, VFO 0 set by WebSocket, valid frames counted over 30-42 s
    = 2.5-3 loops -, `t_trans.py`, `t_indep.py` + the fake heimdall
    replaying a playback .dat, `t_ui.py`, `cpu_all.sh`). Frames, narrow VFO
    in Auto detect vs the same scene widened by hand vs the catalogue's
    fixed mode (the old build's fixed count in brackets): RDS 12.5 kHz 348,
    25.5 kHz 341, 150 kHz 340, fixed 400 kHz 352 (352), talkers PI 9201 60.0
    / PI 3DF5 200.0 deg in every one; TETRA 12.5 kHz (25 kHz stream) 2517,
    24 kHz 2533, fixed 30 kHz 2517 (2517), CC12 70.0 / CC33 235.0 deg - the
    old build on 12.5 kHz in Auto detect: 0 frames; analog TV 12.5 kHz (2.4
    MHz stream) 443, fixed 440 (439) - widened to 2.4 MHz in Auto detect 206,
    the decoder queue dropping (17 plugins resampled from 2.4 MHz - so as
    before); DMR 12.5 kHz (own stream) 547, 25.5 kHz 548, fixed 547 (546),
    old build 12.5 kHz Auto 546, talkers 300 / 300 / 60 / 200 deg; P25 12.5
    kHz 167 (old 167), fixed 167 (166), 60 / 200 / 300 deg. No internal
    stream for fixed modes nor a VFO wide enough (RDS at 150 kHz with
    analogtv unticked 344, old 347; no "internal streams" log line).
    Transitions (`t_trans.py`, 34 checks, also under ASan + UBSan: no
    report, clean exit, only the slowed decoder's queue dropped 101 blocks):
    bandwidth across RDS's need (150 kHz -> its own stream, back), a 30 kHz
    retune and back, PLUGIN:rds / OFF / AUTO, analogtv unticked / ticked,
    the rds exe touched (restarted on its stream), a second VFO in Auto
    detect with its own four streams then deleted, DoA off (`fm_only`), the
    playback stopped and started - streams created / freed as logged, RDS
    decoding after each step. Independent mode and `fm_only` with the fake
    heimdall (`t_indep.py`, 7 checks): RDS from its 150 kHz stream on a
    25.5 kHz VFO, 286-292 frames per 30 s in every case. Offline:
    `digi_test` before / after gives the same plugin set (the old rule) at
    25 and 300 kHz. Beamforming (`sc_bf2.txt`, `bf_run.sh`, after the fix
    above): FD-DAS, TETRA on 12.5 kHz 2528 (0 before the fix; old build 0 -
    it ran TETRA starved), RDS on 25.5 kHz 349 (old build: no RDS), DMR on
    its own 12.5 kHz stream 540 (old build 540); DAS RDS 25.5 kHz 342; MVDR
    DMR 12.5 kHz 544 (old 535); `t_trans.py` with an FD-DAS step 37 checks.
    Shutdown (SIGTERM during an FD-DAS Auto detect session, 17 / 14 plugin
    processes): new 1.46 / 1.56 s, old 1.35 / 1.72 s, the plugins gone with
    it - unchanged (the process ends with `fast_exit`; decoders aren't torn
    down). Page (headless Chromium, real key / mouse events; 10
    checks): the marker, no warning locked onto DMR at 25.5 kHz, Demod ->
    Digital (End key) sends no bandwidth, the RDS note + facts row, its Set
    button (a click) -> 150 kHz and the note gone, fixed TETRA's warning +
    Set 24 kHz as before; the old receiver serving the new page (7 checks):
    the old "needs 2000 kHz" warning and the widening to 2.4 MHz as before.
  - CPU (x86_64 under load, user time of kraken_doa + its plugin processes,
    % of one core, the old and the new build playing the DMR scene AT THE
    SAME TIME, 3 x 60 s, one 12.5 kHz VFO): decoder off 25.9 / 26.6; Auto
    detect 50.7 -> 63.7 (+12.9: kraken_doa +7.2 - four internal streams,
    their copies, AFC and resamplers -, plugins +5.7: analogtv 3.8 idle
    (no TV signal, nobody watching), rds 1.3, ils 0.5); analogtv unticked
    49.5 -> 56.1 (+6.7); a 25.5 kHz VFO (three streams) 49.0 -> 61.0
    (+12.0). Auto detect's own cost (minus the decoder-off figure) grows
    by about half, a quarter without analogtv - on a Pi count on it,
    and untick what isn't needed (not measured on a Pi)
- Station location for the plugins: `dig::set_station_location()` (called
  by `build_map_message` every second from `station_info.resolve()`; 0,0 =
  unknown) bumps a generation; each Runner sends wire STATION "lat,lon" (""
  = unknown) when it changes and after a (re)start -> `kp::Host::station()`
- Map points: wire MAP_POINT (id, lat, lon, label, kind, heading, altitude_m,
  speed_kmh, ttl_s, info - '\0'-separated text, "" = unknown; validated in
  `on_map_point`, ttl clamped 10 s..24 h) / MAP_REMOVE -> `Report::map_set /
  map_remove` (key plugin|id, max 2000 per decoder, expired by ttl).
  NaN marks unknown fields: test with `is_finite_value()`
  (utils/parse_num.hpp) - kraken_doa built with -Ofast until 2026-10-23,
  which folded `std::isfinite` to true (a `"h":nan` broke the page's
  JSON.parse; digi_test (-O2) didn't show that); its Makefile adds
  -fno-finite-math-only now, and is_finite_value is right under any flags
- The protocol receivers live in the plugins (`plugins/<id>/<id>.cpp`), on
  the shared library `plugins/lib/` (libkrakendig.a): `dig_common` (the
  RxContext / Report / Options interface they were written against, mapped
  onto `kp::Host` by `dig::Bridge`; `FmFrontEnd` = 48 kHz Kaiser 6.5 kHz +
  discriminator in Hz; `RrcFrontEnd`), `dig_fec` (block codes by nearest
  codeword, RS GF(64) Berlekamp-Massey, soft Viterbi, CRCs), `dig_fsk4`
  (P25/DMR 4FSK sync receiver -> `Fsk4Sink`), `dig_vocoder` (IMBE, mbelib
  AMBE, TETRA ACELP), `mbe_tables.hpp`. Each receiver is sync-driven -
  nothing is reported unless its own FEC/CRC passed:
  - `Fsk4Receiver` (lib/dig_fsk4.cpp): P25 on an integrate-and-dump filtered
    discriminator, DMR on RRC 0.2; normalized sync correlation per sample,
    the sync's own +-3 symbols give timing, level and centre (= carrier
    offset), no symbol-timing loop. DMR voice bursts B..F (no sync) are
    decoded at positions extrapolated from the last sync. DMR data sync =
    voice sync with every symbol negated (`^ 0xAAAA...`, NOT `~`). The p25
    plugin enables only P25, the dmr plugin only DMR. A DMR VOICE sync off
    the 30 ms grid of a CONFIRMED burst sequence (`dmr_confirmed_`: its
    last burst checked out - EMB / slot type + FEC, or a voice sync on the
    grid) is ignored: random voice payload correlates >= 0.8 with a sync
    word now and then (~1 in 50 bursts of random AMBE data), and burst A is
    checked by nothing, so a false one moved the extrapolation of B..F off
    their timing; a sequence a bare voice sync started is not confirmed
    (any sync re-anchors it, as before - a real MS recording has a false BS
    voice sync 10 ms after a radio's real one) (2026-10-16, found with the
    dmr generator). P25 (2026-11-19): a frame goes to the sink as soon as its
    own length is in (the NID, `p25_nid_decode` in dig_fec, gives the DUID:
    HDU 396 dibits, TDU 72, TDULC 216, LDU 864, a TSDU once its own block
    is) instead of after 864 dibits - a lone trunking block ~33 ms after its
    sync instead of 180 ms, terminators 100-165 ms earlier, a recording's
    last 0.2 s decoded; sample positions unchanged. CQPSK read through the
    discriminator: the +-6 kHz per-sample limit (against noise clicks) cut
    its fast phase steps near the envelope nulls (6 % of on-air samples,
    outer levels squeezed, the centre fit biased as the carrier moved), so a
    second boxcar keeps what the limit took (modulo one turn, 4800 Hz on
    the boxcar); sync detection and the fit stay on the limited one, and a
    frame is sliced from the restored one only when its sync scale was
    squeezed below 0.85 of the exact one (CQPSK 0.65-0.78, C4FM / LSM
    0.91-1.0 down to 4 dB SNR; restoring everywhere cost 0.11-0.16 dB on
    weak C4FM / LSM). Real CQPSK voice channel: 90 of 90 frames at any
    carrier offset within +-1 kHz (before 68-90), half-frame SNR 16.9 ->
    14.25 dB; C4FM / LSM unchanged; cost 1.03 -> 1.06 % of real time
    (plugins/p25/README.md)
  - `P25Proto` (plugins/p25): NID BCH(63,16) (`p25_nid_decode`, dig_fec:
    the last NAC first, else all 65536 - one 512 KB table per process,
    shared with the receiver), TSBK (rate 1/2 trellis + CRC; the trellis'
    branch metric = absolute error capped at 4 level units since 2026-11-19,
    `p25_trellis_half` - the soft symbols' noise is heavy-tailed near
    threshold, kurtosis 9-12 on real C4FM / LSM and 14-30 through CQPSK,
    and a squared error let one click steer the path: half of the blocks
    at 0.3-0.6 dB less SNR, false accepts unchanged), LDU1/LDU2
    (Hamming(10,6) + RS(24,12)/(24,16)), HDU (Golay(18,6) + RS(36,20)),
    TDULC (Golay(24,12) + RS(24,12)), PDU header. IDEN_UP tables turn
    channel numbers into MHz. Option `nac`. Link controls with the SF bit
    (LCO 0x02 / 0x22 / 0x23) are the trunking-block bodies of TSBK 0x02 /
    0x3C / 0x3A and go through `tsbk()`; TSBKs also decoded: secondary
    control channel (0x39, explicit 0x29), SNDCP data channel grant /
    announcement (0x14 / 0x16), location registration (0x2B) - layouts
    checked on real control channels (README). A call (2026-11-19): played
    only once known to be clear (ALGID 0x80 or an unprotected voice-user
    LC), up to 2 LDUs held until then; one "Voice:" / "Call ended" event per
    transmission (the talker logic's), no dedup, nothing from hang-time
    terminators
  - P25 Phase 2 (plugins/p25, 3.0, 2026-11-18; details, measurements and
    sources: plugins/p25/README.md): `P25p2Phy` (p25p2_phy.cpp) runs on the
    same 48 kHz complex input as the Phase 1 4FSK path (decoder.cpp feeds
    both; no phase setting - a traffic channel can switch between Phase 1
    and Phase 2 calls). 6000 symbols/s, 8 sps, one 41-tap low-pass (-6 dB at
    5 kHz). Outbound H-DQPSK: differential detection at the symbol centres,
    acquired on two S-ISCH syncs 180 / 540 symbols apart (either
    polarity), timing / carrier / scale tracked per slot, grid dropped after
    34 slots without a sync, re-anchored after sample slips. Inbound H-CPM
    bursts: the discriminator integrated between symbol centres (a
    duobinary-like response, 489 Hz x (I[n] + I[n+1])) into a 16-state
    sequence detector, voice bursts (no sync) on each radio's own timing /
    carrier / deviation track. It hands 30 ms slots (`P25p2Slot`,
    p25p2_slot.hpp) to `P25p2Proto` (p25p2.cpp): I-ISCH (40,9,16) / DUID
    (8,4,4) by nearest codeword, PN44 descrambling (seed = WACN | system ID
    | NAC from the options, a control channel's network status or BLIND:
    solved from one clean scrambled FACCH / SACCH burst by GF(2) linear
    algebra on its RS parity relations), RS over GF(64) from the (63,35,29)
    mother code with the punctured symbols as erasures
    (`rs64_decode_erasures`, `crc_bits` added to lib/dig_fec), CRC-12 /
    CRC-16, MAC PDUs + messages (partition-01 TSBK bodies through
    `P25Proto::tsbk()`, which shares its channel identifiers and network
    identity), voice + ESS. Options `wacn`, `sysid`, `lch` (Timeslot) next
    to `nac`; they take effect at once while it runs (a complete identity
    replaces the seed in use, a NAC alone drops a contradicting one). Facts
    both phases write (Encryption, Channel type, WACN / System ID / NAC):
    the phase that decoded last shows its value (`P25p2Proto::phase1_frame`
    after every valid Phase 1 frame - Phase 2's cached values went stale
    before). The receiver never hands on a non-finite symbol (non-finite /
    absurd input samples are zeroed); the MAC treats one as no reliability
    (`p2_finite`, a bit test, p25p2_slot.hpp). `rs64_decode_erasures`
    returns errors + erasures also for a word that is a codeword already.
    Call events (2026-11-19) follow the talkers, as Phase 1's: one "voice:"
    when a transmission's unit is named, one "call ended:" when that
    talker ends (`announce` / `announce_end` from `talker_set` /
    `talker_end`, inbound `in_named` / `unit_end`), no dedup; before they
    followed the call with 5 s / 2 s dedup and lost starts / ends on 6 of
    14 real traffic channels
  - `DmrProto` (plugins/dmr): CACH TACT (timeslot), slot type Golay(20,8)
    (max 2 corrections - a third of random words are within 3), BPTC(196,96)
    voice LC header / terminator (RS(12,9)), CSBK, PI and data headers, EMB
    QR(16,7) + embedded LC, GPS and talker alias LCs. Control-channel chatter
    (Aloha, broadcasts, acks) only with the verbose option. Option `slot`.
    The kind + slot that extrapolated bursts are read with (`ext_bs_`,
    `ext_slot_`) come only from a burst that checked out (`set_ext`): a
    false direct-mode data sync inside a repeater's burst made the next
    burst B..F skip its CACH, on the wrong slot (2026-10-16). Real
    recordings: dmr_1 263 -> 269 valid bursts, dmr_it_8 604 -> 605, dmr_2
    identical, talkers unchanged
  - `DstarReceiver` (plugins/dstar): RF header (scrambler, 24-column
    interleaver, K=3 Viterbi, CRC-16/X.25), slow data (text message, GPS /
    DPRS, header copy), end pattern
  - `NxdnReceiver` (plugins/nxdn): NXDN96 (4800 Bd, 10 sps) and NXDN48
    (2400 Bd, 20 sps) searched at once, each on its own RRC 0.2 filter;
    10-symbol FSW (0.88 threshold), whole-frame descrambler (negates
    symbols), LICH (8 bits on the first bit of 8 symbols, second bits 1,
    even parity of bits 7..4), SACCH (5x12 interleave, puncture every 6th
    from 5, K=5 G 0x19/0x17, CRC-6 0x27) and FACCH1 (9x16, every 4th from 1,
    CRC-12 0x80F). A SACCH counts only when it follows the previous good one
    exactly one frame later (CRC-6 alone passes 1 random word in 64);
    FACCH1 counts alone. Layer 3: VCALL (unit/TG, cipher), TX_REL, DCALL,
    trunking CAC (site / service info, grants). Voice: 4 (or 2) AMBE+2
    frames per frame -> mbelib (nW..nZ)
  - `YsfReceiver` (plugins/ysf, 2026-11): Yaesu System Fusion, written
    from Yaesu's "Amateur Radio Digital Standards" 1.02 alone (section
    numbers are that document's). 4800 Bd C4FM, RRC 0.2 (span 4, 10 sps);
    20-symbol FS D471C9634D (0.75 normalized threshold, either polarity,
    level / centre fitted on the FS). After a good frame the next one is
    tried on the 100 ms grid (+-3 samples, the last good level when the FS
    fit is off) even without a sync peak; 4 misses = out of sync (4.7).
    FICH: 5 x 20 dibit deinterleave, K=5 G 0x19/0x17 soft Viterbi, 4 x
    Golay(24,12) (Appendix A = `golay24()`, checked row by row), CRC-16
    (init 0, inverted, S15 sent first) - nothing is reported unless it
    passes (random input passes ~1 in 300000; ~3 sync candidates a second
    in noise). DCH units: 9 (160 bits) or 5 (80 bits, V/D mode 2) x 20
    deinterleave, Viterbi, CRC over the WHITENED data, PN(9,5) (init
    111001001, out S00, S08 <- S00 ^ S04). Layout by FI / DT: HC / TC and
    Data FR two 160-bit units in alternating 72-bit pieces (Data FR unit =
    2 FN + slot), V/D 1 DCH + 5 x 72-bit AMBE+2 frames (rW..rZ, like DMR),
    V/D 2 (DN) 40-bit DCH pieces + 5 x 104 voice bits (26 x 4 deinterleave,
    de-whitened, 27 bits x 3 by soft majority + 22) -> 49 bits ->
    mbelib's `mbe_processAmbe2450Dataf` (`MbeLib::decode49`, optional),
    Voice FR (VW) 5 x 144 (sub header FT 1 / FN 0: CSD3 + VCH 3, 4) ->
    `ImbeDecoder` in the YSF order (`vw_plain_index`: the plain IMBE
    codewords c0..c7 as a 24 x 6 block, read two rows per 12 air bits; at
    first P25's iW..iZ order, wrong - corrected 2026-11-16 after the dsdcc
    check). A call = valid frames < 1 s apart
    (two headers in a row = one; a repeated TC is no new call); its event
    waits for source + destination + CSD2 (late entry: at most 8 frames;
    an all-asterisk destination - an off-air call to everyone sent `*****`
    in Radio ID mode - is shown as "ALL", the fact keeps `***** (all)`);
    a lone FICH with no DCH and no second frame gives no call events.
    Ambiguities resolved: the FICH interleave (text "M = 5, N = 10",
    Figure 4-26: 20 rows and the air order - followed the figure); the CRC
    bit order (not given - S15 first); whitening before the CRC (Figure
    4-27; 4-30 / 4-31 omit it); "divided into 3 bits each" = each bit sent
    three times in a row (Figure 4-32's arrows), its output order S0 S26
    S52 S78 S1 ... taken over its contradictory direction labels; DG-ID =
    the 7-bit SQL code; CS = 10 not enforced. NOT in the standard: the bit
    order inside the voice frames (DN's 49 bits passed as received =
    mbelib's u0 u1 u2 u3 order, the 27 triplicated bits then being u0, u1
    and u2's top 3; V/D 1 as DMR's 72-bit frame; VW in the order the
    reference decoders document). The plugin's generator
    (`tools/ysf_gen.py`, written independently from the same document;
    VW = real IMBE frames) agrees bit for bit; dsdcc (GPL, run in scratch
    as a reference only, 2026-11-16) reads the generator's frames, CSD and
    voice bits the same way (DN 174 / 175, V/D 1 100 / 100, VW 161 / 169
    frames bit-exact - the rest its own symbol errors; plugin README).
    Off-air IQ recordings of a DN and a VW call (2026-11-17): every
    frame decoded, VW's IMBE codewords all valid as received (plugin
    README); DN (mbelib) and VW audio of those recordings is speech,
    V/D mode 1 voice not heard from a real radio yet. DN silence frames
    (b0 124 / 125) are muted, the leading ones of a call also reset the
    codec (`dn_lead_`) - they made a full-scale burst at the call's start
    (*Digital voice*).
    `KRAKEN_YSF_DUMP=1`: frames, DCH units, voice bits to stderr
  - `Mpt1327Receiver` (plugins/mpt1327): MPT1327 / MPT1343 analogue
    trunking signalling, 1200 bit/s FFSK (1 = 1200 Hz, 0 = 1800 Hz) on the
    FM discriminator. Two sliding one-bit (40-sample) tone correlators; each
    tone is normalized by its own running level (radio pre-emphasis makes
    1800 Hz louder - without it the sync never correlated). Sync = 4
    preamble bits + SYNC 0xC4D7 (control) / SYNT 0x3B28 (traffic), hard
    match <= 2 bit errors. The SYNC is the CHECK FIELD of the control
    channel system codeword (CCSC = 0, SYS 15 bits, CCS, 0xAAAA), so the
    CCSC starts 44 bits before the pattern; the address codeword (message)
    follows it. Codewords: 48 bits + BCH(63,48) g 0xE815 with the last check
    bit inverted + even parity; single-bit errors corrected. Messages: GTC
    grants (type < 256), Aloha variants, ACK*, AHOY*, status, CLEAR, MOVE,
    BCAST (SYSDEF), data headers; idents shown prefix-ident. Validated on
    a live NZ control channel (SYS 0x42E1, ~96 % of slots); GTC field
    positions follow sdrtrunk and have not been seen live yet
  - `TetraReceiver` (plugins/tetra, 72 kHz RRC 0.35 input): differential
    detection, 4th-power frequency estimate, slot sync from the training
    sequences (both spectral orientations tried), BSCH (MCC/MNC/colour code
    -> scrambling code), BNCH SYSINFO, AACH (Reed-Muller), SCH/F and SCH/HD
    MAC-RESOURCE with the MM/CMCE PDU type of clear signalling
- Report (`dig_report.hpp`): facts table per plugin id (key, value, age),
  event log (400 entries, identical texts de-duplicated per window; events
  carry `src` = plugin id and `p` = its name, "" = the decoder itself).
  `MessageBuilders::build_digital_message()` pushes `{"digital":[...]}` on
  TOPIC_CTL at 4 Hz (only while a decoder is on), with only the events since
  the previous push; `DIGITAL_HISTORY:id` replies with the whole log,
  `DIGITAL_CLEAR:id` empties it. Status: `mode`, `detected` (plugin id),
  `detected_name`, `frames` {id: [4 s, total]}, `plugins` [{id, name, state
  running|starting|missing|not built|crashed|error, error, dropped, log}],
  `info` {id: facts}, `voice`, `opts`, `map_capable` (a running plugin
  declares `kp::Info::map`), `map_points`, `tables` (below; only in some
  pushes), `choices` (below; in every push, `{}` = none)
- Tables (`kp::Host::table_columns / table_row / table_remove / table_keep`,
  wire 25-27, 32; `table_detail`, wire TABLE_DETAIL 38): one per plugin in
  `Report` (headings + rows by key, max 1000, a row not updated for the
  table's keep time - 10 min, or what the plugin set with `table_keep`, 60 s
  .. 7 days - left out; `table_clear()` empties the rows on a retune / mode
  change, headings stay; a row's details - label / value pairs, <= 200 -
  live in its `TableRow::detail`, kept across row updates, dropped with the
  row, a detail for a missing row ignored). `status_json(.., tables)`:
  `"tables":{"<id>":{"cols":[..],"rows":[[key, age_s, cells..]],
  "det":{"<key>":[[label, value]..]}}}` -
  `build_digital_message` adds it at most once a second per VFO (+ every
  history reply); the page keeps the last tables in between
  (`digOnMessage`, same mode). UI `digTablesHtml(tab)` merges the plugin's
  table of every VFO in the tab with leading "VFO" / "MHz" columns (row key
  `vfo|plugin|key` = its map point) + the tab's incidents -> `#info-dec-table`
  above the facts / log (`#info-body-dec.has-table`; the panel grows to
  420 px the first time): header click sorts (numeric when both cells are
  numbers, empty cells last; per plugin in localStorage
  `kraken_dig_table_sort`), "Seen" column from the row age, a row whose
  `vfo|plugin|key` is a map point is clickable (`digTableRowClick`: opens
  the map, selects + centres the marker); one whose key is a talker of that
  VFO selects it in the 📡 DF panel; one whose key is a value of the
  run-time choices of one of the plugin's options (below; 2026-11-17, dab:
  an audio service's SId) selects that choice (`digRowChoice` /
  `digPickChoice`: the dropdown's `DIGITAL_OPT:<vfo>:<id>.<key>:<value>`,
  not sent when it is the value set; for a `voice` plugin also the tab's
  Listen - `digListenFor` - when the VFO isn't heard, so a listener just
  hears the new choice). The first that applies counts: map point, talker,
  choice (the map / talker rules as before; a plugin wanting a selectable
  row keeps that key out of its map points and talkers). A choice row:
  `tr.choice` (pointer, green VFO cell, tooltip "Listen to this
  (Programme: ...)" / "Select this (...)"), and `tr.sel` when its value is
  the option's value as the receiver last reported it (no optimistic
  mark; a second page follows the broadcast). A setting that is no row's
  key - dab's "" (first audio service) or a part of a name - marks no row:
  the page can't know which service it resolves to, and a wrong mark is
  worse than none; after a row click or a list pick it is a SId (the dab
  plugin marks the programme it decodes in a ▶ column of its own - a plain
  cell, *plugin-dab*). Rows
  without choices (older receiver, data services) as before. Test:
  scratchpad rowclick/t_rowclick.py. A row with details gets a ▸
  toggle (`.dig-det-tog`, `digDetToggle` - stopPropagation, so the row's
  own click isn't fired; open rows in `digDetOpen`, this page only): the
  details render as a row of their own under it (`tr.dig-det`, a
  label / value table in `.dig-det-box` - sticky left, `100cqw` of the
  `.dig-table-wrap` container, so a table wider than the panel doesn't
  push them out of view)
- Options (`dig::Options`): verbose ("Show all messages" - "Log all"
  until 2026-11-19), log (the logging mode, 0 Off / 1 Log / 2 Log All,
  2026-11-19 - *decoder-log.md*; a new VFO / decoder starts at Off), map
  ("Plot on map", default OFF - the
  user ticks it in the decoder tab; persisted as `&m=1`;
  `DIGITAL_OPT:id:map:0|1` also asks for a full map push) + `plugin`
  {"<id>.<key>": value} - the plugins' declared options,
  sent to the plugin as OPTION key=value ("" = back to its default). An
  option declared `hidden` (plugin list `"hidden":true`, 2026-11-17 - dab's
  Programme) gets no control in `digVfoRowHtml` at any stage; it is still
  saved and set by `DIGITAL_OPT` (a table row click, `digRowChoice`).
  `DIGITAL_OPT:id:verbose|log|pos_s|map|<id>.<key>:value` (log: 0 / 1 / 2 or
  off / log / all; old keys dmr_slot / p25_nac map to dmr.slot / p25.nac).
  Persisted as the snapshot field `v=1&l=1&ps=10&dmr.slot=1` (values
  percent-encoded; the old `v/slot/nac/inv` form still loads; without `l` -
  stored by an older version - the VFO gets Log, or Log All where the old
  log types had `raw`). The Invert option (a conj before the mixer, for an
  I/Q-swapped source; no use with the KrakenSDR) was removed 2026-11-19:
  a stored `i=` / the old form's 4th field is ignored, `DIGITAL_OPT:id:invert`
  is refused, the plugins' `kp::Host::rf_inverted()` is always false; the
  plugins' own video options (`analogtv.invert`, `tempest.invert`: black /
  white) are a different thing and stay. Both go to the page in `digital_opts` (decimator
  info) and the status' `opts`; a page that finds no `log` there talks to
  an older receiver. **What the plugin gets as `verbose`**
  (`kp::Host::verbose()`, the OPTION verbose=0|1, `Engine::send_settings`):
  the VFO's verbose OR (log = Log All AND "Allow logging to disk" on,
  `dig::set_log_allowed`) - so Log All writes the repeated messages, and
  they are on screen then too. The host can't keep "verbose-only" reports
  apart from the others (the wire EVENT carries a de-duplication time and
  the text, nothing else; plugins just don't send them without verbose), so
  writing them without showing them would need a plugin-API change. Told
  that Log All would otherwise be the only way to see them, the user asked
  for the separate tick: "Yes add a seperate show all messages checkbox
  too" (2026-11-19). Raw frames (OPTION log_raw=1,
  W_RAW accepted) only under Log All with logging allowed. The panel shows a plugin's options while it is selected, or
  in AUTO once AUTO detected it. An option whose two choices are exactly
  "0" / "1" is an on / off button (`digVfoRowHtml`, `.dig-opt-tog`: "● Label:
  On" highlighted / "○ Label: Off", the choice's text without a trailing
  "(...)", the help + "click to turn on / off" as its tooltip; it sends
  DIGITAL_OPT with the other value and shows the echoed state) - dtv's Path
  separation, adsb's Array combining / Overlap separation (its Bit repair
  is a three-way list since v2.0), dme's DoA per beacon, cells' Cell
  information; other choice lists stay `<select>`s. The settings rows
  (`#info-dec-top`) are rebuilt only while no input / list in them has the
  focus or a mode list is open (`ddOpenSel` - wherever the focus is: the
  list's filter field is in body, not in the rows); a focused BUTTON doesn't hold
  the rebuild back (a clicked button keeps the focus - the on / off button
  showed its new state only after a click elsewhere) and gets the focus
  back afterwards (the same button by position). Test: scratchpad
  e2e/t_dtv_toggle.py clicks with real mouse events (`.click()` doesn't
  focus, so the first test missed it)
- Run-time choices (`kp::Host::option_choices`, wire OPTION_CHOICES 42,
  2026-11-16 - the dab plugin's Programme list): key + value / label pairs
  for one of the plugin's DECLARED options (others ignored). The engine
  (`on_option_choices`) checks them again - it is a child process's text and
  a DAB label is from the air: at most 100 pairs read, a value the page
  couldn't set (`DIGITAL_OPT`: > 100 bytes, a control character) or a
  repeated one dropped, a label's control characters made spaces and cut to
  80 bytes at a character boundary ("" = the value) - and `Report` keeps
  them per `"<id>.<key>"` (32 KB for all lists of a decoder; an empty list
  removes them). Run-time state only, never persisted: cleared by
  `clear_all()` (retune, mode change), for a plugin when its process
  (re)starts or crashes and when the runner set is rebuilt. Status:
  `"choices":{"<id>.<key>":[[value, label],..]}` in every push (the facts
  are too; a list is small), keyed like `opts.plugin`. Page: `digDynChoices`
  -> `digVfoRowHtml` draws such an option as a `<select class="dig-opt-dyn">`
  of them (also when the declaration is a text field; no on / off button),
  every value / label through `digEsc`; a set value that is none of them
  gets an entry of its own first (`[v, v || 'Default']`), so the list
  never silently shows its first choice. Without `choices` (older receiver,
  no list yet) the declared list / text field as before. The focus rule
  above keeps an open / focused list from being rebuilt; the next 4 Hz push
  after it loses the focus redraws it. An older page ignores the key; an
  older receiver ignores the message type (an unknown type has always been
  dropped by `on_message`'s `default:` - only a payload > 1 MB is a
  protocol error); an older plugin sends none. The AI assistant's
  `get_decoder` returns the status, `choices` included (its note says so)
- UI (kraken_doa.html, "Digital decoders" block): any number of VFOs decode
  at once. Each VFO card (Decimators box, `updateDecimatorList`) has a
  "Digital decoder" tick (`digToggle` -> `digOnMode`: DIGITAL_MODE with the
  VFO's last mode other than Off as decimator_info reported it (`digLastOn`,
  this page, dropped when the VFO goes), else the mode picked in this
  browser (`digWanted`, per VFO in localStorage), else AUTO. Before
  2026-11 it was only the latter two, so a mode set elsewhere - a debug
  playback's entry, a saved frequency, another page, the AI assistant -
  came back as AUTO after an untick / tick, and a manual-only decoder (dab,
  adsb ...) silently stopped) and a mode list
  (`digSetModeFor`: picking a decoder also switches it on) plus a state line
  (`digRenderCards`, also run at the end of updateDecimatorList, which
  rebuilds the cards). The mode lists (VFO card + the tab's Mode) are
  two-line dropdowns: a native `<select>` draws options on one line, and
  the then "SFN Separation (DVB-T/T2, ISDB-T, ATSC 1.0/3.0, DTMB, DAB)" ran past the
  card. The `<select>` stays as the hidden model (value, options from
  `digModeOptions`, its onchange) and `ddEnhance(sel)` puts a button
  (`.dd-btn`) after it + a pop-up list (`.dd-pop`, in body, zoomed like the
  sidebar when the button is in it); an option "Name (details)"
  (`ddSplit`) shows the name with the details on a smaller second line, in
  the button and the list. The plugins are listed in GROUPS (the user's
  request 2026-10-23; `digGroups`, also the saved-frequency editor's
  decoder list): one `<optgroup>` - a heading in the pop-up - per
  `kp::Info::category` (`--info` / plugin list `category`, case ignored),
  the shipped groups in a fixed order (Two-way radio, Aviation, Marine &
  distress, Paging & packet, ISM & IoT, Telemetry & tracking, Broadcast &
  cellular), other categories after them by name, none = Other last, each
  group by name; only plugins without one (built before categories) = the
  old single "Decoders" group. Code that sets `sel.value` calls `ddSync(sel)`
  (`digFillSelect` and the tab rebuild do); `digFillSelect` leaves the value
  alone while that list is open (`ddOpenSel`). Keyboard: arrows / Home /
  End / Enter / Escape / Tab; a press elsewhere, a scroll or a resize closes
  it. The pop-up has a filter field on top (`.dd-q`, the user's request
  2026-11-16; `ddFilter`): the query is split into words and an option stays
  when every word is a case-insensitive substring of its name + details,
  group, description (`title`) and plugin id (`data-k`); groups left empty
  hide, nothing left = a muted "No matching decoder" row; options are hidden
  in place, not rebuilt. With a fine pointer (`(pointer: fine)`) the field
  takes the focus at open; touch-only devices don't (it would raise the
  on-screen keyboard at every open) - tap the field, and a character typed
  while the button has the focus goes into it. Arrows / Enter work on the
  visible options (highlight: the current value while the query is empty,
  else the first visible option whose name or id starts with the query,
  else the first with every word in its name / details / id (`data-n`),
  else the first visible - "dm" is DMR, not D-STAR above it through its
  description); Space types a space (on the closed button it still
  opens); Home / End jump only while the field is empty; keys typed in the
  field stop at the capture handler, so no page shortcut sees them. Width
  and side (below / above the button) are fixed at open; one above is
  placed by its `bottom`, so it stays on the button as the filter shrinks
  it (`ddPlace`). A resize that changes only the height while the field has
  the focus (a phone's on-screen keyboard) re-fits the list instead of
  closing it. Tab labels and state lines use `digShort(name)` (without the
  "(details)"; the tab's tooltip has the whole name). Test (scratchpad
  e2e/t_dd_ui.py, headless Chromium, the real plugin list; the filter:
  t_dd_filter.py - headless Chromium reports `(pointer: none)`, so it
  overrides matchMedia for the fine-pointer run and uses touch emulation
  for the touch run). The bottom panel has one tab per decoder MODE
  (plugin), not per VFO: `infoTab` = `dig:<plugin>` (the AI tab is gone -
  the assistant has its own dock; Voice to Text is no tab here either - it
  has the panel to itself, *voice-to-text.md*)
  (`infoPanelOpen('dec:<vfo>')` still works: the tab that VFO is in,
  `digTabOfVfo`). A VFO is in tab P while it runs P fixed or Auto detect
  locked onto P (`digVfoPlugin`, `digTabVfos`). There is NO Auto detect tab:
  a VFO in AUTO appears in the tab of the decoder it detected (its mode list
  there says Auto detect), and in none while it is still searching (its VFO
  card shows "Searching…"). Tabs only for the decoders in use, a list
  for the rest (2026-11-18; the user accepted the recommendation to
  "tame the decoder tabs" - the strip had a tab for every installed
  decoder, 28 on the test build, in a sideways-scrolling strip - and
  asked for it "with a list for the inactive ones"):
  - **Which decoders get a tab** (`digTabsInUse`): a VFO runs it now
    (fixed, or Auto detect locked onto it), or it has something to show -
    events not hidden by Clear (live VFOs, VFOs since switched, removed
    VFOs' `digArchive`) or incident rows. Those a VFO runs first, then the
    rest, each group by name; the tabs' look, state dot and "· D0 D2" are
    as before.
  - **All decoders** (`#dec-all-btn`, "All decoders (28)" - the count of
    installed plugins) after the tabs opens a popover in the View
    popover's pattern (`decAllOpen` / `decAllClose` / `decAllPlace`): under
    or above its button (the panel is low on the page, so usually above),
    inside the window, not modal; closed by Esc (focus back to the
    button), a press outside, the button again, a pick, a scroll of what
    holds the button, a window resize, the panel closing or switching to
    Transcription (the closing ways are the shared `popDismiss` since
    2026-11-18 - *sidebar.md*, *View button and popover*; the placement
    stays its own: it fits the list's height). A press on a help-mode `?` is no press outside, and in
    help mode a tap on a row shows its tip instead of picking it. On top a
    filter box (`#dec-all-q`, a combobox; name or id, case-insensitive),
    focused when the list is opened with a mouse or the keyboard, not on
    touch (no on-screen keyboard popping up); then every installed
    decoder (`digPlugins`, plus a decoder in use this receiver lacks)
    under "In use" (the tabs' decoders, in their order) and "Not running"
    (by name): a row (`role="option"`) with the tabs' state dot, the short
    name, the VFOs running it, and a plugin problem - "Not built",
    "Error", "Out of date" (`built` / `error` / `stale`, as in Setup
    Settings → Decoders), "Not installed"; the row's `title` has the whole
    name, the id, the state, the reason and the plugin's description.
    Arrow keys move the highlight (`aria-activedescendant`), Enter picks,
    typing filters. While filtering the popover keeps the height it opened
    with, so the filter box does not move. On a phone it is as wide as the
    screen allows, rows 42 px.
  - **A picked decoder that is not in use** is a tab while it is the shown
    one (`digTabList` = the decoders in use + `digTabShown`, at the end, in
    the idle look), so the strip always shows where you are; it leaves the
    strip when another tab is picked. Nothing is stored for it beyond the
    selected tab in `kraken_info_panel` (a reload restores it as the
    shown tab; an id no longer installed falls back). Its body is the idle
    one: "Not running" + "Start on D0 D1 ..." (`digSetModeFor`). The same
    holds for a shown decoder whose VFO stops elsewhere (another page,
    the assistant): the panel never leaves a shown decoder by itself; but unticking a VFO's
    decoder here (`digToggle` off) moves the panel off a tab left with
    nothing in it, to the first decoder in use or the empty state.
  - **Nothing in use**: the strip shows only All decoders, `infoTab` is
    `'dig:'` and the body says "No decoder is running" ("Auto detect:
    searching · D0" while a VFO in Auto detect has found nothing) + a `?`;
    as soon as a decoder is in use the panel shows it (`digRenderAll`).
  - The strip (`#info-tab-list`) still scrolls sideways when many are in
    use; All decoders stays visible right of it, the ✕ after it.
  - Test: `test_dectabs.py` in the job's scratch directory (headless
    Chromium: the empty strip, the list against `digPlugins`, groups,
    filter by name and id, keys, Esc / outside / pick, help mode, an idle
    pick as a tab, a VFO switched on / off, data keeping a tab - the ADS-B
    debug playback -, `dec:0` and reloads, Transcription, 1700 / 1300 /
    560-px-high windows, a 400 px phone with touch).
  The panel follows a VFO whose mode is picked
  (`digSetModeFor` / `digToggle` -> `infoPanelOpen('dec:<id>')`). A tab
  (`infoRenderDecoder`) shows: a settings row per VFO in it
  (`digVfoRowHtml`: coloured VFO + MHz + state, mode, Listen - only for
  voice decoders, `kp::Info::voice` (p25 dmr dstar nxdn ysf tetra; `digHasVoice`:
  in AUTO the detected plugin, else any ticked one) -, the plugin's options -
  in AUTO those of the detected plugin -, then `digLogCtlHtml`: **Logging**
  (a select: Off / Log / Log All - the user's words, 2026-11-19, in place
  of the "Log all" tick; greyed while logging to disk is not allowed: Log /
  Log All disabled options, the select showing Off whatever is stored, a
  small Decoder Logging button beside it, the tooltip naming the stored
  mode), **Positions every [N] s** (2026-11-19, only for a decoder whose
  plugin reports positions - `kp::Info::map`, `digReportsPositions`; the
  per-VFO option `pos_s`, `DIGITAL_OPT:<vfo>:pos_s:N`, 1-3600, default 10:
  the decoder log keeps at most one position per object in that time -
  greyed while the VFO's Logging is Off or logging is not allowed;
  *decoder-log.md*) and **Show all messages** (the verbose tick, "Log all" until then;
  ticked and disabled while Log All is in effect, its tooltip saying why;
  the stored value comes back after), then Plot on map,
  bandwidth / plugin problems; an older receiver - no `log` in the options
  - gets the tick only), a decoder log problem (`digLogWarnHtml(ids)`,
  one line under the rows while logging is on and not writing / dropping /
  the disk is nearly full and one of the tab's VFOs logs, with a button to
  Setup Settings' Decoder Logging - *decoder-log.md*), the incident row (messages plugins), the
  tables (below), per VFO a status + facts block (`digFactsHtml(id, tab)`:
  only that plugin's facts / frames), the first
  plugin stderr, and ONE event log merged from every VFO (`digTabEvents`:
  events whose `src` is the plugin - from VFOs in it, VFOs since switched
  to another mode, and removed VFOs (`digArchive`, this page only, marked
  "D3†") - plus the decoder's own lines (no src) of the VFOs in the tab since
  their last "Decoder set to / on"; each line tagged with its VFO in the
  VFO's colour). Clear (`digClearTab`) hides the tab's lines up to now
  (`kraken_dig_cleared`) - the decoders keep their logs (another mode's tab
  still needs them) ⚙ Setup Settings → Decoders
  (a sidebar box, "🔐 Digital Decoders", until 2026-11-17 - *sidebar.md*,
  "Setup Settings dialog") shows the plugin list (`aiRenderPlugins` -> `#ai-plugins`:
  Auto detect tick, ↻ = `PLUGINS_RESCAN` after a manual copy + make; no
  export / import / rebuild in the web UI - plugins move as folders + `make`;
  a plugin is put on a VFO with the VFO card's decoder list; the
  heading shows the count, the Decoders tab a ⚠ count of plugins with an
  error / stale / unbuilt - `#set-tab-dec-n`) and the
  voice codecs (`digOnCodecs`): IMBE / AMBE
  (mbelib) / ACELP (ETSI TETRA) installed or not, with the README's install
  steps for a missing one. Source: `plugins/lib/build/codecs` (lib/tools/
  codecs.cpp, built by plugins/Makefile) - the SAME MbeLib / TetraCodec
  detection the plugins use, run by kraken_doa with its environment
  (KRAKEN_MBELIB, KRAKEN_TETRA_CODEC_DIR, PATH) via
  `dig::codec_status_message()` on a worker thread, on AI_STATE (page
  connect) and `PLUGIN_CODECS` ("↻ Check codecs") -> `{"codecs":{checked,
  imbe_ok, imbe, ambe_ok, ambe, acelp_ok, acelp}}`. Running plugins dlopen
  mbelib once, so a newly installed codec needs the decoder switched off and
  on
  `DIGITAL_HISTORY:id` is asked once per VFO / mode. Demod "Digital" keeps the
  VFO's bandwidth (widened only below a fixed decoder's need - it used to
  force NBFM's 16 kHz, too narrow for TETRA; never for Auto detect on a
  receiver with internal streams)
- Bandwidth: each plugin's `min_vfo_rate` (12 kHz for the 4FSK ones, 24 kHz
  for TETRA). A fixed decoder: the panel warns and offers the narrowest
  wide-enough option. Auto detect needs none (*Internal streams*); an older
  receiver (no `auto_streams`): the largest of the ticked set, warned and
  widened as before
- CPU (Pi 5, 24 kHz VFO): one plugin ~3% of a core, AUTO ~11% with the six
  protocol plugins and ~15% with all nine shipped/user plugins ticked (each
  process has its own front end; was ~4.5% in-process) - unticking unused
  decoders saves it. Since 2026-11-20 a narrow VFO's Auto detect also runs
  the wide plugins on internal streams (rds, ils, analogtv): ~+50 % on top
  of that, measured on x86_64 (*Internal streams*)
- Offline test harness: `tools/digi_test.cpp` (runs the plugins like a VFO,
  synchronously: waits for slow plugins instead of dropping, SYNC-drains at
  the end; .dis files are FM-modulated again; `--opt dmr.slot=1`; run from
  kraken_doa_v2): `g++ -std=c++20 -O2 -Iinclude -isystem
  liquid-dsp/build-kraken/include tools/digi_test.cpp src/digital/*.cpp
  src/utils/process_posix.cpp src/utils/fs_compat.cpp src/ai_sandbox.cpp
  src/geo_address.cpp liquid-dsp/build-kraken/libliquid.a` (liquid built by `make` first - see
  *liquid-dsp*; the plugin processes are kproc's, `utils/process.hpp`; the
  Windows command line is in the file's header)
- Validated on live P25 (VFO on a trunked control channel + its voice
  channel) and DMR Tier III here, and on recordings for TETRA, D-STAR, DMR,
  NXDN, MPT1327 and P25 (regression of 33 checks incl. voice passes through
  the plugins)

