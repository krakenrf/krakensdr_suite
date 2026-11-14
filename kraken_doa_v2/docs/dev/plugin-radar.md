# Pulse radar plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Pulse radar (`plugins/radar/`, README.md there):**
- Every pulsed radar in the 2.4 MHz the tuner receives (L-band ATC /
  en-route surveillance 1215-1400 MHz, wind profilers ...): `sample_rate`
  = `min_vfo_rate` = 2.4 MHz, `lock_vfo`, manual only, talkers. No protocol:
  ELINT-style measurements of each radar's pulses, PRIs and antenna
- `pulse.cpp` `Detector`: per sample |x|^2 + a 3- and a 24-sample sum (10
  us: weak long chirps that fade across their sweep) against the noise
  (median of 64-sample means), the long threshold 6 dB under the short
  (`sens` 10 dB); processed in chunks that don't wrap the ring (2^15
  samples, 13.7 ms - fits the L2 cache). A segment's pulses:
  runs of (3-sample mean > short OR 9-sample mean > long threshold) apart by
  > 4 us; edges = the OUTERMOST -6 dB crossings (raw / 3 / 9-sample power),
  corrected by W / 4 per side (a rectangular pulse comes out exact - the
  outward search from the peak split fading chirps into 3-5 pieces live);
  frequency from phase steps, LFM rate from 6-sample pieces fitted against
  time; clipped = an I / Q sample >= 0.96 full scale
- `decoder.cpp`: pulses -> modes (width / frequency / chirp tolerances,
  medians of 256) -> a radar when a mode has >= 20 pulses in 4 s and a PRI
  structure (`lib/radar_pri.cpp`). Folded into an existing radar (on the real
  recordings a single 60 us chirp radar first came out as 9-15 "radars"): pieces of its weak
  chirps (`piece_of`: same rate, shorter, inside its sweep - its start
  placed by the piece's frequency, one pulse per chirp, no PRI / shape),
  `belongs()` = modes on its PRI grid (a pulse merged with its echo is
  longer, its sweep bent, but comes one PRI after the radar's pulse), its
  PRIs on its frequency, ghosts with its PRIs >= 20 dB down (an image of a
  strong radar from outside the band), `fold_pieces` also radars with its
  scan period on its frequency; `try_link`: a fixed delay (+-1.5 us, 70 %)
  at comparable power = another pulse of its group (frequency diversity /
  fill pulse), >= 10 dB down = its echo (alias). A weaker copy of a pulse
  within 0.3 PRI = an echo, dropped. `merge_modes` joins duplicate modes
- `lib/radar_pri.cpp` (shared with ssr1030): differences 100 us - 30 ms clustered +-max(1.5 us, 0.12 %),
  sums of 2-3 levels with fewer members = missed pulses; kinds constant /
  blocks (runs, the level cycle without breaks) / stagger (period) / mixed
  / jitter; `straddles` = a set switched inside the window (not recorded).
  PRI sets the radar switches between are listed (live: per turn)
- `lib/radar_scan.cpp` (shared with ssr1030): levels in 5 ms bins x 8192 (41 s); main-beam passes = bursts
  within 6 dB of the 41 s max that DWELL there >= half the longest burst's
  time (a reflector lit by the beam elsewhere - and, with the main beam
  clipped, one only ~10 dB down reaches the clip level too - is shorter up
  there); stored passes re-checked every second (the first turn's sidelobe
  bursts drop out); `scan_period` = the spacing most passes repeat at, a
  least-squares fit over the chain; `fit_lobe` = robust parabola in dB over
  the lobe's unclipped top 10 dB (contiguous from the top, not the
  sidelobes) -> -3 dB width; none from a clipped pass
- DoA (ID separation by signature): talker id = frequency to 0.1 MHz
  ("1355.0 MHz", "#2" ...), packets = each candidate pulse +-24 samples
  (>= 2 whole covariance chunks), `freq_hz` = its centre, `bw_hz` = sweep +
  2 / width (> 250 kHz: kraken_doa's chunk ring, the whole band), avg_s
  `doa_avg` 30, keep_s `keep` 1 h; judged DOA_HOLD_S 0.3 s after the pulse:
  unclipped, >= 15 dB SNR, not a piece / alias, within `doa_db` 15 of the
  scan's peak, in a burst whose 150 ms max is within 6 dB of it and whose
  dwell near it (+-0.3 s) >= half the main-beam passes' (`mb_dwell`); only
  after 2 passes (or 30 s: not rotating); <= 60 / s. `valid(start, end)` per
  packet, `valid()` while a radar's pulses come
- Outputs: table (Radar, MHz, Pulse, PRF, PRI, Scan, RPM, Beam, Peak dBFS,
  SNR, Pulses/s, Main beam) + details, facts (strongest radar), events,
  plot (the strongest radar's level over one turn, 0 = main beam on us,
  marker = its talker, threshold = the DoA level; the DF panel's plot
  section names it "radar ... pulse level over one turn"), raw = a line per
  main-beam pass. `RADAR_DUMP=1` logs pulses / modes / passes (offline)
- Generator `tools/radar_gen.py` (scene JSON -> every antenna cu8 / cf32 in
  0.1 s pieces; pulse groups, LFM, PRI kinds + alternating sets, rotating
  antenna, a lit reflector's echo; refuses a scene named like its output's
  sidecar); `tools/scene_three.json`
- Tests (scratch t2/): three radars (scene_three) offline + end to end
  through kraken_doa (fake heimdall): 60.0 / 200.0 / 300.0 deg exact, the
  parameters (README table); a clipped main beam + a reflector -14 dB:
  140.0 (truth; 253.6 = the reflector before the dwell rule); noise 0
  pulses; UI t_ui_radar.py (headless Chromium, 16 checks). Real Auckland
  1355.0 MHz (live + recordings): 59 us LFM -1.17 MHz, PRI 1712 / 1395 us
  blocks of 8 alternating per turn with 1748 / 1424, scan 4.88 s, beam 1.5
  deg (12 dB gain), its short pulse on 1350.07 MHz; array 116-119 deg (~255
  compass). CPU (Pi 5): plugin 2.2-2.9 % live

