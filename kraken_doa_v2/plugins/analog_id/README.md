# analog_id - Analog FM IDs (CTCSS / DCS, MDC1200, FleetSync, 5-tone, DTMF, 1750 Hz)

What an analog FM transmitter sends to say who it is: the sub-audible
signalling under its voice - a CTCSS tone (67-254 Hz, "PL", "tone squelch")
or a DCS code (a 23-bit Golay word at 134.4 bit/s, "DPL") - and the in-band
ID signalling radios send at the start or end of a transmission: **MDC1200**
(Motorola: unit ID, status, emergency, call alert), **FleetSync** (Kenwood:
fleet + unit, 1200 / 2400 bit/s, FleetSync II), **5-tone selcall** (ZVEI,
DZVEI, CCIR, EEA, EIA, NATEL, MODAT), **DTMF** (a PTT ID / ANI, commands) and
**tone bursts** (1750 Hz repeater access). Every transmission becomes a
talker with its exact span, **named after the transmitting radio's own ID**
when one was decoded, else its tone / code: every radio gets its own DoA
lobe on the 🗺 Map's 📡 DF panel - also radios that share one CTCSS tone.

The plugin was `ctcss` ("CTCSS / DCS tones") until 2026-10-22. kraken_doa
maps the old id (`dig::renamed_plugin`): VFOs saved with `PLUGIN:ctcss`, their
`ctcss.*` options and the Auto-detect-off list load as `analog_id`, and an
old `plugins/ctcss/` folder left with only its (gitignored) build after an
update is not listed - it would run next to this one.

Pick "Analog FM IDs (...)" as a VFO's digital decoder, on a 12-24 kHz VFO on
the FM channel (it takes 24 kHz; Auto detect may pick it too: it reports
nothing without a tone or an ID). Listening keeps working - the VFO's Demod
(NBFM) is untouched; Voice to Text transcribes the analog audio and names
each transcript after the radio (below).

## What it reads

### CTCSS / DCS

Following ETSI TS 103 236 V1.1.1 (2014), "CTCSS and DCSS signalling
system" (= TIA-603 / EIA RS-220): table 1 (tones), 4.2 (the DCS codeword),
table 2 (the 83 codes with their bit patterns), 4.2.3 (polarity), 6.x
(encoder tolerances, decoder response times).

