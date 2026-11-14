# Wildlife collars plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Wildlife collars (`plugins/wildlife/`, README.md there):**
- Wildlife tracking transmitters anywhere in VHF / UHF, in any country
  (pulsed CW: mammal / livestock collars, bird / bat tags, Lotek /
  Motus-style coded tags). REGION-NEUTRAL by default (the user's call
  2026-10-15: "don't make it specific to any country"): profile `global`;
  country / vendor specifics only as selectable profiles (`profiles.hpp`)
  that ADD channel numbers, a rate table, a mortality convention, a
  data-stream scheme and band names - never remove the generic detection.
  `sample_rate`
  0 = THE VFO'S OWN RATE (new: `kp::Host::sample_rate()`, see *Decoder
  plugins*): a 2.4 MHz VFO surveys a band, a narrow one (>= 5 kHz,
  `min_vfo_rate`) follows one collar; manual only, talkers; NOT lock_vfo
- `pulse.cpp` `PulseDetector`: N = 2^round(log2(rate / 250)) FFT (2.4 MHz
  8192 / 293 Hz, 48 kHz 256 / 188 Hz), Hann, 50 % overlap; floor per bin
  (gated mean 1 s, creeps +1 dB/s under a steady carrier); short (1 frame >
  19x) and long (~14 ms mean > 5x) detectors, a gate at each known collar's
  next slot (4x / 14x, ~1 dB); a frame with a quarter of the band over =
  impulse, blanked; narrow-line check (bins 4..10 away); finished = 2 frames
  without. Measured on the raw ring (0.6 s): mixed to the bin, boxcar to ~32
  kHz, +-1 kHz Hamming FIR, noise = the bin's floor N0 x r1 x sum h^2;
  pulses = maximum-sum runs (2 ms power over 2 x noise / 0.3 x the max, then
  0.3 ms power over the midpoint, +-5 ms), up to 6 per span (coded bursts),
  a later one >= 0.4 x the first's power and 6 sigma; duplicates of a pulse
  already reported (the long sum's memory) dropped; kept only keyed ones:
  1-150 ms, 1-3 ms outside the edges at the noise (strong pulses), power
  variance inside <= 2 x the noise's + 0.1 S^2 (FSK tones fail), both
  halves' phase-slope frequencies within 60 Hz + 2 / T (FM lines fail)
- `decoder.cpp`: splatter (20 dB within 6 kHz, 10 dB within 3 kHz) and
  twins (0.6-8.5 kHz apart within 10 dB, starting + ending within 15 ms or
  within 60 ms of each other = one data burst's tones - live: a ~5 kHz burst
  every 2.5 s near 160.40 MHz) dropped; channels +-700 Hz; groups < 160 ms. Coded tag = 4 pulses
  in 0.4 s after 0.3 s quiet (on a collar's channel only a tag known there),
  gaps +-2 ms, confirmed after 3 bursts (burst interval = the shortest
  interval all are multiples of, +-30 ms + 5 ms per k - over the longest
  run of the LATEST bursts that agree, `burst_interval`: an older one off
  that grid - a jump in the input, a false burst - is dropped; aliased tags
  = same gaps, other BI, kept apart; a candidate with a confirmed tag's gaps
  + BI that wasn't heard since the candidate's first burst = that tag
  resynchronised, same id - `resume_tag`, it came back as "...b"), id "MHz
  g1/g2/g3@BI". Train = `find_period`
  (`train.cpp`: candidates pairwise / 1..4, score = on-grid - 0.5 empty
  slots, least squares) on the channel's single >= 4 ms pulses of 30 s:
  >= 4 pulses, jitter <= 2.5 ms + 0.1 %, one length (MAD <= 30 %),
  duty < 10 %, 0.4-8 s; then grid
  tolerance max(25 ms, 3.5 %), fragments (8 dB weaker / < 40 % width)
  dropped, two pulses half way between = period halved (a weak collar), a
  rate change = a filled grid (>= 80 % of slots, >= 6 pulses, >= 8 s)
  winning 3 times; the halving needs the pulse half a period after the one
  before (a rate slowed 60 -> 40 ppm put every other pulse half way and read
  as a doubling)
- Modes, generic (`classify`, every profile): baseline = the first steady
  rate; a sustained change to x1.4-3.1 of it = "faster xN", held
  `POSSIBLE_HOLD_S` 120 s (`escalate` from tick) = ⚠ "possible mortality"
  (Kenward / Lotek: mortality sensors raise the rate - double - after hours
  still); slower = "slower xN"; the baseline again = back ("moving again");
  >= 3 sustained changes in 30 min (`ACT_WIN_S`) among rates it already had
  = "activity" (no ⚠; the USFS collar 40 resting / 60 active / 80
  mortality), a NEW rate after toggling is judged against the slowest
  (resting) one; a rate change drops an open data stream (the transition's
  close pulses read as data). Profiles (`profile`): global (default), na /
  eu / au (band names + a doubling - x1.8-2.25 / x2.4-2.9 over the previous
  or resting rate - = MORTALITY at once; no public vendor rate tables
  found, so none invented), motus (channel "Motus 166.380 / 150.100 /
  151.500" within 5 kHz), nz (DOC / Telonics TR4 channels 160.120 + 0.010 n,
  the DOC table 30 normal / 48 incubating / 60, 80 MORTALITY, doubling,
  Wildtech streams); a saved `change` loads as global. The table's "Ch"
  column only with a channel scheme (`set_columns`, re-sent + rows
  republished on a profile change). Band hints `band_hints()`: a worldwide
  list (US FCC 90.248, NA agency bands, Germany, Finland, UK, Japan,
  Australia, NZ, South Africa, Motus) for the "Band" fact / detail only
- Data streams (option `streams`: auto = as the profile says / wildtech /
  raw / off): runs (< min(0.5 P, 1 s)) vs single pulses; without a vendor
  scheme shown as their pulse groups (`describe_runs`: "2 (3) 2 (5) 2 (11)
  2") - no scheme is auto-detected (the same pulses mean different things
  per vendor). Wildtech scheme (`decode_stream`): all runs 2 = double-beep
  (DOC 6.4.3 time since death: N - 2 per digit, 3 digits), else burst
  scheme (digit = run - 2, tens + units with <= 1 single between; 8 numbers
  = Chick Timer fields, 11 = kakapo Egg Timer V6.3); ends after 12 singles
  / 40 s silence / 400 s; same text twice = confirmed
- ID separation: a `kp::Talker` packet per pulse of a confirmed collar /
  tag, span = its edges +-0.1 ms inside, `freq_hz` = its carrier, `bw_hz`
  `chan_bw` (1 kHz), `avg_s` `doa_avg` (10), `keep_s` `keep_h` (12 h);
  label "name · ppm · mode" / "coded · BI". UI kind `collar` 🐾
  (`RDF_PK_KIND` wildlife, "Collar (MHz) / Name · ppm · mode / Pulses",
  by name). Option `names` ("MHz=name, ..." within 1.5 kHz, or id=name)
- Host changes with it: `kp::Info::sample_rate` 0 (digital_decoder.cpp
  `Runner::vfo_rate` / `prate()` / `rate_of`, started with `--serve --rate
  <VFO rate>`, restarted on a VFO rate change; dig_plugin.cpp accepts 0;
  plugin_host.cpp `serve(argc, argv)`, `WireHost` / `TestHost::sample_rate()`,
  offline at the file's rate); talker_doa.cpp `narrow_cov` (channels <= 10
  kHz the 401 taps can't resolve: mixed + summed over D = rate / (16 bw),
  Hamming sinc at that rate, <= WIDE_MAX_OUT points; raw and wide-raw
  rings); the offline `--uca` channel filter centred (was causal: 2 ms
  early for 1 kHz at 48 kHz)
- Generator `tools/collar_gen.py` (`tx` one transmitter / `scene` every
  antenna streamed, interference, `rates` schedules), playback entries
  `wildlife-five` (profile nz), `wildlife-five-survey` (2.4 MHz VFO),
  `wildlife-one-collar` (10 kHz), `wildlife-na-collars` (150.985-151.015
  MHz, profile na), `wildlife-motus-tags` (four coded tags on 166.380 MHz,
  profile motus)
- Tests (scratch t9/, numbers in the README): five transmitters on 5
  antennas (3 collars incl. a 30 -> 60 ppm mortality switch, 2 coded tags on
  one frequency) offline 40.5 / 172.6 / 130.7 / 298.4 / 259.8 deg (truth 40
  / 170 / 130 / 300 / 260); Wildtech streams (time since death 139 h - its repeat
  138, one pulse lost -, Chick Timer 8 numbers, kakapo Egg Timer 11 exact);
  collars from ~32-34 dBHz per 20 ms pulse; 300 s of impulses / CW / FSK data: no collar (a strictly
  periodic tone-FM test signal: one); 2.4 MHz survey: 5 / 5 across +-0.9
  MHz; live debug playback: survey 5 / 5, 10 kHz VFO 1 / 1, 50 kHz VFO 5 /
  5 (was 4 / 5 - the second coded tag never confirmed: the plugin's
  FFTW_MEASURE plan took 6 s at 50 kHz / 23 s at 2.4 MHz before it read a
  sample, kraken_doa dropped 2.3 s of its input between that tag's first two
  bursts, and the confirmation wanted every burst on one grid - FFTW_ESTIMATE
  then, pffft since 2026-10-17 (no planning), + the latest-run rule above),
  settings identical. Live
  (4 x 100 s of 160.115-160.595 MHz, heimdall retuned + restored): no
  collar; data bursts near 160.40 / 160.53 MHz first read as collars, fixed
  (burst-piece, jitter, one-length rules). CPU:
  detector 8-12 % of a core at 2.4 MHz (measured with FFTW_MEASURE; the
  ESTIMATE plan's 8192-point FFT is 77 vs 59 us = ~1 % of a core more; the
  plugin live at 2.4 MHz 9.3 % incl. the pipe),
  0.2 % at 48 kHz. Regression: every plugin's --info identical, aprs /
  beacon406 playback scenes decoded identically (one peak / mean ratio)
- Region-neutral rework (scratch wl2/, README *Region-neutral rework*):
  with `profile=nz` every old offline scene gives the same collars, data
  numbers and bearings (the noise scene's one false tone-FM collar now
  "activity", no ⚠); global: MHz only, "baseline", streams as pulse groups;
  a 300 s modes scene (7 collars) under global / na / nz as designed; NA
  scene 49.2 / 141.3 / 220.7 / 312.3 deg (truth 50 / 140 / 220 / 310),
  Motus 31.5 / 110.5 / 200.6 / 320.0 (30 / 110 / 200 / 320); debug
  playback on the live kraken_doa: all five entries 19 / 19 talkers within
  3.9 deg, settings files byte-identical

