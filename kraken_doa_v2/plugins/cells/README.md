# LTE / 5G NR Cell Decoder

Reads what every LTE / 5G NR cell on a carrier broadcasts to all phones -
the public system information a phone needs before it can even attach:
which network it is (MCC-MNC), its tracking area, the cell's identity (eNB
and sector), band, bandwidth, antenna ports, access rules, uplink and power
settings, random access, paging cycle... - and gives every cell its own
bearing.

Only broadcast system information is decoded: the synchronisation and
reference signals, the MIB and the SI messages with the SI-RNTI (SIB1,
SIB2 ...). Nothing addressed to a phone - dedicated control or data, paging
(it carries phone identities) - is decoded; the plugin doesn't look for it.

A mobile network uses the same carrier in every cell: an LTE / NR carrier is
a mix of many cells' signals - the sectors of nearby base stations and the
ones further away - so a VFO's DoA mixes them. Every cell, though, scrambles
its signals with its own physical cell ID (PCI: 0-503 in LTE, 0-1007 in NR).
Correlating with ONE cell's sequences picks out that cell's channel on every
antenna, while the other cells spread out like noise - so every cell gets
its own bearing: a talker "LTE-<pci>" / "NR-<pci>" of the VFO on the 🗺 Map's
📡 DF panel (lobe, bearing, heat map while driving), clickable in the
decoder tab's plot and table. (The dtv plugin's path separation separates the paths of a
TV signal - reflections, transmitters of a single frequency network - by DELAY: they all
carry the same signal. Here
the cells send DIFFERENT signals and are told apart by CODE: cells at the
same delay - co-sited sectors, synchronised networks - come apart too.)

## What it shows

| Technology | Decoded |
|---|---|
| LTE FDD / TDD (normal cyclic prefix) | PSS / SSS: PCI (group + sector), frame timing, FDD / TDD. **MIB** (PBCH): bandwidth, antenna ports, PHICH configuration, frame number (SFN), LTE-M SIB1. **SIB1**: PLMN list (MCC-MNC + country), tracking area code, cell identity (28 bits = eNB ID + cell), barred / reserved, reselection, CSG, q-RxLevMin, q-QualMin, p-Max, band (+ multi-band), the SI schedule, TDD configuration, value tag, IMS emergency. **SIB2**: access barring, random access (preambles, power, windows), paging cycle, PRACH, reference signal power, PUSCH / PUCCH / SRS / uplink power control, UE timers, **uplink EARFCN + bandwidth**, MBSFN subframes, time alignment. **SIB3** (reselection), **SIB4** (neighbour cells on the same carrier), **SIB5** (other LTE carriers + their cells) when they can be read (below). Other SIBs are named only |
| 5G NR, 15 / 30 kHz SS/PBCH block | PSS / SSS: PCI (group + sector), the beams seen, the SSB's frequency, NR-ARFCN, GSCN, band |

Plus, per cell: power against the strongest cell, its height above the
estimation floor, the frame timing against the strongest cell (the same
timing = a synchronised network / the same site), paths and delay spread,
its carrier offset; per carrier the band / EARFCN, and the network's
frequency against the receiver's (calibrates the receiver: base stations are
accurate to 0.05 ppm).

**A carrier wider than the VFO.** The VFO is 2.4 MHz wide (it sees ~2.1
MHz). The MIB and the sync signals sit in the carrier's middle 1.08 MHz:
always readable. The control channel (PCFICH / PDCCH) and the SIBs' data
(PDSCH) are spread over the whole carrier, so on a 10 MHz carrier the VFO
sees ~23 % of them - decoded anyway:

- the control region's unseen resource element groups are erasures; the
  SI-RNTI DCI (aggregation 4 / 8, a rate-1/3 code repeated) still decodes
  from what is seen (checked against its own re-encoding);
- a SIB's transmissions are combined in the turbo code's circular buffer
  until its CRC passes: different redundancy versions (SIB1 cycles four; an
  SI message sent twice in its window) and control-region sizes (CFI 2 / 3)
  put different coded bits into the visible part. SIB1 and SIB2 decode in
  seconds on a 10 MHz carrier;
