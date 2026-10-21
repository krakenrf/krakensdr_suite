# ISM band devices (`ism`)

Decodes the short OOK / FSK bursts of the licence-free bands - 315, 345,
433.92, 868 and 915 MHz - from weather stations and thermometers, tyre
pressure sensors (TPMS), key fobs and remotes, alarm sensors and energy
meters, and gives **every device its own DoA**.

Pick "ISM devices (433 MHz etc.)" in a VFO card's Digital decoder list (it
is not in Auto detect) and tune the receiver to the band, e.g. 433.920 MHz.
The plugin decodes the **whole 2.4 MHz** the tuner receives (433.920 MHz
covers 432.72-435.12 MHz): picking it puts the VFO on the tuner's centre
with the 2.4 MHz bandwidth, drawn as a single line that can't be moved or
resized (like ADS-B; `.lock_vfo`). Cheap transmitters are often 50-100 kHz
off their nominal frequency - the MHz column shows each device's own.

## Supported devices

69 protocol decoders, ~140 model names (rtl_433's naming and field names,
so its documentation applies):

- **Weather / temperature** - Acurite (592TXR tower, 5-in-1, 3-in-1,
  Atlas, 6045M lightning, 515, 899 rain, leak, Optimus, 609TXC, 606TX, 986,
  985, 590TX, 00275RM), LaCrosse (TX141TH-Bv2, TX141-Bv2 / Bv3, TX141B,
  TX141W / TX145wsdth, TX29-IT, TX35DTH-IT, TX22U-IT), Oregon Scientific
  v1 / v2.1 / v3 (THGR122N, THN132N, THGR810, BTHR918, WGR800, PCR800,
  UV800, RTGN318, SL109H and more), Fine Offset / Ecowitt / Ambient Weather /
  Froggit / Misol (WH2, WH5, WH0530, WH1080 / WH3080, WH1050, WH24 / WH65B /
  WS69, WH25 / WH32, WH0290 / WH41, WH51, WH52, WN34 / WN38, WH31E / WH31B,
  WH40, WN20, WS68, WH53), Bresser (5-in-1, 6-in-1 family, 7-in-1, CO2,
  HCHO / VOC, 3CH), Hideki / TFA / Cresta (TS04, wind, rain, temperature),
  Nexus and its rebrands, Rubicson, Solight TE44, Prologue (= ThermoPro
  TX2), Ambient Weather F007TH, TX-8300, inFactory, ThermoPro TP12
- **TPMS** - Toyota (PMV-C210), Pacific PMV-107J, Ford, Citroen / Peugeot,
  Renault, Hyundai VDO, Kia, Elantra 2012, Abarth 124 / EGQ Q85, Nissan,
  BMW Gen 4/5 / Audi, Schrader (generic, EG53MA4, SMD3MA4, NIS315G3,
  MRXBC5A4), GM aftermarket, Steelmate, Jansite, Jansite Solar, truck TPMS
- **Remotes** - Microchip KeeLoq HCS200 / HCS300 / HCS361 / HCS362 (serial
  and buttons; the hop code is shown, not decrypted), Ford car key, Somfy
  RTS, KlikAanKlikUit / Nexa / Proove, Silvercrest, EV1527 / PT2262
  fixed-code fobs, doorbells and sensors (any Te)
- **Alarm sensors** - Honeywell / Ademco 5800 and 2GIG, Interlogix / GE,
  DSC, Kerui, Chuango, GS558 smoke, generic 433 MHz motion
- **Energy** - Efergy e2 classic, CurrentCost TX / EnviR / OptiSmart

## What you get

