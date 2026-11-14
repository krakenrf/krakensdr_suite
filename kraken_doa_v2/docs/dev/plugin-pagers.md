# Pagers plugin (POCSAG + FLEX)

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**Pagers: POCSAG + FLEX (`plugins/pagers/`, README.md there; was `pocsag` until 2026-10-23 - the user's request: FLEX added, renamed):**
- One plugin, one FM front end (63-tap 8 kHz low-pass, discriminator in Hz
  at 38.4 kHz, `decoder.cpp`) feeding POCSAG (`pocsag.hpp`, the old
  decoder.cpp's chains moved as they were: 512 / 1200 / 2400 bit/s) and FLEX
  (`flex.hpp` / `flex.cpp`); `bch.hpp` = the BCH(31,21) + parity both use
  (FLEX = POCSAG's codeword with every field reversed: `flex_correct` =
  reverse32 around `bch_correct`); `pages.hpp` = the shared facts / counts.
  `min_vfo_rate` 12500 (every FLEX speed decoded through a 12.5 kHz VFO
  filter). Events start "POCSAG " / "FLEX <speed> capcode ..."; raw "POCSAG
  RIC ..." (was "RIC ...") / "FLEX <speed> <cycle.frame> <phase> capcode ..
  V<type> <vector> <words>". POCSAG's pages byte for byte the old plugin's
  (A/B clean - 4 dB); its old facts (Bit rate, Polarity, Deviation, Last
  RIC) are now `POCSAG` + the shared Last address / Last message / Messages
- Renamed: `dig::renamed_plugin` maps `pocsag` -> `pagers` (saved VFO modes,
  `pocsag.*` options, AUTO_DETECT_OFF); an old build-only `plugins/pocsag/`
  (gitignored build/ left by the update) is skipped by the registry scan.
  The playback entry keeps its id `pocsag-three-rates` (cached files stay)
- FLEX format sources (no other decoder's code): Motorola's MC68175 data
  sheet appendix A / B (frame, phases, word layouts as the chip hands them
  over, message formats + checks, address ranges, capcode conversions, time
  BIWs), the FLEX patent's sync drawing as published by Jelmer Bruijn (A1-A4
  words, B, sync 2's C, "LSB transmitted first"). Settled ON A REAL
  RECORDING (sigidwiki FLEX_Pager_IQ_20150816_929613kHz, 6400/4, job scratch
  only - real people's pages: never in the repo): the BCH polynomial is the
  31 received bits reversed (FIW 0 errors in every frame), info LSB first,
  1 = the LOWER frequency (the sync correlated -0.97), 4-level first bit =
  sign + second = inner (sync 2's C symbols match exactly), the 4-bit word
  check = nibble sum 0xF, the alpha fragment check (10-bit one's complement
  of 8/8/5-bit groups, the K field 0), numeric check (8 LSBs, top 2 folded,
  complemented, 6 bits; K4/K5 = the first message word's bits 0-1), alpha
  signature over the whole message WITHOUT its trailing ETX / NUL, long
  address set 1-2 (cap = w1 + 2068480 + 32768 x (0x1FFFFF - w2)), the long
  address's first message word = the second vector slot (vector n counts
  it, b points at the second)
- Phases: 1600/2 A; 3200/2 even symbols A, odd C; 3200/4 sign A, inner C
  (multimon-ng says B - the data sheet's figure says A + C for 3200 bit/s);
  6400/4 even A + B, odd C + D (= multimon-ng). Labels only in raw frames
- Receiver (each rule from a failed test - README *Receiver design*):
  sync 1 hunted every quarter bit (`fit_sync`: 80 smoothed values sliced
  against their mean, the first 64 bits matched <= 5 off - multimon-ng's
  gen-ng sends only those right) -> NEXT state: `sync_at` +-0.5 bit on the
  smoothed discriminator (finest timing: widest |deviation| of the 80);
  after a frame NEXT = where its grid ends (+111.5 bits) - a ClockRecovery
  in front lost the first frame after a key-up (noise in its centre) and
  frames after an idle one (idle words = 0 = one frequency: its centre
  drifted onto it); FIW read on the grid; sync 2's C + inverted C fix the
  data grid's whole symbols (+-2), its fraction from the eye over sync 2 +
  block 0; per block the fraction from a midpoint timing detector over the
  LEVEL CHANGES only (`timing_error`, STEP 0.6 symbol) - the eye opening of
  mostly idle blocks wandered +-0.4 symbol; a block with losses retried at
  +-0.05 / 0.1 / 0.15, a fraction followed only from a block that decoded;
  4-level slicer levels from sync 2's bit syncs, then decision-directed.
  `Smoothed` = half-symbol boxcars (y16 / y32) kept 1.7 s by sample number
- Trust (`parse_phase`): addresses have no check, 4 bit errors make another
  valid codeword (even parity): per block per phase, estimated bit errors
  (corrections + 3 per lost word) 4+ = BIW / address / vector words with at
  most 1 corrected bit, 12+ = none (a real page on a wrong capcode at 2 dB
  without it); message words keep both corrections (their checks catch a
  miscorrection). A word that needed no correction is taken without its
  4-bit check (gen-ng leaves the BIW's 0)
- Messages: alpha / secure / binary fragments put together per (capcode,
  message number) - F 3, 0, 1, 2 ... + C; a missed fragment "…", none for
  4.5 min = shown incomplete. Short instruction 000 = group call
  (temporary address 0x1F7800 + a in frame f): members collected,
  announced once per frame, the message to that address names them.
  Time BIWs (f 1 date, 2 time, 5 zone - zone table A-19 as reconstructed
  from the data sheet's garbled text extraction, NOT checked on air) ->
  fact `FLEX time`. Long set 2-4: no published conversion - shown as its two
  words
- `Host::valid` per block with <= 1 lost word per phase (block 0 from sync
  1's start): the Digital squelch stays open on a FLEX channel (frames back
  to back); the AFC gets the carrier offset from sync 1
- Generator `tools/flex_gen.py` (every speed / page kind / fragments / group
  calls / time BIWs, Gaussian BT 0.5 at 3200 Bd, idle words 0); playback
  `flex-four-speeds` (929.6625 MHz, 120 deg, six frames, 6 events + VFO
  DoA). Offline: `FLEX_CW=1` prints syncs + every codeword with its fraction
- Tests (job scratch 1c0b4517, 2026-10-23): the real recording 13 syncs /
  101 valid blocks / 39 pages (3 incomplete - fades); generated 6 frames x
  6 pages per speed: exact at every speed down to 6 dB CNR (in 25 kHz), at 4
  dB 36 / 35 / 32 / 29 of 36 (1600/2 .. 6400/4), 1600/2 36 / 36 at 2 dB, no
  page with a wrong capcode or text at any level; offsets +-1.5 kHz,
  inverted, +-100 / +300 ppm, 12.5 / 16 / 25 kHz VFO filters - all exact;
  10 min of noise: no FLEX event; multimon-ng (GPL, an outside reference
  only - compiled by hand with gcc in scratch: its CMake build links a GUI
  exe under MSYS2 when SDL3 is installed; gen-ng needs -DDUMMY_AUDIO)
  decodes the generator's 1600/2 / 3200/2 / 3200/4 / 6400/4 short-address
  pages, and the plugin decodes gen-ng's pages + group call;
  tools/digi_test.cpp: PLUGIN:pagers / PLUGIN:pocsag / AUTO all decode the
  playback scene (66 / 66 blocks). digi_test's header build line now also
  needs src/ai_sandbox.cpp + src/geo_address.cpp (dig_plugin.cpp's
  sandbox calls). Linux x86_64 / aarch64 compile clean (zig); Windows -Werror

