# ssr1030 - SSR / Mode S / TCAS interrogations (1030 MHz)

The 1030 MHz UPLINK: the interrogations that ground radars (SSR, Mode S),
fixed interrogators (multilateration / WAM) and aircraft TCAS / ACAS send -
the questions the 1090 MHz replies (the `adsb` plugin) answer. The plugin
reads every interrogation in the 2.4 MHz around 1030 MHz, tells the
interrogators apart and gives each one its own bearing.

Pick "SSR / Mode S / TCAS interrogations (1030 MHz)" as a VFO's digital
decoder: like `adsb` it tunes the VFO and its tuner to 1030 MHz at 2.4 MHz
(`kp::Info::fixed_freq_hz`), the VFO a locked line; manual only.

## What it reads

- **Mode A / C** (and B / D) interrogations: P1 + P3 0.8 µs pulses 8 / 21
  (17 / 25) µs apart; **P2** (SLS, from the radar's omni control antenna) 2
  µs after P1 and its level against P1 - ≥ 9 dB below in the main beam,
  above it in the sidelobes; **P4** 2 µs after P3 - short (0.8 µs) = Mode
  A/C-only all-call, long (1.6 µs) = Mode A/C/S all-call (intermode); **S1**
  2 µs before P1 = ACAS whisper-shout suppression pulse.
- **Mode S** interrogations: P1 + P2 (preamble) + P6, 16.25 / 30.25 µs of
  DPSK = 56 / 112 bits at 4 Mchip/s, with the sync phase reversal 1.25 µs
  into P6. The bits ARE read (below): UF0 (short air-air / ACAS), UF4 / 5
  (surveillance, altitude / identity request), UF11 (Mode S-only all-call),
  UF16 (long air-air: ACAS coordination / broadcast), UF20 / 21 (Comm-A),
  UF24 (Comm-C ELM), each field shown (PC, RR, DI, SD, IIS / SIS, PR, IC /
  CL, RL, AQ, UDS, MA / MU ...).
- **Address / parity**: CRC-24 (0xFFF409) of the first 32 / 88 bits XOR the
  address - ENCODED for the uplink: the address bits are the coefficients
  x^47 .. x^24 of A(x)·G(x) (Annex 10's uplink AP; checked on the live
  signals: every UF11 all-call gives the encoded all-ones address, the
  discrete ones the addresses of aircraft heard on 1090 MHz). An all-call
  (UF11) or broadcast (UF16, address FFFFFF) can be **checked** - and
  corrected: one wrong run of chips (two bit errors 1-3 bits apart, or one
  in the last 8) is fixed against the known address. A discrete
  interrogation's AP just says whom it was sent to: no check is possible.
- **Interrogator code**: the II (0-15) or SI (1-63) code of a radar from
  its checked UF11 all-calls (IC + CL), and the IIS / SIS field of its
  roll-calls.
- **ACAS address**: UF16 broadcasts (UDS 0x32) carry the sending aircraft's
  own address (MU bits 65-88); coordination messages its MID.

## Mode S at 2.4 MHz (dpsk.cpp)

The chips are 0.25 µs - 0.6 samples per chip - and the receiver passes
only about ±1.1 MHz of the ±4 MHz they occupy. Chip by chip they can't be
read; as a whole they can. The interrogation is a known waveform up to its
first data chip (P1, P2, P6 with the sync reversal), and every sample is a
known LINEAR function of the chip signs through the receiver's response:
an edge of the transmitted envelope with jump e at time u adds e·S(t − u),
S = the receiver's complex STEP response (R820T + RTL2832 filters, plus
what arrives with the edge). So:

1. P6's timing (12.5 ns grid) and the complex amplitudes of P1, P2, P6 by
   least squares on the known part;