- **Device table** (decoder tab): model, id, type, channel, the reading
  (temperature, humidity, wind, rain, tyre pressure, button / door state...),
  battery, the device's own frequency, SNR, message count. A device stays
  (here, in the 🗺 Map's 📡 DF list and for its bearing) for the "Keep
  devices" time after its last message - 24 hours by default.
- **Events**: a new device, button presses, door / alarm / tamper states,
  low battery and other alarms (unusual ones start with "⚠"); every message
  with "Log all messages" (verbose).
- **DoA per device** (🗺 Map → 📡 DF panel, coherent mode, DoA on): each
  decoded transmission is reported as a talker packet of its device (model +
  id), at the burst's measured carrier frequency and occupied bandwidth.
  kraken_doa filters exactly that channel out of every antenna before the
  covariance, so a device's bearing never mixes in another device on a
  neighbouring frequency of the same VFO. With the default "Each
  transmission alone" the device's lobe is its latest transmission's and its
  earlier transmissions are in its bearing history (one per transmission);
  "Averaged over 1 / 10 min" combines them (a fixed sensor). The DF panel
  lists the devices (📟) by name - a list that reordered on every
  transmission was hard to click; a click on a column heading sorts by it
  (Bearing, Heard, Tx...). A device keeps its lobe as long as it is kept
  ("Keep devices"): picked in the list hours later, its last lobe shows on
  the map; "Every device" draws only the lobes of devices heard in the last
  minute.
- **Digital squelch**: a VFO with squelch method "Digital" takes its own DoA
  only from the samples of decoded transmissions.
- **Raw frames** (Decoder Logging → raw): one JSON line per message
  (`model`, `id`, `channel`, `temperature_C`, `humidity`, `pressure_kPa`,
  `battery_ok`, ... and `mic` = what proved it: CRC, CHECKSUM, PARITY,
  REPEAT...) plus the device's frequency and SNR.

## Unknown signals

A burst no protocol decodes is sliced by its own timing (PWM if the pulse
widths fall in two groups and most pulse + gap pairs keep one period, PPM if
the gaps fall in two groups, else NRZ at the shortest width - NRZ whose
runs are 1 and 2 bits long also has short + long gap = long + short gap, and
was sliced as PWM before the pair test). If the burst repeats a row of >= 12 bits it is shown as an
**unknown device** whose id is the code (e.g. `OPWM25 3671538`) - it gets
its own DoA like any other device, which is what you want when hunting an
unidentified transmitter. A clean burst without a repeat is grouped by its
signal class (modulation, coding, bit rate, frequency) and shown once the
same class is heard again within 10 minutes (one-off fragments, collisions
and wake-up bursts are not devices); a burst belongs to a class of the same
modulation and coding within 15 % of its bit rate and 30 kHz and takes its
name (matched by the name - rate to 0.1 kbit/s, 25 kHz bins - one device's
jitter gave it a new name nearly every burst, so it never showed). Needs 12
dB (was 15). Option "Unknown signals" turns this off.

A decode with the same maker and id as one made from a burst >= 15 dB
stronger, at the same time and > 30 kHz away, is dropped as a copy (a 3 dB
spur of a 37 dB weather station 214 kHz away decoded as another model with
its id). The Bursts fact counts them ("duplicate copies dropped").

## Options

| option | values | |
|---|---|---|
| Units | Metric / US | °C, kPa, m/s or °F, PSI, mph in the table and labels |
| DoA per device | Each transmission alone / 1 min / 10 min | the `avg_s` of its talker packets |
| Unknown signals | Show / Ignore | see above; Ignore also removes the unknown devices already heard from the table and the 📡 DF list |
| Burst detection | 8 / 10 / 13 / 16 dB | how far above the noise a burst must rise |
| Keep devices | 1 h / 6 h / 24 h / 7 days | how long a device stays in the table and the 📡 DF list after it was last heard (default 24 h; `kp::Talker::keep_s` + `Host::table_keep`) - at most 1000 devices here, 500 talkers in kraken_doa (the least recently heard go first) |

## How it works

`frontend.cpp` - detection: a 640-point FFT every 320 samples of the 2.4 MHz
(3.75 kHz bins, a frame every 133 µs) with a noise floor per bin. A bin is
active when it and its neighbours, in this frame and the one 640 samples
earlier, hold the threshold over their floor (6 bins' worth of noise: no
false bursts in 20 s of white noise at 8 dB). A package continues on 2 dB
less, but never below 7.5 dB: noise crosses the statistic at 6 dB once per
~3000 bin-frames, so at the 8 dB setting a package's region (~40 bins) saw a
crossing in 93 % of the 30 ms it needs to end - packages ran to 1.2 s,
swallowed the next transmissions and were reported > 1 s late (7.5 dB: 5 %).
Active bins are assigned to
**packages by frequency** - up to eight at once, so two devices
transmitting at the same moment on different
frequencies become two bursts, each demodulated (and its DoA filtered) on its
own. A package's frequency region grows during its first 50 ms (that is how
the second tone of a wide FSK transmitter joins it); a strong signal
elsewhere later starts its own package, unless it is > 20 dB under an active
one or under what starts in the same frame (the splatter of strong OOK
keying). A package ends after 30 ms of quiet (1.2 s at most - a held key fob
is cut into pieces). Three or more packages born in the same frame, of which
three or more last >= 5 ms, are a broadband event (a wideband transmission /
interference - all frequencies rise together), not devices: not demodulated,
counted ("broadband events skipped"). A click (a 251 kHz comb of spurs from a
switching supply here, < 1 ms) is no broadband event: a device that starts in
the same frame as one is kept.

