# ISM band devices (`ism`)

Decodes the short OOK / FSK bursts of the licence-free bands - 315, 345,
433.92, 868 and 915 MHz - from weather stations and thermometers, tyre
pressure sensors (TPMS), key fobs and remotes, alarm sensors and energy
meters, and gives **every device its own DoA**.

Pick "ISM devices (433 MHz etc.)" in a VFO card's Digital decoder list (it
is not in Auto detect: it needs a wide VFO). Use a **240 kHz** VFO (the
default bandwidth; the plugin runs at 240 kHz, so this needs no resampling)
centred on the band, e.g. 433.920 MHz: devices within +-120 kHz of the VFO
are decoded (the last ~10 kHz at each edge are attenuated by the VFO's
filter). Cheap transmitters are often 50-100 kHz off their nominal
frequency - the MHz column shows each device's own.

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
  battery, the device's own frequency, SNR, message count. Rows stay 10
  minutes after the last message; devices are forgotten after 1 hour.
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
  lists the devices newest first (📟).
- **Digital squelch**: a VFO with squelch method "Digital" takes its own DoA
  only from the samples of decoded transmissions.
- **Raw frames** (Decoder Logging → raw): one JSON line per message
  (`model`, `id`, `channel`, `temperature_C`, `humidity`, `pressure_kPa`,
  `battery_ok`, ... and `mic` = what proved it: CRC, CHECKSUM, PARITY,
  REPEAT...) plus the device's frequency and SNR.

## Unknown signals

A burst no protocol decodes is sliced by its own timing (PWM if the pulse
widths fall in two groups, PPM if the gaps do, else NRZ at the shortest
width). If the burst repeats a row of >= 12 bits it is shown as an
**unknown device** whose id is the code (e.g. `OPWM25 3671538`) - it gets
its own DoA like any other device, which is what you want when hunting an
unidentified transmitter. A clean burst without a repeat is grouped by its
signal class (modulation, coding, bit rate, frequency) and shown once the
same class is heard again within 10 minutes (one-off fragments, collisions
and wake-up bursts are not devices). Option "Unknown signals" turns this off.

## Options

| option | values | |
|---|---|---|
| Units | Metric / US | °C, kPa, m/s or °F, PSI, mph in the table and labels |
| DoA per device | Each transmission alone / 1 min / 10 min | the `avg_s` of its talker packets |
| Unknown signals | Show / Ignore | see above |
| Burst detection | 8 / 10 / 13 / 16 dB | how far above the noise a burst must rise |

## How it works

`frontend.cpp` - detection: a 64-point FFT every 32 samples (3.75 kHz bins)
with a noise floor per bin. A bin is active when it and its neighbours, in
this frame and the one 64 samples earlier, hold the threshold over their
floor (6 bins' worth of noise: no false bursts in 20 s of white noise at
8 dB). Active bins are assigned to **packages by frequency** - up to four at
once, so two devices transmitting at the same moment on different
frequencies become two bursts, each demodulated (and its DoA filtered) on its
own. A package's frequency region grows during its first 50 ms (that is how
the second tone of a wide FSK transmitter joins it); a strong signal
elsewhere later starts its own package, unless it is > 20 dB under an active
one (the splatter of strong OOK keying). A package ends after 30 ms of quiet
(1.2 s at most - a held key fob is cut into pieces).

Demodulation, from a 2 s ring, in two passes: a wide filter finds the
carrier or the two FSK tones (2-means of the discriminator), then a filter
centred on them gives the OOK envelope (sliced at half the signal level,
hysteresis, >= 3x the noise; glitches < 18 µs, dropouts < 40 µs and spikes
< 22 µs merged away) and the FSK pulse train (higher tone = pulse): the
discriminator sliced between the tracked tones, or - for tones >= 50 kHz
apart, the +-60 kHz deviation of the HopeRF-style 868 / 915 MHz devices -
the two tones' own filtered levels compared, which a tone near the band
edge (attenuated by the VFO filter) doesn't upset.

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

CPU (Pi 5): ~1.3 % of a core while the band is quiet (the detector), ~7 %
with bursts back to back.

## Test results

On the rtl_433_tests recordings (1125 files; 250 kHz to 1 MHz, resampled to
240 kHz by the offline host, padded with 0.4 s of noise): **960 of the 1030
recordings with an expected output decode completely** (every device, id
and field), with 3 wrong fields - all from an HCS200 whose carrier sits
beyond +120 kHz and is folded back by the offline 250 -> 240 kHz resampler.
Not decoded: models that aren't implemented (some LaCrosse, Rubicson pool /
meat thermometers, Alecto, Owl), ThermoPro TX2 (reported as Prologue - the
same bits), signals at the very edge (+-120..125 kHz) of 250 kHz recordings,
and rtl_433's own false positives in a few expected files.

End to end (a fake heimdall streaming a 5-element UCA scene into kraken_doa;
EV1527 / PT2262 fobs, an EV1527 PIR, Toyota and Schrader TPMS, an Acurite
tower and an unknown 71-bit PPM device at 5 bearings and frequencies, some
transmitting at the same time): every device 0.0° off its true bearing, every
transmission in its history right.

## Testing

```bash
make -C plugins PLUGIN=ism
plugins/ism/build/decoder --file rec.cu8 --rate 250000 --raw [--verbose]
ISM_DUMP=1 plugins/ism/build/decoder --file ...    # every burst's pulse trains (µs) to stderr
ISM_TRACE=1 plugins/ism/build/decoder --file ...   # the detector's active bins per frame
ISM_ROWS=prologue plugins/ism/build/decoder ...    # a protocol's rows (those that call dbg_rows)
```

Protocol notes come from rtl_433's documentation (GPL-2.0) - used as a
protocol reference, and its test recordings (rtl_433_tests) as test data
only; the implementation is our own.
