# Digital TV (DVB-T/T2, ISDB-T, ATSC 1.0/3.0, DTMB)

What a broadcast digital TV signal says about itself, read from the 2.4 MHz
slice of the channel the VFO sees: the standard and its mode, the channel
(number on the raster, the exact centre measured on the signal), the
transmission parameters the signal signals in every symbol (DVB-T TPS,
ISDB-T TMCC) and the echo profile - the paths the signal arrives on, from
the channel impulse response (CIR) measured against the signal's known
reference.

**Path separation** (off by default; the "Path separation" button in the
decoder tab, option `separate`): the signal reaches the antennas on several
PATHS - reflections off hills and buildings and, in a single frequency
network (SFN), the other transmitters sending the same signal. To an
antenna array that is one signal from several directions at once, so a
VFO's DoA mixes them. With path separation on the plugin tells them apart:
every path arrives with its own DELAY, so in each antenna's CIR the paths
are separate taps, and at a path's tap the antennas' values are its
spatial signature alone. Each path gets its own DoA lobe - a talker
"Path-n" of the VFO on the 🗺 Map's 📡 DF panel (lobe, bearing, heat map
while driving), clickable in the decoder tab's delay-profile plot and
table; delays are counted from the MAIN (strongest) path, so a weaker path
arriving earlier has a negative delay (the table's "From first" column has
the delay after the first path to arrive - the classic SFN measurement).
It analyses every antenna (about 3x the CPU); off, the plugin analyses
antenna 0 only. Turning it off clears the paths at once - the 📡 DF
panel's list, their table (`kp::Host::spatial_remove`); a path not seen
for 60 s leaves the same way.

