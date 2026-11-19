# NXDN decoder plugin

NXDN (Kenwood NEXEDGE / Icom IDAS), NXDN96 (4800 Bd) and NXDN48 (2400 Bd)
searched at once: frame sync, LICH, SACCH (CRC-6), FACCH1 (CRC-12), UDCH /
FACCH2 (CRC-15) and the outbound control channel's CAC (CRC-16), all with
the punctured rate-1/2 K=5 code, and the frame scrambler; layer 3: VCALL
(unit / talkgroup, cipher), TX_REL, TX_REL_EX, DISC, data calls, and the
Type-C trunking control channel (site / service / control channel /
adjacent site information, voice and data channel grants, registrations
with Verbose). Voice: AMBE+2 (half rate) through a user-installed mbelib;
9600 bps EFR (full-rate) calls are not played. Details:
`kraken_doa_v2/docs/dev/digital-decoders.md`.

Written from the NXDN Forum's Technical Specifications TS 1-A (Common Air
Interface) and TS 1-C (Trunking Procedures, Type-C), version 1.3 (November
2011) - the comments cite their sections.

## Trunking (Type-C) - trunk following
On a control channel (LICH RF channel type RCCH, outbound) the plugin
reports, besides its facts and events:
- `trunk_system`: key = category + system code of the Location ID
  (`global.123`, `regional.4000`, `local.70001` - the same on every site),
  site = `site <code>`, `control` on an RCCH or a composite control channel;
  from SITE_INFO / SRV_INFO: at once after a `reset()` or a change, again
  every 5 s, and ahead of a grant when it wasn't sent in the last 0.5 s
  (the host may have dropped it - Auto detect's lead changing - without a
  reset; a dual-format CAC's SRV_INFO is handled before its grant).
- `trunk_channel`: the site's 1st / 2nd control channel (SITE_INFO), the
  control channels CCH_INFO announces (Curr / Add) and the adjacent sites'
  control channels (ADJ_SITE_INFO, same system only).
- `control_channel`: CCH_INFO with "New" for this site (the control channel
  moves, TS 1-C 16.5).
- `grant`: every VCALL_ASSGN / VCALL_ASSGN_DUP (update) and DCALL_ASSGN /
  DCALL_ASSGN_DUP (data), from the CAC in the single or the dual message
  format, and from FACCH1 / FACCH2 / a SACCH superframe of a traffic or
  composite control channel. Group = call type broadcast / conference;
  individual and telephone calls are unit calls; `emergency` from the CC
  Option. The grant carries no cipher type (encryption shows on the
  traffic channel). Options for the traffic channel: `ran` (the control
  channel's RAN - every channel of a site uses it) and `rate` (4800 / 9600
  from the Transmission Mode).
- No `grant_end`: a Type-C control channel announces no clear-down (DISC
  there cancels a request and names no channel); it stops repeating the
  _DUP.

Channel numbers: Type-C channels are numbers 1-1023 whose frequencies are
programmed into the radios (TS 1-C 6.1.5), so every channel is `CH <n>`
(number n) with no frequency - the trunk channel table supplies it. Direct
frequency assignment is not in TS 1-A 1.3 (its Channel Access Information
is all spare) and is not decoded.

On a traffic channel (RTCH, RTCH_C) `call()`: ACTIVE from the VCALL (FACCH1
at the start, the SACCH superframe for late entry; target / group / source,
`encrypted` = a cipher type, `emergency`), repeated once a second; ENDED at
TX_REL, at TX_REL_EX (the TC's hang time - talk-back permitted or not, TS
1-C 16.4) or when no VCALL / voice frame came for 1 s; RELEASED at DISC (the
TC releases the channel, TS 1-C 5.5.2). A new VCALL with other IDs is a new
call. Conventional channels (RDCH) report no calls.

Hidden options (set by a grant): `ran` - only frames whose SR names this RAN
(a frame without one takes the verdict of the frames right before it), so a
co-channel site is ignored; `rate` - only NXDN48 (`4800`) or NXDN96
(`9600`). Both take effect at once; `""` = any.

Not covered: Type-D (TS 1-E / 1-F: no control channel; home repeaters'
SCCH, a different frame layout) - not decoded. The control channel has
NOT been seen on air: everything above is checked against the
specification and the plugin's generator only.

## ID separation (DoA per unit)
Each call is a talker (`host.talker`): the VCALL's source unit ID (FACCH1 or
the SACCH superframe), label "TG n" / "unit call to n" (+ " · via repeater"
on an outbound channel), from its first frame to the TX_REL; every unit gets
its own bearing from its own calls. On a repeater's output (a trunked
traffic channel too) every unit's bearing is the repeater's.

