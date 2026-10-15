# ctcss - CTCSS / DCS tones (analog FM)

The sub-audible signalling under an analog FM transmission: a CTCSS tone
(67-254 Hz, "PL", "tone squelch") or a DCS code (a 23-bit Golay word at
134.4 bit/s, "DPL", "digital code squelch"). Repeaters and users set one so
that receivers open only for them - so on a channel several repeaters /
users share, the tone or code is the transmitter's ID. The plugin finds it
in every transmission, measures it, and makes each transmission a talker
"CTCSS 88.5" / "DCS 023N" with its exact span: every tone gets its own DoA
lobe on the 🗺 Map's 📡 DF panel (kind 🔔 tone), even with several
transmitters taking turns on one frequency.

Pick "CTCSS / DCS tones (analog FM)" as a VFO's digital decoder, on a
12-24 kHz VFO on the FM channel (it takes 24 kHz; Auto detect may pick it
too: it reports nothing without a tone). Listening keeps working - the VFO's
Demod (NBFM) is untouched; Voice to Text transcribes the analog audio and
names each transcript after the tone (below).

## What it reads

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
  the table's "Measured"), the deviation in Hz. Encoders are within ±0.75 %:
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

Not decoded: DTMF, 5-tone / selcall (ZVEI, CCIR, EEA), MDC1200, 1750 Hz
tone bursts, 2-tone paging - possible follow-ups (all audible-band
signalling, a separate detector each). Nothing here is encrypted or
addressed: the tone / code is a plain broadcast identifier.

## How (`rx.cpp`)

