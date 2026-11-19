# TETRA downlink decoder plugin

TETRA (EN 300 392-2) base-station downlink, 25 kHz: pi/4-DQPSK at 18 ksym/s,
the continuous carrier of 255-symbol timeslots. 72 kHz input, RRC 0.35
matched filter, differential detection, slot sync from the training
sequences (both spectral orientations), 4th-power frequency estimate
(sync within about +-2.2 kHz of the VFO; the host's AFC re-centres it).

Decoded: BSCH (MCC, MNC, colour code, TDMA time), BNCH SYSINFO (main carrier,
band, offset, duplex spacing, location area, services), AACH (timeslot
usage, traffic usage markers), and the MAC PDUs of every signalling channel
(MCCH / SCCH on SCH/F and SCH/HD, the STCH stolen from a traffic slot):
MAC-RESOURCE with the whole header - addresses incl. event label and usage
marker assignments, power control, slot granting, the channel allocation
element (allocation type, timeslot bit map, up / downlink, CLCH permission,
cell change, carrier, extended carrier numbering, monitoring patterns, the
augmented fields) - then its TM-SDU; fragmented TM-SDUs (MAC-RESOURCE start
+ MAC-FRAG + MAC-END, the MAC-END's own allocation) reassembled per
timeslot; the basic link (BL-ADATA / DATA / UDATA / ACK, the FCS checked when
present) -> MLE -> MM PDU names and the CMCE call control PDUs with their
elements: D-SETUP (call identifier, time-out, hook, simplex / duplex, basic
service information: circuit mode, end-to-end encryption flag, communication
type, calling party), D-CONNECT, D-CONNECT ACKNOWLEDGE, D-CALL PROCEEDING,
D-ALERT, D-INFO, D-TX GRANTED / INTERRUPT (transmission grant, transmitting
party, encryption control), D-TX CEASED, D-TX WAIT, D-TX CONTINUE,
D-CALL RESTORE, D-DISCONNECT / D-RELEASE (cause); type-3 elements skipped by
their length. Voice: ACELP through the user-built ETSI codec (README
"Digital voice codecs"); calls with air-interface or end-to-end encryption
are muted. Details: `kraken_doa_v2/docs/dev/digital-decoders.md`.

Facts beside the cell's: "Channel allocations" (heard / parsed to the end /
with call control / air-encrypted), "Grants", "Not parsed" (PDUs the decoder
skips, by kind and count), "Trunk following" (shown when air-interface
encryption makes following impossible), "Traffic timeslot" (on a traffic
instance).

## Trunking (grants, calls)
`Info::trunking`. The system is the network: key "MCC-MNC" ("530-1"), site
"LA n CC c", `control` = no traffic timeslot set and timeslot 1 of this
carrier carries the MCCH (AACH common control); sent at the first BSCH, on
a change and every 5 s. The main carrier from SYSINFO is a `trunk_channel`
("3600/1", slot 0); a VFO that hears the cell on another carrier (its
`rf_hz()` more than 6.25 kHz from the main carrier, no MCCH on timeslot 1)
sends `control_channel` once.

**Grants.** Every channel allocation element (MAC-RESOURCE or MAC-END)
whose TM-SDU is a CMCE call control PDU - D-SETUP, D-CONNECT, D-CONNECT
ACKNOWLEDGE, D-TX GRANTED, D-CALL RESTORE, D-INFO, D-ALERT, D-CALL
PROCEEDING - becomes a `grant()`; an allocation with an SNDCP PDU a data
grant; one without a TM-SDU or with timeslot bit map 0000 (back to the
MCCH) none. Channel: `name` "<carrier>/<timeslot>" (TETRA timeslots 1-4,
"3600/3"; several assigned timeslots "3600/2+3", `slot` = the first;
"<band>:" in front when the element's extended numbering names another band
/ offset), `number` = the carrier, `freq_hz` = base (band x 100 MHz) +
carrier x 25 kHz + offset (+ the augmented bandwidth's centre) with
SYSINFO's band / offset or the element's extended ones (NAN before
SYSINFO), `uplink_hz` from the duplex spacing table of ETSI TS 100 392-15
(band x code; normal = minus, reverse = plus; NAN for 0 MHz / reserved
codes), `slot` 0-based. A call table by call identifier (64 calls, the
least recently heard go first) carries the call across PDUs: `target` =
the SSI the D-SETUP is addressed to (the GSSI of a group call; a D-CONNECT
ACKNOWLEDGE's addressee for an individual call), `group` from the
communication type (unknown: group), `source` = the D-SETUP's calling
party, the D-CONNECT's addressee (the caller), or the D-TX GRANTED's
transmitting party; `encrypted` = the basic service's end-to-end encryption
flag or D-TX GRANTED's encryption control; `emergency` = call priority 15
(pre-emptive 4); `data` = circuit mode data. `update` = the call was
granted on this channel before (back-up D-SETUPs, late entry, a D-TX
GRANTED for the next talker). `grant_end` at D-RELEASE of a granted call.
Options for the traffic instance (hidden): `timeslot` (1-4), `mcc` / `mnc` /
`cc` (the scrambling identity), `call` (call identifier), `group` (0 / 1).

Checks before anything counts: the block's CRC-16 (and for a reassembled
TM-SDU every fragment's; a fragment lost to a failed CRC drops the whole
TM-SDU), the MAC length indication (the PDU inside its block, fill bits
with their marker), the allocation element complete, the LLC FCS (CRC-32)
when the basic link has one, and the CMCE PDU's elements ending EXACTLY
where the TM-SDU does (every one of the 14 CMCE PDUs in the real recordings
does; a PDU cut by 1 to 24 bits inside a valid block gives no grant, none
of 17 such in a test). Air-interface encryption (MAC encryption mode != 0)
encrypts the channel allocation and the TM-SDU: such PDUs give no grant
and set the fact "Trunk following: not possible".

**Traffic instance** (`timeslot` set - the voice VFO trunk following puts
on a granted channel): it plays only that timeslot (end-to-end encrypted
calls muted), reports valid frames (its TCH slots included) and cell
packets only from it and only while the AACH marks it assigned (the filler
blocks of an unallocated slot keep no voice VFO alive: 0 valid frames on
the real recording's free timeslot 3, 576 on its traffic timeslot 2), sends
no grants from other timeslots, and reports `call()` from the timeslot's own
signalling: ACTIVE at the first TCH/S slot (AACH traffic usage marker,
Reed-Muller checked) or D-TX GRANTED (source = the transmitting party), a
new ACTIVE when another party is granted, repeated once a second; ENDED at
D-TX CEASED / WAIT / INTERRUPT, at the AACH's "assigned control" (hang
time) after the last traffic slot, or 1 s without traffic; RELEASED at
D-RELEASE on the timeslot or two AACHs "unallocated". Target / source only
from what it decodes itself (a call it heard set up on the MCCH - a
traffic slot of the main carrier - by the `call` option). The scrambling
options let it decode before its first BSCH; the timeslot NUMBERS come
from the BSCH (frame 18 of every carrier, at most 1.02 s). Without the
option (a user VFO, the control VFO) the decoder plays the first timeslot
carrying traffic as before - with trunk following on, the control VFO does
not mute itself (it cannot see the host's switch); it is only heard while
someone listens to it.

## ID separation (DoA per cell, per call)
The downlink is the base station's, so its identity is the CELL: id
"MCC-MNC CCn" (e.g. `530-1 CC12`), label "LA n · carrier MHz" once SYSINFO is
heard. The colour code exists to tell co-channel cells apart - it seeds the
scrambling, so another cell's blocks fail their CRC here - and every slot
whose block passed its CRC under this cell's code (or that carried its BSCH)
is a talker packet of the whole slot (`RxContext::packet`, the matched
filter's delay taken off), in the 25 kHz channel, averaged over 10 s, kept 30
min. Cells sharing a carrier in frequency reuse, heard in turn as
propagation changes or while driving, keep their own bearings (🗼 rows in the
🗺 Map's 📡 DF panel). Only the strongest cell on a carrier is decoded at a
time. On a traffic instance each transmission is also a talker: id = the
transmitting party's SSI ("?" until a D-TX GRANTED names it), label "TG
<GSSI>", `Talker::channel` = the timeslot - 1, from its first to its last
traffic slot, so Voice to Text names the radio. Its bearing is still the
base station's (a downlink); mobiles (uplink) and direct mode (DMO) are not
decoded.

## Tests
Generator `tools/tetra_gen.py` (one cell's carrier with real framing: SB with
BSCH + BNCH SYSINFO in frame 18, SCH/F normal bursts, AACH Reed-Muller, CRC,
RCPC 2/3, interleaver, scrambler, RRC 0.35): every slot decodes. Two cells
of one network on one carrier (CC12 at 70°, CC33 at 235°, 4 dB weaker, heard
in turn; `../sdk/tools/uca_scene.py`): 70.0 / 235.0° offline
(`--channels 5 --uca 0.12`) and through kraken_doa (fake heimdall, 50 kHz
VFO). ~1.6 % of real time on a Pi 5. Unchanged by the trunking work
(2026-11-20: the same 839 valid frames, 382 / 409 packets, 70.0 / 235.0°).

Trunked site (2026-11-20; `tetra_gen.py --seconds 15 --call ...`, catalogue
`tetra-trunk-two-calls`): main carrier 3600 (390.0 MHz) with the MCCH and
call 101 (TG 9001) on its timeslot 3 - 2001001 1.3-4.5 s, 2001002 5.5-7.5 s,
D-RELEASE after the hang time at 8.3 s; carrier 3580 (389.5 MHz) with call
102 (TG 9002, end-to-end encrypted) on timeslot 2, 6.3-11.5 s, D-TX CEASED +
D-RELEASE in the stolen halves of its last slot; back-up D-SETUPs every 2 s,
the first of each call fragmented (MAC-RESOURCE + MAC-END, BL-UDATA with
FCS). Offline, the control channel: 10 grants (2 + 8 updates) at 1.03 /
1.09 / 3.07 / 5.05 / 5.45 / 7.04 s (3600/3, 390.000000 MHz, up 380.000000, slot 2,
opt.timeslot=3) and 6.02 / 6.07 / 8.11 / 10.04 s (3580/2, 389.500000 MHz,
slot 1, flags e), grant end 3600/3 at 8.31 s; each grant 0.3 s ahead of
its call. Traffic instances with the grants' options: 3600 timeslot 3
ACTIVE 1.34 (2001001) / ENDED 4.46 / ACTIVE 5.59 (2001002) / ENDED 7.46 /
RELEASED 8.31 s, talkers 2001001 1.33-4.45 s and 2001002 5.58-7.45 s;
3580 timeslot 2 ACTIVE 6.31 (2001003, flags e, muted) / ENDED + RELEASED
11.47 s. In the 5-antenna 2.4 MHz scene (`sdk/tools/wide_scene.py`, main
carrier +300 kHz at 60°, 3580 -200 kHz at 200°): the same lines, talkers
2001001 / 2001002 at 60.0°, 2001003 at 200.0°. `tools/digi_test` (the
real engine) delivers the same SYSTEM / CHANNEL / GRANT / GRANT_END and
CALL reports; after a retune (`--retune 9:390000300`) the RESET, then the
system again 2.3 s later. Control channel at reduced SNR (noise over 72 kHz,
SNR in 25 kHz): all 10 grants down to 12 dB, 9 at 10-9 dB, 6-8 at 8-6 dB,
none at 5 dB (no sync); never a wrong channel, target or frequency. +-1 kHz
offset: all 10. Noise only: nothing. `--truncate 1..24` (every CMCE PDU cut
inside a valid block): 0 grants, 0 grant ends, the PDUs counted as "Not
parsed". A RESET part-way (a retune): the 2.1 receiver did not re-acquire
the clean generated carrier at all (the search window kept its place in the
slot grid; fixed: it moves half a slot).

Real recordings (verified against them, not in this repo):
sigidwiki's "TETRA" IQ (SDR#, 48 kHz, 41.7 s, a downlink): MCC 228 / MNC
8889, colour code 1, SYSINFO main carrier 426.7000 MHz (carrier 1068), LA
1, the services, timeslot usage, D-TX-GRANTED / CEASED on call 113 - 4892
blocks that passed their CRC. Every block but the BSCH is scrambled with
the code made of MCC + MNC + colour code, so thousands of CRC-valid blocks
also confirm the network identity read from the BSCH. Trunking
(2026-11-20): 8 MAC-RESOURCEs with channel allocations, all 8 parsed to
the end - D-CONNECT to the caller (twice, 10.68 / 10.73 s) and D-SETUP to
the group (10.79 / 10.85, back-ups at 25.64 / 25.69 and 40.65 / 40.71 s),
all to carrier 1068 timeslot 2 (426.7000 MHz, uplink 416.7000): the
timeslot the AACH marks as traffic (usage marker 51). On it (traffic
instance, timeslot 2): TCH from 10.98 s (0.3 s after the D-CONNECT), the
STCH D-TX CEASED of call 113 at 25.54 s (ENDED), the next talker's D-TX
GRANTED on the timeslot at 28.83 s (ACTIVE with that source, the group the
D-SETUPs named), traffic to the end; 27.4 s of ACELP audio from the codec
(not listened to here). Every one of its 14
CMCE PDUs ends exactly at its TM-SDU's end; nothing in "Not parsed".
GopherTrunk's real-air control channel captures (Apache-2.0, 1.5 s at
50 kHz and 2 s at 144 kHz, MCC 250 / MNC 13 / CC 44): the same 210 / 213
valid blocks as before, no channel allocation in them. No independent
decoder was built for the generator (osmo-tetra needs libosmocore and its
GNU Radio demodulator); the generator's framing / coding / pi/4-DQPSK at
18 ksym/s RRC 0.35 is decoded by the same receiver that reads the real cell.
Not verified on air: the fragment reassembly, the FCS, extended carrier
numbering, augmented allocations, D-RELEASE (no release in the
recordings), individual calls, air-interface-encrypted cells.

## Sources
ETSI EN 300 392-2 V3.8.1 (air interface: bursts, coding, scrambling, BSCH /
SYSINFO / ACCESS-ASSIGN, MAC-RESOURCE / FRAG / END, the channel allocation
element, the basic link and its FCS (annex C), the CMCE PDUs, annex E
encoding rules), ETSI TS 100 392-15 V1.5.1 (frequency bands, duplex
spacings). osmocom-tetra used as documentation only.
