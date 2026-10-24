# ISM devices plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**ISM devices (`plugins/ism/`, 315 / 345 / 433 / 868 / 915 MHz; README.md there):**
- The whole 2.4 MHz the tuner receives (`sample_rate` = `min_vfo_rate` =
  2.4 MHz), `lock_vfo` (picking it puts the VFO on its tuner's centre at
  2.4 MHz, a locked line - see *Fixed-frequency decoders*), manual only,
  talkers. Weather stations, TPMS, key fobs / remotes, alarm sensors,
  energy meters: 69 protocol decoders (~140 model names, rtl_433's naming
  and JSON keys). LICENCE: GPL-2.0-or-later, its own (`plugins/ism/LICENSE`,
  2026-10-22) - its decoders follow rtl_433's device decoders (GPL-2.0-or-
  later) one by one; every file names the rtl_433 decoders it follows and
  their authors (README *Licence*). Built like every plugin with its own
  LICENSE: no `lib/`, no kraken_doa headers (docs/dev/licensing.md) -
  don't copy code between ism and the source-available plugins / lib in
  either direction; rtl_433_tests recordings are its test data. (A 240 kHz variant - the same decoders on a 240 kHz VFO -
  existed first; removed 2026-10-12: the full band did everything it did at
  about the same CPU, for ~60 MB more memory)
- `frontend.cpp`, `FrontEnd(10)` (input = 10 x `ism::FS`, 240 kHz):
  640-point FFT every 320 samples (3.75 kHz bins, 133 us hop; per-bin arrays
  in frequency order), noise floor per bin (gated mean, a constant carrier
  absorbed in ~10 s); a bin is active when it + its neighbours in this frame
  and the one 640 samples back hold the threshold (option `sens`, 10 dB; 0
  false bursts in 20 s of noise at 8 dB). Active runs of bins go to
  PACKAGES by frequency (`max_pkgs_` 8 at once):
  within `MARGIN_BINS` 6 of a package's region = that package; a package's
  region grows during its first `LEARN_S` 50 ms (strong segments only - the
  second tone of a wide FSK burst; CurrentCost sends 33 ms of one tone
  first); later a strong segment elsewhere (>= -20 dB of the strongest
  active one: not splatter) starts its own package = another device
  transmitting at the same time on another frequency (demodulated and its
  DoA filtered on its own - two overlapping fobs got each other's bearing
  before). Packages ending within 2 ms are merged (one transmitter's
  tones). A package ends after `GAP_END_S` 30 ms of quiet or `MAX_LEN_S`
  1.2 s (a Prologue sends 7 copies in 0.95 s; a held fob is cut)
- A finished package is mixed down by its centre and decimated to 240 kHz
  (`make_segment`: Blackman 321 taps, cut-off 108 kHz - flat to ~88 kHz,
  -74 dB from 128 kHz; computed at every 10th input sample only; input ring
  2^22 = 1.75 s, 32 MB); its noise = the median floor of the 64 bins around
  it x sum(h^2) (the RTL's 2.4 MHz isn't flat). Positions are in 240 kHz
  samples from here on (every protocol works at 240 kHz), `Burst::fc` is the
  offset in the whole band (talker `freq_hz`, device MHz). `FrontEnd(1)`
  still takes a 240 kHz input and demodulates from the ring directly
- Demodulation of that segment, two passes: a wide filter finds the carrier
  or the two FSK tones (2-means of the discriminator; the middle must lie in
  the occupied band - near the band edge a noisy discriminator suggested
  -140 kHz), then a filter centred on them: OOK = envelope > half the
  signal level (hysteresis, >= 3x the noise), runs < 18 us merged, dropouts
  < 40 us next to a 2x longer pulse and spikes < 22 us merged; FSK (higher
  tone = pulse) = the discriminator sliced between tracked tones, or for
  tones >= 50 kHz apart (HopeRF-style +-60 kHz: Bresser, IT+, WH24...) the
  two tones' own filtered levels compared - a tone near the band edge,
  attenuated by the VFO filter, broke the discriminator; a carrier dip < 20
  us keeps the tone (Renault)
- `bits.cpp`: slicers with rtl_433's r_device parameters and semantics
  (PWM, PPM - also its empty rows -, PCM NRZ / RZ with its preamble-based bit
  width and the gap_limit / long cap on trailing zeros, Manchester zero-bit
  - its prepended 0 -, DMC, PIWM); rows carry their sample span (the DoA) and
  their message (`Rows::split()`: rows between gaps > reset_limit = one
  rtl_433 decoder call - one burst can hold several). CRC-8 / -8le / -16 /
  -16lsb, LFSR digests, sums, parity