- **CTCSS**: every tone of ETSI table 1 (38 EIA tones + 69.3) and the
  non-standard ones radios offer (150.0 NATO, 159.8, 165.5, 171.3, 177.3,
  183.5, 189.9, 196.6, 199.5, 206.5, 229.1, 254.1 - 51 in all); a tone off
  every list (option "Non-standard tones") is named by its measured
  frequency (`CTCSS 140.0`), the same id again within 0.5 %. The
  frequency is measured over the transmission (± ~0.01 Hz at good SNR -
  the table's "Details"), the deviation in Hz. Encoders are within ±0.75 %:
  a tone is the listed one within 0.85 % (+ the measurement's spread at
  lock). How it ended: **reverse burst** (the tone's phase turned 120-180°
  ~0.2 s before the carrier drops - Motorola's "reverse burst", GE's
  "squelch tail elimination"; the angle is shown), **tone off** (the tone
  stopped, the carrier stayed), **carrier off**, or **another
  transmitter** (a stronger one captured the receiver with its own tone).
- **DCS**: 23 bits, bit 1 sent first, bits 1-9 the code (3 octal digits),
  10-12 fixed 0 0 1, 13-23 the check bits of the cyclic Golay (23,12) code
  g(x) = x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 (0xC75; checked against
  all 83 bit patterns of the ETSI table); a 1 = a positive frequency shift
  (normal polarity). Every rotation of a codeword is a codeword, so a code
  is where bits 10-12 read 0 0 1 - some waveforms read as several codes,
  and every waveform read inverted is another code (023N = 047I): the name
  is the ETSI code if one reading gives one (normal before inverted), then
  the 104-code list most radios offer (+21: 036 053 122 145 212 225 246 252
  255 266 274 325 332 356 446 452 454 455 462 523 526), else any reading
  (non-standard); the label gives the other polarity's name
  (`023N = 047I`). The **turn-off code** (134.4 Hz for ~0.2 s after the
  code: the receivers' squelch tail elimination) is recognised and noted.

### MDC1200 (`mdc.hpp`)

The format as the public decoders document it - Matthew Kaufman's
mdc-encode-decode (GPL-2.0: read as documentation and run as a test oracle,
not copied), the OneOfEleven UV-K5 firmware's opcode list (Apache-2.0),
sdrtrunk's field names:

- 1200 bit/s MSK, **XOR-precoded**: a bit that differs from the one before is
  1½ cycles of 1800 Hz, an equal one a cycle of 1200 Hz. Preamble (0x55... =
  steady 1800 Hz), sync `07 09 2A 44 6F`, then a block of 112 bits: 7 bytes
  (op, arg, unit ID high / low, CRC low / high, 0) and 7 parity bytes of a
  rate ½ convolutional code (p[t] = d[t] ^ d[t-2] ^ d[t-5] ^ d[t-6], bits LSB
  first), interleaved 16 × 7. CRC-16 (0x1021 reflected = 0x8408, init 0,
  out ^ 0xFFFF) over op, arg and unit. Ops 0x35 / 0x55 are double packets: a
  second block follows at once.
- Messages: PTT ID (op 01), emergency (00, ⚠), status (46 / response 06),
  message (47 / 07), request to talk (40 / 41), radio check acknowledge
  (03 / 30), inhibit / enable acknowledge (0B) - **sent by the radio named**;
  radio check (63), radio inhibit / enable (2B), status request (22), remote
  monitor (11), acknowledge (23), emergency acknowledge (20) - **sent by a
  console to the radio named** (no talker: the transmitter has no ID in it;
  logged "MDC1200 → 5678: Radio check"); call alert / selective call (35, a
  double packet: the second block names the caller - the talker - and the
  unit field the radio called). Unknown ops are logged "op XX arg YY".
- Pre-ID / post-ID: the arg's bit 7 is said to mean "pre" (0x80) - but the
  real "leading PTT ID" recording on Wikipedia sends arg 00 at key-up. So the
  plugin reads it from where the burst sits: "(at key-up)" within 1.5 s of
  the transmission's start, "ID at release" in the end line when it ended
  within 0.8 s before the end.

### FleetSync (`fleetsync.hpp`)

The field layout as Kaufman's fsync decoder and sdrtrunk's FleetSync classes
(both GPL) document it - read as documentation of the format's facts only;
no code of theirs is used:

- FFSK without precoding: a 1 = 1200 Hz, a 0 = 1800 Hz (1200 bit/s) or
  2400 Hz (2400 bit/s). Preamble 1010..., sync `23EB` (also `052B`), then
  64-bit blocks, each an **MPT 1327 codeword**: 48 data bits, the 15 check
  bits of MPT 1327's BCH(63,48) code (g = 0xE815) with the last one inverted,
  and an even parity bit - the check of the `mpt1327` plugin, which was
  written from MPT 1327 itself. A message = the blocks that check, back to back (ANI /
  status: one; another fleet: two; data: more). One error among a block's 4
  least certain bits is corrected - not in a nearly constant word (< 6 bits
  of either value): a steady 1200 Hz stretch of voice after a chance sync
  reads as all 1s, and one of the 2016 words with two 0s checks (it made a
  "FleetSync 354-5094" out of voice once in 50 min). A message whose sender
  is no radio (fleet above 349 / unit above 4999 - the all-ones fields) is
  dropped.
- Block 1: byte 0 = active-low flags (emergency, lone worker, paging, end
  of transmission) - in a status message the status number; byte 1 = type
  (ANI 0x80, status 0x40, acknowledge 0x20, GPS / data 0x02, another fleet
  0x01); fleet = byte 2 + 99 (100-349), from / to unit = 12 bits each + 999
  (1000-4999). The talker = the sender, `FleetSync 100-1001`.
- **FleetSync II**: each 16 bits of a block are sent as 4 nibbles in 4
  BCH(15,7) codewords (x^8 + x^4 + x^2 + x + 1 with bit 14 = x^0 - the
  reciprocal of BCH(15,7)'s usual generator; one error corrected each), the
  four 16-bit fields of each 64 bits on air in pairs, the second of each
  pair first; 4 such 64-bit words per FleetSync block - the layout as the
  public decoders read it (no real FleetSync II recording found: synthetic
  + the reference only).
- Data / GPS payloads (type 0x42) are shown as text when printable (length
  in bytes 9-10, the text from the third block on, each block last byte
  first - the decoders' reading, not confirmed on a real radio); GPS
  positions are not decoded (no verified layout).

### 5-tone selcall (`selcall.hpp`)

Tone tables, tone lengths and tolerances from Midian Electronics' "Tone
signaling charts" (the five / six / seven tone table): ZVEI1 / ZVEI2 / PZVEI
(70 ms), ZVEI3 / DZVEI / PDZVEI (70 ms), CCIR-1 (100 ms) / CCIR-2 (70 ms) /
PCCIR (100 ms), EEA (40 ms), EIA (33 ms), NATEL (70 ms), MODAT (40 ms). A
sequence = 4-16 steady single tones without pauses, each the nearest tone of
one standard within its tolerance (ZVEI ±1.5 %, CCIR / EEA ±1 % + 8 Hz, EIA
±16 Hz), their lengths that standard's (0.55-1.6 x, the first may be a long
lead-in); E (repeat) stands for the digit before ("11223" is sent 1 E 2 E 3
in ZVEI1); the same symbol twice in a row is no sequence. Standards with the
same digits share a talker prefix (`ZVEI 12345`, `CCIR 04221`, `EEA ...`),
the table names the standard(s) that fit ("CCIR1/PCCIR").

**Ambiguity**: DZVEI's digits are ZVEI's tones moved up one place, and
ZVEI2's repeat tone (970 Hz) is DZVEI's 1 - many sequences read both ways
(ZVEI2 33344 = DZVEI 41451). No decoder can tell them apart without being
told: by default the common reading wins (a small per-standard prior: ZVEI
over DZVEI) and the other is shown ("... (or DZVEI 90210)"); the **Selcall
standard** option takes only one family.

### DTMF

The eight DTMF frequencies of each 20 ms frame (Goertzel on the windowed
frame): the strongest low and high tone, each >= 4x the others of its group,
twist -13 .. +13 dB (receiver audio is often de-emphasised: the high group
6-10 dB down), the two holding >= 75 % of the 250-3400 Hz power, each tone's
own frequency (the FFT's peak) within ±1.8 % of the DTMF one and holding
still (±0.6 %) over the digit, not two harmonics of one voice pitch (talk-off:
1209 / 697 ≈ 7 / 4 - f0 174 Hz puts its 4th and 7th harmonics on "1"; the
neighbouring harmonics of that f0 must be empty). A digit = 4 frames (~35 ms
of tone; the standard's minimum is 40 ms), a lone digit 8 (~60 ms);
digits until 0.5 s without one make a sequence: `DTMF 123A#0`.

### Tone bursts

A steady tone (±0.8 %) of 0.25 s+ on the standard burst list (1600-2550 Hz
in 50 Hz steps; 1750 Hz = the European repeater access tone) is a burst
(`Burst 1750`); other steady tones of 0.5 s+ (a 1 kHz test tone) are logged
as events only.

Not decoded: 2-tone sequential paging (Motorola Quick Call), GPS positions
of FleetSync / MDC data packets, DTMF faster than ~35 ms per digit (Genave
"superfast"). Nothing here is encrypted: these are plain identifiers.

## How (`rx.cpp`, `sig.cpp`)

- Front end (24 kHz complex in): 41-tap channel filter (±7.5 kHz), FM
  discriminator in Hz. Three streams from it: 24 kHz → 2400 → 1200 Hz
  (Blackman FIRs, flat to ~350 Hz) for CTCSS (45 Hz high-passed) and DCS
  (130-290 Hz low-passed); and the voice band at 12 kHz (63-tap low-pass at
  4.2 kHz) for the signalling. The carrier power, aligned with them, also in
  10 ms bins kept 82 s (`carrier_rise`: where a transmission an ID burst found
  began).
- CTCSS detection: every 50 ms a Hann-windowed FFT (2048 points, 0.59 Hz
  bins) of the last 0.3 s - or only what follows a carrier's onset inside
  it; a candidate = the strongest peak 60-257 Hz ≥ 13 dB above the band's
  median, holding ≥ 55 % of the 55-275 Hz power, with ≥ the "Weakest tone"
  deviation (80 Hz) and ≤ 1.5 kHz, and not one line of a periodic signal's
  family (lines at f ± f/k, k = 2..10, within 15 dB: a 151.7 / 162.65 MHz
  data carrier with lines every 33.33 Hz, seen live). A tone = 3 consecutive
  candidates (2 when strong) that agree in frequency (0.12-0.8 Hz by SNR) and
  amplitude (±30 %), and whose phase advances evenly over the last 0.2 s.
  Off the lists: one candidate more and ≥ 70 % purity; near 67.2 Hz (half
  DCS's bit rate) 5 more. 134.4 Hz is never a tone.
- CTCSS tracking: the tone mixed down, a 97-tap 35 Hz low-pass, every 10
  ms: amplitude, phase (the measured frequency = the whole phase advance /
  its time, a reverse burst's step taken back out), the end (amplitude below
  half, or the carrier gone), a reverse burst (the phase turning > 100° -
  confirmed when the tone or carrier ends within 0.5 s). Another tone
  dominating the FFT for 3 hops = another transmitter. The start: the
  carrier's onset, or where the tracker's amplitude rose through half.
- DCS: zero-crossing clock recovery with a rate loop (encoders ±2 %),
  decision-directed levels, hard bits. A code = 46 bits 23-periodic without
  an error and a Golay codeword, or 69 bits whose 3-word majority is one with
  ≤ 5 bits disagreeing. Locked: > 6 wrong of the last 23 = the end. No DCS
  search while a CTCSS tone is locked.
- **FFSK** (MDC1200, FleetSync): per 12 kHz sample two sliding one-bit
  complex correlators (1200 / 1800 Hz for 1200 bit/s, 1200 / 2400 Hz for
  2400 bit/s) on a 300 Hz high-passed copy. Sync search at every sample: the
  tone soft value (|a| - |b|) / (|a| + |b|) at bit spacing against the sync
  word's tones (MDC: 39 tone bits - its first depends on the preamble - ≤ 4
  wrong; FleetSync: 32, ≤ 2), first plain, then (if close) with the decision
  threshold balanced: one-bit correlators part the tones only to 0.64, and a
  receiver's audio chain leaves that crosstalk unequal (the live MDC
  recording: 0.75 on 1200 Hz bits, 0.51 on 1800 Hz bits); any weight g on |b|
  between the two reads every bit right, g = √(r_b / r_a) is the middle.
  (Scaling each tone by its own level - tried first - put g outside it: the
  live sync read 20 of 39 bits wrong.) The best sample of a sync + a
  parabola = the bit timing.
  - MDC1200 is read **coherently**: the tone of a bit only says whether it
    differs from the one before, so tone decisions would turn one error into
    a wrong rest of the packet. The waveform's phase at a bit's start is the
    previous data bit (π per change), so the correlators of window i + 1 -
    each in its own tone, the 1800 Hz oscillator's π per bit taken off -
    against references learnt from the sync's known bits (and kept
    following, decision-directed) give data bit i on its own. Then
    de-interleave, the convolutional code's single errors (majority of the 4
    syndromes a data bit touches), CRC.
  - FleetSync: tone = bit, 64-bit blocks while they check; the bit timing
    tracked on the transitions. Each block is read **coherently** first:
    both modems keep the phase continuous, so the phase of a bit's window
    follows from the bits before it (a tone that turns π per bit - 1800 Hz
    at 1200 bit/s, 1200 Hz at 2400 bit/s - flips everything after it, as
    does the correlator oscillator that turns π per bit). A 2-state Viterbi
    over that parity, each tone's expected correlator output learnt from the
    sync's 32 known bits (then following the blocks that check), is the
    matched filter. Then the tone reading (the balanced threshold of the
    preamble), then one error among the 4 least certain bits of either.
    Why: at 2400 bit/s (5 samples a bit) the tone reading took the last
    2400 Hz bit before a 1200 Hz run for a 1200 Hz one (the correlators'
    crosstalk) - the real sigidwiki recording decoded at 1 of 5 sampling
    phases, never through the plugin; coherently at every phase, and 2-4 dB
    more sensitive (Tests).
- **Tones**: every 5 ms a Hann-windowed FFT of the last 20 ms (512 points):
  the strongest tone 280-3300 Hz (parabolic interpolation) and its share of
  the 250-3400 Hz power, the eight DTMF frequencies. Frames with one tone
  holding ≥ 60 % at ≥ 150 Hz deviation make steady segments (±1.2 %); a
  segment's length = its frames' span + half a window. Segments without
  pauses (≤ 60 ms) form a sequence; the best run of 4+ that one standard
  reads is reported as soon as a later segment (voice) broke it, or when the
  tones stop; steady tones of 0.25 s+ on the burst list at once.

## Transmissions and talkers (`decoder.cpp`)

- One transmission at a time: opened by a tone / code (from its backdated
  start) or by an ID burst (from where the carrier came up, searched back in
  the carrier history - the start of a transmission whose ID came at the
  end), ended by the tone tracker / DCS, or, without a tone, by the carrier
  (below 0.4 of its level for 30 ms). A tone locking within 1 s of a burst's
  transmission joins it. Another radio's ID in the same carrier (a repeater
  output's continuous carrier, a capture without a tone change) starts a new
  one there.
- **Naming**, in this order: a radio's own ID (MDC1200 / FleetSync sender)
  names it at once; else the transmission waits **1.6 s** (a 5-tone / DTMF
  ANI is decoded ~1-1.5 s after the key-up) - or, once IDs were heard on the
  channel (radios with a post-ID: their ID comes at the end), **until its end,
  at most 4.2 s** (kraken_doa keeps DoA frames no span claims for 6 s) - and
  takes a selcall / DTMF sequence, else the tone / code, else a tone burst.
  A radio's ID decoded after it was named (a post-ID after the wait, or on a
  fresh channel's first transmission) takes over the spans not reported yet
  (the host closes the old talker there). An ID up to 0.8 s after the end
  still belongs to the ended transmission.
- Talker spans (`host.talker` / `talker_end`) CONFIRM_LAG 0.3 s behind the
  receiver, the end exact minus 20 ms (`END_GUARD`); the label = what was
  sent + the tone (`PTT ID · CTCSS 88.5`).
- Valid spans every update while the transmission is allowed by the squelch
  (`host.valid(start, end)`) → the VFO's **Digital squelch** opens on a tone,
  code or ID and the VFO's DoA is taken from exactly those samples;
  `kp::Info::squelch_hold_s` = 0.35 s.
- **ID / tone squelch** (option `squelch`: comma-separated entries - tones /
  codes `88.5`, `023N`, `D023N`, `047I` (spaces split them too, as before:
  `88.5 123.0`), IDs `MDC 1029`, `FleetSync 100-1001`, `CCIR 12345`, `DTMF
  123`, `Burst 1750`): only those transmissions get valid spans and a talker
  - with the squelch method "Digital" the VFO's audio and DoA follow only
  them. A transmission whose tone isn't listed waits for an ID until it ends
  (a post-ID then opens nothing in time - inherent). Others are still listed
  ("(squelch: ignored)").
- Table (row key = talker id: a row click shows its DoA): ID / tone, Type,
  Details (the last message / the measured tone), With (the IDs / tones heard
  in the same transmissions), Deviation, Tx, Last ("on air"), Air time,
  Ending; details ▸. Kept 24 h. Rows for every ID and tone a transmission
  carried, and for the radios a console addressed.
- Facts: On air, Last transmission, IDs / tones heard, Signalling decoded
  (counts), ID / tone squelch. Events: a new tone / code / radio (once), every
  ID message ("MDC 1029: PTT ID (at key-up) · CTCSS 88.5", "⚠ EMERGENCY ..."),
  every transmission's end ("MDC 1029 · 12.3 s · CTCSS 88.5 (88.47 Hz) · ID
  at release · reverse burst 179°"), the DCS turn-off code, other steady
  tones. Raw (Decoder Logging's raw frames): `MDC1200 op= arg= unit=`,
  `FleetSync 1200 <hex>`, `SELCALL <std> <digits>`, `DTMF <digits>`.
- Options: ID / tone squelch, Name transmissions by (`ids`: radio IDs first /
  the tone only - the old behaviour), Selcall standard (`selcall`), Weakest
  tone (`min_dev`), Non-standard tones (`nonstd`).

## ID separation

An MDC1200 unit ID and a FleetSync fleet-unit are the transmitting radio's
own: radios sharing a tone get separate bearings. Console commands name the
radio they address, not the transmitter - they name no talker. A 5-tone /
DTMF sequence at the start or end of a transmission is usually the sender's
ANI, but the same tones are also used to CALL a radio (sent by the caller):
treat a selcall / DTMF talker as "the transmitter that sent this sequence".
A CTCSS tone / DCS code is chosen per transmitter or group (users sharing a
tone are one talker); a repeater's OUTPUT carries the repeater's tone and
repeats the users' ID bursts - every ID's bearing there is the repeater's.
Transmissions, not packets: the DoA is computed from the MUSIC frames
entirely inside a transmission's span (kraken_doa's DoA per talker); the 📡
DF list (kind `tone`: 🆔 radio ID, 🔢 selcall / DTMF, 🔔 tone / code / burst;
"ID / tone · Signalling · tone · Bearing · Heard · Last · Air · Tx", newest
first) shows each one's count, last heard, the latest transmission's length
and the air time (TalkerInfo `dur_s` / `air_s`).

## Voice to Text

A VFO with "🗣 Transcribe" and this decoder: the decoder decodes no voice
(`kp::Info::voice` false), so Voice to Text takes the VFO's ANALOG demod as
without a decoder (`DigitalDecoder::takes_voice()`) and names the transcript
after the decoder's talker (`DigitalDecoder::lead_talker`: from = `MDC 1029`,
to = `PTT ID · CTCSS 88.5`; while a transmission is not named yet, the
transcript adopts the name it gets); another radio after it while the
squelch stayed open splits the transcript where the first one was last
heard.

## Tests

Generator `tools/analog_gen.py` (was `ctcss_gen.py`; the old options are
unchanged): one transmitter's FM baseband (cf32), keyed on / off (`--tx
A:B,...`), voice = real speech (`--speech`) or synthetic speech, through a
transmitter's audio chain, CTCSS (`--ctcss HZ`, `--tone-err`, `--tail`) or DCS
(`--dcs`, `--baud-err`), `--narrow`, `--deemph`, `--offset-hz`, `--cnr`; and
the ID signalling of every transmission (`--sig-at pre|post|both`, the voice
muted while it is sent, `--sig-dev`, `--sig-preemph`, `--sig-baud-err`):
`--mdc OP:ARG:UNIT[:E0E1E2E3]` (double packets), `--fleetsync FLEET-UNIT[:TO]
[:TYPE[:FLAGS]]` (`--fs-baud 2400`, `--fs2`), `--selcall STD:DIGITS` (repeats
→ E), `--dtmf DIGITS[:ON:OFF]`, `--burst HZ[:MS]`; `--audio-u8` writes the
modulating audio at 8 kHz for the reference decoders. Several transmitters at
bearings: `plugins/sdk/tools/uca_scene.py` ("at": 0).

Offline diagnostics (`decoder --file`): `CTCSS_DUMP=1` (tone / code
locks), `ANALOG_ID_DUMP=1` (sync hits, every FFSK block read coherently and
by tone, tone segments, selcall / DTMF candidates), `ANALOG_ID_AUDIO=<file>`
(the 12 kHz audio the signalling decoders get, float32 in Hz).

Results (2026-10-22/23, job scratch, this PC; not in the repo):

| test | result |
|---|---|
| the generator against Kaufman's GPL decoders (built in the scratch as the oracle, 8 kHz audio) | every MDC1200 packet (single, double 35:89:5678:830D1234, emergency, console 63:85) and every FleetSync message (1200 / 2400 bit/s, another fleet, FleetSync II) decoded to the bytes it was given |
| 40 synthetic cases (scratch suite.py: talker id + span vs truth) | 40 / 40 - MDC pre / post / both with CTCSS, DCS and no tone, double packets, console commands (no talker), pre-emphasised, de-emphasised, narrow, ±0.3 % bit rate, carrier 2 kHz off; FleetSync 1200 / 2400 / II / status / pre-emphasised; ZVEI1 / ZVEI2 / DZVEI (with the option; auto = the ZVEI reading) / CCIR1 / CCIR2 / EEA / EIA / NATEL, pre and post; DTMF pre / post / 40 ms; 1750 Hz bursts; tone only; carrier only (no talker). Spans: start within 2 ms, end within 21 ms (END_GUARD) |
| weak IDs: 10 transmissions of 0.8 s, the ID at key-up, CNR in 16 kHz (scratch sens.py; coherent / tone reading alone) | FleetSync 1200: 4 dB 5 / 2, 6 dB 10 / 7, 8 dB+ 10 / 10; FleetSync 2400: 4 dB 2 / 0, 6 dB 7 / 3, 8 dB+ 10 / 10; FleetSync II 2400: 6 dB 6 / 0, 8 dB 10 / 4, 10 dB 10 / 7, 12 dB 10 / 9; FleetSync 2400 pre-emphasised: 6 dB 6 / 1, 8 dB 9 / 6, 10 dB 10 / 9; MDC1200 (always coherent + its code): 10 / 10 from 4 dB. No wrong ID at any level |
| CTCSS / DCS regression, old `ctcss` vs `analog_id` (24 tones, 12 codes, reverse 120°, tone off, no tail, ±0.75 %, narrow CNR 8, CNR 4, no TX high-pass, de-emphasis, DCS ±2 %, DCS narrow CNR 4) | 46 / 46: the same talkers, starts / ends within 2 ms, the same end reasons |
| false alarms: 5 × 600 s, every 10 s a transmission of synthetic voice only (normal, no TX high-pass, narrow CNR 20, CTCSS + 4.5 kHz voice) + noise | 2 false events in 50 min of voice (a DTMF "444", a lone "1" - voice holding two harmonics on DTMF tones); noise 0 |
| scene `analog-ids-six` (offline, 5-element UCA r 0.12 m, SNR 20) | MDC 1234 60.0°, MDC 2002 200.0° (both on CTCSS 88.5 - apart by their IDs), FleetSync 100-1001 300.0°, CCIR 12345 140.0°, DTMF 4455* 20.0°, Burst 1750 250.0° (truth exact); squelch `MDC 1234`: only it talks, valid frames 5.2 s = its two transmissions; `FleetSync 100-1001, 123.0`: the FleetSync radio and the DTMF transmission (on CTCSS 123.0) |
| scene `ctcss-four-tones` | 60.0 / 200.0 / 300.0 / 144.4° (as before) |
| kraken_doa's engine (tools/digi_test, `PLUGIN:ctcss`) | runs as analog_id at 24 kHz, the table / details / facts in the status JSON |
| CPU (this PC, Ryzen 9 9950X3D) | 0.5 % of real time offline (was 0.6 % on the Pi 5 for CTCSS / DCS alone; not yet measured on the Pi) |

Real recordings (downloaded to the job scratch, FM-modulated again as IQ;
not in the repo):

| recording | plugin |
|---|---|
| Wikipedia "Trucking leading PTT ID MDC1200 (non-Motorola receiver)" (44.1 kHz audio, 41 s) | MDC 1029 PTT ID at 0.56, 7.30 and 30.99 s = Kaufman's decoder (op 01 arg 00 - at key-up: arg 00, see *Pre-ID / post-ID*); the same recording through a Motorola receiver (data muted): nothing, like the reference |
| sigidwiki "FleetSync 1200bd" | 167-1002 ANI at the start (flags FE) and the end (FC: end of transmission) = the reference; also its CTCSS 167.9 Hz and reverse burst |
| sigidwiki "FleetSync 2400bd" | 167-1002 ANI at the start (FE) and the end (FC), 2400 bit/s, at every sampling phase (the audio shifted by 0.2 .. 2.5 samples) - the reference decodes nothing, the tone reading alone 1 phase in 5 |
| sigidwiki "FleetSync Databurst", "Preamble" | nothing (the reference neither) |
| an off-air 467.55 MHz recording "idling on 12345" (sigidwiki ZVEI page) | ZVEI 12345 every time |
| sigidwiki "CCIR_04221" | CCIR 04221 (CCIR-2) |
| sigidwiki CCIR-1 / CCIR-7 (= CCIR-2) / PCCIR / ZVEI-1 samples | 91672 at 100 / 70 ms, PCCIR 01234, ZVEI 91672 - the author's test numbers |
| sigidwiki "ZVEI-2", "ZVEI-3", "DZVEI", "PDZVEI" samples | read as ZVEI 88561 (or DZVEI **91672**), 99123 (or DZVEI **01234**) ...: that program's "ZVEI-2" uses the DZVEI tones - the ambiguity above; with the Selcall standard option = DZVEI they read as the author's numbers |
| sigidwiki DTMF sample (78 s) | 0123456789ABCD*# complete, every time; the dialled numbers in it |
| sigidwiki "Whelen 40/20" (461PPPP333; tones 1-1.6 % low, high group 6-10 dB down) | DTMF 461####333 |
| sigidwiki "page normal 50/50" | DTMF \*C#4D34\*\* - what the file contains (its tones, measured independently), not its name |
| sigidwiki "Genave superfast 20/20, 20/05" | 6 of 10 digits (20 ms tones: below the 4-frame digit) |
| sigidwiki "Motorola rate 250ms" | its 990 / 1975 Hz pairs are no DTMF: nothing but a "1" and "47" |

The CTCSS / DCS results from before (2026-10-15/16, unchanged code):

| test | result |
|---|---|
| every CTCSS tone (51), synthetic voice, CNR 25 dB, tone error ±0.5 % | 51 / 51 right; lock after the onset median 190 ms (max 690 ms with voice in the tone band); start +1 ms, end (reverse burst) found +1 ms (the talker span -19 ms: `END_GUARD`); measured frequency within 0.06 Hz (mean -0.002 Hz) |
| every DCS code (104) normal + inverted (208), turn-off code | 208 / 208 named as the waveform reads (047I = 023N), no CTCSS talker; lock median 360 ms (max 520); start +1 ms, end +24 ms mean |
| encoder tolerances | 103.5 ±0.75 %, 69.3 (between 67.0 / 71.9), 150 Hz deviation, de-emphasised chain, real speech without TX high-pass, carrier 3 kHz off, DCS ±2 % bit rate, 627 (longest runs), 754I with carrier 2 kHz off: all right; non-standard 140.0: 4 of 5 |
| ends | reverse burst 180° / 120° (also at CNR 12, 67.0 and 250.3 Hz), tone off before the carrier, carrier off, DCS turn-off / carrier off: all at the right place (±1-3 ms CTCSS, +6-16 ms DCS), the right reason |
| back to back / capture / re-key | gaps 0.3 / 0.1 s: each from its onset to its end; 151.4 capturing 88.5 at +6 dB: 88.5 ends at the capturing carrier's rise; the same tone keyed twice 0.3 s apart: two transmissions |
| false alarms: no tone anywhere, 300 s each | noise only 0; synthetic voice 0; real speech without TX high-pass 0; weak carrier CNR 6 + voice 1; carrier keyed every 2 s with voice from the first moment 1 |
| weak signals, 10 × 3 s, CNR in 16 kHz | CTCSS 88.5 (600 Hz deviation): 10 / 10 from 2 dB; 192.8 narrow (350 Hz): 10 / 10 from 4 dB; DCS 023N 10 / 10 from 2 dB |

Real CTCSS / DCS recordings: sigidwiki "CTCSS" IQ (482.768 MHz): CTCSS 173.8,
173.79 Hz, two transmissions each ending in a reverse burst; sigidwiki's
"DCS" IQ is a 150 bit/s trunking code (nothing - correct). Live (Auckland,
2026-10-15): no CTCSS / DCS user caught; two false alarms (256 Hz at 5.7 kHz
deviation, a 33.33 Hz-line data carrier) fixed by the deviation limit and the
periodic-family rule.

## Sources

- ETSI TS 103 236 V1.1.1 (2014-01): CTCSS and DCSS signalling system.
- Wikipedia "Continuous Tone-Coded Squelch System" (non-standard tones,
  reverse burst); RadioReference wiki "DCS"; yo3iiu.ro (the turn-off tone).
- Midian Electronics, "Tone signaling charts" (5 / 6 / 7-tone tables, burst
  tones, DTMF) - via sigidwiki.
- Matthew Kaufman, mdc-encode-decode and fsync-encode-decode (GPL-2.0;
  russinnes/fsync-mdc1200-decode): read for the MDC1200 / FleetSync formats
  and built in the job scratch as a test oracle - no code of them is used.
  (A licence audit of 2026-10-23 found the first FleetSync block check and
  generator - the plugin's own lines - restating that decoder's CRC routine
  too closely; both were rewritten as the MPT 1327 codeword check above, and
  the payload / FleetSync II assembly with arithmetic of their own.)
- MPT 1327 (the codeword format: BCH(63,48) + parity) - as in the `mpt1327`
  plugin.
- OneOfEleven uv-k5-firmware-custom `mdc1200.h` (Apache-2.0): the MDC1200
  opcode list. sdrtrunk (GPL-3.0) MDCMessage / Fleetsync2Message: field names
  and FleetSync flags (documentation only).
- sigidwiki.com and Wikipedia Commons: the recordings above.