2. a maximum-likelihood sequence estimator over the chips (Viterbi, 32
   states = the last 5 chips; each survivor carries its own chip history,
   so the response's long tail - up to 5 µs - comes from the path itself);
3. the carrier offset from the decoded frame's phase drift, then 2. again.

S is a table measured on the live interrogations here (`s_default.h`:
−1 .. 4 µs, 1/32 µs; least squares over the parity-checked frames) and is
re-fitted while running from every frame that checks (UF11 / broadcasts:
the whole waveform with its chips, slowly forgetting; the known part of
strong frames adds a little). Live (30 s, ~50 Mode S / s): ~55 % of the
discrete interrogations give an address that repeats (real aircraft - the
same ones the adsb plugin hears), the rest have a bit error or two; the
SSR's UF11 all-calls check in its main beam. A **target** is listed after
3 interrogations to the same address.

Without a response close to the receiver's the bits are lost: on synthetic
signals through a different filter (a windowed-sinc low-pass) nothing
checked, though the same frames decode 100 % when the decoder is given
that filter's response - the known part of the frames alone did not pin a
different response down. The plugin is tuned to the KrakenSDR's receiver;
the interrogators, their structure and bearings don't depend on the bits.

## ID separation (interrogators)

Every interrogator is a talker - its own DoA lobe and history on the 🗺
Map's 📡 DF panel (kind `interrogator`: 🛰 ground / ✈ ACAS, "Interrogator /
Type · scan · PRF / Interr.", listed by name). Which transmitter sent an
interrogation (track.cpp):

- **SSR** (rotating secondary radar): its all-calls form a PERIODIC train
  (PRF 50-500 Hz, often jittered a few %): sequence search over the
  unattributed all-calls (the PRI most pairs share, chains at multiples of
  it), then each new all-call within the train's own PRI spread (√n for n
  missed slots) and level. Between main beams the train is lost and found
  again; its sidelobe interrogations (P2 above P1) are its by their **P2
  level** - the omni control antenna arrives at one level all round.
  Named by its code: **"SSR II 10"** / "SSR SI 23" (from its checked UF11 -
  roll-calls whose IIS / SIS equals that code are then its too), else "SSR
  #n". Its DoA: only the MAIN BEAM (within `doa_db`, 10 dB, of the scan's
  peak, unclipped) - straight from the radar; after its second pass.
- **Fixed** (non-rotating ground interrogator - multilateration / WAM, a
  test transponder): not rotating, so it arrives at ONE level: three bursts
  at one level (±3 dB) within 15 s make one, then everything at that level
  that isn't an SSR's (UF11, P2 as strong as P1) or an ACAS's (S1, UF16) is
  its. "Fixed #n" (its timing - here 1 s bursts - varies with the traffic
  it serves). DoA from all its interrogations.
- **ACAS / TCAS** (on an aircraft): whisper-shout sequences - Mode C-only
  all-calls 15-20 ms apart, stepping up in power, each with S1 - about one a
  second; its UF0 / UF16 at its level within 1.5 s. Named by the aircraft's
  own address from a checked UF16 broadcast: **"ACAS 7C4A51"** (also when
  only its broadcasts are heard), else "ACAS #n".
- Roll-calls (UF4 / 5 / 20 / 21 / 0 / 16): to the interrogator whose code
  they carry, whose train / burst / ACAS cycle they fall in at its level -
  or to none (counted).

Packets (`kp::Talker::packet`): each interrogation from P1 − 10 µs to its
end + 10 µs (≥ 2 whole 16-sample covariance chunks; the strong pulses
dominate), the whole band, avg_s 30 (SSR, `doa_avg`) / 10 (fixed) / 5
(ACAS), keep_s 1 h (ACAS 10 min), ≤ 60 a second per interrogator, never
later than 1 s after the interrogation (kraken_doa keeps 1.5 s).

## Analysis per interrogator

Table (a row per interrogator + details ▸): Type, Interrogations (the
interlace: e.g. "A · C + UF11 every 3 periods (P4 short: A/C-only
all-calls)"; fixed: what a burst holds; ACAS: whisper-shout steps, dB /
step, ms / step, S1), PRF, PRI (radar_pri: constant / jittered / stagger /
blocks), Scan s, RPM, Beam ° (-3 dB, radar_scan's lobe fit on unclipped
passes), SLS P2 dB (in the main beam), Mode S (UF counts), Targets
(addresses seen ≥ 3 times), Level dBFS, SNR, interrogations / s, main beam
(s ago). Details: the code (checked / from roll-calls), PRF / PRI,
interlace, scan, beam, P2 main beam / elsewhere, Mode A / C kinds, UF
counts, the targets, DoA count. Facts: interrogations / s (A/C, Mode S),
noise floor, Mode S decoded / checked / model re-fits, the interrogators,
the strongest SSR. Events: a new SSR / fixed / ACAS, an interrogator code,
an ACAS address, the scan period, lost / heard again. Plot: the strongest
SSR's interrogation level over one turn (0° = main beam here; the DF
panel's plot section too). Raw frames (decoder data log): every Mode S
interrogation in hex + UF + address. `valid(start, end)` per DoA packet
and per checked frame (the Digital squelch).