**A transmitter or a reflection?** Nothing in a TV signal says: both are
delayed copies of exactly the same signal from their own direction (an SFN
must send identical symbols - even DVB-T's cell ID is the same on every
transmitter of one; DVB-T2's optional transmitter signature isn't read).
That is why it was renamed from "SFN separation": on the six Auckland
DVB-T muxes below, which come from one site each, every path but the main
one is a reflection. Hints: a path whose bearing points at a broadcast mast
(the 🗺 Map's Masts layer) - and whose delay matches the extra distance to
it - is likely a transmitter; a reflection's bearing moves differently
from a transmitter's while driving.

| Standard | Signalling read | Reference for the CIR | Where |
|---|---|---|---|
| DVB-T (2k / 8k, all guards, 5-8 MHz) | **TPS**: constellation, hierarchy, code rates, guard, mode, **cell ID**, DVB-H bits; BCH-checked; net bit rate | scattered pilots | `dvbt.cpp` + `ofdm.cpp` |
| ISDB-T (modes 1-3, 6-8 MHz) | **TMCC**: system, partial reception (one-seg), layers A / B / C (modulation, code rate, time interleaving, segments), next parameters + countdown, **emergency warning (EWS) start flag**; net bit rate | scattered pilots | `isdbt.cpp` + `ofdm.cpp` |
| DVB-T2 (1k-32k, PP1-PP8, normal + extended carriers, 1.7-8 MHz) | FFT size, guard, pilot pattern (from the signal; L1 not read) | scattered pilots, frame PN signs, P1 for timing | `dvbt2.cpp` + `ofdm.cpp` |
| ATSC 3.0 (8k-32k, SP3_2-SP32_4, 6-8 MHz) | FFT size, guard, pilot pattern (from the signal; bootstrap / L1 not read) | scattered pilots | `atsc3.cpp` + `ofdm.cpp` |
| ATSC 1.0 (8-VSB) | the standard's fixed parameters (19.39 Mbit/s) | field sync segment (PN511 + PN63s) | `atsc.cpp` |
| DTMB (PN420 / PN595 / PN945, 6-8 MHz) | frame header (PN length, guard, phase varying or not) | PN frame headers | `dtmb.cpp` |

Pick it on a VFO (Decimators box -> Digital decoder -> "Digital TV"), with
the VFO on the TV channel at the **2.4 MHz** bandwidth (`min_vfo_rate` 2
MHz; picking the decoder sets it and locks the VFO at the tuner centre -
`kp::Info::lock_vfo`, one line like ADS-B's: tune the receiver to move
it). The receiver doesn't have to sit on the channel centre: any 2.4 MHz
slice of a 6-8 MHz channel works. The metadata and the echo profile work in
every operating mode; path separation needs **coherent mode**, DoA on (MUSIC
DoA box) and the array set up as for any DoA.

DAB / DAB+ moved out (it was part of the earlier "SFN Separation" plugin):
it will be a plugin of its own (ensemble / service information, TII, audio).
A saved VFO that ran the old `sfn` plugin loads as `dtv` (kraken_doa maps
the id and its `sfn.*` options; a saved standard "dab" falls back to Auto).

Mobile networks (LTE / 5G NR) are different: their cells send different
signals (scrambled with the cell ID) on one carrier - the `cells` plugin
reads their broadcast information and separates them by code
(`../cells/README.md`).

## What it shows

- **Facts**: Standard (mode found), Channel centre (measured), Channel (the
  raster's number: UHF 21-69 on the 8 MHz raster, 7 MHz Band III E5-E12 /
  Australia 6-12 + Australian UHF, the 6 MHz North American / Korean
  channels, ISDB-T's Japanese / Americas numbering with the 1/7 MHz offset,
  China's DS channels for DTMB), Guard correlation, the signalling lines
  (TPS / TMCC as decoded, with the net bit rate computed from them), Paths
  (count, spread, and with separation on how far the first arrives before
  the main), Delay window, Antennas.
- **Events**: Locked / lost, the TPS / TMCC parameters when first read and
  whenever they change, the DVB-T cell ID, "⚠ Emergency warning broadcast"
  when an ISDB-T TMCC sets the EWS start flag (and when it clears).
- **Plot**: "Echo profile" (separation off: dB above the estimation floor,
  x = us from the strongest path, each path marked "+2.6 us, -23 dB") or
  "Channel impulse response" (separation on: x = us from the main path, the
  axis from a tenth of the window before the first path; a clickable marker
  per path, "main" and "#n").
- **Table** (separation on): path, from main (us), from first (us), extra
  path (km, from the main), power, above noise, seen.
- **Events** (separation on): "New path Path-n: +x us from the main path,
  y dB", "Path Path-n gone".
- Valid frames (Digital squelch) while the standard is locked.

## How it works

kraken_doa sends the plugin every antenna's samples (an ARRAY plugin,
`kp::Info::array`) in bursts of 0.35 s every second. Per burst:

1. **The standard's estimator** (`estimator.hpp`; option `standard`, Auto
   tries them all and keeps the one that locked) finds the signal's known
   reference in the slice and emits CIR snapshots: every analysed antenna's
   CIR on one tap grid (`CirSnapshot`); OFDM standards also read their
   signalling carriers (below).
2. **The core** (`sfn_core.cpp`) sums `C(tau) = sum h(tau) h(tau)^H` per tap
   over the snapshots (non-coherently - Doppler / drift between snapshots
   don't matter), estimates the noise floor `F` from the taps without
   paths, finds the peaks of `D(tau) = tr(F^-1 C(tau)) / m` above
   `min_snr` and drops a stronger peak's sidelobes.
   - Separation off (`set_profile_only`): the peaks within the "Weakest
     path" limit are the echo paths; one is marked when it was seen at
     about the same delay (from the strongest) in one of the two bursts
     before - estimator artefacts don't stay put, and one antenna can't
     tell them by direction. A standard can declare a floor for one
     antenna's profile (`Estimator::profile_floor_db`; ATSC 3.0: 22 dB -
     its symbol tracking leaves ghosts 24-28 dB down).
   - Separation on: also drops estimator echoes (same direction, 17 dB
     weaker), tracks the peaks across bursts (the receiver's timing
     reference may move: a common offset is fitted first; 3 hits confirm a
     path, 60 s unseen forget it) and reports each confirmed one as a
     `kp::Spatial` (its covariance, delay, power) - the host runs MUSIC with
     one source on it. The main path is the strongest confirmed one (kept
     unless another is 1 dB stronger, so two paths of about the same power
     don't swap it every burst); the order on the circular delay axis comes
     from the first path, the one after the largest gap. IDs are the
     plugin's own (not saved: heat maps of paths live as long as the plugin
     runs).

