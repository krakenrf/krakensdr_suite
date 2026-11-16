# Yaesu System Fusion (YSF / C4FM) decoder plugin

Yaesu System Fusion amateur digital voice, written from Yaesu's "Amateur
Radio Digital Standards" (rev. 1.02, 2015-07-29) alone: 4800 Bd C4FM (RRC
0.2, wide and narrow deviation, either polarity), 100 ms frames found by
their frame sync and then followed on the 100 ms grid (a frame whose sync is
too weak is still tried there, 4 misses = out of sync as in 4.7). Per frame:
FICH (deinterleave, soft Viterbi K=5, 4 x Golay(24,12), CRC-16) - nothing is
reported unless it passes; every valid frame is reported with its samples
(Digital squelch) and its carrier offset. Then, by frame and data type:

- Header / Terminator (any data type): CSD1 (destination, source) and CSD2
  (repeater downlink, uplink), two DCH units.
- V/D mode 1: one 160-bit DCH per frame rolling CSD1, CSD2, CSD3 (Rem1-4),
  DT1-5; five 72-bit AMBE+2 frames.
- V/D mode 2 ("DN"): one 80-bit DCH rolling Dest, Src, Down, Up, Rem1+2,
  Rem3+4, DT1-2; five 49-bit voice frames (27 bits x 3 by soft majority +
  22, de-whitened, 26 x 4 deinterleave).
- Voice FR ("VW"): the sub header (FT 1 / FN 0: CSD3 + 2 voice frames), then
  5 x 144-bit full-rate frames.
- Data FR: two 160-bit DCH units per frame (CSD1-3, DT1-13), block number /
  total; the data itself is shown as text when printable, else hex (the
  standard doesn't define its content).
- Every DCH unit: deinterleave, soft Viterbi, CRC-16, PN(9,5) de-whitening.

Facts: mode, source, destination (Radio ID mode: the destination / source
radio IDs), downlink / uplink callsigns, Rem1-4 (downlink / uplink ID, VoIP
relay ID, source radio ID), call type, DG-ID / squelch code, deviation,
message route, VoIP, data, voice codec. Events: "Call: SRC → DST, DN, DG-ID
n, via RPT" once per call (late entry: once the DCH cycle has named source,
destination and repeater) and "Call ended" (terminator, or 1 s without
frames). Two headers in a row (N0 > 1) are one call, a repeated terminator
is no new one.

Voice (only while someone listens): VW through the built-in IMBE decoder -
the standard gives the VW frame only as 144 bits, 88 voice + 56 FEC, at 7200
bit/s, which is the IMBE full-rate frame; where its bits go on air isn't in
the standard - the plugin uses the YSF order as the reference decoders
document it (24 x 6 block, `vw_plain_index` in ysf.cpp; checked against
dsdcc, below). V/D mode 1 and DN through a user-installed mbelib (README:
Digital voice codecs): V/D mode 1's 72-bit frames like DMR's; DN's 49
parameter bits through mbelib's `mbe_processAmbe2450Dataf` (optional: an
mbelib without it means no DN voice, said in the voice status), in the
order received (u0 12, u1 12, u2 11, u3 14 bits, mbelib's own order - the
standard only says "49 bits"; dsdcc does the same).

Debug: `KRAKEN_YSF_DUMP=1` writes every frame's FICH, DCH units, CSD fields,
voice bits as handed to the codec (VW: also its 88 voice bits) and every
sync attempt to stderr.

## ID separation (DoA per station)
Each transmission is a talker (`host.talker`): the source callsign
(trailing spaces off), from the first frame (the header) to the terminator
or a 1 s gap; label "DST · DN" (+ " · via RPT" when CSD2 names a repeater,
or "via repeater" from the message route). The header names the source at
once; a late entry gets it from the DCH cycle (V/D mode 1: every 3 frames,
DN: every 6) and is reported from the transmission's first frame. A Voice FR
call joined after its header / sub header is named only by its terminator
(the standard says so, 4.4). A new source without a terminator ends the
previous talker where it was last named. On a repeater's output every
station's bearing is the repeater's.

## Tests (2026-11, x86_64, offline)
Generator `tools/ysf_gen.py`, written independently from the same standard
(black box here until the dsdcc check below): every frame's FICH word, DCH
unit and voice bits in its JSON sidecar (VW: real IMBE frames, + their 88
voice bits).

- Bit-exact against the sidecar (`KRAKEN_YSF_DUMP=1`): the generator
  engineer's 10 example files - DN, VW, V/D mode 1, DN / V/D mode 1 with
  text, via repeater (CSD2, Rem1/2, MR 1), DG-ID 42, narrow deviation,
  individual (CM 3), Radio ID mode (CM 1), two calls 1 s apart (DN then VW)
  - 217 frames, 969 voice frames; plus one of my own from the same
  generator (V/D mode 1, VoIP + MR 2, DG-ID 99: 17 frames, 75 voice
  frames). Every frame found, every FICH, DCH unit and voice frame equal
  (VW: the 144 air bits and the 88 voice bits, 169 frames), no extra
  frames; each call started and ended once with the fields as set.
  Re-checked from a clean rebuild by a second session (2026-11-16): the 10
  files pass, a DN call at 9 dB SNR and +700 Hz offset 22 / 22 frames.
- VW audio through `ImbeDecoder` on the generator's own files: `vw`
  97 / 97 frames clean (1.94 s), `two_calls` 72 / 72, the catalogue scene's
  VW calls all clean. The frames are placeholders (seeded voice bits, valid
  FEC): a buzz, not speech.
