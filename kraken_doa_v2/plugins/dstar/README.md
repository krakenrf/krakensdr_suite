# D-STAR decoder plugin

D-STAR (JARL) digital voice, GMSK 4800 bit/s: the RF header (scrambler,
24-column interleaver, rate-1/2 K=3 code, CRC-16/X.25: MY / UR / RPT1 / RPT2
callsigns, flags), the slow data of the voice frames (text message, GPS /
DPRS -> map points, header copy), the end pattern. Voice: AMBE through a
user-installed mbelib (README "Digital voice codecs"). Details:
`kraken_doa_v2/CLAUDE.md`.

## ID separation (DoA per callsign)
Each transmission is a talker (`host.talker`): the MY callsign of its RF
header (or of the header copy in the slow data on a late entry), from the
header to the end pattern / the last data sync in the 420 ms cadence; label
"/suffix to UR via RPT1". Every callsign gets its own bearing from its own
transmissions. On a repeater's output every callsign's bearing is the
repeater's.

A header 756 bits before a data sync always starts a new transmission (since
2026-10-15): before, a transmission that began within two superframes of one
that ended without its end pattern was taken for a false sync inside the old
one and came up without a talker. An end pattern in noise long after a
transmission no longer logs "End of transmission".

## Tests
Generator `tools/dstar_gen.py` (preamble, frame sync, RF header with real
FEC / CRC, voice frames with data syncs and scrambled slow data, end
pattern). Scene: the real F1NSR transmission of dsdcc's `dstar_f1zil_1.dis`
(FM-modulated) at 60°, then generated ZL1ABC (200°), ZL2XYZ (310°) and ZL1ABC
again, back to back: 60.0 / 200.0 / 310.0° offline (`--channels 5 --uca
0.12`) and through kraken_doa (also with the FFT squelch at 10 dB and with
the Digital squelch). dsdcc's samples decode as before.
