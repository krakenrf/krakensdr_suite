# 406 MHz distress beacons (`beacon406`)

Cospas-Sarsat 406 MHz distress beacons - EPIRB (ships), ELT (aircraft), PLB
(personal), SSAS (ship security alerts) - on every 406.0-406.1 MHz channel
at once, from the public specifications C/S T.001 (first generation) and
C/S T.018 (second generation, detected only).

**A heard DISTRESS beacon must be reported to the rescue coordination
centre** (New Zealand: RCCNZ, 24 h 0508 472 269 = 0508 4 RCCNZ, from abroad
+64 4 577 8030 - maritimenz.govt.nz; elsewhere the national RCC) with its 15-hex ID,
time, frequency and bearing / position. The decoder reports nothing to
anyone itself, and a single receiver can't tell a real distress from an
inadvertent activation - that is the RCC's job (it has the beacon
registrations).

## What it decodes

Picking the decoder tunes the VFO to 406.050 MHz at 100 kHz (`fixed_freq_hz`,
manual only). Every burst on any channel (T.012 raster 406.022 + 3 kHz:
channel letters A-S, in use B, C, D, F, G, S) is found and decoded:

- **Distress vs test, unmistakable**: a burst with the SELF-TEST frame sync
  (011010000) is a test - event "Self-test burst (NOT an alert)", status
  `self-test`. A burst with the normal frame sync (000101111) is a distress
  alert: event "⚠ DISTRESS BEACON: ... - report it to the rescue
  coordination centre" (again every 5 min while it keeps transmitting),
  table status `⚠ DISTRESS`, talker / map label starting with ⚠, the fact
  "⚠ Distress". Ship security alerts: "⚠ SHIP SECURITY ALERT". Test
  protocols (test user, standard / national / RLS / ELT(DT) location test)
  and orbitography (reference) beacons are named as such, never as alerts.
  An ELT(DT) cancellation message is shown as a cancellation
- **Checks**: bit sync (15 ones, <= 2 wrong) + frame sync (<= 1 wrong), then
  BCH-1 (82,61) over bits 25-106 - up to 3 errors corrected - and BCH-2
  (38,26) over bits 107-144 - up to 2 (syndrome tables, `bch.hpp`). A burst
  failing BCH-1 shows nothing (verbose: a line). A failing BCH-2 drops only
  the PDF-2 fields (fine position, homing). A long-message self-test that
  stops after bit 112 (allowed by T.001 Rev.13) is recognised
- **Fields** (`cs406.cpp`, T.001 Annex A): format / protocol flags, country
  (MID, `mid.hpp`), protocol, beacon type, the **15-hex beacon ID** (bits
  26-85 with the position bits at their defaults), and per protocol:
  - user protocols (short / long): maritime (MMSI last 6 digits or call sign,
    beacon number), radio call sign, aviation (aircraft registration, ELT
    number), serial user (ELT / float-free / non-float-free EPIRB / PLB
    serial, ELT 24-bit aircraft address, operator designator + serial, TAC),
    test user, orbitography, national user; the auxiliary radio-locating
    device (121.5 MHz homing, 9 GHz SART); short messages' non-protected
    bits 107-112 (activation, maritime / non-maritime emergency codes)
  - user-location: position to 4 minutes
  - standard location (EPIRB MMSI, ELT 24-bit address, ELT / EPIRB / PLB
    serial + TAC, ELT operator designator, SSAS, test): 1/4 degree coarse +
    offset to 4 s
  - national location (ELT / EPIRB / PLB / test): 2' coarse + offset to 4 s
  - RLS location (Galileo return link: RLM types accepted / acknowledged,
    provider, TAC series + serial or MMSI): 1/2 degree + offset
  - ELT(DT) location (distress tracking: identity by aircraft address /
    operator / TAC, activation, altitude band, fix age, operator in the
    rotating field, cancellation): 1/2 degree + offset
  - position source (internal / external navigation device), 121.5 MHz
    homing
- **The map**: an encoded position becomes a map point (ship / aircraft /
  person by type; label ⚠ for distress), popup with ID, type, country,
  identification, position encoding, homing, channel
- **Tracking over time**: per 15-hex ID - bursts (normal / self-test), the
  mean interval (first generation: 50 s +-5 %), the last 12 bursts with
  channel, C/N0 and the bits corrected (row details), position changes as
  events. Kept `keep_h` hours (default 24) after the last burst
