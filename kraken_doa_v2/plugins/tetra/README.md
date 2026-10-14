# TETRA downlink decoder plugin

TETRA (EN 300 392-2) base-station downlink, 25 kHz: pi/4-DQPSK at 18 ksym/s,
the continuous carrier of 255-symbol timeslots. 72 kHz input, RRC 0.35
matched filter, differential detection, slot sync from the training
sequences (both spectral orientations), 4th-power frequency estimate.

Decoded: BSCH (MCC, MNC, colour code, TDMA time), BNCH SYSINFO (main carrier,
location area, services), AACH (timeslot usage), the MAC-RESOURCE headers of
the control channel (addresses, clear MM / CMCE PDU types). Voice: ACELP
through the user-built ETSI codec (README "Digital voice codecs"); calls with
air-interface encryption are muted. Details: `kraken_doa_v2/CLAUDE.md`.

## ID separation (DoA per cell)
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
time. Mobiles (uplink) and direct mode (DMO) are not decoded, so radios get
no bearing of their own.

## Tests
Generator `tools/tetra_gen.py` (one cell's carrier with real framing: SB with
BSCH + BNCH SYSINFO in frame 18, SCH/F normal bursts, AACH Reed-Muller, CRC,
RCPC 2/3, interleaver, scrambler, RRC 0.35): every slot decodes. Two cells
of one network on one carrier (CC12 at 70°, CC33 at 235°, 4 dB weaker, heard
in turn; `../sdk/tools/uca_scene.py`): 70.0 / 235.0° offline
(`--channels 5 --uca 0.12`) and through kraken_doa (fake heimdall, 50 kHz
VFO). ~1.6 % of real time on a Pi 5.

## Sources
ETSI EN 300 392-2 (air interface: bursts, coding, scrambling, BSCH / SYSINFO
PDUs). osmocom-tetra used as documentation only.