- Protocols `p_*.cpp`, `ISM_PROTOCOL(name, strength, fn)`: STRONG (CRC /
  digest >= 8 bits) and MEDIUM (checksum / parity + structure) run on every
  burst, WEAK (repeat-only: EV1527 / PT2262, Nexus, Prologue, TX141-Bv2...)
  only when nothing stronger decoded it. One message per device (model + id)
  and burst. `generic.cpp`: a burst nothing decoded, sliced by its own timing
  (PWM / PPM / NRZ) - a row >= 12 bits sent twice = an "Unknown" device with
  the code as its id (valid frame); else a clean long row = a signal class
  (modulation, coding, bit rate, frequency), shown only when heard again
  within 10 min (one-off fragments / collisions / wake-ups are not devices)
- `decoder.cpp`: device table (Model, ID, Type, Ch, Reading, Battery, MHz,
  SNR, Msgs; key = the talker id), events (new device, button / state /
  alarm, low battery, verbose: every message), raw = the message as one JSON
  line (rtl_433 keys + `mic`) + MHz + SNR, `valid(start, end)`. DoA: one
  `kp::Talker::packet` per message, id "model id" (<= 32), span = the latest
  0.24 s of its rows (the whole burst when one device decoded from it -
  kraken_doa takes <= 0.25 s and keeps 2.5 s at 2.4 MHz), `freq_hz` = the burst's
  carrier / FSK centre, `bw_hz` = occupied + 10 kHz (25-160 kHz), `avg_s` =
  option `doa_avg` (1 = each transmission alone, 60, 600). Options `units`
  (si / us), `unknown` (0 = none from now on, and the ones heard so far are dropped: `table_remove` + `Host::talker_forget` - wire TALKER_FORGET 33 -> `TalkerSpan::forget` -> `TalkerDoa::add_span` erases the talker and its waiting packets), `keep_h` "Keep devices" (1 / 6 / 24 / 168 h,
  default 24): a device not heard for that long is forgotten here (max
  1000, then the least recently heard), its table row hidden
  (`Host::table_keep`, wire TABLE_KEEP 32 -> `Report::table_keep`, per
  plugin table, 60 s .. 7 days, default 10 min) and kraken_doa drops its
  talker (`Talker::keep_s`). Was 1 h here, 10 min in the table and 2 min
  for the talker: devices vanished from the 📡 DF list after 2 min
- UI: `rdfKind` 'device' for `pl` 'ism' (📟, "Device / Reading / Tx", no
  position columns, by name in the DF list - see *DoA per talker*, Order)
- EVERY ANTENNA (2026-10-13, `kp::Info::multichannel`, the user's request:
  "improve sensitivity"): in coherent mode with DoA on, kraken_doa hands
  the plugin all antennas (`process_multi`; wire SAMPLES_MC 8 = uint32 m +
  n x m complex int16 / 8192, sample by sample). `FrontEnd::push_multi`:
  antennas 1.. in 16-bit rings beside antenna 0's (+67 MB), each FFT hop
  every antenna through the one FFT plan, each brought to the antennas'
  mean noise (`xn_` = its median bin power, ~1 s; `xg_` the factor - the
  five here differ by up to 5.7 dB) -> detection on their mean power; each
  package sums the M x M covariance of its active bins' (normalised)
  spectra (`Pkg::R`); `finish` takes its main eigenvector (power iteration),
  weights w = v sqrt(g) (|v| = 1: the combination's noise = the floor's),
  antenna 0's weight real, and `make_segment` mixes down sum conj(w_k) x_k
  - blind MRC, no calibration. Measured: real 5-antenna recording (20 s):
  antenna 0 8 messages, best antenna 44, combined 88 (+ a Toyota TPMS and a
  distant WH65B only combined); a real WH65B frame planted at set SNRs into
  5 noise channels: antenna 0 decodes down to 6 dB (100 kHz), combined down
  to 0 dB; 5-channel noise: nothing. CPU 38 % vs 14 % of a core (offline).
  Not in independent / wideband mode or with DoA off (one channel, as
  before; checked through the engine with digi_test u8 vs u8mc). Antenna 0
  of the array here hears far less than the others (its noise 5 dB higher):
  the single-channel decoder used the CH selector's antenna