A finished package is mixed down by its centre and decimated to 240 kHz
(Blackman low-pass, 321 taps, flat to ~88 kHz, -74 dB from 128 kHz,
evaluated at every 10th sample only); its noise level comes from the 64
bins around it (the RTL's 2.4 MHz isn't flat). From here on everything works
at 240 kHz - burst positions are 240 kHz samples, so the protocols don't
know about the 2.4 MHz; `Burst::fc` is the offset in the whole band (the
device's MHz, its DoA channel).

Demodulation of that piece, in two passes: a wide filter finds the
carrier or the two FSK tones (2-means of the discriminator), then a filter
centred on them gives the OOK envelope (sliced at half the signal level,
hysteresis, >= 3x the noise; glitches < 18 µs, dropouts < 40 µs and spikes
< 22 µs merged away) and the FSK pulse train (higher tone = pulse): the
discriminator sliced between the tracked tones, or - for tones >= 50 kHz
apart, the +-60 kHz deviation of the HopeRF-style 868 / 915 MHz devices -
the two tones' own filtered levels compared, which a tone near the edge of
the piece (attenuated by its filter) doesn't upset.

`bits.cpp` - the slicers, taking the usual device parameters (short / long
width, gap limit, reset limit, sync, tolerance) with rtl_433's semantics:
PWM, PPM, PCM (NRZ / RZ, bit width refined from a preamble), Manchester
with mid-bit edges, differential Manchester, PIWM - plus CRC-8 / CRC-16,
LFSR digests, sums and parity. Rows record the samples they came from (the
DoA span) and the message they belong to (rows after a gap > reset limit
are another message: `Rows::split()`).

`p_*.cpp` - the protocols, registered with `ISM_PROTOCOL(name, strength,
fn)`. CRC / digest-checked ones (STRONG) and checksum / parity ones
(MEDIUM) run on every burst; the repeat-only ones (WEAK, no check at all:
EV1527, Nexus, Prologue...) only when nothing stronger decoded it.
`generic.cpp`: the unknown-signal analysis. `decoder.cpp`: devices, table,
events, raw frames, talker packets.

**Every antenna (coherent mode, `Info::multichannel`).** With DoA on,
kraken_doa sends all antennas (`process_multi`). Detection then runs on
their mean power per bin, each antenna first brought to the antennas' mean
noise (each antenna's noise followed as its median bin power; here the five
differed by up to 5.7 dB). Each package also sums the spatial covariance of
its active bins' spectra (normalised antennas) over its frames; when it ends,
the covariance's main eigenvector gives the weights and the package is
demodulated from the antennas combined (w^H x, |w| = 1 on the normalised
antennas, so the noise stays the floor's) - blind maximum-ratio combining:
up to 10 log10(m) dB more SNR whatever the device's direction, no
calibration needed. Antennas 1.. are kept as 16-bit samples next to antenna
0's ring (+67 MB with 5). Facts: "Antennas: 5, combined per burst (+x dB on
average)" (the combination's power over antenna 0's, normalised). In
independent / wideband mode or with DoA off the plugin gets one channel and
works as before.

Measured (Pi 5): a real 5-antenna recording (20 s, 434 MHz, `decoder --file
REC --channels 5`): antenna 0 alone 8 messages, the best single antenna 44,
all five combined 88 - and a Toyota TPMS and a distant WH65B (a broken T/H
sensor, low battery) only combined. A real WH65B frame planted 12 times at a
set SNR into 5 channels of independent noise (each copy with random antenna
phases): antenna 0 decodes all down to 6 dB per antenna (in 100 kHz), none at
3 dB; combined, all down to 0 dB, none at -3 dB - >= 6 dB (7 dB in theory
for 5). 5 channels of pure noise: no bursts. CPU: 38 % of a core for the 5
antennas offline (reading included) vs 14 % for one.

For each device's DoA kraken_doa filters the device's channel out of the
2.4 MHz of every antenna (it keeps the stream as 16-bit samples for that:
2.5 s, 120 MB with 5 antennas) - with the whole band, devices transmitting at the
same time got each other's bearings.

CPU (Pi 5): the detector ~4 % of a core; the plugin ~7 % with a few devices
and 14-17 % in a busy synthetic scene (7 devices, bursts ~40 % of the time;
the 19 MB/s of samples it receives included). Memory: 32 MB (the 1.75 s
input ring). Built with `-O3` (every plugin's default since 2026-10-22; it
was a `CXXFLAGS` file in this folder): the detector's per-bin loops
vectorise; the output is the same as -O2's. (The plugin first
ran at 240 kHz - +-120 kHz around a VFO; the whole band does everything that
did at about the same CPU, and the 240 kHz version was removed.)

## Test results

