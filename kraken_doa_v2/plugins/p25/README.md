# P25 decoder plugin

APCO P25 Phase 1 (TIA-102.BAAA), 12.5 kHz C4FM, on a 12-24 kHz VFO. The
4FSK receiver (`plugins/lib/dig_fsk4.cpp`: integrate-and-dump filtered
discriminator, frame sync correlation, levels / timing / carrier offset from
the sync) hands each frame to `p25.cpp`:

- NID (NAC + DUID, BCH(63,16)); option `nac` = only one NAC
- HDU (Golay(18,6) + RS(36,20,17)): talkgroup, algorithm, key
- LDU1 link control (Hamming(10,6) + RS(24,12,13)): group / unit-to-unit
  voice user (talkgroup, source unit ID, emergency, encrypted); LDU2
  encryption sync (RS(24,16,9)); 9 IMBE frames each
- TDU, TDULC (Golay(24,12) + RS(24,12,13)) - the call's end
- TSDU: trunking signalling blocks (rate-1/2 trellis + CRC-16) - grants,
  channel identifiers (channel numbers -> MHz), RFSS / network / adjacent
  site status, affiliations, registrations; PDU headers
- Voice: IMBE, built in (port of mbelib, ISC); encrypted calls (ALGID not
  0x80 or the LC "protected" bit) are muted, never decrypted

## ID separation (DoA per radio)

A transmission = valid HDU / LDU1 / LDU2 frames less than 0.75 s apart; its
talker = the source unit ID of the LDU1 / TDULC link control (label "TG n"
/ "unit call to n"), reported with the frames' sample boundaries, ended by
the TDU / TDULC, a new HDU or 0.75 s without frames. Each unit ID gets its
own DoA, history and heat map on the 🗺 Map's 📡 DF panel. On a repeater
output every unit's bearing is the repeater's.

## Tests

- Real recordings: the dsd-samples P25 discriminator files (FM-modulated to
  cf32) and a live trunked capture (iq_143.0.u8: two radios, unit 2010621 /
  3250, per-talker DoA 59.9 / 200.0 deg end to end) - see
  kraken_doa_v2/CLAUDE.md *DoA per talker*.
- Generator `tools/p25_gen.py` (HDU, LDU1 / LDU2 with link control and
  encryption sync, placeholder IMBE frames with valid FEC - Golay(23,12),
  Hamming(15,11), the PN whitening; TDU / TDULC; status symbols; C4FM with
  the raised cosine x inverse sinc shaping filter, +-600 / +-1800 Hz) ->
  debug playback entry `p25-three-units` (`tools/testsignals.json`): units
  2010621 (60 deg, twice), 3250 (200 deg, ends with a TDULC) and 1234567
  (300 deg) taking turns on TG 101, each at its own carrier offset and
  level. Offline (`decoder --file scene.cf32 --channels 5 --uca 0.12 --rf
  453612500`, 20 dB SNR): all 58 frames decoded, IMBE 9 / 9 frames clean,
  bearings 60.0 / 200.0 / 300.0 deg; end to end through the playback: see
  kraken_doa_v2/CLAUDE.md *Debug playback*. The generated symbol levels
  match the real recordings' (equally spaced, outer = 3 x inner).

## Sources / limits

TIA-102.BAAA / .AABF / .AABC as published in the open-source P25 community's
documentation (DSD, OP25 - used as documentation only); IMBE and the frame
interleave tables from mbelib / DSD (ISC, notice in `lib/mbe_tables.hpp`).
Not decoded: Phase 2 TDMA, encrypted content.