- Front end (24 kHz complex in): 41-tap channel filter (±7.5 kHz), FM
  discriminator in Hz, 24 kHz → 2400 → 1200 Hz (Blackman FIRs, flat to ~350
  Hz, the voice band kept away from 0-300 Hz). Two streams: high-passed (45
  Hz, the carrier offset out) for CTCSS, low-passed (130-290 Hz) for DCS;
  plus the carrier power (the channel filter's |y|²), aligned with them.
- CTCSS detection: every 50 ms a Hann-windowed FFT (2048 points, 0.59 Hz
  bins) of the last 0.3 s - or only what follows a carrier's onset inside
  it (the noise before it would hide a new tone for a whole window); a
  candidate = the strongest peak 60-257 Hz ≥ 13 dB above the band's median,
  holding ≥ 55 % of the 55-275 Hz power (one pure tone - not voice, noise or
  DCS's line spectrum), with ≥ the "Weakest tone" deviation (80 Hz) and ≤
  1.5 kHz (a CTCSS tone is 300-800 Hz; one at several kHz is the whole
  modulation - seen live: 256 Hz at 5.7 kHz on 157.9 MHz), and not one line
  of a periodic signal's family (lines at f ± f/k, k = 2..10, within 15 dB:
  seen live, a 151.7 / 162.65 MHz data carrier with lines every 33.33 Hz -
  30 ms TDMA slots - whose 200 Hz line otherwise passed for 199.5). A tone =
  3 consecutive candidates (2 when strong) that agree in frequency to the
  measurement's precision (0.12-0.8 Hz by SNR: a tone is crystal-stable, a
  voice harmonic drifts) and in amplitude (±30 %: a tone's deviation is
  constant, voice comes in syllables), and whose phase advances evenly over
  the last 0.2 s (residual < 0.12 rad or 3x the noise's). Off the lists: one
  candidate more and ≥ 70 % purity; near 67.2 Hz (half DCS's bit rate - a
  code's 1010 runs, next to the 67.0 tone) 5 more, so a DCS code locks first.
  134.4 Hz (DCS's bit rate, between 131.8 and 136.5) is never a tone.
- CTCSS tracking: the tone mixed down, a 97-tap 35 Hz low-pass, every 10
  ms: amplitude, phase (→ the frequency; "Measured" = the tone's whole phase
  advance over the transmission / its time - a reverse burst's step taken
  back out -, checked against the median of the ticks' frequencies), the end (amplitude
  below half, or the carrier gone), a reverse burst (the phase turning > 100°
  with the amplitude and carrier there - confirmed when the tone or carrier
  ends within 0.5 s, else a glitch: the transmission goes on). Another
  tone dominating the FFT for 3 hops = another transmitter. The start is
  found back in the stream: the carrier's onset, or (a tone switching on a
  carrier already there) where the tracker's amplitude rose through half.
- DCS: zero-crossing clock recovery with a rate loop (encoders ±2 %; the
  rate leaks back to nominal while searching), decision-directed levels
  (TX droop, carrier offset), hard bits. A code = the last 46 bits
  23-periodic without an error and a Golay codeword (noise: ~1 false lock
  per 4 years), or 69 bits whose 3-word majority is a codeword with ≤ 5 bits
  disagreeing (weak signals). Locked: every bit checked against the code;
  > 6 wrong of the last 23 = the end, at the last clean bit. The start: the
  carrier's onset if near, else back in the bits while 8-bit windows hold ≤
  1 error. No DCS search while a CTCSS tone is locked (a sampled sine can be
  made 23-periodic by the clock loop's ±3 %).

## Output

- Talkers (`kp::Host::talker` / `talker_end`, the transmission's exact
  span, ending 20 ms before the end found - `END_GUARD`, what follows may
  be another transmitter -, 40 ms guard on the host side): id `CTCSS 88.5` / `DCS 023N`, label
  the measured frequency (`88.47 Hz`) / the equivalent (`023N = 047I`).
- Valid spans every 50 ms while the tone / code is there (`host.valid(start,
  end)`) → the VFO's **Digital squelch** opens only on a tone / code and the
  VFO's DoA is taken from exactly those samples; `kp::Info::squelch_hold_s`
  = 0.35 s (the host's default 1.5 s would pass 1.5 s of noise after every
  transmission).
- **Tone squelch** (option `squelch`: `88.5`, `023N`, `D023N`, `047I` ...;
  several comma-separated; empty = any): only that tone / code gets valid
  spans and a talker - with the squelch method "Digital" the VFO's audio and
  DoA follow only that transmitter, like a radio's tone squelch. Others are
  still listed (table, events, "(tone squelch: ignored)").
- Table (row key = talker id: a row click shows its DoA): Tone / code,
  Type (CTCSS / DCS, non-standard), Measured, Deviation, Tx, Last (length;
  "on air"), Air time, Ending; details ▸. Kept 24 h.
- Facts: On air, Last transmission, Tones / codes heard, Tone squelch.
  Events: a new tone / code (once), every transmission's end ("CTCSS 88.5
  Hz · 12.3 s · 88.47 Hz · reverse burst 179°"), the DCS turn-off code.
- Options: Tone squelch, Weakest tone (40 / 80 / 150 / 250 Hz deviation),
  Non-standard tones (on / off).

## ID separation

A tone / code is chosen per transmitter (repeater, radio), so it names the
RF transmitter of the samples - with one caution: a repeater's OUTPUT
carries the repeater's tone (often the input's, regenerated) at the
repeater's bearing. On a repeater input or a simplex channel each user's
tone is theirs; users sharing one tone are one talker. Transmissions, not
packets: the DoA is computed from the MUSIC frames entirely inside a
transmission's span (kraken_doa's DoA per talker), the latest transmission's
bearing + a history per tone; the 📡 DF list (kind `tone` 🔔, "Tone / code /
Measured / Bearing / Heard / Last / Air / Tx", newest first) shows each
tone's count, last heard, the latest transmission's length and the air
time (TalkerInfo `dur_s` / `air_s`, status fields `dur` / `air`).

## Voice to Text

A VFO with "🗣 Transcribe" and this decoder: the decoder decodes no voice
(`kp::Info::voice` false), so Voice to Text takes the VFO's ANALOG demod as
without a decoder (`DigitalDecoder::takes_voice()`; Auto detect still
searching counts as voice when any of its plugins decodes voice) and names
the transcript after the decoder's talker (`DigitalDecoder::lead_talker`:
from = `CTCSS 88.5`, to = `88.47 Hz`); another tone after it while the
squelch stayed open splits the transcript where the first one was last
heard. The digital voice path is unchanged.

## Tests

Generator `tools/ctcss_gen.py`: one transmitter's FM baseband (cf32),
keyed on / off (`--tx A:B,...`), voice = real speech (`--speech a.wav`,
looped) or synthetic speech (glottal pulses through moving formants, pitch
90-220 Hz, syllables and pauses), through a transmitter's audio chain
(pre-emphasis, 300 Hz 6th-order high-pass - `--no-hpf`: none, the
false-tone worst case -, 3 kHz low-pass, limiter), CTCSS (`--ctcss HZ`,
`--tone-err PCT`, `--tail reverse|reverse120|off|none`) or DCS (`--dcs
023N|047I`, real Golay words, 250 Hz encoder filter, slight droop,
`--baud-err PCT`, `--tail turnoff|none`), `--narrow` (12.5 kHz deviations),
`--deemph` (a repeater's de-emphasised chain), `--offset-hz`, `--cnr DB`
(noise, CNR in 16 kHz). Several transmitters at bearings:
`plugins/sdk/tools/uca_scene.py` ("at": 0 - each file keeps its timing).

Offline (`decoder --file`, generated files, scratch suite):

| test | result |
|---|---|
| every CTCSS tone (51), synthetic voice, CNR 25 dB, tone error ±0.5 % | 51 / 51 right; lock after the onset median 190 ms (max 690 ms with voice in the tone band); start +1 ms, end (reverse burst) found +1 ms (the talker span -19 ms: `END_GUARD`); measured frequency within 0.06 Hz (mean -0.002 Hz) |
| every DCS code (104) normal + inverted (208), turn-off code | 208 / 208 named as the waveform reads (047I = 023N), no CTCSS talker; lock median 360 ms (max 520); start +1 ms, end +24 ms mean (the turn-off tone is the same transmitter's) |
| encoder tolerances | 103.5 ±0.75 %, 69.3 (between 67.0 / 71.9), 150 Hz deviation, de-emphasised chain, real speech without TX high-pass, carrier 3 kHz off, DCS ±2 % bit rate, 627 (longest runs), 754I with carrier 2 kHz off: all right; non-standard 140.0: 4 of 5 (one read as 141.3 - 0.92 % away, inside the listed tone's tolerance at lock) |
| ends | reverse burst 180° / 120° (also at CNR 12, 67.0 and 250.3 Hz), tone off before the carrier, carrier off, DCS turn-off / carrier off: all at the right place (±1-3 ms CTCSS, +6-16 ms DCS), the right reason |
| back to back / capture / re-key | gaps 0.3 / 0.1 s: 88.5 / 123.0 / 023N each from its onset to its end (±1 ms; DCS +28 ms); 151.4 capturing 88.5 at +6 dB: 88.5 ends at the capturing carrier's rise, 151.4 from there, 88.5 again after it; the same tone keyed twice 0.3 s apart: two transmissions |
| false alarms: no tone anywhere, 300 s each | noise only 0; synthetic voice (CNR 20; no TX high-pass CNR 30; narrow no high-pass CNR 12) 0; real speech (JFK, Apollo 13, a two-way) without TX high-pass 0, de-emphasised 0; weak carrier CNR 6 + voice 1; carrier keyed every 2 s with voice from the first moment 1 - 2 false transmissions in 40 min |
| weak signals, 10 × 3 s, CNR in 16 kHz | CTCSS 88.5 (600 Hz deviation): 10 / 10 from 2 dB; 192.8 narrow (350 Hz): 6 / 10 at 2 dB, 10 / 10 from 4 dB; DCS 023N 10 / 10 from 2 dB; 754N narrow 10 / 10 from 2 dB |

ID separation: a 30 s scene on 145.05 MHz, 5-element UCA r 0.12 m (tools/ctcss_gen.py + uca_scene.py, real speech, SNR 20): 88.5 at 60° (two transmissions), 123.0 at 200° (two; the second one captured), DCS 023N at 300° (two), 151.4 at 140° +6 dB capturing 123.0's second transmission for 2 s. Offline `--channels 5 --uca 0.12`: 60.0 / 200.0 / 300.0°, 151.4 146.7° (its first 2 s hold both carriers; MUSIC with one source). End to end (kraken_doa from a scratch cwd, fake heimdall streaming the scene at 2.4 MHz): 60.0 / 200.0 / 300.0 / 146.0°, every transmission in the history. Tone squelch 123.0 + Digital squelch: the only talker CTCSS 123.0 (200.0°), the squelch open 26 % of the time (its duty: 26 %), the VFO's published DoA 200.0° throughout (before the confirmation lag and the capture-rise end it showed 190° for a moment at the capture). Voice to Text (Transcribe on, squelch off): 17 analog transcripts, each named after its tone ("CTCSS 88.5" / "88.50 Hz", DCS "023N = 047I"), split where the tone changed; the digital path (job 27e7efb6's voicetest plugin) unchanged (1234567 11.0 s, 7654321 split, "?" -> 5550001).

Live (Auckland, 2026-10-15, real heimdall + a scratch kraken_doa, VFOs
moved onto every 12.5 kHz channel that came on): ~1.5 h over 145.35-147.75,
150-174, 437.8-440.2, 444.9-447.3 and 475.7-478.1 MHz - no CTCSS / DCS user
caught (the channels heard were digital, tone-less or data). Two live false
alarms, both fixed and covered by the rules above: 256 Hz at 5.7 kHz of
deviation on 157.925 / 157.950 MHz ("254.1"; now > 1.5 kHz = not a tone),
and a 151.7 / 162.65 MHz data carrier read as "199.5" (200.000 Hz by its
phase, with lines at every multiple of 33.33 Hz - 100, 133.3, 166.7, 233.3,
266.7, 300 Hz - a periodic frame, not a tone; now the periodic-family rule:
its 160 s capture gives no talker).

CPU (Pi 5): 0.6-0.7 % of a core live (24 kHz in; offline 0.6 % of real time).

## Sources

- ETSI TS 103 236 V1.1.1 (2014-01): CTCSS and DCSS signalling system - the
  tone table, the DCS codeword structure, polarity, the 83 codes' bit
  patterns, encoder tolerances (±0.75 %, ±2 %), deviation (300-800 Hz),
  decoder response times (CTCSS < 250 ms, DCS < 350 ms).
- Wikipedia "Continuous Tone-Coded Squelch System": the non-standard
  tones, reverse burst (180° at first, 120-135° later), the 134.4 Hz
  interaction with 131.8 / 136.5 Hz decoders.
- RadioReference wiki "DCS": the normal / inverted code pairs (023 = 047,
  025 = 244 ...), reproduced from the ETSI patterns.
- yo3iiu.ro "Digital Coded Squelch codec": the 134.4 Hz turn-off tone.
- The 104-code DCS list amateur / PMR radios and their programming tools
  offer (the 83 + 21).
