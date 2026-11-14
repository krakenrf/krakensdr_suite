# Digital squelch

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Digital squelch (squelch method "Digital", with the VFO's decoder on):**
- The squelch methods are FFT (0) and DIGITAL (3) - the eigenvalue methods
  (1 / 2: λ ratio with a manual / self-learned threshold) were removed (too
  hard to set up): saved snapshots and old `DEC_SQUELCH_METHOD:EIGEN*` map to
  FFT, `DEC_SQUELCH_EIGEN:` is ignored, MUSIC's eigen gate is gone (only the
  publish hold is left). `SquelchMethod` (types.hpp), snapshot field 6 =
  the method, field 7 = "0" (was the threshold - the column stays)
- In effect only while the VFO's digital decoder is on
  (`DecimatorManager::effectiveSquelchMethod`; else FFT - the choice is kept
  and comes back with the decoder), in EVERY operating mode (it needs no
  MUSIC). Open = the active plugin (fixed, or the one Auto detect locked onto)
  reported a valid frame within `DIGITAL_SQUELCH_HOLD_MS` 1.5 s - or the
  plugin's own hold, `kp::Info::squelch_hold_s` (`--info`
  "squelch_hold_s", `PluginInfo::squelch_hold_s`, the engine's
  `hold_ms_by_`; analog_id 0.35 s: it reports a valid span every 50 ms while a
  tone is there, and 1.5 s would pass 1.5 s of noise after every
  transmission) (`digitalSquelchOpen` -> `DigitalDecoder::squelch_open`,
  the engine's `last_valid_by_` per plugin) - the pipeline's squelch_open,
  the audio gate, the web mapper
- The DoA (coherent, `dig_doa` in the pipeline): MUSIC runs with its publish
  held (`setPublishHold`) and `TalkerDoa::set_digital(true)` keeps the
  stream (raw <= 500 kHz / chunk ring above, as for packets). Every
  CONFIRMED piece goes in as the covariance of exactly its samples
  (`dig_confirm`: only what wasn't taken yet - `dig_done_` - and is still in
  the ring; >= 16 samples): valid frames with their samples (wire VALID with
  a payload -> `TalkerSpan::signal`), talker transmissions (as they grow;
  "?" too), packets (their own covariance, channel-filtered for AIS).
  `TalkerDoa::update` (rdf sampler, 10 Hz) publishes at most every
  `DIG_PUBLISH_MS` 200 ms: R of the samples confirmed since, averaged with
  the previous like the processor's frames (`getCovarianceAveragingAlpha`)
  -> `MUSICProcessor::publishFromCovariance` (pseudospectrum, peak, 3D
  elevation, stamp - so the DoA plot, map lobe, logger, web mapper, mobile
  DF and the beamformer's steering all see only confirmed samples). Nothing
  confirmed = the published DoA stays at the last confirmed one
- Plugin API: `kp::Host::valid(start_s, end_s)` (Host::time() of the frame's
  first sample / its end; counts as a valid frame; default = valid()) ->
  wire VALID payload "start\0end" (old hosts ignore it) -> `on_valid_span`
  (vfo_pos mapping, <= `MAX_SIGNAL_SPAN_S` 3 s). Lib protocols:
  `RxContext::valid_at` + `dig::report_valid(ctx, mode, a, b)` with receiver
  sample indexes, `Bridge::clock(rate)` / `restart_clock()` (in
  `talkers(rate)` too): p25 (frame_a_..frame_b_, a TSDU ends after its last
  good TSBK), dmr (burst), nxdn (frame; the CAC path computes its own),
  dstar (RF header; a superframe from the previous data sync), tetra (the
  slot), mpt1327 (CCSC / SYNT + address codeword). Standalone: pagers (a
  synced POCSAG batch with <= 2 bad codewords - replaces the old valid() per
  address codeword -, a FLEX block with <= 1 lost word per phase), aprs + v23_telemetry (frame end - length at 1200 bit/s),
  radiosonde (`FskRx::clock` per block -> `FskFrame::t_start / t_end`;
  `Frame::t_start / t_end`; DFM / LMS6 from the FSK frame that completed the
  position; iMet-4 from its UART bytes). The offline host prints "valid
  frames with their samples: N, covering X s" (`--verbose`: each one)
- UI: the VFO card's method list = "FFT Peak (dB)" + "Digital (decoded
  frames)" only while the decoder is on (`sqEffMethod`, `digDecoderOn`;
  the card rebuilds when the decoder goes on / off); Digital hides the dB
  slider (and the spectrum's squelch line) and shows a one-line note
- Tests (scratchpad acal/): `dig_sim.py` (POCSAG bursts from 60 deg every
  4 s + an FM interferer in the gaps from 200 deg, 5-element UCA r 0.5 m at
  145 MHz; `file` / `serve` = a fake heimdall) + `t_dig.py` against the
  test build: no squelch 47 / 53 % pager / interferer bearings, Digital 100 %
  pager, open 50 % of the time (expected 49 %), FFT at 5 dB 44 / 56 %,
  Digital with the decoder off = FFT, EIGEN_AUTO -> FFT; `t_sq_ui.py`
  (headless Chromium: methods offered, rows, OPEN / CLOSED following the
  bursts, decoder off / on). Offline: pocsag batches within ~1 ms of the
  true spans; radiosonde recordings unchanged frame counts (RS41 119, M10 /
  M20 120, iMet-4 119, iMet-54 241, LMS6 120, noise 0). P25 / DMR / NXDN /
  D-STAR / TETRA / MPT1327 spans: built, not tried on recordings (none here)