- an SI message sent ONCE per window (always redundancy version 0) shows
  the same bits every time: if its resource blocks are mostly outside the
  VFO it never reaches its CRC (on the 10 MHz carrier below: SIB3 and SIB5,
  stuck at 36-40 % of their code). Carriers of 1.4 / 3 MHz are wholly
  visible, 5 MHz about half.

## Using it

Pick it on a VFO in **coherent mode** (Decimators box -> Digital decoder ->
"LTE / 5G NR Cell Decoder"), DoA on and the array set up as for any DoA. The
VFO takes the whole 2.4 MHz band (`kp::Info::lock_vfo`): picking the
decoder sets that bandwidth and puts the VFO on the tuner's centre, where it
is drawn as one locked line like ADS-B's - you move it by tuning the
receiver. Tune somewhere into the carrier: the sync signals sit in the
middle 1.08 MHz of an LTE carrier (NR: the SS/PBCH block, not necessarily
the carrier's middle), and the plugin looks for them on the LTE 100 kHz
raster / the NR sync raster (GSCN) within +-0.6 MHz of the tuning. Nothing
there: it **retunes the receiver itself** (option "Find the carrier",
default within 10 MHz) - 1.2 MHz at a time, alternately up and down - until
it finds a carrier, and then centres the receiver on it (it centres a
carrier found next to the tuning too). Each retune makes heimdall
recalibrate (a few seconds a step, so a 10 MHz search can take a minute or
two); every other VFO moves along. Nothing found: the receiver goes back to
where you tuned and stays; tune elsewhere to search again from there.
The standard KrakenSDR tunes up to ~1.77 GHz: LTE bands 700-1700 MHz and NR
in the 600-900 MHz bands; higher bands need the Wideband variant.

The decoder tab's table lists the cells - open a row (▸) for everything
known about that cell. Outside coherent mode the plugin gets the VFO's one
tuner: the cells and their information, no bearings.

## How it works

kraken_doa sends the plugin every antenna's samples (an ARRAY plugin,
`kp::Info::array`) in bursts of 0.3 s every second, at the VFO's 2.4 MHz -
160 samples per 15 kHz OFDM symbol, so nothing is resampled: the
fractional cyclic prefixes (11.25 / 12.5 samples) are put back as phase
ramps (`ofdm.hpp`). Per burst and technology:

**Cells and bearings** (`decoder.cpp`, `search.cpp`, `chest.cpp`)