- **Second generation** (T.018, DSSS OQPSK, 38.4 kchip/s per arm, 1 s
  bursts on 406.050 MHz): DETECTED only - a 0.9-1.15 s spread-spectrum
  burst around 406.050 MHz: the MEDIAN bin power of +-30 kHz (first-
  generation carriers don't move a median) up >= -5 dB over its floor on
  average, >= 3x the rise of the VFO's edges (>= 40 kHz away - a change of
  the receiver's own level raises both), flat over 8 sub-bands -> "⚠ Second-generation
  beacon burst ... not decoded: its ID and distress / test status are
  unknown", a table row and a talker "SGB" (every burst its own bearing).
  Its ID, test / normal PRN and message are NOT decoded (follow-up: the
  PRN is public - LFSR x^23 + x^18 + 1, T.018 Table 2.2 - so despreading,
  BCH(250,202) and the 23-hex ID are possible)
- **Raw frames** (decoder data log): `FFFE2F` / `FFFED0` + bits 25-144 in hex
  + C/N0 (+ "self-test")

Not done: 121.5 MHz homing (swept AM tone; a follow-up), the obsolete
short-format location protocols, orbitography contents (C/S T.006).

## ID separation

Each beacon's 15-hex ID is a `kp::Talker` packet per burst: its 160 ms
unmodulated carrier + the first bits (0.24 s, from the carrier's start found
by the frame sync) on its own channel (`freq_hz` = the measured carrier,
`bw_hz` 3 kHz - kraken_doa filters that channel out of every antenna, so
beacons bursting at the same moment on different channels keep their own
bearings), `avg_s` = option `doa_avg` (default 1 = each burst alone: a
moving search follows it; 5 / 30 min to average), `keep_s` = `keep_h`
(default 24 h). Label `⚠ EPIRB · New Zealand · MMSI ...123456` /
`self-test · PLB · ...`. The 📡 DF panel lists them as kind `sar` 🆘
(Beacon ID / Status · type · country · ID / Bursts, Dist + Δ pos for those
with an encoded position, newest first). DF is how beacons are found.

## Receiver

`rx.cpp`: 1024-point FFT every 512 samples (97.7 Hz bins), gated noise
floor per bin; a carrier = a local maximum >= 10 dB over its floor,
confirmed in 3 of its first 4 frames, followed +-4 bins while >= 7 dB; its
frequency from its strongest frame in the first 150 ms. Spurs of a stronger
burst (the biphase modulation's 400 Hz x odd harmonics) are ignored. Each
burst: 0.67 s of input from the ring, mixed to 0 Hz, 401-tap low-pass to
+-1.5 kHz (channels are 3 kHz apart), decimated to 10 kHz; the carrier
frequency from the burst's strongest line (16384-point FFT); bit + frame
sync found NON-coherently (sum of template x signal = A e^(j theta) j
sin(1.1) C - independent of the carrier phase), both sync patterns, bit rate
-1 / 0 / +1 %, twice: over all 24 sync bits (best on a weak, steady carrier)
and in 6 pieces of 4 bits with their magnitudes summed (a carrier whose
phase wanders); both peaks are demodulated when they disagree and the
cleaner sync wins. Carrier amplitude and C/N0 from 20 ms pieces of the 138
ms before the first bit (scatter around each piece's own mean), its phase
from the last piece; bits by integrate-and-dump of the two half-bits with a
timing loop on the mid-bit transition (+-1.5 % bit rate) and a
decision-free phase loop (a biphase bit's mean is the residual carrier, cos
1.1), then again with each bit's phase from the means of the 5 bits around
it (non-causal: a phase step costs <= 2 bits) - the more confident of the
two is kept. Normal / self-test from the decoded frame-sync bits.
Biphase-L sense as T.001 Fig 2.5 (1 = +1.1 rad first); the opposite is
accepted too (a mirrored input) - settled by the decoded 15-bit bit sync,
not by the carrier's phase. `B406_BITS=1` (offline) prints every
demodulated burst's soft bits. CPU (Pi 5): 0.6 % of a core on noise, 3 % during the
playback scene (5 transmitters).

## Tests

Generator `tools/beacon_gen.py` (real framing: bit / frame sync, BCH-1 /
BCH-2 per Annex B, every protocol above, positions with offsets, self-test
bursts, bit rate error, carrier offset, `--hex` for any message, `--sgb-at`
= a second-generation-shaped DSSS burst with random chips - its spectrum
only). `tools/scene_five.json` + `plugins/sdk/tools/uca_scene.py` = the
multi-beacon scene; `tools/testsignals.json` = the debug playback entry
`beacon406-five`.

- Published worked examples - T.001 Annex B1 and C/S G.005 Annex A
  examples 1-7, 10, 11 (serial user EPIRB, maritime MMSI, radio call sign,
  serial ELT, aviation with an emergency code, serial PLB, test user,
  serial user-location, standard location EPIRB MMSI, national location
  EPIRB) - modulated with `--hex` and decoded: every 15-hex ID and field as
  published (e.g. 2024F724E4FFBFF: MMSI ...506151, 43°43'56"N 0°11'12"E;
  20341500BF81FE0: national 10753, 43°43'00"N 0°01'00"E)
- Every protocol of the generator round-trips (17 cases incl. RLS, ELT(DT),
  SSAS, operator designators, call sign BCD)
- Sensitivity (10 long bursts each, C/N0 = SNR at 100 kHz + 50 dB):
  10 / 10 down to 34 dBHz (BCH corrected 1-3 bits in PDF-1, PDF-2 lost in
  2), 6-7 / 10 at 32 dBHz; bit rate +-1 %: 10 / 10 at 40 dBHz; 60 s of
  noise: no burst, no event
- Scene (5-element UCA r 0.12 m, 406.05 MHz): EPIRB 406.040 MHz at 40 deg
  and PLB 406.028 MHz at 130 deg bursting at the same moment, ELT ZK-OKA
  (short, +0.5 % bit rate) 220 deg, a PLB self-test 300 deg, an SGB burst
  260 deg: offline (`decoder --file scene.cf32 --channels 5 --uca 0.12`)
  40.0 / 130.0 / 220.0 / 300.0 / 260.0 deg; end to end through the live
  kraken_doa's debug playback: 5 / 5 talkers 0.0 deg off, the settings
  files byte-identical after
- Second-generation detection (the generator's DSSS bursts, rectangular
  chips): 2 / 2 from 0 down to -6 dB SNR at 100 kHz, 0 / 2 at -7 dB
- BCH: G.005 A10 with bits flipped - 3 in PDF-1 corrected (same ID), 4
  refused, 2 in PDF-2 corrected, 3 there = PDF-2 dropped, PDF-1 kept; a
  3-bit correction is only taken when all three bits were weak soft
  decisions (|soft| < 0.8 - else a 4-error burst could become a phantom ID);
  no wrong ID in any low-SNR run
- Real recordings (2026-10-16; verified against them, not in this repo):
  - sigidwiki "EPIRB" IQ (`Epirbsignal.zip`, SDR# 37.5 kS/s, 2015, one
    burst padded with noise and repeated): a New Zealand PLB, national
    location protocol, serial 13016, distress frame, ID 4016196C3F81FE0.
    The first version decoded PDF-1 but lost PDF-2 (BCH-2) in every copy:
    the recording's carrier wanders ~0.3 rad per bit and jumps by ~2.3 rad
    once in the carrier and twice in the data - the phase loop fell behind
    and the 138 ms carrier mean (across a jump) read C/N0 32 dBHz on a
    strong signal. An independent Python demodulator (each bit's phase
    from its own mean) gave both BCH checks clean at 399 bit/s; with the
    per-bit phase pass the plugin now decodes the same 144 bits, PDF-2
    included (C/N0 44 dBHz - the coherent figure, phase noise counted;
    63 from the amplitude alone). It is "inverted" (1 = -1.1 rad first) -
    a mirrored recording or receiver; T.001's sense is kept.
  - F4EHY's dec406 test set (`scan406_PI_V9.tar`, jgsenlis.free.fr:
    FM-discriminator WAVs, turned into IQ by integrating the audio; dec406
    itself built in scratch as the reference): the off-air
    `406discri_N42_39_16_E2_57_8` (a French standard-location test-protocol
    alert) - the first version refused it (its carrier phase, from the 160
    ms before, was > 90 deg off, so every bit came out inverted: bit sync
    15 / 15 wrong) - now decoded = dec406 (8E3E0425A7..., 42.65444 N
    2.95222 E = the file name), and three generated frames (national
    location EPIRB 10753, standard location MMSI 506153, serial user
    float-free EPIRB 506153 with 4' position) = dec406, hex and fields. Its
    ELT(DT) frame didn't survive the audio-to-IQ conversion (a filtered
    audio chain); its message through `beacon_gen.py --hex` decodes as
    dec406 does (aircraft 123456, 42.95444 N 1.36444 E, 800-1200 m,
    France). Two more off-air files (an ADRASEC exercise beacon, Lanester)
    neither decoder reads.
  - The generator matches the real bursts (+-1.0-1.1 rad, 160 ms carrier,
    bit rate 398-400) except the carrier's phase noise: `--phase-noise RAD`
    (random walk, rad rms per bit) adds it. Old vs new demodulator, 10
    bursts each: 0.15 rad/bit 40 / 34 dBHz 9 -> 10 / 7 -> 9; 0.3: 6 -> 9
    / 4 -> 7; 0.45: 3 -> 7 / 0 -> 6; without phase noise unchanged (36 /
    34 dBHz 10 / 10; 32 dBHz 19 vs 21 of 40 over 4 seeds), +-1 % bit rate
    10 / 10, 60 s of noise nothing, the published examples and the scene
    (40.0 / 130.0 / 220.0 / 300.0 / 260.0 deg) as before
- Live (Auckland, 2026-10-15 15:17-15:33, heimdall retuned to 405.75 MHz via
  its control port, settings restored): ~9 min at 406.0-406.1 MHz, nothing
  heard (no beacon, no self-test). The first version's second-generation
  detector made 6 false detections in 1.5 min of it - the band's level
  moving with the whole receiver ("8/8 sub-bands lit") -, hence the band
  medians + the edge reference + 1.0 s; 3 min kept and re-decoded with
  the final version: nothing

## Sources

C/S T.001 Issue 4 Rev.13 (Oct 2025) "Specification for Cospas-Sarsat 406
MHz Distress Beacons" (bit layouts, BCH, Baudot, modulation, timing), C/S
T.012 (channel plan), C/S T.018 Issue 1 Rev.13 (second generation), C/S
G.005 Issue 3 Rev.2 (beacon coding guidance, worked examples) - all from
cospas-sarsat.int; ITU MID list (country codes). No third-party decoder
code used.
