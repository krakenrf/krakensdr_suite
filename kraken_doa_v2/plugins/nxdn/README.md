# NXDN decoder plugin

NXDN (Kenwood NEXEDGE / Icom IDAS), NXDN96 (4800 Bd) and NXDN48 (2400 Bd)
searched at once: frame sync, LICH, SACCH (CRC-6) and FACCH1 (CRC-12) with
the punctured rate-1/2 K=5 code, the frame scrambler; layer 3: VCALL (unit /
talkgroup, cipher), TX_REL, data calls, trunking control channel (site /
service information, grants). Voice: AMBE+2 through a user-installed mbelib.
Details: `kraken_doa_v2/CLAUDE.md`.

## ID separation (DoA per unit)
Each call is a talker (`host.talker`): the VCALL's source unit ID (FACCH1 or
the SACCH superframe), label "TG n" / "unit call to n" (+ " · via repeater"
on an outbound channel), from its first frame to the TX_REL; every unit gets
its own bearing from its own calls. On a repeater's output every unit's
bearing is the repeater's.

## Tests
Generator `tools/nxdn_gen.py` (NXDN96 RDCH call: header frame with VCALL in
both FACCH1 halves, voice frames with the VCALL in the SACCH superframe,
TX_REL; real FEC / CRC / scrambling, RRC 0.2 4FSK). Scene: the real unit 1203
of dsd-samples `nxdn96_3_48000.raw` (FM-modulated) at 80°, then generated
units 2001 (190°) and 2002 (330°) and 2001 again, back to back: 80.0 / 190.0
/ 330.0° offline (`--channels 5 --uca 0.12`) and through kraken_doa.
dsd-samples' recordings decode as before.