Options: pulse threshold (10 dB), Mode S decoding on / off, DoA per
interrogator, SSR DoA from (main-beam window), SSR DoA averaging, keep.

## DME on 1030 MHz (coexistence)

1030 MHz is also the interrogation frequency of DME channel 6X: aircraft
interrogating a 6X beacon send 12 µs pairs of 3.5 µs pulses here. The two
plugins see different pulses: ssr1030 takes only 0.35-1.45 µs pulses (and
P6 ≥ 10 µs), the dme plugin only 2.4-5.6 µs ones. Checked on one file: the
synthetic four-interrogator scene + 1509 DME pairs at 25 dB - ssr1030 found
the same four interrogators and nothing more; dme found "aircraft
interrogating 6X, 160 pairs/s" and nothing from the SSR pulses (as in its
own "SSR on 1030 MHz" test). Both can run on two VFOs at once.

Replies: the `adsb` plugin decodes the 1090 MHz answers (DF0 / 4 / 5 / 11 /
16 / 20 / 21). Pairing each interrogation with its reply (128 µs + the
path) is not done - a second VFO on 1090 MHz in independent mode would
allow it (checked offline below).

## Generator

`tools/ssr_gen.py SCENE.json OUT.cu8|OUT.cf32` - interrogators at known
bearings on kraken_doa's UCA, every antenna, written in 0.1 s pieces: SSRs
(PRI + jitter, A / C interlace, P4, UF11 every n-th period with II / SI,
rotating beam + sidelobes, omni P2 at a fixed level, roll-calls UF4 / 5
with IIS to targets in the main beam), fixed interrogators (bursts of
all-calls + a roll-call per target 1 ms apart), ACAS (whisper-shout steps
with S1, UF0 to targets, UF16 broadcasts with the own address). Real
formats and parity (uplink AP encoding); each interrogation's envelope
edges through the receiver's step response ("measured" = `s_default.h`, or
a different "lowpass"), a carrier offset per interrogator, an optional echo,
noise, cu8 clipped like the receiver. `OUT.json` = rate, channels, rf_hz,
radius_m, truth. `tools/scene_four.json`: two SSRs, a fixed interrogator,
a TCAS. `SSR_DUMP=1 decoder --file ...` logs every interrogation (kind,
levels, UF, address, the interrogator it went to) and the chains found;
`SSR_PULSES=1` every pulse.

## Tests (Pi 5, 2026-10-15)

Synthetic (`scene_four.json` at 20 s, 5 antennas, UCA r 0.12 m):

| interrogator | truth | found |
|---|---|---|
| SSR-A | II 10, PRF 200 Hz ± 1.5 %, A / C + UF11 every 3rd, scan 4 s, beam 2.5°, 60° | "SSR II 10" (UF11 checked), PRF 199.4 Hz, "A · C + UF11 every 3 periods (P4 short)", scan 4.00 s, beam 2.4°; **60.0°** |
| SSR-B | no Mode S, PRF 303 Hz, A · C · C, scan 6 s, 150° | "SSR #3", PRF 303.0, "A · C · C", scan 6.00 s; **150.0°** |
| MLAT | fixed, 1 s bursts (A A C + 4 roll-calls), 200° | "Fixed #4", bursts 1.00 s, targets C82929 C820CF C87EE0 7C6B07; **199.9° / 200.0°** |
| TCAS | C82081, 8 steps 1 dB / 17.5 ms, UF0 + UF16 broadcast, 300° | "ACAS C82081" (broadcast checked), 7 steps 1.1 dB / 17.5 ms; **300.0°** |

Offline (`--channels 5 --uca 0.12`) and end to end (the real kraken_doa
fed by a fake heimdall streaming the cu8, DF status `doa`) both exact:
60.0 / 150.0 / 200.0 / 300.0°; end to end 1567 of 1580 packets used, none
late. Noise only (8 s): nothing. All 12 dB weaker: both SSRs (II 10
checked), the 10-12 dB fixed one and TCAS not. With 1509 DME pairs mixed
in: unchanged. A different receiver response: see above. UI (headless
Chromium, scratch t_ui_ssr.py, 17 checks): the DF list (✈ / 🛰, columns,
"Every interrogator (4)", by name), one SSR's line, the DF panel's plot
section, the decoder tab's table, details, plot.

