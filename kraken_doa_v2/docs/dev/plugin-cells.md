# LTE / 5G NR Cell Decoder plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**LTE / 5G NR Cell Decoder (`plugins/cells/`, README.md there; ARRAY plugin API):**
- Plugin id `cells` (kept from "Cell Separation"; name "LTE / 5G NR Cell
  Decoder", v2.0): the public broadcast information of every cell on a
  carrier (LTE MIB, SIB1, SIB2 - SIB5; NR: PCI / beams / SSB frequency) +
  each cell's own bearing. Only broadcast system information - nothing
  addressed to a phone (no C-RNTI / P-RNTI / paging) is looked for
- LTE / 5G NR cells share a carrier (frequency reuse 1): one VFO DoA mixes
  them. Every cell scrambles its sync / reference signals with its PCI, so
  correlating with one cell's sequences gives its channel alone - separation
  by CODE, also for cells at the same delay (co-sited sectors, synchronised
  networks), where dtv's path separation separates by delay. Talkers "LTE-<pci>" / "NR-<pci>"
  (spatial, no delay; label + "eNB n / c" once SIB1 is read)
- Broadcast information (LTE; option `decode`, default on): `lte_dl.cpp`
  (`CellRx`: a subframe's 14 symbols on every antenna at the cell's frame
  timing, channel per port 0-3 from the CRS - LS, subframe-averaged with the
  cell's CFO turned back, interpolated across subcarriers, `set_ports`
  limits it to the MIB's count; `txd_llr`: SFBC / SFBC + FSTD over all
  antennas, unseen REs = 0; PBCH -> MIB (4 SFN-mod-4 hypotheses x 1 / 2 / 4
  ports, CRC16 mask = ports); PCFICH -> CFI; `control_layout` (PCFICH +
  PHICH REGs, PDCCH REGs by subcarrier then symbol, quadruplet interleaver +
  PCI shift); `pdcch_llr` (common search space only, CCE 0-15);
  `dci_search` (AL 4 / 8, formats 1A / 1C, SI-RNTI, re-encoded agreement
  >= 0.8)), `lte_pdsch.cpp` (`parse_dci` -> `Grant`: RIV, distributed VRB ->
  PRB per slot (36.211 6.2.3.2), TBS from `lte_tables.hpp`; `pdsch_res` (no
  CRS / PBCH / sync / control); `pdsch_llr`; `TbAcc` = soft combining in the
  turbo circular buffer + coverage), `turbo.cpp` (QPP, encoder, rate
  matching, max-log-MAP with 0.7 extrinsic scaling and CRC early stop),
  `fec.cpp` (CRC16 / 24A / 24B, tail-biting K=7 encoder + 3-pass Viterbi,
  convolutional rate matching), `lte_rrc.cpp` (UPER reader; SIB1 incl. v890
  / v8h0 / v9e0 / v920; SystemInformation with SIB2 - SIB5 field by field,
  extension additions skipped as open types, any other root SIB ends the
  message - `complete = false`), `lte_meta.cpp` (`meta_burst`: the MIB every
  burst = the frames' SFN - two agreeing frames the first time; SIB1 =
  subframe 5 of even frames, RV ((SFN/2) mod 4) for 1C; each SI message in
  its window from SIB1's schedule, RV from the window index, round robin
  over the messages; per cell `META_CELL_S` 0.1 s, per burst `META_BURST_S`
  0.2 s, the cell served longest ago first; read again after 600 s; a
  changed SIB1 value tag clears the SI messages), `lte_info.hpp` (36.101
  band table EARFCN <-> MHz incl. uplink, NR bands, NR-ARFCN, MCC ->
  country). `lte_tables.hpp` (QPP, TBS, TBS 1C, MCS -> I_TBS) is written by
  `tools/lte_tables_gen.py` from the 3GPP specifications' Word files (3GPP
  archive zips; 3gpp.org answers curl's default User-Agent with 403): TS
  36.212 V19.3.0 / 36.213 V19.5.0, every entry equal in V8.8.0; TBS[6][0] is
  328 in every release (2026-10-22: it had been "corrected" to 88 from the
  srsRAN copy the file came from - never read, SI DCI 1A uses N_PRB 2 / 3)
- Output: table Cell, PCI, Mode, Operator, TAC, eNB / cell, Band, (E)ARFCN,
  Bandwidth, Ports / beams, Power, Above noise, Timing, Spread + every row's
  details (`table_detail`: physical, MIB, downlink, SIB1, its DCI, SI fields
  "SIB2 · ...", "Decoding: waiting for ..."); facts LTE carrier, Network
  (PLMN), System information; an event per decoded block
- Partial visibility: a 10 MHz carrier is ~23 % inside the VFO - the DCI
  decodes from the visible REGs, SIBs are combined until the CRC passes. An
  SI message sent once per window (RV 0 every time) shows the same bits in
  every repeat and stays unread when its PRBs are mostly outside (SIB3 /
  SIB5 on the live band-8 carrier: 36-40 % coverage). `MetaParams::verbose`
  (the plugin's verbose option) logs every transmission combined (DCI,
  window index, RV, coverage)
- Any VFO rate with a whole number of samples per 15 kHz works (a wider
  SDR): the usable band is `VIS_FRAC` 0.4375 x the rate (+-1.05 MHz at 2.4
  MHz; `vis_hz_`), the raster reach `search_span()` = it - `SYNC_HALF_HZ`
  0.45 MHz (>= 0.6 MHz), the scan step `scan_step_` 2 x that. The cell
  search runs on a decimated copy (`search_at` -> `search_decim`: the
  largest D with fs / D >= 1.92 (LTE) / 2.4 (NR 15 kHz) / 4.8 MS/s (NR 30
  kHz) and a usable numerology; `decim_taps` / `decimate` in dsp_util.hpp,
  centred FIR, 14 D + 1 taps; `SearchParams::band_hz` = 0.4 x the decimated
  rate limits the sync subcarriers used; hits' sample positions scaled back
  by D; `SearchOut::rate` = the fold's rate for the plot), everything else at
  the whole rate. 2.4 MHz: D = 1 - output byte-identical to before. The PSS
  fold's overlap-save block grows past 1024 when a symbol is > 256 samples.
  Tested (scratchpad wide/): synthetic LTE at 15.36 MS/s and NR at 9.6 MS/s
  off the VFO centre (bearings within 0.5 deg), the live capture upsampled
  to 9.6 MS/s (MIB / SIB1 / SIB2). CPU at wide rates not addressed (mixing
  per search candidate + channel estimation at the whole rate: 0.6-2.6 s per
  burst at 9.6 MS/s on a Pi 5)
- Files: `seq.hpp` (36.211 / 38.211 sequences: LTE PSS ZC, SSS, CRS Gold +
  RE mapping; NR PSS / SSS), `ofdm.hpp` (numerology at the input rate: 2.4
  MHz = 160 samples per 15 kHz symbol, no resampling; `SymFft` = FFT window
  started early inside the CP, the fractional start put back as a phase
  ramp), `search.cpp` (`cp_offset`, PSS fold, image-aware peak choice, SSS
  test), `chest.cpp` (per-cell CRS / SSS impulse responses -> per-tap
  covariance -> the path's covariance), `decoder.cpp` (raster search / lock /
  frequency tracking per technology, tracks, table, plot, facts), bursts 0.3
  s every 1 s
- Carrier: candidates on the LTE 100 kHz raster (|offset| <= 0.6 MHz) / NR
  GSCN (<= 0.3 MHz) around the VFO centre, `SEARCH_PER_BURST` 4 per burst;
  fine offset = the cyclic-prefix correlation folded over 0.5 ms, mean
  removed (CW / DC), modulo the subcarrier spacing; whole-subcarrier
  ambiguity swept (0, +1, -1 per raster sweep: receivers up to ~22 kHz off);
  after 3 sweeps without a cell, search every 4th burst. A candidate counts
  only CONFIRMED cells (a ZC PSS shifted in frequency correlates at another
  TIME: wrong raster positions show peaks + SSS "hits"). Locked: the
  frequency is tracked from the cells (`f_abs`; per cell `df_hz` = the SSS's
  phase against the PSS / dt; a > 300 Hz residual from a z >= 8 cell re-mixes
  the burst once) - the CP estimate is biased when two systems share the
  slice (DSS); LOCK_MISSES 5 bursts. Short burst fragments (< half a burst)
  are ignored
- PSS fold (2 antennas, FFT 1024 overlap-save, 5 ms LTE / 20 ms NR); peak
  candidates strongest first, judged against expected images: `X3` (each
  PSS as received - CP + symbol - against each template, per lag) from the
  peaks taken and the SSS waveforms of the cells found (prefix sums); peaks
  of another N_ID2 within 4 samples are exempt (co-timed sectors = images
  there); max 8 per N_ID2
- SSS: PSS channel by least squares of all three PSS over 9 subcarriers
  (filters precomputed per N_ID2), w = SSS x conj(H), summed over antennas
  and <= 32 occurrences (LTE per frame half; locked: only the carrier's
  duplex), scored by |sum W d| over every N_ID1 (LTE x sf0/sf5 x FDD/TDD),
  robust z (median / MAD of all hypotheses) >= 6; cancellation = the
  detected SSS x its smoothed (+-5 subcarriers) W d, at the same peak and
  for every co-timed (<= 24 samples) found cell before a peak is tested
- Confirmation: `estimate(quick)` (40 slots, port 0) must find a path >=
  `min_snr`; full estimate: LTE CRS ports 0 / 1, symbols 0 + 4 (3-comb, 22.2
  us window from -5.4 us; the cell's CFO slot to slot removed between the
  symbols), 160 slots, energy gate drops TDD uplink slots; port 1 counts
  within 10 dB of port 0 at the same delay; NR SSS of every block (67 us).
  `analyse()`: per-tap covariance, floor = low-trace taps, D = tr(F^-1 C)/m,
  path = strongest or first within 15 dB (`path` option), main lobe minus F
  -> unit-trace covariance, power, snr, spread
- Carrier scan (`scan` option: 0 / 5 / 10 / 20 MHz, default 10; `Scan` in
  decoder.cpp, survives reset()): a whole k=0 raster sweep where the
  receiver is tuned finds no cell -> `kp::Host::tune` to f0 +1.2, -1.2,
  +2.4 ... MHz (SCAN_STEP_HZ: the raster reaches +-0.6 MHz, LTE and NR), a
  scan position's whole raster in ONE burst; found -> centred once (a carrier
  > 5 kHz off the tuning, also without scanning; LTE before NR); nothing
  within the range -> back to f0, stays (`done`). Bursts from before an asked
  retune are skipped; an RF change it didn't ask for (> 2 kHz) = the user's:
  starts over from there; a retune not seen after 15 s is given up, two =
  scan stopped. Host side: `Info::tunes` + wire TUNE 37 (rf_hz text) ->
  Engine (only plugins declaring it, >= 2 s apart; a refusal = an event in
  the decoder's log) -> `DigitalDecoder::set_tune_handler` (DecimatorManager:
  holds only the VFO id) -> `ControlHandler::request_plugin_tune` (checks
  wideband scan mode / the tuner range on the decoder thread, then
  `loop->defer`: VFO looked up there, FREQ:<rf - offset> or
  TUNER_FREQ:<tuner>:.. in independent mode through
  `handle_websocket_message` - saved, sent to heimdall, synced to pages)
- UI (kraken_doa.html): talkers with `sp` and `pl === 'cells'` are kind
  'cell' (📱; list head Cell / Technology · PCI · power, strongest first, no
  "1st" tag, selected line "x dB below the strongest cell"); the DF panel
  section says "N cells" with a sync-correlation tooltip
- Tests: 10 synthetic cases (`tools/cell_gen.cpp`; README table) - every
  cell above the limits found, bearings within 1 deg, no false cells on
  noise; live (scratchpad e2e/: fake_heimdall_rec.py + t_cells_plugin.py +
  t_cells_ui.py on the TEST build): 3 LTE + 1 NR cell within 0.1 deg, UI
  picks; scan (fake_heimdall_multi.py: retunable, set_frequency on the
  control port switches between recordings of one 5 MHz LTE carrier at
  806.0 MHz seen from 808.0 / 809.2 / 806.8 / 810.4 / 805.6 / 806.0; +
  t_cells_scan.py): from 808.0 it stepped 809.2, 806.8, 810.4, 805.6 (~7 s a
  step), found the carrier +400 kHz off, centred 806.000, cells at 40.0 /
  199.9 / 299.9 deg. CPU (Pi 5): ~15-20 % of a core per locked technology.
  Live (2026-10-14, the user's stack on 954.8 MHz, LTE band 8): three FDD
  cells (PCI 191 / 190 / 253, 2 ports each; 190 / 191 co-sited - timing
  +0.04 us) found and tracked, receiver +1.9-2.1 ppm; bearing confidences low
  (0.16-0.34) and the co-sited pair 48 deg apart - multipath, not yet checked
  against the real towers. Broadcast information on 6 s / 20 s captures of
  that carrier (10 MHz, 2 ports, the SI-RNTI DCI in format 1C, AL 4 at CCE
  12, from ~23 % of the control region): the MIB in every frame, SIB1 of PCI
  191 / 253 after 2-8 transmissions (530-01 New Zealand, TAC 41776, eNB
  5330 / 12 and 42599 / 11, band 8), SIB2 after 3-14 (uplink EARFCN 21748 =
  band 8's duplex spacing); SIB3 / SIB5 not (above). Streamed through the
  test build the events arrive the same; UI (scratchpad e2e/t_meta_ui.py:
  the page with a recorded status injected): ▸ toggles, details open, stay
  open across pushes, close; facts. The SDK's offline host now streams
  multichannel files block by block (a 20 s 5-antenna cf32 is 1.9 GB: read
  whole it was OOM-killed; 74 MB now)

