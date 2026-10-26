# ADS-B plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**ADS-B (`plugins/adsb/`):**
- 2.4 MHz input (`min_vfo_rate` 2 MHz; picking the decoder sets the 2.4 MHz
  "No decimation" bandwidth and tunes to 1090 MHz - above). Magnitude -> chips integrated
  at 5 sub-sample timings (box-filter weights, 0.5 us = 1.2 samples - the
  same slicer as dump1090's 2.4 MHz demodulator) -> amplitude gate on
  preamble pulses 1-3 vs the gap (noise floor = lowest 512-sample mean,
  smoothed) -> preamble score at the 5 timings (pulses >= 2x gaps, no gap
  as strong as a pulse) -> the best 2 sliced -> CRC-24. DF17/18: 1-bit
  repair (syndrome table) or 2 bits among the 8 least certain (CRC linear:
  XOR of per-bit syndromes) - repaired / DF11 with an interrogator id /
  address-parity replies (DF0/4/5/16/20/21, syndrome = address) only for
  aircraft already heard cleanly; an aircraft counts (events, map,
  valid()) from its 2nd message
- `modes.cpp`: CRC, AC12 / AC13 altitude (25 ft and Gillham), squawk,
  callsign, surface speed, CPR global (even/odd within 10 s airborne,
  25 s surface - surface needs a reference) and local decoding (against
  the aircraft's position < 60 s old), NL(). Position checks: within the
  `range` option of the station (default 500 km) and <= ~Mach 2.5 from the
  last position (4 consecutive failing global fixes replace it)
- Unusual activity -> events starting with "⚠" (once per occurrence, with
  position + altitude, so the decoder data log keeps them): emergency
  (7500 / 7600 / 7700 or TC28 emergency state) and its end, a CHANGED
  squawk, special squawks 7400 (lost link UAV) / 7777 (US interceptor) /
  0000, categories A6 high performance / B2 lighter than air / B3
  parachutist / B4 ultralight / B6 UAV / B7 space vehicle, |vertical rate|
  >= 6000 ft/min (`VRATE_ALERT_FPM`, re-armed below 2/3 of it), >= 400 kt
  below 10000 ft (`FAST_LOW_KT/FT`, ES altitude + velocity < 30 s old).
  Plus "IDENT (SPI)" (no ⚠; ES surveillance status 3, or 2 replies with
  flight status 4/5 within 10 s; at most every 2 min). A squawk from a
  DF5/21 reply (parity-checked only - one bit error is another code) is
  only taken when the next reply within 30 s repeats it; TC28's at once.
  Test: scratchpad adsb/gen_unusual.py (one aircraft per case + an
  ordinary one that must stay silent + a corrupted reply)
- Output: the aircraft table (`TABLE_COLS`: every decoded field, a row per
  aircraft updated at most 1/s, key = the map point id), summary facts,
  events (new aircraft, callsign, first position, emergencies 7500/7600/7700
  or the TC28 emergency state), map points (kind aircraft / vehicle for
  C1-C2, heading = track, ttl 60 s), every message in verbose. Aircraft
  forgotten 60 s after the last message; `reset()` drops them all
- Validated on synthetic recordings (`--station` for the offline test):
  the published "1090 MHz Riddle" messages decode to their documented
  values (KLM1023; 40621D at 52.2572, 3.9194, 38000 ft; 485020 159 kt /
  183 deg), 100% of messages at >= 15 dB SNR, ~67% at 12 dB, none from 20 s
  of noise. CPU (Pi 5): plugin ~5% of a core, kraken_doa +~10% for
  the 19 MB/s of samples it forwards. Not yet tried on live 1090 MHz.
  Generator in the repo (2026-10-16): `tools/adsb_gen.py` (DF17 ident /
  CPR even + odd position / velocity + DF11, PPM built at 24 MHz and
  integrated to 2.4, each aircraft its carrier offset, forced collisions;
  positions around the station given at generation time, fixed - the
  loop) + `tools/scene_six.json` = playback `adsb-six-aircraft`: offline
  190 of 197 messages (the 6 lost ones overlap another), the CRC = the
  Riddle's KLM1023 frame, every aircraft 0.0 deg, positions 25-120 km at
  their bearings; end to end 6 / 6 at 0.0 deg
- v2.0 (2026-10-20, the user's request: weaker / more distant aircraft,
  better error correction, the coherent array - each an option in the
  decoder tab; README *Every antenna* + its test table):
  - `kp::Info::multichannel`: in coherent mode with DoA on every antenna
    (`process_multi`, `xs_` ring beside `mag_`). Option `array` (on): the
    detector's magnitude = sqrt of the noise-normalised power sum
    (`g_` / `pn_` per antenna, `set_noise`), gate `GATE_ARRAY` 1.3; what
    the sum doesn't decode -> `decode_beams`: a preamble >= `ARRAY_RATIO`
    1.35 x its gaps with the LOOSE gap rule (no gap above the pulses'
    mean - the strict min-pulse rule lost 40 % of the 5 dB messages) ->
    MRC weights = principal eigenvector of the preamble pulses'
    covariance (`array.hpp`: power iteration, solve), strict preamble on
    the beam, slice -> decision-directed weights from every sliced pulse
    -> option `separate`: MVDR (target = principal of pulses minus gaps,
    the window's covariance + one antenna's noise) and `scan_inside` (a
    second message inside a decoded one, its signature projected out:
    `decode_null`). Option `pulse_doa`: each packet's `kp::Talker::cov` =
    its own pulse samples, minus samples of another accepted message's
    pulses (`acc_`, `dirty`); packets wait in `pk_` until the separation
    scan after their decode (`flush_packets`) - kraken_doa keeps no
    stream ring for such a VFO and never drops them for an overlap.
    Aircraft signature (`Aircraft::sig`, raw antennas, learnt from exact-
    parity DF17/18 / DF11): repaired messages, repaired replies and DF11
    with an IID must reach |a^H s|^2 >= `SIG_RHO_MIN` 0.7
  - `fix` 0 / 1 / 2 (was on / off: a saved 1 = standard, as before):
    extended = DF17/18 up to 3 of the 12 least certain bits (`search`),
    DF11 1 anywhere (IID 0) / 1-2 weak with any IID (known AA), replies
    1-2 of the 8 weakest -> a known aircraft's address at its level
    (`level_ok` 6 dB) AND content (`reply_consistent`: altitude within
    1000 ft of the last 2 min / its squawk) - without the content check a
    sens + extended run made 1 wrong DF20 in 6243
  - `cpr` (on): pairs up to `PAIR_MAX_MOVE_M` 4 km / ground speed (10-30
    s; 15 s without a velocity), the dead-reckoned last position
    (`reference`) the local reference for `REF_AGE_LONG_S` 180 s
  - `sens` (off): gates 1.35 / 1.2, every timing sliced, one antenna also
    a preamble >= `RATIO1_SENS` 1.6 with the loose rule
  - Facts "Antennas" (combined, decoded only with a beam, separated,
    bearings from pulses; one antenna: says DoA must be on) and "Bit
    repair"; `ADSB_STATS=1` (offline) prints path / repair counters
  - Tests (job scratch e4b30c3e t/: mkscene.py + score.py - every decoded
    message against the truth hex -, trace / pos / track.py): v2.0 with
    one antenna = v1.0 exactly on every scene (also through digi_test);
    array: sweep 18 -> 73 %, busy 66 -> 94 %, replies sweep 15 -> 69 % /
    busy 55 -> 89 %, ~6 dB more sensitive, noise 0, 0 wrong anywhere;
    numbers in the README. `adsb_gen.py` got `replies` (DF4/5/20/21 per
    aircraft) and `moving` (scene) - both opt-in, the playback's cache
    unchanged. digi_test's Windows build line needs src/ai_sandbox.cpp +
    src/geo_address.cpp now (dig_plugin.cpp calls aisandbox)
- Performance pass (2026-10-26; a 2015 MacBook Air i5-5350U, Apple
  clang 14; Pi / Windows not measured):
  `process_multi`'s per-sample loop over the antennas (keep the samples,
  the weighted power sum's magnitude, each antenna's lowest 512-sample
  power) is `sum_block<M>` - the antenna count a template parameter, the
  antennas' pointers and weights in locals: m = 5, 16384-sample chunks
  ~1.4-1.5x (e.g. 24.6 -> 16.3 ns per input sample), bit-identical with
  Apple clang (not with `-ffp-contract=fast`: rounding only). Tested, not
  taken: the same as passes per antenna (power, sum, minima: 0.5-0.65x -
  the block no longer fits L2). `adsb-six-aircraft` identical; offline
  user CPU without the bearings 0.57 -> 0.49 s (-15 %)
