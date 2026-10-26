# DoA per talker

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**DoA per talker (P25 / DMR / NXDN unit IDs, D-STAR callsigns, ADS-B aircraft, AIS ships, DME beacons, ISM devices, APRS stations, radiosondes, TETRA cells, MPT1327 sites, VOR / ILS idents, pulse radars, 1030 MHz interrogators, UAT aircraft / ground stations, FLARM / OGN aircraft, analog FM radios (MDC1200 / FleetSync IDs, 5-tone / DTMF, CTCSS tones / DCS codes), 406 MHz distress beacons, LoRa / Meshtastic nodes / LoRaWAN DevAddrs, wildlife collars / coded tags (any country), FM broadcast stations (RDS PI); `src/talker_doa.cpp`, `plugins/{p25,dmr,nxdn,dstar,adsb,ais,dme,ism,aprs,radiosonde,tetra,mpt1327,ils,radar,ssr1030,uat,flarm,analog_id,beacon406,lora,wildlife,rds}`, 🗺 Map 📡 DF panel):**
- RULE (ID separation, the user's 2026-10-15): every decoder whose signal
  carries a coded ID per TRANSMITTER reports it as a talker - a core part
  of every such decoder, new ones included. How + how to test:
  plugins/SDK.md *ID separation*. Not applicable (README says why): pagers
  (the RIC / capcode is the recipient; simulcast transmitters send identical
  signals), v23_telemetry (proprietary payload, no known address), dtv
  (SFN copies are identical - separated by delay instead), radiosonde's
  iMet-4 (sends no serial), glide slopes / marker beacons (no ident)
- Goal: a VFO on a P25 / DMR / NXDN / D-STAR channel carries many radios; the VFO's DoA
  mixes them. The decoder names who transmits when, the VFO's signal is cut
  at those boundaries and each radio's own samples go through MUSIC - one
  bearing (+ history + mobile DF heat map) per unit ID, picked on the map
- Plugin API (kraken_plugin.hpp): `kp::Talker {id, label, start_s, end_s,
  channel}`, `Host::talker(t)` (again per frame: end_s grows; a new id ends
  the previous one ON ITS CHANNEL where it starts) / `talker_end(at_s,
  channel)`, `Info::talkers`
  (`--info` "talkers", `PluginInfo::talkers`). Times are `Host::time()` =
  plugin input samples / rate since the process started (WireHost: during
  process() it is the block's FIRST sample). Wire TALKER 30 (id, label,
  start, end as "%.4f" text [, channel - omitted for 0]) / TALKER_END 31
  (at [, channel]). The offline test host cuts spans like the live host and
  prints them ("talkers: N transmissions", "ch N")
- Channels (DMR timeslots): `Runner::talkers` holds one open talker per
  channel; `TalkerSpan::channel`. TalkerDoa commits a frame inside a
  channel != 0 span only `CHANNEL_HOLD_MS` (1.5 s) after MUSIC computed it -
  the other slot's radio may be reported later with a start backdated over
  it. Id "?" (`tdoa::unnamed_talker`) = a busy channel whose radio isn't
  known yet: its span blocks frames like any other (two ids -> dropped) but
  creates no talker; frames only inside it wait. When the plugin learns the
  id it reports it from the same start, so the host closes "?" at zero
  length. Lib plugins: `dig::Bridge::talkers(rate)` sets RxContext::talker /
  talker_end (receiver indexes + channel -> Host::time()), `restart_clock()`
  in reset()
- DMR (`DmrProto`, channel = slot 0 MS / direct without slot, 1, 2): burst
  `burst_a_ = sync_end - 77.5 sps`, 132 symbols. A transmission = confirmed
  bursts (voice LC header, voice B..F with a valid EMB, terminator) < 0.5 s
  apart, talker = full / embedded LC source (FLCO 0 "TG n", 3 "unit call to
  n"; label + talker alias, " · slot N", " · via repeater" for BS-sourced);
  ended by the terminator or `tick()`. A busy slot 1/2 without an ID yet is
  reported as "?". BS bursts whose slot the CACH didn't give are skipped.
  Fixed with it: bursts extrapolated from a sync (sync NONE) took the slot
  from the CACH even on MS-sourced / direct signals, which have none - a
  random slot, so B..F (and the embedded LC) never decoded there; they now
  take the kind + slot of the burst they follow (`ext_bs_`, `ext_slot_`)
- NXDN (`NxdnReceiver`): frame `frame_a_ = sync_end - 9.5 sps`, 192
  symbols. A transmission = valid frames with voice (LICH option != 0, not
  UDCH) or a VCALL, < 0.5 s apart; talker = VCALL source (label "TG n" /
  "unit call to n", " · via repeater" when the LICH says outbound); TX_REL
  ends it (its frames included); `tick()` from process()
- D-STAR (`DstarReceiver`, no FEC on voice frames): sync events in sample
  order from correlate(): a data sync in the 420 ms cadence (<= 2 missed)
  confirms everything up to it; the first sync of a transmission exactly
  756 bits after a header frame sync starts it at that header (else late
  entry from the sync); the end pattern confirms up to its start and ends
  it; 2 missed syncs end it. Talker = MY callsign (padding squeezed: "F1ZIL
  B"), label "/sfx to UR via RPT1", from the RF header with a good CRC
  (matched by its sync position - the CRC job runs after the first data
  sync may have been seen) or, on a late entry, the slow-data header copy
- P25 (`P25Proto`): frame boundaries from the sync - frame dibit 0 is centred
  23 symbols before `sync_end`, so `frame_a_ = sync_end - 23.5 sps`, length
  HDU 396 / LDU 864 / TDU 72 / TDULC 216 dibits. A transmission = valid
  HDU / LDU1 / LDU2 frames < 0.75 s apart (`gap()`); its talker = the LDU1 /
  TDULC link control source (LCO 0x00 "TG n", 0x03 "unit call to n";
  source 0 ignored). Reported on every valid voice frame (start = the
  transmission's first frame, end = this frame's end), ended at the TDU /
  TDULC start, at a new HDU, or by `tick()` (called after each block) when
  frames stopped for gap(); a source change without a terminator ends the
  old one at the end of its last LC frame. Positions are receiver samples
  (48 kHz since its reset); decoder.cpp maps them with `t0_` = host.time()
  at the reset (RxContext::talker / talker_end, dig_common.hpp). On
  iq_143.0.u8 (-787.5 kHz): 2010621 0.134-0.314 s, 3250 0.912-2.255 s,
  2010621 6.813- s - matching the decoded frames
- Generators in the repo (2026-10-16): `plugins/p25/tools/p25_gen.py`
  (HDU, LDU1 link control / LDU2 encryption sync, placeholder IMBE frames
  with valid FEC, TDU / TDULC, status symbols, C4FM = raised cosine x
  inverse sinc) and `plugins/dmr/tools/dmr_gen.py` (repeater: CACH, both
  timeslots, idle bursts; direct mode TS1 / TS2; voice LC header,
  superframes with EMB + embedded LC, terminator, placeholder AMBE+2
  frames with valid FEC) -> playback `p25-three-units` (2010621 / 3250 /
  1234567 at 60 / 200 / 300 deg) and `dmr-repeater-and-direct` (a repeater
  at 300 deg with 3100001 on slot 1 and, overlapping, 3100002 on slot 2;
  3100011 direct TS1 at 60, 3100012 direct TS2 at 200): offline every
  frame / burst decoded, every talker 0.0 deg off
- Sample clock: `DecimatorInstance::stream_pos` counts the VFO's decimated
  samples; run_pipeline stamps each block (`MultiChannelDecimated::
  stream_pos`, fetch_add of min_samples) and passes it to MUSIC and to
  `DigitalDecoder::push(.., pos)` (NO_POS = count locally, offline tests).
  Each `Runner` keeps `pos_map` (plugin sample p0 + i = VFO position v0 + i
  * in_rate / plugin_rate, per SENT chunk - dropped chunks never reach the
  plugin's clock; 60 s kept) and turns TALKER times into VFO positions ->
  `dig::TalkerSpan {plugin, id, label, start, end, closed, rate}` ->
  `DigitalDecoder::set_talker_handler` (set by `DecimatorManager::
  ensureDigital`, holds the TalkerDoa, never the instance). Open talkers are
  closed by the host on RESET (retune / mode change), process restart /
  crash / rebuild and stop
- MUSIC: `acc_pos_` maps accumulator samples to stream positions, so
  extractSnapshotsOptimized knows each frame's [a, b); the frame's OWN
  trace-normalised covariance (before the temporal averaging, which would
  mix in earlier frames) goes to `setFrameTap` for every computed frame -
  published or not (publish hold), but none while MUSIC is
  skipped (FFT squelch closed, calibration, retune hold).
  `spectrumFromCovariance(R)` = MUSIC on a given R with the live steering
  vectors (2D max-projection for 3D custom arrays), source count (auto: the
  estimate without hysteresis), half-plane and array offset, own
  eigensolver - the processor's results untouched
- `tdoa::TalkerDoa` (one per VFO, `DecimatorInstance::talker_doa`, active
  while its decoder is on - `setDigitalMode`; off = forget all): frames wait
  (<= FRAME_WAIT_S 6 s - the unit ID comes ~0.5 s + queue after the call
  start) until a span claims them: a frame lies GUARD_S (40 ms) inside one
  talker's span and touches no other talker's span -> its covariance is
  added (weighted by samples) to the talker's CURRENT transmission (one
  span = one transmission; a new one pushes the previous bearing into
  `hist`, max 20) and MUSIC is re-run on the sum (dirty ones each update,
  all every 2 s so array setting changes follow); the frame alone is also
  run through MUSIC and returned as a `TalkerFrame` (a mobile DF record).
  Frames straddling a boundary / two radios / gaps are dropped. A MUSIC
  frequency move > 10 kHz resets it. Talkers kept 30 min after last heard
  (packet talkers: below), MAX_TALKERS 500 (the least recently heard go
  first). Lock order: the tap runs under the MUSIC lock and takes ours;
  update() never calls MUSIC while holding ours
- rdf_mapper: the sampler (10 Hz) calls `update()` + `snapshot()` per VFO
  with DoA (coherent + DoA on) and feeds each TalkerFrame into the key
  {vfo, tid}'s own gate -> its own solver (created with its first record or
  a saved session; at most MAX_TALKER_MAPS 32 in memory - the stalest is
  parked, ~3 MB of grids each). The status adds `talkers:[{vfo, tid, label,
  active, tx, tframes, tx_frames, age, tx_age, doa (array frame, as the
  DoA plot), lobe / peak (the latest transmission turned to north with the
  heading at its last frame, cached once the 20 s track no longer reaches
  it), conf, held, frames, gate_m, moved_m, records, hist:[[age, compass |
  null, doa, conf, frames]]}]`, and `tfull`: false = only the talkers
  heard in the last TALKERS_RECENT_MS 10 s (every push but each
  TALKERS_FULL_EVERY 5th - 2.5 s - and the one after GET_RDF; an ISM
  receiver keeps hundreds of devices for 24 h, all of them twice a second
  was a few hundred kB/s per page). Those partial pushes are sent as
  `{"rdf_part":..}` (same fields), the full ones as `{"rdf":..}`: a page
  loaded before partial pushes existed - an open tab reconnects by itself
  after the stack restarts, keeping its old code - took each partial push
  for the whole list and the DF list flashed between one device and all of
  them; it ignores `rdf_part` (full list every 2.5 s, lobes 0.4 Hz until
  reloaded). `rdfOnStatus(m, partial)` keeps the others from the
  previous pushes, their ages (age, tx_age, hist) moved on by the time
  since, `active` false. `rdf_grid` has `tid`. `RDF_RESET:<vfo>`
  resets the VFO and its talkers, `RDF_RESET:<vfo>:<tid>` one talker
- UI ("Mobile DF" block): `#map-df-tid` (shown when the selected VFO(s)
  have talkers): Whole signal ('' - the VFO as before) / Every radio ('*':
  every talker's latest lobe + bearing labelled with its ID, own colours
  from the ID hash, dashed + faint when not on air, estimates) / one radio
  ('vfo|tid': its lobe, earlier transmissions as faint bearing lines, its
  heat map / lines / estimate, status line). `#map-df-tl` lists the
  talkers (on air ●, ID, label, bearing - compass, or "arr" = the DoA
  plot's angle when there is no heading -, last heard, transmissions);
  clicking a row selects it (again = Whole signal). `mapState.rdfTid`
  (localStorage); `rdfGrids` keyed "vfo|tid"
- Order (`rdfTalkersListed`, list + dropdown), per kind (`rdfKinds`): the
  column heading the user clicked (`th[data-sort]` -> `rdfSetSort`, picked
  by press + release like the rows; again = reversed, counts start with
  the most; `mapState.rdfSort[kind] = [col, 1|-1]`, localStorage; ▲ / ▼ on
  the heading), else `RDF_SORT_DEF`: radios newest first; packet talkers
  (aircraft / ships) NEAREST first by their reported position (Dist
  column; needs the decoder's Plot on map), those without one by name -
  newest first reshuffled a busy ADS-B list with every packet; DME beacons
  and ISM devices by name (newest first moved the device the user was about
  to click on every transmission, and devices heard in the same second
  swapped places on every push: there was no tie-break). Ties always go by
  name, ID, VFO; empty values last. Radios stay above packet talkers. Name
  and reading cells are clipped (130 / 80 px, `.clip`, full text as the
  tooltip) so Bearing / Heard / Tx fit the 380 px panel. The list is only
  built while the panel is open. Column headings per kind (`heads` in
  rdfRenderPanel; a kind missing there gets the generic `talker` ones - until
  2026-10-16 a missing one threw: LoRa nodes ('node') were in the ID
  selector but the list stayed empty and the map wasn't redrawn after a DF
  push or a panel control (`mapSet` -> `mapUpdateControls` threw before
  `mapDraw`) - the user's "nodes only in the lower info pane"). A NEW KIND needs
  `RDF_PK_KIND`, `rdfIcon`, `heads`, `RDF_SORT_DEF` (and `RDF_PK_UNIT` /
  `RDF_POS_KINDS` / `RDF_KIND_WORD` where they apply). Dist / Δ pos columns
  only while a listed talker has a reported position (map point + station):
  empty ones pushed Heard / Tx out of the panel. Aircraft are named by callsign (`rdfShortName` / `rdfAirParts`:
  the ADS-B talker label is "CALLSIGN · 35000 ft"): the lobe label, the
  list's first column (bold; ICAO second), the dropdown "CALLSIGN (ICAO)";
  the ICAO until a callsign is known. A row is picked by press + release
  (`rdfTlDown` / `rdfTlUp`, the row remembered at the press, <= 8 px
  movement), not `click`: the list is rebuilt several times a second and a
  click whose press and release hit different row elements was lost
- On the map: the talker whose lobe is shown alone (`rdfTrackedPt()`: one
  picked in the list / ID selector, with a map point - its decoder's Plot on
  map) gets a 19 px ring in its lobe's colour; the point whose popup is open
  (also a decoder-table row click, `digTableRowClick`) a white 14 px one -
  both with a dark edge. A decoder point's popup (`mapPointPopupHtml`) has a
  "📡 DoA" button in its header when the point is a talker of the DF status
  (`rdfPopupTalker`: same VFO + id; coherent only): `rdfTrackBtn` picks it
  (switches the panel's VFO select to its VFO unless All, opens the panel,
  Lobe on), again = Whole signal; "DoA ✓" while picked. It acts on
  pointerdown: the popup is rebuilt every second ("Updated N s ago")
- Packets (ADS-B per aircraft; `kp::Talker::packet`, wire flags field "p",
  times sent with 7 decimals; since adsb v2.0 - every antenna, option
  `pulse_doa` - each with its own pulse covariance, `kp::Talker::cov`, so
  the ring below isn't kept for it - see *ADS-B*): the plugin reports every accepted message
  of a confirmed aircraft (`queue_packet` in plugins/adsb: start = the
  first preamble pulse - mag_ index + timing phase / 5, mapped through
  `abs0_` / `blk_abs_` to host.time() -, end = + (8 + bits) us; offline
  check on a synthetic feed: start within 0.5 sample of the truth, sd
  0.16). Engine: packet -> one closed TalkerSpan {packet} (vfo_pos rounds;
  <= 50 ms). TalkerDoa: while packets come (`want_blocks_`, off after
  PACKET_IDLE_MS 30 s without, ring freed) the pipeline calls
  `add_block()` for every block except while heimdall calibrates / the
  retune hold runs (a gap: packets across it are dropped) - NOT gated by the
  FFT squelch (the decoder confirms each packet itself; until 2026-10-13 it
  sat inside MUSIC's squelch gate, and on a 2.4 MHz ISM VFO with the FFT
  squelch on the averaged spectrum opened late or never for a short burst:
  devices decoded at once took minutes to reach the DF list, or never did)
  -> per CHUNK (16) stream samples the
  upper-triangle covariance into a ring of RING_S 1.5 s (5 ch at 2.4 MHz:
  ~27 MB, ~8 % of a Pi 5 core); a packet = sum of the chunks entirely
  inside it (>= 2), trace-normalised; waits PACKET_HOLD_MS 200 for an
  overlapping packet of another talker (both dropped). Per aircraft
  R = sum of packets weighted exp(-dt / PACKET_TAU_S 1.5 s) in stream
  time, MUSIC at most every PACKET_RECOMPUTE_MS 1 s, `hist` every 10 s
  (its bearing track), tx = packets, tx_frames = packets in the average;
  forgotten after PACKET_TTL_MS 2 min (or 30 x avg_s, or the plugin's
  `kp::Talker::keep_s` - wire TALKER field 13, `TalkerSpan::keep_s`,
  `TalkerInfo::keep_s`; ism: its "Keep devices" option, 24 h). No mobile DF
  records. rdf_mapper: `pk:true`, lobe turned with the heading at
  `spec_ms`, no lobe 60 s after its last packet (a talker with `keep_s` - ISM devices - keeps its lobe as long as it is kept: picked in the DF list hours later it still shows; "Every ..." draws only those heard in the last 60 s). UI: ✈ rows, "Every
  aircraft", Δ pos column + selected-aircraft line = DF bearing minus the
  bearing to its ADS-B map point (needs the decoder's Plot on map;
  `rdfPosCheck`), "ADS-B check" = median / quartiles of that over the
  aircraft heard < 30 s (`rdfPosSummary`) - a steady offset = array
  mounting / heading error.
  What became of a VFO's packets is counted (`TalkerDoa::packet_stats()`,
  `PacketStats`: in / ok / late - its samples had left the ring / gap - the
  ring started after it / early / overlap / other, the report lag - stream
  past the packet's end when it came - last / median / max of the last
  minute, the ring's length, the latest 20 drops with id, reason, lag) ->
  the DF status per VFO `pk` -> the 📡 DF panel's "Dn: N decoded, M with a
  bearing · k too late (reported up to x s after, y s kept), ..." line
  (`rdfPkLine`, the drops in its tooltip). Added 2026-10-13: ISM devices
  decoded at once that never reached the DF list could not be told apart
  (every drop was silent).
- Channel filtering (AIS): `kp::Talker::freq_hz` / `bw_hz` (packet's
  centre in the plugin's band - the engine maps it to the VFO's: + afc_hz_,
  mirrored with invert), `avg_s` (averaging; also TTL = max(2 min, 30
  avg_s) and the lobe kept max(60 s, 15 avg_s)). Wire TALKER fields 7-9.
  TalkerDoa keeps the raw samples while the VFO rate <= RAW_MAX_RATE (500
  kHz; 5 antennas at 100 kHz ~ 6 MB for RING_S) and `packet_cov_raw`
  mixes each antenna by -freq, Hamming-windowed-sinc low-pass (cut-off
  bw / 2, ~3 fs / bw taps, run-in from the samples before the packet),
  covariance over exactly the packet; overlapping packets are only dropped
  when on the same channel. Above 500 kHz the chunk ring as before (ADS-B,
  DME; freq ignored) - unless a packet named a channel <= WIDE_CHAN_MAX_BW
  250 kHz (the ism plugin's devices in 2.4 MHz): `want_wide_` then keeps the
  samples themselves as 16-bit (`wide_raw_`, x WIDE_SCALE 8192, WIDE_RING_S
  2.5 s: 120 MB at 2.4 MHz with 5 antennas; not released after
  PACKET_IDLE_MS - a fob / door sensor after a quiet spell lost its
  transmission - only with the decoder. It was 1 s: the ism plugin reports a
  device when its burst group ends, up to MAX_LEN_S 1.2 s after the device's
  rows when other activity keeps the group open, + the queue - live 1.3-1.8
  s for a weather station on the tuner centre, 8 of 19 transmissions without
  a bearing, seen with the drop counters below) and `packet_cov_wide` applies the
  same channel filter as complex taps h[t] e^(-iwt) (mixing first differs
  only by a phase common to all antennas) at <= WIDE_MAX_OUT 1024 points
  spread over the packet, at most one per fs / (3 bw) - the filter at all
  576k samples of a 0.24 s packet was ~1 G multiply-adds; ~2 M now; without
  a channel (digital squelch) <= 16384 samples spread over the piece. A
  channel <= 10 kHz that the 401 taps can't resolve (2 ceil(3 fs / bw) + 1 >
  401: a wildlife collar's 1 kHz in 2.4 MHz - the taps passed ~36 kHz)
  goes through `narrow_cov` in both raw modes: mixed down and summed over D
  = fs / (16 bw) samples, then the Hamming sinc at fs / D (<= WIDE_MAX_OUT
  outputs) - no existing plugin's channel is that narrow. The
  16-bit ring costs less per sample than the chunk ring (3.6 vs 7.8 % of a
  Pi 5 core, scratchpad bench/ring_bench). 2026-10-26 (performance pass):
  `add_block`'s loops take the antenna count as a template parameter
  (`chunkCovT` / `storeWideT` / `storeRawT`): at 2.4 MHz x 5 antennas the
  chunk ring 6.7 -> 3.4 % of a core, the 16-bit ring 5.4 -> 2.6 %, the
  float ring (240 kHz) 1.3x, on a 2015 MacBook Air (Pi / Windows not
  measured). The rings are bit-identical; the chunk ring now sums each pair
  over the chunk's samples (the same float sums in another order - closer
  to a double reference, 1.5e-7 vs 3.9e-7 of sqrt(R_aa R_bb)), packet
  covariances within 1.1e-8, bearings identical. Why (scratchpad e2e/ab.sh, 7
  devices across the 2.4 MHz, transmissions overlapping): filtered 7/7
  bearings exact in every run, whole band 5-6/7 (Schrader read the
  LaCrosse's 90 deg, a fob 96 deg) at the same kraken_doa CPU (~130 %).
  PACKET_IDLE_MS 5 min; MAX_PACKET_S 0.25 (5 AIS slots).
  Status `pl` = the talker's plugin (UI: ✈ / ⛴ / 📡 beacon for `dme` / 📟
  device for `ism` - `rdfKind`; "Every aircraft / ship / beacon / device",
  "Position check"; beacons and devices have no reported position: no Dist
  / Δ pos columns, no Plot-on-map hint; beacons counted in pulse pairs,
  devices in transmissions; both listed by name. A bearing whose lobe has
  gone - 60 s after the last packet - is the compass bearing from the
  station heading when the station is static (`rdfStaticCompass`), else
  the DoA plot's angle "arr")
- Packets with their OWN covariance (`kp::Talker::cov` + `m`, wire TALKER
  fields 14 / 15 "m" / "re,im,...", `TalkerSpan::cov_m` / `cov`; 2026-10-16,
  the lora plugin's separation by code - see *LoRa*): `add_packet` takes R
  from it (Hermitian, trace-normalised) instead of `packet_cov` - it sets
  neither `want_blocks_` nor `want_wide_` (no stream kept for such packets:
  a VFO whose packets all come coded holds no 120 MB wide ring), the
  `Packet::coded` one is never marked `bad` by an overlap (an uncoded packet
  it overlaps on the same channel still is: that one holds both), and its
  talker runs MUSIC with ONE source (`Talker::coded`, like spatial ones).
  `PacketStats::coded` = how many came so (DF status `pk.coded`; the 📡 DF
  panel's line "N decoded, M with a bearing (all / k separated by code)").
  The invert option conjugates it (every antenna's Q was negated). An older
  kraken_doa ignores the fields (split0 stops at 13): the stream path, as
  before
- ID separation added 2026-10-15 (packets, the channel always named):
  - APRS (`plugins/aprs`): a packet per frame, id = the frame's RF
    TRANSMITTER: the source, or the last path entry with the H bit ("*") when
    it is a callsign (a digipeater that inserted its call); repeated under a
    generic alias only (WIDEn / RELAY / TRACE ..., `generic_alias`) = no
    talker. Label = packet type / "digipeats SRC". Samples: the frame's last
    <= 0.24 s, 3 ms short of the decoded end (a 1200 bit/s frame is 0.15-2
    s); 0 Hz / 16 kHz, avg_s 30, keep_s 1 h
  - Radiosonde: a packet per frame, id = type + serial (= its map point /
    table key), label subtype + altitude; the frame's FIRST <= 0.24 s from
    its sync (the RS41's decoded length is the extended frame's 0.86 s - the
    end may already be another signal: the first version took the end and
    one RS41 frame read a DFM that followed at 140 deg -> 133.9 instead of
    30); bw 24 kHz for M10 / M20 / iMet-4 types, else 15 kHz; avg_s 10,
    keep_s 3 h. iMet-4: no talker (no serial)
  - TETRA (`TetraReceiver::cell_packet`): the downlink is the base
    station's - id "MCC-MNC CCn" (e.g. "530-1 CC12"; the colour code seeds
    the scrambling, so another co-channel cell's blocks fail the CRC here),
    label "LA n · carrier MHz" (from SYSINFO); every slot with a block that
    passed its CRC (BSCH, BNCH, SCH/F, SCH/HD) = one packet of the whole
    255-symbol slot, the RRC front end's 24-sample delay taken off
    (`input_delay`); 0 Hz / 25 kHz, avg_s 10, keep_s 30 min. Co-channel
    cells (frequency reuse) heard in turn keep their own bearings
  - MPT1327: each control channel slot with a good CCSC = a packet of the
    SITE, id "SYS hhhh", CCSC + address codeword (107 ms) 2 ms inside both
    ends; 12.5 kHz. Mobiles' return channel isn't decoded (their idents:
    not implemented); traffic channels name no transmitter
  - ILS / VOR (`plugins/ils`): id "<ident> LOC|VOR" while the ident was
    heard within 30 s on this carrier (within 50 Hz of where it was heard -
    another navaid on the channel doesn't inherit it); a packet = the middle
    0.24 s of every analysed block of the recognised navaid, at the
    carrier's offset, LOC 4 kHz / VOR 22 kHz wide, avg_s 10, keep_s 1 h
  - `dig::RxContext::packet` (dig_common.hpp): packets for the lib plugins
    in receiver sample indexes, mapped by `Bridge::clock(rate)` (tetra,
    mpt1327)
  - Pulse radars (`plugins/radar`, task 2 of the batch): no coded ID - a
    radar is told apart by its SIGNATURE (pulse kinds + PRI pattern), id =
    its frequency to 0.1 MHz; packets = its main-beam pulses only (unclipped,
    within 15 dB of the scan's peak, in a burst that dwells near the peak
    like its main beam: a lit reflector stays out), each with its own
    channel, avg_s 30, keep_s 1 h - see *Pulse radar*
  - 1030 MHz interrogators (`plugins/ssr1030`, task 3 of the batch): an
    SSR by its periodic all-call train + its checked UF11 code ("SSR II
    10"), a fixed interrogator by its steady level ("Fixed #n"), an ACAS by
    whisper-shout / its checked UF16 broadcast address ("ACAS 7C4A51");
    packets = each interrogation +-10 us (an SSR's only in its main beam),
    the whole band, avg_s 30 / 10 / 5, keep_s 1 h (ACAS 10 min) - see *1030
    MHz interrogations*. UI kind `interrogator` (rdfIcon: ✈ when the id
    starts with "ACAS", else 🛰; "Interrogator / Type · scan · PRF /
    Interr.", by name, unit "interrogation")
  - UAT (`plugins/uat`, task 4 of the batch): every ADS-B message an
    aircraft / vehicle / beacon sends itself (address qualifier 0 / 1 / 4 /
    5) = a packet of its address (sync .. last parity bit, the whole band,
    avg_s 5, keep_s 10 min, its position at the packet's time when NACp >=
    8); TIS-B / ADS-R messages are sent BY a ground station - packets of
    "GS site N" (the TIS-B site ID), like its uplink frames (avg_s 20, keep_s
    30 min, the station's uplinked position) - never of the aircraft they
    describe. UI: aircraft kind ✈ like adsb's, ground stations kind `gs` 🗼
    (`rdfKind`: pl uat + tid "GS ..."; "Station / UAT ground station · slot
    / Frames", position columns, by name) - see *UAT*
  - FLARM / OGN (`plugins/flarm`, task 4): every packet an aircraft sends
    itself = a packet of its address ("FLR DD1234", "ICA C81A2B", "OGN
    3C5A21", "RND / ANO ..."), its first sync chip .. last data chip, on its
    channel (freq_hz = the burst's measured carrier, bw_hz 160 kHz - aircraft
    transmitting at once on different channels of the VFO keep their own
    bearings), avg_s 10, keep_s 1 h; an OGN packet with a relay count names
    nobody who sent it: no talker. FLARM's payload is encrypted (unpublished
    keys): ID + bearing only. UI kind `flarm` (✈, 🪂 paraglider / hang glider
    / parachute, 🚁, 🎈 by the label's aircraft type; "Aircraft / Reg ·
    altitude · type / Pkts", position columns, nearest first;
    `RDF_KIND_WORD` names it "aircraft" in "Every ..." / counts) - see
    *FLARM / OGN*
  - Analog FM IDs (`plugins/analog_id`, was `plugins/ctcss`, task 5; the
    radio IDs 2026-10-22): an analog FM transmission = a TRANSMISSION
    talker (`host.talker` / `talker_end`, not packets) named by the
    transmitting radio's own ID when one was decoded (MDC1200 unit "MDC
    1029", FleetSync "FleetSync 100-1001" - radios sharing one CTCSS tone
    apart), else a 5-tone / DTMF sequence ("CCIR 12345", "DTMF 123"), else
    its sub-audible tone / code ("CTCSS 88.5", "DCS 023N"), else a tone
    burst ("Burst 1750"); label what was sent + the tone ("PTT ID · CTCSS
    88.5"), from the carrier's (or the tone's) onset to its reverse burst /
    tone off / carrier off, to a few ms; the DoA from the MUSIC frames
    inside it like a radio's. With the ID / tone squelch option set only
    those get talkers. UI kind `tone` (`rdfKind`: not a packet, pl
    analog_id or ctcss; 🆔 / 🔢 / 🔔 by the ID; "ID / tone / Signalling ·
    tone / Tx" + two columns only it has: Last = the latest transmission's
    length, Air = all of them - TalkerInfo `dur_s` / `air_s` from the spans,
    status `dur` / `air`, `RDF_SORT_KEYS` dur / air; newest first) - see
    *Analog FM IDs*
  - 406 MHz distress beacons (`plugins/beacon406`, task 7): every
    first-generation burst that passed its BCH = a packet of its 15-hex
    beacon ID (the 160 ms carrier + first bits, 0.24 s from the carrier's
    start found by the frame sync), `freq_hz` = its measured carrier,
    `bw_hz` 3 kHz (the channel raster - beacons bursting at once on other
    channels keep their bearings), `avg_s` = option `doa_avg` (1 = each
    burst alone), `keep_s` = `keep_h` (24 h); label "⚠ EPIRB · New Zealand ·
    MMSI ..." / "self-test · PLB · ..."; a second-generation burst (not
    decoded) = talker "SGB" (avg_s 1). UI kind `sar` 🆘 (`RDF_PK_KIND`
    beacon406; "Beacon ID / Status · type · country · ID / Bursts", Dist +
    Δ pos from the encoded position - `RDF_POS_KINDS` -, newest first,
    "Every distress beacon") - see *406 MHz distress beacons*
  - Wildlife collars / coded tags (`plugins/wildlife`, task 9): every
    pulse of a confirmed collar (pulse train, id = its MHz to the kHz) or
    Lotek / Motus-style coded tag (id = MHz + gaps + burst interval) = a
    packet, its edges +-0.1 ms inside, `freq_hz` = its measured carrier,
    `bw_hz` 1 kHz (`chan_bw`), avg_s 10 (`doa_avg`), keep_s 12 h
    (`keep_h`); coded tags on ONE frequency apart by their codes. UI kind
    `collar` 🐾 ("Collar (MHz) / Name · ppm · mode / Pulses", by name).
    Narrow channels in a wide VFO: `narrow_cov` (Channel filtering below) -
    see *Wildlife collars*
  - FM broadcast stations (`plugins/rds`, 2026-10-22): every RDS group of
    the station on air (its PI confirmed, >= 3 good blocks) = a packet `PI
    xxxx` of its 104 bits (87.6 ms), the FM channel (carrier, 200 kHz),
    avg_s 10, keep_s 30 min - co-channel stations taking turns (DX, a drive
    between two transmitters) apart; one network's transmitters share the
    PI (one talker). UI kind `broadcast` 🎙 ("Station (PI) / Call sign · PS
    · MHz / Groups", by name) - see *RDS / RBDS + SCA*
  - UI (kraken_doa.html): `RDF_PK_KIND` plugin -> kind (station 📻 aprs,
    sonde 🎈, site 🗼 tetra + mpt1327, navaid 🧭 ils, radar 📡 radar - list
    "Radar / PRF · pulse · scan / Pulses", by name; ship / aircraft /
    beacon / device as before), `RDF_PK_UNIT` (slot, frame, block, pulse ...),
    `RDF_POS_KINDS` (aircraft, ship, station, sonde: Dist / Δ pos columns,
    position check, "Plot on map" hint), list headings, `RDF_SORT_DEF`
    (stations / sondes nearest first, sites / navaids by name). The AIS
    base-station / aid-to-navigation icons only for ais talkers (an APRS
    point is no ⚓)
  - D-STAR fix found by the scene test: a new header within two
    superframes of a transmission that ended WITHOUT its end pattern (cut
    off / lost) was taken as a false sync inside the old one - the new
    transmission came up as a late entry without a talker. A header 756
    bits before a data sync now always starts a new transmission
    (`tx_sync`, `new_hdr`). Also: an end pattern in noise long after
    (> 2 superframes) no longer logs "End of transmission"
- Tests 2026-10-15 (scratch t1/; generators in the repo, scenes from
  `plugins/sdk/tools/uca_scene.py`, 5-element UCA r 0.12 m):
  offline `decoder --file scene.cf32 --channels 5 --uca 0.12` (per-talker
  bearings in the offline host, `talker_bearings` in plugin_host.cpp) and
  end to end through the real kraken_doa (scratch cwd, fake heimdall
  streaming the scene's 2.4 MHz cu8, DF status `doa`): every talker within
  0.1 deg offline and 0.0 deg end to end -
  APRS 3 transmitters (one a digipeater repeating another; an alias-only
  repeat gives no talker), AIS 4 ships on A / B with simultaneous A + B
  transmissions, D-STAR the real F1NSR (dsdcc dstar_f1zil_1) + 2 generated
  callsigns back to back (also with FFT squelch 10 dB / Digital squelch),
  NXDN the real unit 1203 (dsd-samples nxdn96_3) + 2 generated units,
  radiosonde 3 real sondes (radiosonde_auto_rx RS41 / DFM-09 / M20) one
  after the other, TETRA 2 co-channel cells, MPT1327 2 sites, VOR 2
  co-channel idents (offline only). kraken_doa ~50 % of a Pi 5 core in
  these runs (2.4 MHz, 5 antennas); plugins 0.4-3 %. Regression (old vs new binaries): p25 /
  dmr / dstar / nxdn on the dsd / dsdcc samples identical; the others only
  add their talker lines (+ the D-STAR fix). No live check: no AIS
  (162 MHz, 7 min) or APRS (144.575 MHz, 3 min) traffic at the station
- Not split: MPT1327 voice (analogue). On a repeater OUTPUT every ID's
  bearing is the repeater's. A DMR repeater INPUT with both slots busy
  can't be split (MS bursts carry no slot) - the two radios' LCs land on
  slot 0, so their frames mostly touch both and are dropped
- Offline (plugin `--file`, recordings from dsdcc / szechyjs dsd-samples
  FM-modulated to cf32): dstar_f1zil_1 full (158 s): F1NSR 1.584-149.692 s,
  F1ZIL B (the repeater's reply) 149.982-150.543, F5LKW 154.553-; DMR MS
  dmr_1_48000: 3 calls of ID 1 matching the LC header / terminator bursts;
  dmr_it_8 (BS slot 2): "?" 0.257 s collapsed, 2222223 0.257-19.964 ch 2
- Test 3 (scratchpad e2e3/: gen_air.py + t_air.py + cdp3.py): six
  synthetic aircraft (DF17 ident / position / velocity + DF11, random
  sample timing, some colliding) at compass 20..320 deg, 25-80 km, steered
  from the bearing of their encoded position onto a 5-element UCA (120 mm,
  1090 MHz, static station heading 0), 2.4 MHz VFO: every aircraft 0.0 deg
  off its true bearing (~9 packets averaged), the VFO bearing jumping;
  kraken_doa ~85-90 % of a core total. UI from captured messages: list,
  Every aircraft, one aircraft, ADS-B check 0.0 / +3.0 deg (bearings
  turned 3 deg)
- Test 2 (scratchpad e2e2/: gen3.py + t3.py, three VFOs on one fake feed):
  D-STAR F1NSR 60 / F1ZIL B 200 / F5LKW 120 deg, DMR BS slot 2 2222223 60,
  DMR MS ID 1 200 - every transmission exact, VFO bearings jumping
- Test (scratchpad e2e/: gen_feed.py + fake_heimdall5.py + t_talk.py):
  iq_143.0.u8's P25 channel (two radios) steered per talker onto a 5-element
  UCA (2010621 -> 60 deg, 3250 -> 200 deg, gaps 300, rest of the band 0,
  independent noise), streamed by a fake heimdall into the real kraken_doa
  (scratch cwd, VFO -787.5 kHz 24 kHz P25, RADIUS 500): per-talker DoA 59.9
  / 200.0 deg every transmission, while the VFO's own bearing jumps between
  52 / 160 / 300. UI checked from file:// with a stub WebSocket replaying the
  captured messages (headless Chromium can't reach servers in the agent
  sandbox; map tiles failed fast = offline mode)

