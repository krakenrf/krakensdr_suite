# 1030 MHz interrogations plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**1030 MHz interrogations (`plugins/ssr1030/`, README.md there):**
- SSR / Mode S / ACAS interrogations, the uplink of the 1090 MHz replies:
  `sample_rate` = `min_vfo_rate` = 2.4 MHz, `fixed_freq_hz` 1030 MHz (like
  adsb: picking it tunes the VFO + tuner), manual only, talkers
- `front.cpp`: raw power per sample (0.8 us pulses are 2 samples, P1 -> P2
  / S1 -> P1 / P3 -> P4 only 2 us apart): segments of samples over the
  threshold (`sens` 10 dB over the noise = median of 64-sample means, >=
  a quarter LSB^2), short pulses = local peaks with their edges at a
  quarter of the peak (interpolated, a valley ends a pulse); a LONG pulse
  (Mode S P6) = a run of the 5-sample mean over threshold - 3 dB for >= 10
  us, split at its first deep valley in the first 3 us (P1 / P2 merge into
  the mean); a 2^15-sample ring keeps the samples for the DPSK
- `interr.cpp`: a pulse waits 40 us, then as P1: Mode S = a long pulse from
  +1.7..3.9 us (P2 merged or not) of 17-23 / 30-37 us total; else P3 at
  8 / 17 / 21 / 25 us (+-0.3, same level +-3 dB, 0.35-1.45 us wide), P2 at
  +2 (any level: SLS), P4 at P3 + 2 (width >= 1.2 us = long), S1 at -2
  (not stronger). A B / D whose +2 us pulse has an A / C of its own = an S1
  (ACAS steps came out as Mode D)
- `dpsk.cpp`: P6 timing (12.5 ns grid over 3.25-3.75 us after P1) + P1 /
  P2 / P6 amplitudes by least squares on the known part (to t6 + 1.45 us);
  per-survivor Viterbi over the chips (32 states, chips 0-4 +1, 5-6 -1, then
  the data, 2 guard chips; g(x) = S(x) - S(x - 0.25 us) over -1 .. 4.25
  us); carrier offset from the decoded frame's phase slope, a 2nd pass; the
  lower residual wins. S = complex step response on -1 .. 4 us / 32 per us,
  default `s_default.h` (measured here: least squares over checked live
  frames), re-fitted (Cholesky, smoothness 5 % + 0.4 % pull to the table)
  from the normal equations of checked frames (forgetting 1/300; known
  parts of strong frames added at weight 0.3, refits only on checked ones -
  a preamble-only bootstrap was tried on synthetic other receivers and
  didn't converge: removed). ~0.5 ms per frame
- `uplink.cpp`: CRC-24, the uplink address encoding (top 24 coefficients
  of A(x)G(x)) and its GF(2) inverse, fields per UF; UF11 / UF16 corrected
  against the all-ones address (two bit errors 1-3 apart or one in the last
  8 - a wrong run of chips; ~1 in 50000 random frames would pass)
- `track.cpp` (the ID separation): SSR = sequence search on unattributed
  all-calls (difference histogram 20 us bins, +-2 % smoothed local maxima,
  shortest PRI first; chains at n = 1..3 x P within JIT 3.5 %, >= 10
  members, >= 50 % of the slots; a chain at a multiple of a known SSR's PRI
  or one that went quiet = that SSR), live tracking within 4x its own mean
  PRI deviation x sqrt(n) (n <= 5) and 9-12 dB; sidelobe all-calls (P2 >=
  P1 + 2 dB) by the omni P2's absolute level (+-3 dB); UF11 within 0.6 ms of
  a slot; roll-calls by the checked II of an SSR (IIS / SIS), else within 4
  PRIs at its level. Fixed = three bursts (gaps <= 25 ms) at one level
  (+-3 dB, >= 13 dB) within 15 s, then everything at that level that may be
  a fixed one's (`fixed_ok`: no S1, no UF16 / UF11, no P2 >= -6 dB A / C,
  no B / D); bursts > 3 s apart whose all-calls keep an SSR's PRI = that
  SSR's main beam, not fixed. ACAS = whisper-shout sequences (Mode C with
  S1, steps <= 60 ms apart, not falling > 4 dB, >= 3 steps, half with S1)
  matched ~k s later (+-12 %) at +-8 dB; a checked UF16 broadcast = an ACAS
  by its address (also without whisper-shout); UF0 / UF16 prefer an ACAS
  heard within 1.5 s at +-6 dB. Scan / beam with `radar_scan`, PRI with
  `radar_pri` (moved from the radar plugin to `plugins/lib`, identical
  radar output checked), interlace = the A / C letter cycle over the slots
  + the UF11 spacing. DoA candidates judged 0.3 s later, dropped beyond 1 s
  (kraken_doa's 1.5 s ring; the discovery replays up to 4 s back)
- Outputs: table (Interrogator, Type, Interrogations, PRF, PRI, Scan, RPM,
  Beam, SLS P2, Mode S, Targets (>= 3 sightings), Level dBFS, SNR, Per s,
  Main beam) + details, facts, events, plot (strongest SSR over one turn;
  the DF panel's plot section shows it as "🛰 ... interrogation level over
  one turn"), raw = each Mode S frame's hex. `SSR_DUMP=1` / `SSR_PULSES=1`
  offline logs
- Generator `tools/ssr_gen.py` (SSRs, fixed, ACAS at known bearings,
  every antenna, real formats / parity, a receiver response "measured" or
  "lowpass", echo, df); `tools/scene_four.json`
- Tests (scratch t3/, details in the README): scene_four offline + end to
  end 60.0 / 150.0 / 200.0 / 300.0 deg exact, noise nothing, DME pairs on
  1030 ignored (dme sees them, not the SSR pulses), UI headless Chromium;
  live: SSR II 10 (199 Hz, scan 5.00 s, 2.6 deg, A / C + UF11 every 3rd,
  215 deg array frame), multilateration 1 s bursts (276 deg), TCAS by
  broadcasts; interrogation -> reply pairs with adsb on a 1090 MHz tuner
  (independent mode) confirmed the address encoding. CPU ~4 % of a core