1. **Carrier.** While searching: the raster positions within reach of the
   VFO centre (LTE every 100 kHz, NR on the GSCN), a few per burst; the fine
   offset (modulo the subcarrier spacing) from the cyclic prefixes, whose
   correlation is folded over 0.5 ms with its mean removed (a CW tone / DC
   adds the same everywhere). A receiver more than half a subcarrier off
   (7.5 kHz: 9 ppm at 800 MHz) is covered by sweeping the raster again with
   +1 and -1 whole subcarriers. Once cells are found the carrier is locked
   and its frequency TRACKED from the cells themselves (the phase of each
   cell's SSS against its PSS). A technology with nothing found is searched
   again every 4th burst, after 12 sweeps every 16th.
2. **PSS fold.** Two antennas correlated with the three PSS of the
   technology (one OFDM symbol each, FFT overlap-save), |c|^2 folded modulo
   the PSS period (LTE 5 ms, NR 20 ms = the default SS burst period): a cell
   is a peak in the fold of its N_ID2.
3. **Peaks, strongest first, judged against images.** A strong cell's PSS
   correlates with the other PSS (LTE: peaks ~9 dB below it, every ~23
   samples) and its SSS symbol with all of them; each candidate must stand
   4.8 dB above the images the stronger peaks and the cells found so far
   (their SSS is known then) leave at its position. Peaks of another N_ID2 at
   the same timing (within 4 samples) are always tested - a synchronised
   network's co-sited sectors look exactly like images there.
4. **SSS test.** At up to 32 occurrences of the peak, the PSS channel is
   estimated on the sync subcarriers by least squares of all three PSS over
   9 subcarriers, the SSS symbol is equalised with it, summed over the
   antennas and occurrences and tested against every N_ID1 - LTE: both
   halves of the frame (subframe 0 / 5) and FDD / TDD. Scored by MAGNITUDE;
   the best must stand out from all hypotheses (robust z-score >= 6). Found
   cells are cancelled and the test repeated (up to 4 cells per peak).
5. **Confirmation.** Every candidate must show its own reference signals: a
   cheap channel estimate (40 slots, CRS port 0 / the NR SSS) must have a
   path above the detection threshold.
6. **Channel estimation.** LTE: CRS of ports 0 and 1 in symbols 0 and 4 of
   up to 160 slots, descrambled with the cell's Gold sequence (22 us of
   impulse response; TDD uplink slots dropped). NR: the SSS of every SS/PBCH
   block, all beams. Per snapshot every antenna's impulse response -> the
   per-tap spatial covariance -> the estimation floor (noise + the other
   cells' leakage) -> the chosen path's main lobe minus the floor = the
   cell's spatial covariance (`kp::Spatial`); kraken_doa runs MUSIC with one
   source on it (averaged over 4 s).
7. **Tracking.** A cell is confirmed after 2 bursts, forgotten 30 s after
   its last sighting.

**Broadcast information** (LTE; `lte_dl.cpp`, `lte_pdsch.cpp`,
`lte_meta.cpp`, `lte_rrc.cpp`, `fec.cpp`, `turbo.cpp`) - per cell, within
0.1 s of processing per cell and 0.2 s per burst, the cell served longest
ago first:

8. **Resource grid** (`CellRx`): a subframe's 14 OFDM symbols on every
   antenna at the cell's frame timing, the channel of each antenna port
   (0-3) from its CRS - least squares on the pilots, averaged over the
   subframe with the cell's carrier offset turned back, interpolated across
   the subcarriers. Transmit diversity (SFBC for 2 ports, SFBC + FSTD for 4)
   is combined over all antennas into soft bits; REs outside the VFO are
   erasures.
9. **MIB** (36.211 6.6, 36.212 5.3.1): subframe 0 slot 1 symbols 0-3, the
   middle 72 subcarriers -> 480 soft bits a frame, descrambled for each of
   the 4 frames of a MIB period (the SFN's 2 low bits), rate-1/3 tail-biting
   Viterbi; the CRC16 mask gives the antenna ports (1 / 2 / 4). Read every
   burst (it gives the frames' SFN); two agreeing frames the first time.
10. **PCFICH -> control region.** The CFI (2 bits in a 32-bit code over 4
    REGs - one visible REG decides it), the PHICH groups (from the MIB), the
    PDCCH's REGs (36.211 6.8.5: by subcarrier then symbol, interleaved by
    quadruplets, shifted by the PCI). A doubtful CFI: all three are tried.
11. **DCI** (36.212 5.3.3): the common search space (aggregation 4 and 8,
    CCE 0-15), formats 1A and 1C, CRC scrambled with the SI-RNTI; accepted
    when the re-encoded DCI agrees with >= 80 % of the received soft bits.
    Format 1A: localized / distributed allocation, MCS, RV, the TBS table
    column; 1C: gap, VRBs, TBS index (RV from the frame / window).
    Distributed VRBs are mapped to their PRBs per slot (36.211 6.2.3.2).
12. **PDSCH** (36.211 6.3 / 6.4): the grant's REs (without the CRS, PBCH,
    sync signals and the control region), descrambled with the SI-RNTI ->
    soft bits -> the turbo code's circular buffer (sub-block interleavers,
    RV start) - **combined** with the earlier transmissions of the same
    block -> max-log-MAP turbo decoding (QPP interleaver, 10 iterations,
    early stop) -> CRC24A.
13. **RRC** (36.331, unaligned PER): SIB1 and SystemInformation (SIB2 -
    SIB5 decoded field by field; the extension additions skipped as open
    types). SIB1 gives the SI schedule: each SI message is looked for in its
    window (`si-WindowLength`, periodicity), the RV of format 1C from the
    subframe's index in the window (36.321 5.3.1). Decoded blocks are read
    again after 10 minutes; a changed SIB1 value tag re-reads the SI
    messages.

## Options