- Detector fixes found on a 30 s live recording at sens 8 (same day): a
  package continues on max(threshold - 2, 7.5) dB (6 dB: noise kept 93 %
  of package regions active - packages ran to 1.2 s, merged transmissions,
  DoA reports came 1.3-1.8 s late); splatter is also compared with what
  starts in the same frame; >= 3 packages born in one frame with >= 3 of
  them >= 5 ms long = a broadband event (not demodulated; clicks - a 251
  kHz spur comb here - don't count). Unknown signals: PWM only when most
  pulse + gap pairs keep the period (NRZ 1T / 2T runs fooled it), classes
  matched within 15 % / 30 kHz and keeping their first name (the name
  jittered every burst), 12 dB; a decode with the same maker + id as a >= 15
  dB stronger overlapping one > 30 kHz away is dropped as a copy. Notes:
  the "LoRa-like" wide bursts at 433.997 MHz are a real wide-FSK (+-65 kHz)
  device whose two tones, decoded apart, looked like two OOK devices with
  inverted codes; 9 "Jansite" TPMS ids at 433.997 MHz live were most
  likely its preamble passing the checksum-less Jansite decoder (not
  reproduced on the recording - left as is)
- Keep tests (scratchpad e2e/: scene_keep.json - the devices stop at 40 s,
  3 antennas; run_keep.sh + t_keep.py, niced next to a live stack):
  default - all 8 still listed at 190 s, 159 s after the last packet (the
  old 2 min dropped them at 120 s); `keep_h` 0.025 (90 s) - talkers and
  table rows leave together, none older than 88 s. t_ui_sort.py (replayed
  capture with partial pushes): the list complete between full pushes, one
  order throughout, by name, compass bearings, heading sorts + reverse +
  localStorage, a row still picks its device
- Built with `-O3` (every plugin's default since 2026-10-22 - see
  docs/dev/compiler-flags.md; it was ism's own `CXXFLAGS` file): the front end's per-bin loops
  (`__restrict` pointers) vectorise - GCC's -O2 cost model takes no loop
  that needs an epilogue; the output is identical (checked on all 1125
  recordings against the -O2 build, scratchpad scripts/cmp_bin.py). CPU
  (Pi 5): detector ~4 % of a core (bench/fe_bench; 9.1 % before the
  frequency-order arrays and -O3), the plugin 14-17 % live in a dense scene
  (7 devices, bursts ~40 % of the time; the 19 MB/s pipe included), ~7 %
  with 5 devices; kraken_doa no busier than with a 240 kHz VFO (no
  decimation at 2.4 MHz)
- Generator in the repo (2026-10-16): `tools/ism_gen.py` (streamed 2.4 MHz
  scene, real framing + checks) + `tools/scene_six.json` = playback
  `ism-six-devices` (EV1527, PT2262, Acurite 592TXR, LaCrosse
  TX141TH-Bv2, Toyota TPMS, Fine Offset WH24 across the band, four
  overlapping pairs): offline 13 / 13 messages (antenna 0 and all
  combined), noise nothing; end to end 6 / 6 devices at 0.0 deg (after the
  playback's no-drop fix - see *Debug playback*)
- Multi-device tests (scratchpad e2e/): gen_wide.py scene_w1.json -> a 20 s
  2.4 MHz cu8 (7 devices at -950..+900 kHz, six starting within 150 ms):
  every transmission of the 6 known devices decoded at its frequency, the
  LaCrosse recording as the same unknown code as at 240 kHz; end to end
  (`run_e2e.sh scene_w1.json 70 --plugin ism --bw 0`): every device's
  bearing exact, every transmission in its history
- Tests: rtl_433_tests (1125 recordings at 250 kHz - 1 MHz, rate / RF from
  the file name, padded with 0.4 s of noise - a burst ends after 30 ms of
  quiet), each converted to a 2.4 MHz cf32 as a 2.4 MHz receiver would
  record it (scratchpad scripts/to_wide.py, `ISM_WIDE=1 run_tests.py`):
  resampled, the band outside the recording's filled with noise at its own
  density (per-bin 20th percentile over time / 0.223, median over frequency)
  and its DC left as recorded. 954 of the 1030 with expected output decode
  fully, no wrong fields. Plain upsampling (empty band) gave 853: the
  detector's noise floors start from the median bin - near zero there - and
  rise only 0.01 %/frame; a fill that was too strong or a DC step from
  subtracting the file mean also broke files - all test artefacts. Against
  the 240 kHz version (960 + 3 wrong fields on the same files at 240 kHz):
  5 carriers at the edge (+-120..125 kHz) of 250 kHz recordings (HCS200 x4,
  Rubicson gfile039 - the conversion's edge; the 240 kHz one folded them
  back), 2 with a signal from the first sample (TX35DTH-IT, Steelmate leak:
  the floors start from the whole band's median, a package opened at t=0
  took what came later), 1 marginal (14 dB); it missed WH2303 gfile018 and
  THGR122N-0001. The misses common to both are models not done (other
  LaCrosse, Rubicson pool / meat, ThermoPro TX2 = Prologue bit for bit,
  TX31UIT) and rtl_433's own false positives (GS558 on EV1527 fobs).
  Scratchpad: scripts/run_tests.py, one.py (`ISM_DUMP=1`: every burst's
  pulse trains; `ISM_TRACE=1`: the detector; `ISM_ROWS=<name>`: a
  protocol's rows), dump2.sh (old 240 kHz vs full band, one recording)
- End to end (scratchpad e2e/: fake_heimdall_ism.py streams a 5-element UCA
  scene, 433.92 MHz, r 290 mm, into a test kraken_doa on ports 39080 /
  39091; t_ism.py): synthetic EV1527 / PT2262 fobs + real recordings (EV1527
  PIR, Toyota TPMS, Acurite tower, Schrader TPMS, a 71-bit PPM unknown) at
  5 bearings / frequencies, some transmissions overlapping in time: every
  device 0.0 deg off, every transmission in its history right; the unknown
  device too (`run_e2e.sh scene2.json 70 --plugin ism --bw 0`; first run
  with the 240 kHz version, the same with the full band)