## Tests
Generator `tools/nxdn_gen.py`, written from TS 1-A / 1-C (the scrambler
from its polynomial - it equals the decoder's table -, CRCs preset to ones,
the puncturing matrices and interleave depths of 4.5): conventional or
trunked (`--trunk 1` RTCH, `2` RTCH_C) calls - header frame with VCALL in
both FACCH1 halves, voice frames with the VCALL in the SACCH superframe,
TX_REL, then optionally the TC's TX_REL_EX hang (`--hold`, with
VCALL_ASSGN_DUP in FACCH1: `--dup`) and DISC (FACCH1 or FACCH2: `--disc 1|2`)
-, and a Type-C control channel (`--control`: 9-frame superframe, SITE_INFO
/ SRV_INFO / ADJ_SITE_INFO, grants as VCALL_ASSGN x 3 then _DUP every
0.5 s, CCH_INFO "New"), NXDN96 or NXDN48 (`--bps 4800`); RRC 0.2 4FSK.

Conventional scene: the real unit 1203 of dsd-samples
`nxdn96_3_48000.raw` (FM-modulated) at 80°, then generated units 2001
(190°) and 2002 (330°) and 2001 again, back to back: 80.0 / 190.0 / 330.0°
offline (`--channels 5 --uca 0.12`) and through kraken_doa. dsd-samples'
recordings decode as before (2026-11-20, 2.1 against 2.0: `nxdn96_1/2/3`
discriminator files and the `_78125` / `_234375` IQ files - frames, events,
talkers identical; the catalogue scene `nxdn-two-units` (2001 at 190°,
2002 at 330°) 210 -> 214 valid frames: a false
sync inside a frame whose LICH read as UDCH reset the SACCH lock and lost
the next real frame, now the UDCH path checks its CRC-15 and leaves the lock
alone; bearings unchanged). Generator through an independent decoder
(2026-10-16): dsdcc's `dsdccx` (f4exb, GPL - built in scratch, run only;
`-fn`, the output FM-demodulated to 48 kHz at 25 dB SNR) reads
`NXD>H RD 01 01 01203>G00012` (RAN 1, unit 1203 -> TG 12) and `...
04567>G00089` for a second call alone, inbound and outbound alike; two calls
1 s apart in one file: it reads only the first (it doesn't resync after the
TX_REL - the plugin reads both).

Trunking (2026-11-20, plugin 2.1). Wide scene `nxdn-trunk-two-calls` (2.4
MHz, 5 antennas, 20 dB): control channel CH 88 (+300 kHz, 300°), TG 100 <-
2001 granted at 1.5 s on CH 32 (-400 kHz, 40°), AES-encrypted TG 200 <- 2002
at 7.5 s on CH 116 (+650 kHz, 200°). Offline at `--offset 300000`: the
system, CH 88 and the adjacent site's CH 3, and 22 of 22 grant messages
(each 3 ASSGN + 8 _DUP), the first 0.06 / 0.14 s after its time, every
channel / target / source / option as generated; at -400000 / 650000 with
`--opt ran=9 --opt rate=9600`: ACTIVE at 1.84 / 7.84 s (0.28 s after the
grant was decoded), ENDED at TX_REL (4.76 / 10.76 s), RELEASED at DISC
(5.80 s on FACCH1, 11.80 s on FACCH2), talkers 2001 at 40.0° and 2002 at
200.0°, `flags=e` on the second. `tools/digi_test` (the real engine): 22
GRANTs, SYSTEM, the CHANNELs, 375 / 375 frames; on each traffic channel
CALL ACTIVE, ENDED, RELEASED in that order. NXDN48 (`--bps 4800`): control
channel grants with `opt.rate="4800"`, the call ACTIVE / ENDED / RELEASED
with `rate=4800`, nothing with `rate=9600`. VCALL_ASSGN_DUP in the hang
frames of an RTCH and an RTCH_C: 12 / 12 grants (updates). Co-channel: two
sites' calls (RAN 9 and RAN 10) on one channel - `ran=9` 115 frames and
only its calls, `ran=10` 63 and only the other's, none 178.

Robustness of the control channel (NXDN96, C/N in 12.5 kHz, 3 noise seeds
each, grants of 22 / frames of 375): 12 dB and up all; 10 dB 21.7 / 374; 8 dB
20.3 / 359; 7 dB 18.7 / 328; 6 dB 14.7 / 242; 5 dB 6.3 / 98; 4 dB 1.0 / 15.
Carrier +-500 / +-1000 Hz: all 22 at 20 dB. Never a false grant: 600 s of
noise (0 frames), 112 of 375 frames hit by 2 ms bursts at 3x the signal (19
grants, all right), 200 random cuts of the file. Every grant comes from a
message that passed its CRC: CAC CRC-16, FACCH1 CRC-12, FACCH2 CRC-15, or a
SACCH superframe whose 4 fragments passed CRC-6 each and came from 4
consecutive frames in order. Decode time 1.5-1.8 % of real time (x86).

No NXDN control channel or trunked traffic channel has been received off
air for these tests (none on air at the receiver, 2026-11-20); no published
CAC test vector was found.