### OFDM standards (`ofdm.cpp`)

The slice is resampled to the standard's elementary rate / D (a power of 2,
so an FFT of N / D samples holds exactly the carriers inside the slice):

- **Symbol timing + fractional carrier offset**: the cyclic-prefix
  correlation folded over the symbol length, every FFT size / guard tried
  (DVB-T, ISDB-T). DVB-T2: P1 symbols found from their C-A-B structure
  (derotated lag-542 T / lag-482 T correlations; works on any slice) reset
  the grid every frame - frames are contiguous, so P1s give every symbol
  before and after them. ATSC 3.0: the bootstrap resets the grid and
  time-aligned frames add excess guard samples, so symbols are TRACKED
  from guard peak to guard peak, each run folded for start + period, and
  the guard is the longest allowed one that fits the measured period.
- **Pilot pattern**: per symbol the residue class of bins with the boosted
  power (DX DY classes), consistent from symbol to symbol. Each bin's power
  is taken against its own average over the burst's symbols first: the
  channel's fading stays put while the pilots move (two paths 37 us apart
  fade with a period of ~12 carriers in DVB-T 2k - the classes' raw powers
  showed the fading, not the pilots, and 2k didn't lock).
- **Carrier offset** (which carrier is bin 0): the candidate whose pilots
  divided by their known values give the sparsest CIR.
- **Combs**: DY consecutive symbols with every pattern phase once combine
  into one comb of DX (unambiguous range Tu / DX; DVB-T 8k: 299 us). Every
  symbol carries a common phase that steps from symbol to symbol (the
  offset's whole-bin part x the symbol period - fractional with most
  guards / VFO offsets); it is measured on the pilots that symbols DY apart
  share and taken out before combining (else ghosts a quarter / half of the
  range away). DVB-T2's pilots also flip sign with the frame PN: the symbol
  index from the P1 gives the sign. Fractional window starts are corrected
  per bin; runs (frames) are aligned to the burst's first by their delay
  profile.
- DVB-T vs DVB-T2: T2 needs P1 symbols; the DVB-T estimator rejects a
  signal whose combs need sign flips (DVB-T2 in PP1).
- **Signalling** (`OfdmStandard::signal_carriers` / `signalling`,
  `signalling.hpp`): the standard's signalling carriers inside the slice,
  antenna 0, every symbol (the fractional window start undone). Each
  carrier carries the same bit per symbol, DBPSK: the products of
  consecutive symbols summed over the carriers, against the common phase
  step from symbol to symbol - from carriers with the same value in every
  symbol (DVB-T's continual pilots: exact), or else the squared products
  (ISDB-T: modulo pi, the polarity settled by a known bit).
  - **DVB-T TPS** (EN 300 744 4.6): 17 (2k) / 68 (8k) carriers spread over
    the band - a 2.4 MHz slice of 8k holds ~19, of 2k ~5. 68 bits per
    frame: the sync word (0x35EE frames 1 / 3, 0xCA11 frames 2 / 4, <= 1
    bit error) finds the frame, s1-s67 must pass the BCH(67,53) code (g =
    x^14 + x^9 + x^8 + x^6 + x^5 + x^4 + x^2 + x + 1, the codeword's
    remainder 0). The cell ID's high byte comes in frames 1 / 3, the low
    byte in 2 / 4 (the frame number s23-s24). Net bit rate = data carriers
    (1512 / 6048) x bits x code rate x 188 / 204 / symbol time.
  - **ISDB-T TMCC** (ARIB STD-B31 3.15): the synchronous segments' TMCC
    carriers (1 / 2 / 4 per segment, mode 1 / 2 / 3), 204 bits per frame,
    B0 the reference: sync word w0 / w1 (complements: the segment type
    B17-B19 = 000 for synchronous segments settles the polarity), then
    B20-B121. The parity B122-B203 is not checked; a frame counts when the
    reserved bits B110-B121 are all 1, every used layer has known codes and
    the layers' segments add up to 13 (current parameters; next: 13 or
    none). A mode-3 frame (204 symbols, ~0.23 s) is longer than the 0.2 s a
    burst's CIR analysis uses: the signalling is then read from the whole
    0.35 s burst, antenna 0 (`signalling_symbols`). Every segment is assumed
    synchronous (the pilot engine needs scattered pilots).