Live, Auckland (5-element UCA r 0.12 m, gain 43.4 dB; real heimdall +
a scratch kraken_doa, 3 min, and 12 / 30 / 120 s recordings):

- **SSR II 10**: PRF 198.6-199.4 Hz (PRI ~5025 µs ± 0.9 %, jittered),
  "A · C + UF11 every 3 periods" - Mode A / C-only all-calls (short P4)
  alternating, a UF11 all-call (PR 0, II 10) ~140 µs before every third;
  scan **5.00 s (12.0 rpm)**, main beam **2.5-2.7°** at −3 dB, P2 −13 to
  −15 dB in the main beam (+1 to +5 dB in the sidelobes), main beam ~37 dB
  over the noise (−6.8 dBFS). Bearing **214.6-215.7°** (array frame) in
  every run, history steady at 216°.
- **Fixed** interrogator (multilateration): steady ~18 dB, a burst every
  1.000 s - 4 Mode A + 1-2 Mode C all-calls (short P4) 5 ms apart, then a
  UF4 / UF5 roll-call (IIS 0) per target 1 ms apart: 16 targets in 30 s -
  the 6 aircraft the adsb plugin heard at the same time on 1090 MHz
  (C82929, C82827, C8195C, C827A3, C820CF, C82081), 7C-block (Australian)
  aircraft, and C87EE0 / C87EF1 / C87EF2 (airport vehicles?). Bearing
  **273.7-277.8°**.
- **TCAS**: whisper-shout sequences (3-4 steps heard, 17.4-17.8 ms apart,
  S1 −1.9 dB, every 1.00 s) at 328.6°; aircraft named by checked UF16
  broadcasts over 3 min: AA92F9, 7C4A51, C81A5A, C8195C, C81D8E, C8221A,
  C8244C (their bearings vary: moving aircraft, approximate attribution of
  their roll-calls).
- **Interrogation ↔ reply**: 30 s with tuner 0 on 1030 and tuner 1 on 1090
  MHz (independent mode): 73 interrogations whose decoded address and type
  match a reply decoded by the adsb plugin (UF4 → DF4 / DF20, UF5 → DF5),
  128 µs + the path later (150-175 / 425-475 µs) - the uplink address
  encoding and the bits confirmed. Mode S: 1440 interrogations in 30 s, 31
  checked UF11; with the correction against the known address 155 instead
  of 115 checked in 120 s.

The compass bearings follow from the station heading (the radar plugin's
README: its 1355 MHz radar at 117° array frame ≈ 255° compass).

CPU (Pi 5): the plugin **3.9 % of a core** live (~85 Mode S / s decoded),
2.4 % on the synthetic scene; kraken_doa itself 82-88 % with a 2.4 MHz VFO
and DoA on (as with adsb / radar).

## Limits

- Mode S bits need the KrakenSDR's receiver response (above); a discrete
  interrogation's address can't be checked (targets: ≥ 3 sightings).
- Several fixed interrogators at the same level (±3 dB) become one; an
  ACAS whose whisper-shout and broadcasts aren't heard shows as Fixed (one
  at 25 dB here: Mode C-only all-calls once a second + UF0); a moving ACAS's
  roll-calls are given to it by time and level only.
- Two SSRs with the same PRF and no Mode S, seen at the same moment, can
  swap interrogations; their main beams keep the bearings apart.
- No reply pairing (adsb on 1090 MHz) in the plugin.

## Sources

ICAO Annex 10 Vol IV (SSR / Mode S signals in space: P1-P6 timing and
widths, SLS, intermode / Mode S-only all-calls, DPSK, UF formats and
fields, AP generation incl. the uplink address encoding, II / SI codes);
RTCA DO-185 (TCAS II: whisper-shout, ACAS broadcast UF16 / UDS 0x32 with
the own address); Orlando & Drouilhet, *Mode S Beacon System: Functional
Description* (MIT LL ATC-42); the radar plugin's PRI / scan analysis
(plugins/lib/radar_pri, radar_scan). No code from elsewhere.
