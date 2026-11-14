# ILS / VOR / marker plugin

Moved verbatim from `kraken_doa_v2/CLAUDE.md` on 2026-10-24 (that file now keeps
only what every kraken_doa session needs, plus an index of these files). Not
loaded automatically. *Italic* topic names refer to the other files in this
folder (index: `README.md` here, or the table in `kraken_doa_v2/CLAUDE.md`);
"above" / "below" meant elsewhere in the old file.

**ILS / VOR / marker (`plugins/ils/`, README.md there):**
- 48 kHz (`min_vfo_rate` 48 kHz: the VOR subcarrier reaches +-10.4 kHz).
  Every 0.5 s the last 1 s: one complex FFT (1 Hz bins); carrier = the
  strongest bin within +-20 kHz >= 15 dB over the median (Hann power from
  the unwindowed bins: 0.5 X[k] - 0.25 X[k-1] - 0.25 X[k+1]); a second
  carrier 4-32 kHz away, not at +-9960 Hz, a line (30x what is 30 Hz
  aside), >= 20 dB, carrying 90 / 150 Hz too = two-frequency ILS
- `demod`: the carrier's bins +-bw (12 kHz; sep - 1.5 kHz next to a second
  carrier) -> 24000-point IFFT -> envelope -> m = e / mean - 1 -> r2c FFT:
  tone depth = 2 sqrt(sum |M|^2 over +-w bins) / N (30 / 90 / 150 / 400 /
  1020 / 1300 / 3000 Hz, 9960 +-800). VOR: bins 9960 +-900 -> 2048 IFFT ->
  instantaneous frequency (times between samples) -> DFT at 30 Hz = the
  reference phase; radial = arg(R) - arg(M[30]) (the variable lags the
  reference by the radial); circular 1 s blocks hold whole 30 Hz cycles,
  so no filter delays; averaged as vectors (0.8 / 0.2)
- Type (`judge`, three agreeing blocks): VOR (9960 Hz > 10 %, 30 Hz > 8 %,
  FM 250-800 Hz), ILS (SDM 0.2-1.2, a tone > 8 %, no 30 Hz; LOC / GS by
  the RF band, else SDM < / >= 0.6), marker (a keyed 400 / 1300 / 3000 Hz
  tone > 30 %; within 0.5 MHz of 75 MHz when the RF is known - airband
  voice has keyed-looking 400 Hz bursts), else "AM carrier". CPU (Pi 5):
  0.7 % of a core
- Keying: 10 ms Hann Goertzel ticks of the middle 0.5 s of each block
  (contiguous with the 0.5 s hop); `Keyer`: 98th / 15th percentile of 30 s,
  keyed when >= 6x apart (noise alone ~5x) and >= 2 %, hysteresis; the
  1020 Hz Morse starts after a 1.2 s gap once keyed (else the first ident
  is cut); markers' / glide slopes' keying never makes an ident. Facts
  (1 s): navaid, frequency + pairing (`nav::` tables), carrier, ident,
  modulation (per type), course / glide path (DDM, side, needle uA, fly
  left / right / up / down, SDM), second carrier, radial (+ the station's
  bearing; true with the `magvar` option), marker, audio (LOC / VOR only).
  `valid(t0, t0 + 1)` per recognised block
- DoA per navaid (ID separation, `talker`): LOC / VOR blocks become
  packets of "<ident> LOC|VOR" while the ident is fresh (30 s, same carrier
  within 50 Hz) - see *DoA per talker*. Generator `tools/ils_gen.py`
  (LOC DDM / VOR radial + keyed 1020 Hz ident)
- Tests (scratchpad dme/gen_nav.py, t_vor.py): see the README