### ATSC 1.0 (`atsc.cpp`)

The field sync segment (every 24.2 ms): 728 known symbols. The reference is
built for exactly the slice in the frequency domain (the symbols' DTFT x the
root-raised cosine 0.1152 at the channel centre's offset; no aliasing).
Channel centre: `channel_mhz`, else the 6 MHz raster channels overlapping
the slice; the pilot (2.69 MHz below the centre) gives it exactly when the
slice holds it, else a carrier search +-24 kHz. Timing from a 64k-point
correlation over one field; per field a 1024-sample window, H = Y conj(R) /
(|R|^2 + lambda), zero-padded IFFT (0.21 us taps, 427 us window); the field
parity (inverted middle PN63) per burst; the sample-clock drift inside a
burst measured from the fields' profiles and taken out. The unknown data
around the sync limits a path's tap to ~18 dB above its own data noise per
field - averaging fields and bursts takes care of it.

### DTMB (`dtmb.cpp`)

The frame header (420 / 595 / 945 chips at 7.56 Msps, the chip on I and Q,
RRC 0.05) of every 555-625 us frame, as for ATSC: the slice's reference,
64k-point correlation for timing (and the header mode), per frame a 512-
sample window and a regularised division. PN420 / PN945 headers repeat their
m-sequence (1.85 / 1.65 periods): a near-full division (lambda 0.03) keeps
the echo one period away at -19 dB. Phase-varying headers (the m-sequence
started 0, +1, -1, +2, ... chips later frame by frame over the 125 ms
superframe) only move the CIR against the phase-0 reference: each frame's
move is measured, the burst is fitted to the schedule (either direction -
the standard's bit order is the one uncertain point) and the exact moves
are taken out; a burst no model fits uses the measured ones.

## Options

| Option | |
|---|---|
| Path separation | Off (default) / On - the button in the decoder tab (key `separate`, as when it was "SFN separation") |
| Standard | Auto (default) or one standard |
| Channel width | Auto, or 5 / 6 / 7 / 8 MHz (OFDM standards, DTMB) |
| Channel centre (MHz) | ATSC / DTMB: when the VFO doesn't see the ATSC pilot and the channel isn't on the raster |
| Detection threshold (dB) | a path's peak above the floor (default 8) |
| Weakest path (dB below the strongest) | default 30, 0 = keep all - a strong signal's local reflections 20-35 dB down pass the threshold too |
| Max paths | separation: default 8, the strongest (key `max_tx`) |

## Limits

- **Service names, programme guide, network name** are in the transport
  stream (PSI / SI tables): they need the whole 6-8 MHz channel
  demodulated and error-corrected - the VFO sees 2.4 MHz. TPS / TMCC work
  on a slice because their carriers are spread over the whole band.
- DVB-T2's L1 signalling (in the P2 symbols) and its P1 S1 / S2 fields
  (carriers to the channel edges) and ATSC 3.0's bootstrap / L1 are not
  read: the mode shown is what the signal's own structure gives (FFT size,
  guard, pilot pattern).
- ISDB-T: the TMCC parity is not checked (the structure checks above
  instead); differential (DQPSK) segments are not handled.
- DTMB's system information (constellation, code rate, interleaver - in
  the frame body) is not read.
- Delays must differ by more than about 2 / the slice's bandwidth (~1 us
  at 2.4 MHz); closer paths merge (the stronger one's bearing wins).
  Whether a path is a transmitter or a reflection isn't known (above).
- The delay range is the estimator's: DVB-T / ISDB-T Tu / 3 (8k: 299 us),
  DVB-T2 Tu / DX, ATSC 3.0 Tu / DX, ATSC 1.0 427 us, DTMB 213 us.
- ATSC 3.0: the scattered pilots must be boosted (boost >= 1; SP3_2 >= 2)
  - the pattern phase of each symbol is found from them; MISO (TDCFS)
  filters each transmitter's pilots differently (not handled); a frame's
  later subframes with another FFT size / pattern are not used. One
  antenna's echo profile marks no path more than 22 dB below the strongest.
- ID separation: the transmitters of a single frequency network send
  identical signals (the TPS cell ID is the network's, the same from every
  site), so there is no per-transmitter ID to report - path separation
  tells them apart by delay instead. ATSC 3.0's transmitter ID (TxID
  watermark) and DVB-T2's optional TX-SIG are not decoded.