On the rtl_433_tests recordings (1125 files at 250 kHz to 1 MHz), each
turned into what a 2.4 MHz receiver would record - resampled, the rest of
the band filled with noise at the recording's own noise level (plain
upsampling leaves it empty, and the detector's noise floors then start near
zero: 853 decoded that way) - and padded with 0.4 s of noise: **954 of the
1030 recordings with an expected output decode completely** (every device,
id and field), no wrong fields. The 240 kHz version of the plugin decoded
960 with 3 wrong fields on the same files at 240 kHz; the difference is 5
recordings whose carrier sits at the very edge (+-120..125 kHz) of a 250 kHz
recording (an artefact of the conversion; the 240 kHz decoder only got 3 of
them, with wrong fields, by folding the edge back), 2 with a signal from the
first sample (the noise floors start from the whole band's median - only the
first moments after the decoder starts are affected) and 1 at 14 dB; it
missed 2 that the full band decodes.
Not decoded: models that aren't implemented (some LaCrosse, Rubicson pool /
meat thermometers, Alecto, Owl), ThermoPro TX2 (reported as Prologue - the
same bits), and rtl_433's own false positives in a few expected files.

End to end (a fake heimdall streaming a 5-element UCA scene into kraken_doa;
EV1527 / PT2262 fobs, an EV1527 PIR, Toyota and Schrader TPMS, an Acurite
tower and an unknown 71-bit PPM device at 5 bearings and frequencies, some
transmitting at the same time): every device 0.0° off its true bearing, every
transmission in its history right.

## Test transmission (debug playback)

`tools/ism_gen.py` writes a 2.4 MHz scene on the 5-element UCA (r 0.12
m), every antenna streamed in 0.1 s pieces, with each protocol's real
framing and check: `tools/scene_six.json` = playback `ism-six-devices`,
433.92 MHz, 6 s looped - EV1527 fob `9b37d` (433.050 MHz, 330°), LaCrosse
TX141TH-Bv2 `90` (434.182, 95°, LFSR digest), Acurite 592TXR tower `12053`
(433.442, 170°, checksum + parity), Toyota TPMS `4e3a91c7` (433.767, 30°,
FSK, differential Manchester, CRC-8), Fine Offset WH24 `101` (434.825,
250°, FSK, CRC-8 + sum) and PT2262 fob `0F1F0F1F` (434.538, 210°); 13
transmissions, four pairs at the same moment on other frequencies.
Offline 13 / 13 messages (antenna 0 alone and all antennas combined), no
unknown or extra device, bearings within 0.1°, its noise alone nothing;
through kraken_doa's debug playback 6 / 6 devices at 0.0°.

The generator through rtl_433 (2026-10-16; rtl_433 built in scratch, run
only - GPL): each device's channel of the scene mixed down and decimated
to 240 kHz as an RTL-SDR would record it, `rtl_433 -r ch.cu8 -s 240000 -F
json`: LaCrosse TX141TH-Bv2 90 (CRC, 21.3 °C, 48 %), Acurite-Tower 12053
A (checksum, 18.4 °C, 62 %), Toyota TPMS 4e3a91c7 (CRC, 33.25 PSI, 24
°C) - every field as generated, every transmission. Found: the WH24 frame
ended in a whole 0x00 byte (8 trailing bits) - rtl_433 classifies by the
preamble / postamble length (WH24 196 bit periods, "nominal 3 bits
postamble"; WH65 12) and read it as a WH65B with the WH65B's wind / rain
factors (1.53 / 2.55 m/s, 10.4 mm for 3.4 / 5.6 m/s, 12.3 mm). Now a
40-bit preamble + 3 bits after the CRC / sum (195 periods): rtl_433
Fineoffset-WH24 101, 15.2 °C, 71 %, 225°, 3.36 / 5.6 m/s, 12.3 mm, UV
1200, 25430 lux - and the plugin as before (WH24). EV1527 / PT2262 at the
scene's clocks (Te 350 / 300 us) are outside rtl_433's Generic-Remote
window (short 464 +- 200 us, long 1404 +- 200 us); at Te 464 us:
`Generic-Remote id 39735 cmd 210` = 9B 37 D2 and tristate `0Z1Z0Z1Z0001`
(its Z = F) = 0F1F0F1F0001, all 10 frames each.

## Testing

```bash
make -C plugins PLUGIN=ism
plugins/ism/build/decoder --file rec_2400k.cu8 --rate 2400000 --rf 433920000 --raw [--verbose]
ISM_DUMP=1 plugins/ism/build/decoder --file ...    # every burst's pulse trains (µs) to stderr
ISM_TRACE=1 plugins/ism/build/decoder --file ...   # the detector's active bins per frame
ISM_ROWS=prologue plugins/ism/build/decoder ...    # a protocol's rows (those that call dbg_rows)
```

Protocol notes come from rtl_433's documentation (GPL-2.0) - used as a
protocol reference, and its test recordings (rtl_433_tests) as test data
only; the implementation is our own.
