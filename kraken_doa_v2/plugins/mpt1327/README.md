# MPT1327 decoder plugin

MPT1327 / MPT1343 analogue trunking signalling: 1200 bit/s FFSK (1 = one
cycle of 1200 Hz, 0 = one and a half of 1800 Hz) on NBFM. Control channel
slots of 128 bits: the control channel system codeword (CCSC, carrying the
system identity SYS; its BCH check bits are the SYNC 0xC4D7) and an address
codeword (Aloha, grants, acknowledgements, clear-down, broadcasts ...).
Codewords: 48 bits + BCH(63,48) + parity, single-bit errors corrected. Voice
is analogue NBFM on the granted traffic channel. Details:
`kraken_doa_v2/CLAUDE.md`.

## ID separation (DoA per site)
A control channel slot is the SITE's transmission and its CCSC names the
site: every slot with a good CCSC is a talker packet "SYS hhhh" - CCSC +
address codeword (107 ms), 2 ms inside both ends - in the 12.5 kHz channel,
averaged over 10 s, kept 30 min (🗼 rows in the 🗺 Map's 📡 DF panel). Sites
reusing a control channel frequency, heard in turn, keep their own bearings.
Not decoded: the mobiles' return channel (their requests carry their own
idents - not implemented), traffic channels (no transmitter ID, analogue
voice).

## Tests
Generator `tools/mpt1327_gen.py` (a site's forward control channel: CCSC with
the CCS chosen so the check bits are the SYNC, an ALOHA address codeword,
FFSK on FM). Two sites (SYS 42E1 at 100°, SYS 42E7 at 345°, heard in turn):
100.0 / 345.0° offline (`--channels 5 --uca 0.12`) and through kraken_doa.
Validated earlier on a live NZ control channel (SYS 0x42E1).

## Sources
MPT 1327 / MPT 1343 (UK Radiocommunications Agency); sdrtrunk as
documentation for the GTC field positions.