- DTMB: only the 8 MHz raster is known (6 / 7 MHz: set the channel centre);
  delays beyond the header's cyclic extension still work (linear method).
- CPU (Pi 5): the estimation per burst on the real 522 MHz DVB-T capture
  ~35 ms with separation off, ~100 ms on (wall time, the Pi busy with the
  live stack; ~3x); searching (nothing locked) ~10-15 % of a core per
  standard tried for the first 10 bursts, then every 4th burst. kraken_doa
  sends every antenna's samples either way (~17 MB a second; kraken_doa
  +6-16 %, the plugin's reading of them included in its ~25 % of a core
  measured live with separation on).

## Tests

`tools/sfn_gen.cpp` writes synthetic signals (every standard: real pilots /
headers / sync from `std/*_tables.hpp`; DVB-T with real TPS frames - 16-QAM
2/3, the generated guard / mode, `--cell` ID, BCH parity -, ISDB-T with
real TMCC - one-seg layer A QPSK 2/3 + layer B 12 segments 64-QAM 3/4,
`--ews 1` sets the emergency flag; transmitters with their own bearing,
delay, power, Doppler; the clockwise UCA kraken_doa expects; a VFO offset
from the channel centre; noise):

```bash
g++ -std=c++20 -O2 -Idtv -Isdk dtv/tools/sfn_gen.cpp -o /tmp/sfn_gen     # from kraken_doa_v2/plugins
/tmp/sfn_gen --std dvbt --mode 8k --gi 4 --rf 600.95e6 --offset 0.95e6 --snr 15 \
    --tx 30,0,0 --tx 150,37,-4 --tx 260,112,-10 --out /tmp/dvbt.cf32
plugins/dtv/build/decoder --file /tmp/dvbt.cf32                                   # metadata + echo profile
plugins/dtv/build/decoder --file /tmp/dvbt.cf32 --opt separate=1 --uca 0.2122 --all   # path separation
```

Path separation, offline (4 s each, 5-element UCA, Auto standard; truth in
brackets; the strongest path is also the first in these) - the numbers are
unchanged since it was "SFN separation"; with separation off every
recording shows exactly its 3 paths:

| Recording | Found | Delays (us) | Powers (dB) | Bearings (deg) |
|---|---|---|---|---|
| DVB-T 8k GI 1/4, VFO 1.5 MHz off | DVB-T 8k GI 1/4 | 0 / 37.0 / 112.0 | 0 / -4.1 / -10.0 | 30.0 / 150.0 / 260.0 (30 / 150 / 260) |
| DVB-T, VFO 0.95 MHz off | DVB-T 8k GI 1/4 | 0 / 37.0 / 112.0 | 0 / -4.0 / -10.0 | 30.0 / 150.0 / 260.0 |
| ISDB-T mode 3 GI 1/8 | ISDB-T mode 3 GI 1/8 | 0 / 23.5 / 61.0 | 0 / -3.0 / -8.0 | 70.0 / 200.0 / 310.0 (70 / 200 / 310) |
| ATSC 1.0 ch 27, pilot outside / inside | ATSC 1.0, ch 27 | 0 / 18.5 / 47.0 | 0 / -3.2 / -7.9 | 59.7-59.9 / 185.1 / 299.2-301.3 (60 / 185 / 300) |
| DVB-T2 32k GI 1/128 PP7 ext | DVB-T2 32k GI 1/128 PP7 | 0 / 5.25 / 14.0 | 0 / -3.0 / -7.0 | 45.0 / 160.0 / 275.0 |
| DVB-T2 8k GI 1/8 PP2, VFO 2.5 MHz off | DVB-T2 8k GI 1/8 PP2 | 0 / 48.0 / 95.0 | 0 / -2.0 / -6.4 | 20.0 / 130.0 / 240.0 |
| DTMB PN420 phase varying | DTMB PN420 | 0 / 9.52 / 30.9 | 0 / -3.0 / -6.8 | 35.0 / 149.9 / 256.0 |
| DTMB PN945 constant | DTMB PN945 | 0 / 22.0 / 64.0 | 0 / -2.0 / -7.1 | 100.2 / 219.9 / 330.5 |
| DTMB PN595 | DTMB PN595 | 0 / 14.0 / 39.9 | 0 / -4.0 / -8.1 | 10.2 / 120.2 / 200.1 |
| ATSC 3.0 16k GI5 SP12_4, 37 / 0 excess samples | ATSC 3.0 16k GI5_1024 SP12_4 | 0 / 12.0 / 39.9 | 0 / -3.0 / -6.0 | 75.0 / 190.0 / 310.0 |
| DVB-T 8k, a weaker path 20 us BEFORE the strongest | DVB-T 8k GI 1/4 | -20.01 / main / +40.03 (from first 0 / 20.01 / 60.05) | -6.0 / 0 / -10.0 | 30.0 / 150.0 / 260.0 (30 / 150 / 260) |