| Option | Default | |
|---|---|---|
| Technology (`tech`) | Auto | Auto = LTE + NR 15 kHz; LTE; 5G NR 15 kHz SSB; 5G NR 30 kHz SSB |
| Carrier / SSB centre (`carrier_mhz`) | empty | the LTE carrier's / NR SS/PBCH block's centre in MHz instead of the raster search |
| Detection threshold (`min_snr`) | 8 dB | a cell's path above the estimation floor (noise + the other cells' leakage) |
| Bearing from (`path`) | the strongest path | or the first path above the threshold (within 15 dB of the strongest): the line of sight when a reflection is stronger |
| Max cells (`max_cells`) | 12 | per technology |
| Cell information (`decode`) | Decode | off = cells and bearings only (saves CPU) |
| Find the carrier (`scan`) | within 10 MHz | off / 5 / 10 / 20 MHz: retune 1.2 MHz at a time to find a carrier, then centre it |

## Output

- **Table** (one row per cell): Cell, PCI, Mode (LTE FDD / TDD, NR 15 / 30
  kHz), Operator (MCC-MNC + country; "+n" = more PLMNs), TAC, eNB / cell,
  Band (SIB1's, or every band holding the frequency until SIB1 is read),
  (E)ARFCN (LTE downlink EARFCN, NR: the SSB's NR-ARFCN), Bandwidth, Ports /
  beams, Power (dB against the strongest), Above noise (dB), Timing (us
  against the strongest cell), Spread (us). A row is clickable (the cell on
  the 🗺 Map's 📡 DF panel); **▸ opens its details** (`kp::Host::
  table_detail`): every decoded field - physical (PCI group / sector,
  signal, timing, paths, carrier offset), MIB, downlink band / EARFCN, SIB1,
  the SIB1 transmission's DCI, each SI message's fields ("SIB2 · ..."), and
  what is still being collected ("waiting for SIB3 (12 tx, 36 %)").
- **Talkers** "LTE-<pci>" / "NR-<pci>" (spatial; label with the eNB / cell
  once known): 📱 cells on the DF panel, strongest first.
- **Facts**: Signal, Cells, LTE carrier (frequency, FDD / TDD, band,
  EARFCN, bandwidth, SFN, the share of it the VFO sees), Network (PLMN),
  System information (cells with MIB / SIB1 / all SI messages), Frequency
  offset, Antennas.
- **Plot** "Sync correlation (PSS)": the PSS fold around the strongest
  cell, a marker per cell (clickable).
- **Events**: carrier found / lost, new cell / lost, "LTE PCI n MIB: ...",
  "SIB1: PLMN ..., TAC ..., eNB ... cell ..., band ...", "SIB2 decoded ...:
  Uplink ...", changes (MIB / SIB1 value tag), the scan's retunes.

## Limits

- The sync signals must come inside the VFO: the middle of an LTE carrier,
  an NR SS/PBCH block - the carrier scan moves the receiver there. The scan
  and the centring retune the receiver: in coherent mode every VFO moves. One
  VFO running this decoder at a time (two would both retune).
- Wide carriers: see above - SIBs sent once per window with their resource
  blocks outside the VFO stay unread (SIB3 / SIB5 on the 10 MHz band-8
  carrier here). Reading them would need the receiver to look at the other
  parts of the carrier.
- NR: cell ID, beams and frequency only - the NR MIB (polar code) and SIB1
  (LDPC, CORESET 0) are not decoded yet.
- LTE extended cyclic prefix and MBSFN subframes are not handled; LTE-M /
  NB-IoT information is not decoded (the MIB's LTE-M SIB1 field is shown).
- TDD: the SI windows skip special subframes; the PHICH factor m_i is tried
  for 0 / 1 / 2 until SIB1 gives the configuration. Formats 1A / 1C, 1, 2
  and 4 antenna ports, localized / distributed allocations and TDD are
  implemented from the specifications; the live network here is FDD, 2
  ports, format 1C, distributed - the others are untested on real signals.
- Synchronised networks: co-timed cells were found down to 10 dB below the
  strongest; a cell 14 dB down sharing the N_ID2 and 16 samples from a
  stronger one was missed.
- A cell needs about -6 dB per-subcarrier SINR to be found; its
  broadcast information needs more (the MIB a few dB, SIBs on a wide carrier
  ~10 dB+).
- A repeater / DAS re-sends its donor cell (same PCI and information): the
  bearing points at whichever is stronger here.
- CPU (Pi 5): ~15-20 % of a core per locked technology; while the cell
  information is collected up to +20 % (0.2 s per burst at most), then a
  MIB per cell per burst (~1 ms).

## A wider receiver

Nothing in the plugin is tied to 2.4 MHz: any VFO rate with a whole number
of samples per 15 kHz subcarrier works (LTE's 3.84 / 7.68 / 15.36 / 23.04 /
30.72 MS/s, 9.6 MS/s ...). The usable band is 0.4375 x the rate (+-1.05
MHz at 2.4 MHz), the raster search reaches that minus 0.45 MHz (the sync
signals must be inside; +-0.6 MHz at 2.4 MHz) and the carrier scan steps
twice that. The cell search runs on a copy decimated to the rate the sync
signals need (LTE 1.92 MS/s, NR 15 kHz 2.4, NR 30 kHz 4.8; Blackman-windowed
sinc, 14 D + 1 taps, the search uses +-0.4 x that rate) - its PSS fold and
SSS tests don't grow with the input - while the channel estimation, the
bearings and the broadcast channels use the whole rate: a carrier inside the
VFO is read whole (every SIB from one transmission). Tested on synthetic
cells at 15.36 MS/s (LTE, a 10 MHz carrier 1.3 MHz off the VFO centre:
bearings 40.0 / 199.9 / 300.0, truth 40 / 200 / 300) and 9.6 MS/s (NR, the
SS/PBCH block 0.8 MHz off: 30.0 / 150.0 / 260.5, truth 30 / 150 / 260), and
on the live capture below upsampled to 9.6 MS/s (MIB, SIB1, SIB2 as at 2.4
MHz). Not done for it: the CPU - mixing every search candidate and the
channel estimation run at the whole rate (~0.6-2.6 s per burst at 9.6 MS/s
on a Pi 5, real time needs < 1 s), so a wide-band receiver needs a faster
computer or that work moved onto the decimated copy / fewer subframes; and
the host side (heimdall, kraken_doa's VFOs, the plugin pipe) is built for
the RTL-SDRs' 2.4 MS/s.

## Tests

`tools/cell_gen.cpp` synthesises LTE / NR cells on one carrier: per cell
its bearing (+ an optional reflection), power, frame timing, carrier offset,
CRS ports or SS/PBCH beams, as received on kraken_doa's UCA through a 2.4
MHz VFO, written as a multichannel cf32 + .json sidecar. The OFDM symbols
are built at 2.4 MHz directly (fractional cyclic prefixes and path delays as
phase ramps: exact), only the subcarriers inside the VFO's +-1.15 MHz.

    g++ -std=c++20 -O2 -Icells -Isdk -I../external/pffft cells/tools/cell_gen.cpp lib/build/libkrakendig.a -o /tmp/cell_gen
    /tmp/cell_gen --secs 4 --rf 806.13e6 --lte_off -130e3 --snr 15 \
        --cell lte,101,40,0,0,20,2 --cell lte,102,200,-4,1234.5,-15,1 --out /tmp/cells.cf32
    cells/build/decoder --file /tmp/cells.cf32 --uca 0.1582

`--cell TECH,PCI,AZ,DB[,TOFF_US[,CFO_HZ[,PORTS_OR_BEAMS[,R_AZ,R_DELAY_US,R_DB]]]]`,
TECH `lte` (FDD), `ltetdd` (TDD configuration 1) or `nr`; `--lte_off` /
`--nr_off` place the carrier / SSB against the VFO centre, `--rxoff` is the
receiver's frequency error, `--load` the data fill, `--fs` / `--vis` the
rate and the generated half-width (a wider receiver). (The generator doesn't
make PBCH / PDCCH / PDSCH yet: the broadcast information was tested on the
live network, below.)

Cell detection and bearings (`decoder --file ... --uca R`, 3-12 s
recordings, 5 antennas):

| Recording | Bearings (truth) | Result |
|---|---|---|
| 3 LTE FDD cells, unsynchronised, VFO 130 kHz off the carrier, 2-port cells, a reflection | 40.0 / 199.6 / 299.8 (40 / 200 / 300) | all; powers -4.2 / -8.7 dB (-4 / -9), frame timings exact, ports right |
| 5 synchronised LTE TDD cells: 3 sectors of one site (one N_ID1), one sharing an N_ID2 at +3.3 us, one -14 dB | 60.0 / 179.8 / 299.9 / 99.0 (60 / 180 / 300 / 100) | 4 of 5; the -14 dB one (same N_ID2 as a -10 dB cell, 16 samples later) missed |
| 3 NR cells (15 kHz), two co-timed with the same N_ID2, 1-4 beams | 30.0 / 149.9 / 260.0 (30 / 150 / 260) | all |
| LTE at 3 dB per-subcarrier SNR: cells 0 / -6 / -12 dB | 20.0 / 110.2 (20 / 110) | the -12 dB cell is below the threshold |
| LTE cells with colliding CRS (same PCI mod 6 and N_ID2) | 70.0 / 250.0 / 159.9 (70 / 250 / 160) | all |
| LTE carrier + NR SS/PBCH block in one slice (DSS-like, one subcarrier grid) | 50.0 / 170.1 / 290.4 (50 / 170 / 290) | all |
| noise only | - | no cell |
| a reflection 4 dB stronger than the direct path (from 80 deg) | 200.0 strongest path, 80.3 with "first path" | as intended |
| the receiver 12 kHz off (15 ppm) | 40.0 / 199.9 (40 / 200) | found in the +1 subcarrier sweep, "Frequency offset +12009 Hz" |

Live pipeline: a test build of kraken_doa fed by a fake heimdall streaming
recordings in heimdall's 8091 format: the talkers' bearings within 0.1 deg,
the carrier scan (stepped 809.2, 806.8, 810.4, 805.6 MHz from 808.0, centred
806.000), the page (cells in the DF list, table row / plot marker picks, the
locked VFO, the ▸ row details).

**Live network** (2026-10-14, 954.8 MHz, LTE band 8, 10 MHz carrier; 6 s
and 20 s coherent captures of all 5 antennas, offline and streamed through
the test build): two FDD cells (PCI 191 and 253, 2 antenna ports each) -
MIB in every frame (30 of 30 per burst: 50 RB, 2 ports, PHICH normal 1/6,
the same SFN on both - a synchronised network); the SI-RNTI DCI (format 1C,
aggregation 4, CCE 12) from the ~23 % of the control region inside the VFO,
93-99 % of its re-encoded bits agreeing; **SIB1** after 2-8 transmissions
(PLMN 530-01 New Zealand, TAC 41776, eNB 5330 cell 12 / eNB 42599 cell 11,
band 8, q-RxLevMin -130 dBm, p-Max 23 dBm, SIB2 / SIB3 / SIB5 every 160 /
160 / 1280 ms, window 20 ms, IMS emergency); **SIB2** after 3-14
transmissions (its two transmissions per window carry RV 2 and RV 0) with
the uplink on EARFCN 21748 = 3748 + 18000 (band 8's duplex spacing, 909.8
MHz) - a field at the end of a long chain of SIB2 structures, so the whole
chain parsed right; RACH, paging, PRACH, reference signal power 19 dBm, UE
timers. SIB3 / SIB5 not (sent once per window, RV 0; 36-40 % of their code
inside the VFO).

## Sources

Written from the 3GPP specifications: TS 36.211 (LTE physical channels: 6.2
resource grid / REGs / VRB-to-PRB mapping, 6.3 scrambling / modulation /
layer mapping / precoding, 6.6 PBCH, 6.7 PCFICH, 6.8 PDCCH, 6.9 PHICH, 6.10.1
CRS, 6.11 synchronisation signals, 7.2 pseudo-random sequences), TS 36.212
(5.1 CRCs, segmentation, convolutional and turbo coding, rate matching; 5.3.1
BCH, 5.3.3 DCI formats, 5.3.4 CFI), TS 36.213 (7.1.6 / 7.1.7 resource
allocation and transport block sizes, 9.1 search spaces), TS 36.321 (5.3.1
the RV of broadcast transmissions), TS 36.331 (MIB, SIB1, SystemInformation,
SIB2 - SIB5; unaligned PER - ITU-T X.691), TS 36.101 5.7.3 (bands, EARFCN),
TS 38.211 (NR: 7.4.2 PSS / SSS, 7.4.3 SS/PBCH block), TS 38.213 4.1, TS
38.104 5.4 (NR-ARFCN, sync raster, GSCN), ITU-T E.212 (MCC). The QPP and
transport block size tables (`lte_tables.hpp`) were generated from the
standard's tables (as transcribed in srsRAN's sources, checked: every QPP
pair is a permutation; one TBS entry differing from 36.213 corrected).