- Coding round trips with injected errors (scattered channel bit errors):
  FICH >= 99.5 % up to 8 errors of 200, 96 % at 11; DCH 160 bits 100 % up
  to 6 of 360, 95 % at 15; DCH 80 bits 100 % up to 4 of 200, 96 % at 10;
  DN voice: one error in each of 5 triplets always corrected. Random input
  passes the FICH checks 1 in 300000 (DCH: 2 / 5 in 300000).
- Sensitivity (10 s calls, AWGN in 12.5 kHz, 3 seeds; frames decoded / DCH
  units / voice frames bit-exact before the vocoder's own FEC):

  | SNR | DN | VW | V/D mode 1 | NXDN96 (nxdn plugin, its generator) |
  |---|---|---|---|---|
  | 12 dB | 100 / 100 / 100 % | 100 / 100 / 100 % | 100 / 100 / 100 % | 99.7 % |
  | 10 dB | 100 / 100 / 99.7 % | 100 / 100 / 98.4 % | 100 / 100 / 99.4 % | 99.6 % |
  | 8 dB | 100 / 99.7 / 92.5 % | 100 / 100 / 79.9 % | 100 / 100 / 88.7 % | 93.6 % |
  | 6 dB | 99.3 / 97.0 / 55.3 % | 99.3 / 88.9 / 16.9 % | 99.0 / 92.4 / 38.2 % | 54.2 % |
  | 4 dB | 71.2 / 50.0 / 10.2 % | 76.5 / 12.5 / 0.4 % | 68.3 / 32.5 / 2.8 % | 7.2 % |

  Narrow deviation (DN): 100 % of frames down to 10 dB, 99.7 % at 8 dB.
  Carrier offset -1500 .. +1500 Hz at 15 dB: 100 % in every mode.
- No false decodes: 10 min of noise (~3 sync candidates a second, none
  passed), the nxdn, p25, dmr (BS and DMO) generators' calls: 0 frames. YSF
  calls in all modes (DN, VW, V/D mode 1, narrow) through the p25, nxdn and
  dmr plugins: 0 frames.
- ID separation: `uca_scene.py`, ZL1AAA (DN, twice) at 40°, ZL2BBB (VW) at
  160°, ZL3CCC (V/D mode 1) at 280°, 20 dB, own offsets / levels, back to
  back 0.3 s apart: `decoder --file scene.cf32 --channels 5 --uca 0.12` -
  40.0 / 160.0 / 280.0°. Catalogue entry `ysf-three-radios`
  (`make_testsignal.py`): 50.0 / 170.0 / 285.0° offline; through the
  debug playback of a scratch kraken_doa test build (no heimdall, 45 s,
  2026-11-16) Check 3 / 3 - the same bearings, each +0.0, 0 packets and 0
  samples dropped, every call's start / end event in the decoder log.
- CPU: 0.7 % of real time (10 min of noise: 4.1 s; the nxdn plugin 6.4 s,
  p25 4.2 s); 1.5 % while decoding VW audio.
- Independent decoder (2026-11-16): dsdcc's `dsdccx` (f4exb, GPL - built in
  scratch without mbelib, run only; `-fy`, the generator's files
  FM-demodulated to 48 kHz) reads every Communication and Terminator frame
  as set, e.g. `YSF>C V2 GC 0:5 WL042|ZL3CCC    >ALL       |...` (DG-ID 42),
  `YSF>T V2 GC 0:0 WL---|ZL1AAA    >ALL       |ZL1RPT    >ZL1RPT    |12345`
  (repeater, Rem4), `YSF>C V2 IN 0:5 NL---|ZL2BBB    >ZL1AAA    |...`
  (narrow, individual), `YSF>C VF GC 0:1 ...|AB123` (the VW sub header's
  CSD3), V1, Radio ID mode `99999:12345`. Headers: read when the call
  starts the file; after 1 s of noise its first FICH fails (its level
  acquisition), also for a header right after another call (1 of 3 read).
  Voice frames, compared bit by bit through a scratch tool linked against
  libdsdcc (its recovered frames before the vocoder; 80 dB, no lead): DN
  174 / 175 equal (49 bits after its majority; one with one bit off), V/D
  mode 1 100 / 100 (72 bits), VW 161 / 169 (its de-interleaved,
  descrambled 144 bits; the other 8: 1-2 bits off, twice in c0's data so
  its descrambling of the rest went wrong - the same frames at 40 and 80
  dB, while the plugin reads every bit of these files: its symbol
  decisions, not the layout). Found with it: the plugin first fed VW frames
  to the IMBE decoder in P25's order - now the YSF order, and `ysf_gen.py`
  sends real IMBE frames in it. dsdcc's own 88 VW bits after its FEC step
  match only 4 / 169: its Golay / Hamming helpers read a codeword last bit
  first while its PN seed and layout take the first bit first, so its
  corrections flip data bits; the first-bit-first reading (the one the seed
  needs for c1..c6 to descramble at all) is the one used here.

Not verified (2026-11): no off-air recording - the only YSF signals were
the generator's, so a misreading shared by Yaesu's document, the generator,
the plugin and dsdcc would go unnoticed. dsdcc confirms the framing (FS,
FICH, every DCH unit, the CSD fields) and where the voice bits sit (DN's
VCH / VeCH, whitening and majority, V/D mode 1's VCH, VW's interleave and
scrambling); the bit order inside the AMBE+2 frames rests on mbelib's
convention, which dsdcc shares. AMBE+2 audio (DN, V/D mode 1) was not
exercised at all - mbelib wasn't installed; VW audio only with placeholder
frames, never with speech.
