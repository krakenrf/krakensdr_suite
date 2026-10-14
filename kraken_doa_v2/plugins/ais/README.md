# AIS decoder plugin

AIS (ships), both channels in one 100 kHz VFO on 162.000 MHz (picking the
decoder tunes there): A 161.975 MHz (-25 kHz), B 162.025 MHz (+25 kHz).
GMSK 9600 bit/s per channel: FM discriminator, 2 x 8 slicers, NRZI, HDLC,
CRC-16/X.25; messages 1-3, 4 / 11, 5, 9, 18 / 19, 21, 24, 27 -> table, map
points, facts, events (SART / MOB / EPIRB as "⚠"), NMEA raw frames.
Details: `kraken_doa_v2/CLAUDE.md` *AIS*.

## ID separation (DoA per ship)
Every frame is a talker packet of its MMSI (the map point / table key): its
samples from the training sequence to the closing flag, its channel (-25 /
+25 kHz, 16 kHz wide - kraken_doa filters it out of every antenna, so two
ships transmitting at once on A and B don't mix), averaged over 20 s, kept
30 min after the ship's last frame (like the vessel list). Ships,
base stations and aids to navigation get their own bearing (⛴ / 📡 / ⚓ rows
in the 🗺 Map's 📡 DF panel; with "Plot on map", the DF error against the
reported position).

## Tests
Generator `tools/ais_gen.py` (message 1 of one MMSI on A / B at given
times: GMSK BT 0.4, NRZI, HDLC, CRC). Four ships at 20 / 110 / 200 / 300°
on both channels, pairs transmitting at the same moment on A and B: 20.0 /
110.0 / 200.0 / 300.0° offline (`--channels 5 --uca 0.12`) and through
kraken_doa; all 16 frames decoded. No AIS traffic reaches the station here
(162 MHz watched for 7 minutes on 2026-10-15), so no live check.