Signalling, synthetic (3 s each):

| Recording | Decoded |
|---|---|
| ISDB-T mode 3 GI 1/8, VFO +700 kHz, ch 21 (Japan) | TMCC every frame: A 1 segment QPSK 2/3 I = 2, 0.42 Mbit/s; B 12 segments 64-QAM 3/4 I = 1, 16.85 Mbit/s; partial reception; separation on: the 3 paths exact |
| ISDB-T mode 1 GI 1/4, VFO -900 kHz, EWS flag | TMCC 8 frames, layers with I = 8 / 4, "⚠ Emergency warning broadcast" |
| DVB-T 2k GI 1/4, VFO +1.5 MHz, cell 0x1234 | TPS 29 frames BCH ok: 16-QAM 2/3, GI 1/4, 2k, 13.27 Mbit/s, cell ID 0x1234; echo paths 0 / 37.0 us (-5 dB); separation 30.1 / 150.0 deg |

Real signals (5-channel coherent captures of six Auckland DVB-T
multiplexes, 522-618 MHz, UCA 205 mm, 4 s each): all locked as DVB-T 8k
GI 1/16, 8 MHz (channel centres found ~1 kHz high, guard correlation
0.92-0.99), UHF channels 27 / 29 / 33 / 35 / 37 / 39; TPS in every frame
with the BCH check passing (8-9 frames per 4 s): 64-QAM 3/4, GI 1/16, 8k,
non-hierarchical, 26.35 Mbit/s, cell IDs 32100 / 10100 / 20100 / 31100 /
30100 / 33100 (0x7D64 ... - round decimal numbers: the byte order is
right). Each has ONE transmitter 48-55 dB above the estimation floor (the
main path: 522 / 538 MHz from about 54-58 deg, 570-618 MHz from 250-270 deg
in the array frame - two sites) and 2-5 more paths 13-28 dB weaker within
18 us from scattered directions: local reflections, no second SFN
transmitter there. Not yet tried on a real SFN.

Live in kraken_doa (a test build fed by a fake heimdall streaming the DVB-T
recording in a loop, the plugin on a 2.4 MHz VFO): Path-1 main / Path-2
+37.0 us / Path-3 +112.0 us at 30.0 / 150.0 / 260.0 deg, the same as the
plugin before the rename on the same feed (a run with the Pi at load ~10 -
the live stack + tests - gave wrong paths for both: samples late / lost).
UI (headless Chromium, real mouse clicks): the "Path separation" button
(Off by default, follows each click), the DF panel's "Every path (3)",
"main" tag, a picked path "37.0 us after the main path (11.1 km more
path)", off clears them; picking a path in the decoder tab's plot / table;
an old `PLUGIN:sfn` mode loading as dtv.

## Sources

`std/*_tables.hpp` were extracted from the standards (EN 300 744, EN 302
755, ARIB STD-B31, ATSC A/53 Part 2, ATSC A/322) and, for DTMB (GB 20600,
not available), reconstructed from ITU-R BT.1306 and public vendor / patent
/ receiver sources; each file says where every value comes from and how it
was checked.
