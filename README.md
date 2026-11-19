# KrakenSDR Suite V2

Software for the 5-channel KrakenSDR, the 4-channel KerberosSDR and the
KrakenSDR Wideband: radio direction finding, a live spectrum with as many
receivers (VFOs) as you need, audio, a map, and decoders for digital voice,
aircraft, ships, pagers, sensors and more.

V2 is written in C++ instead of Python. It is fast enough to show the
spectrum, play audio and run MUSIC direction finding at the same time on a
Raspberry Pi 4 or 5.

**The suite is in beta.**

It is made of two programs:

| Program | What it does | Web page |
|---|---|---|
| **Heimdall** | Reads the tuners and keeps them coherent (sample and phase calibrated) | `http://krakensdr.local:8070` |
| **KrakenSDR DOA** | Everything you use: spectrum, VFOs, audio, direction finding, decoders, map, recorder | `https://krakensdr.local:8080` |

It runs on the Raspberry Pi 4 / 5 (the main target), x86_64 Linux, Windows
10 / 11 and macOS, natively or (on Linux) in Docker.

## Contents

- [Install](#install)
- [Run](#run)
- [Features](#features)
- [Optional setup](#optional-setup): voice codecs, Voice to Text, the AI assistant, plugins
- [More documentation](#more-documentation)
- [Licence](#licence)

## Install

### Raspberry Pi and Linux

```
git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
./install.sh
```

This installs the packages it needs and builds both programs and every
decoder. You may need to reboot afterwards. `WHISPER=1 ./install.sh` also
sets up [Voice to Text](#voice-to-text).

### Docker (Linux)

Use this instead of `install.sh` / `run.sh`:

```
# Docker + Compose (Raspberry Pi OS / Debian trixie; on Bookworm or Ubuntu use
# "curl -fsSL https://get.docker.com | sudo sh" instead)
sudo apt-get install -y docker.io docker-compose
sudo usermod -aG docker $USER          # then log out and back in

# Keep the DVB-T kernel driver off the dongles
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf
sudo modprobe -r dvb_usb_rtl28xxu

git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
docker compose up -d                   # first run builds the image (~5 min on a Pi 5)
```

The containers then start at every boot. Don't run the native install
(`run.sh` or its boot service) at the same time. Settings, hardware variants
and troubleshooting are in [DOCKER.md](DOCKER.md).

### Windows 10 / 11

```
git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
build_windows.bat                      # installs MSYS2 (the compiler) if needed, builds everything
run.bat                                # starts both programs (run.bat stop stops them)
```

Each tuner needs the WinUSB driver once (Zadig). The driver step, options
and troubleshooting are in [WINDOWS.md](WINDOWS.md).

### macOS

Tested on an Intel Mac with macOS 12. It needs the Xcode command line tools
and [Homebrew](https://brew.sh).

```
git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
./install.sh                           # Homebrew packages and both programs
./run.sh
```

There is no driver step. The Mac doesn't idle-sleep while the receiver runs
(the display still may). Docker doesn't work on macOS, because its
containers can't reach the Mac's USB devices.

## Run

Connect and power the KrakenSDR, then in any terminal:

```
./run.sh          # both programs in a split terminal (tmux)
./run.sh stop     # stop them
```

Then open the web pages from any computer on the network:

- **KrakenSDR DOA:** `https://krakensdr.local:8080` or `https://PI_IP_ADDR:8080` (note the `s` in https)
- **Heimdall:** `http://krakensdr.local:8070` or `http://PI_IP_ADDR:8070`

Options:

| Option | Use |
|---|---|
| `--wideband`, `-w` | KrakenSDR Wideband (downconverter) |
| `--kerberos` | KerberosSDR. Calibration is manual: disconnect the antennas and start it from Heimdall's web page |
| `--kerberos_sw` | KerberosSDR with antenna switches, for automatic calibration |
| `--ext_noise` | Add-on antenna array with its own noise source on the CH0 bias tee |
| `NO_TMUX=1 ./run.sh` | No terminal dashboards; log to `logs/` instead |
| `NO_AUTOBUILD=1 ./run.sh` | Don't build first (see below) |

**Automatic build.** If a program isn't built yet, was built for another CPU
(an SD card moved from a Pi 5 to a Pi 4) or is older than a `git pull`,
`run.sh` builds it first. That takes 10-30 minutes on a Pi the first time.
Meanwhile the web addresses above show a progress bar, and they switch to
the real page when it is ready. The build log is `logs/build.log`. If the
build fails, an older working build is started with a warning, or the page
shows the error (for missing packages, run `./install.sh` once).

**Start at boot (Pi).** `./install-pi-service.sh` installs a service that
runs `run.sh` at every boot; `./install-pi-service.sh --uninstall` removes
it. For a hardware variant: `VARIANT_FLAGS=--wideband ./install-pi-service.sh`.

**Version.** The bottom of the KrakenSDR DOA sidebar and the top right of
Heimdall's page show the version of the code, e.g. `r294 · 63a4917` (commit count and commit;
`· modified` = files changed locally; hover for the date and branch). It
follows every `git pull` by itself; `heimdall_v2/heimdall --version` prints it
too. Please include it when you report a problem.

**Restart and shut down from the web page.** At the bottom of the DOA
page's sidebar, **Restart** restarts the software (both programs) or
reboots the computer, and **Shut down** powers it off - unplug a Pi once
its green LED has stopped blinking. Recordings are finished and settings
saved first. What your installation can't do is shown greyed out, with
the reason (Setup Settings → System lists all three): the software restart
needs `run.sh` (or Docker / `krakensdr.exe`); rebooting and shutting down
need the permission `install-pi-service.sh` sets up (on a desktop, a
logged-in user usually has it already; not in Docker). To give only that
permission, without the boot service (e.g. a desktop PC started with
`run.sh`), run `./install-pi-service.sh --power-only` (it asks for your sudo
password once; `--power-only --uninstall` takes it away again), then
restart the software. `KRAKEN_NO_POWER=1 ./run.sh` hides the buttons.

**Antenna bias tees from the web page.** The **Bias tee** button in the DOA
page's top bar, right of Mode, opens a switch per antenna (and All): DC on
that antenna port for an active antenna or an LNA - only for antennas that
take it. The button turns amber and shows how many are on ("2/5"). heimdall
keeps the setting across restarts (its own page on port 8070 has the same
switches). Not with generic RTL-SDRs or the KrakenSDR Wideband. After
updating, rebuild and restart **both** programs (`./run.sh stop`, then
`./run.sh`): a heimdall that is still running has the old control port, and
the button then says "heimdall is too old for this".

## Features

Everything below is on the KrakenSDR DOA page. Each feature has a detailed
description, linked under [More documentation](#more-documentation).

The **top bar** has no captions; an icon marks each control, and its name
shows when the mouse rests on it (on a touch screen: the **?** button, then
tap the control). Left to right: the frequency; **Gain** (an amplifier
triangle, then the gain bar); **Audio** (the speaker button - blue while the
audio is on, a crossed speaker while it is off - then the volume bar). The
gain and volume bars show their value inside and fill up to it: drag in the
box, or turn the mouse wheel over it; **Step**, the tuning step (two marks with a
double arrow); **Mode** (a ring of dots); the **Bias tee** button. With the
KrakenSDR Wideband the **mixer** (a circle with a cross) follows the
frequency. On a phone,
Gain, Audio and Step are in the sidebar drawer, with their names.

### Modes

The **Mode** selector in the top bar (the ring-of-dots icon) picks how the tuners are used:

| Mode | Tuners | Use |
|---|---|---|
| **Coherent** (default) | One phase-calibrated antenna array | Direction finding, beamforming, the web mapper, DoA logging |
| **Wideband** | Side by side, one stitched spectrum (about 5 x 1.9 MHz on a KrakenSDR) | A wide view and the discrete scanner. No direction finding |
| **Independent** | Each its own receiver with its own frequency and gain | One spectrum and waterfall per tuner. No direction finding, no calibration |

The mode is remembered across restarts. Switching back to Coherent retunes
every tuner to the common frequency and recalibrates. The KrakenSDR Wideband
hardware only has Coherent mode.

**Ordinary RTL-SDR dongles.** Without a KrakenSDR or KerberosSDR, the suite
also runs on one or more ordinary RTL-SDR dongles (any serial numbers): just
plug them in and use `./run.sh` without options. Heimdall then starts in
Independent mode, every dongle its own receiver (Wideband too, with two or
more). Coherent mode, direction finding and calibration need a KrakenSDR /
KerberosSDR and are not available, and Heimdall never switches a dongle's
bias tee. Heimdall's page and terminal say "generic RTL-SDR" when this is
the case.

In every mode the mouse wheel zooms the spectrum (up to 32x), and the bar
between spectrum and waterfall can be dragged to change the split. The
**View** button on the spectrum's header opens its display settings: the
dB scale, zoom, and the waterfall's speed, colours and contrast (Auto, or Min
/ Max: the levels shown as the darkest and the brightest colour). In
Independent mode every tuner pane has its own View button, and its
settings are that tuner's own (kept in the browser); the header's View
button then sets all tuners at once. Levels
are drawn in dB above the receiver's noise floor. The receiver remembers
that floor, so a signal that fills the whole band (DVB-T, LTE) still shows
at its real height.

### VFOs and audio

A VFO is one receiver inside the band, from 1 kHz to 2.4 MHz wide. Add as
many as you need: each one demodulates NBFM, AM or WBFM for listening in the
browser, has its own squelch, and can run a digital decoder. In Independent
mode a VFO can sit on any tuner's pane.

Point at a VFO's bar to see its signal level in dBFS, its signal-to-noise
ratio and an estimate in dBm at the receiver input.

### Decoders

Tick **Digital decoder** on a VFO and pick a decoder, or **Auto detect**.
Each decoder in use has a tab in the panel under the waterfall (the
**Decoders** button) with its settings, the decoded data and an event log;
**All decoders** at the end of the tabs lists every installed decoder (type
to filter) and shows the one you pick. Any number of VFOs can decode at once.

| Decoder | What you get |
|---|---|
| [P25 Phase 1](kraken_doa_v2/plugins/p25/README.md), [DMR](kraken_doa_v2/plugins/dmr/README.md), [D-STAR](kraken_doa_v2/plugins/dstar/README.md), [NXDN](kraken_doa_v2/plugins/nxdn/README.md), [TETRA](kraken_doa_v2/plugins/tetra/README.md) | Networks, talkgroups, radio IDs, callsigns, call activity, GPS positions (DMR, D-STAR), and the voice (see [Digital voice codecs](#digital-voice-codecs)) |
| [MPT1327](kraken_doa_v2/plugins/mpt1327/README.md) | Analogue trunking control channels |
| [Analog FM IDs](kraken_doa_v2/plugins/analog_id/README.md) | CTCSS / DCS tones, MDC1200, FleetSync, 5-tone selcall, DTMF, 1750 Hz tone bursts |
| [Pagers](kraken_doa_v2/plugins/pagers/README.md) | POCSAG and FLEX at every speed. Street addresses in the pages become incidents on the map |
| [APRS](kraken_doa_v2/plugins/aprs/README.md) | Stations, objects, positions, messages |
| [ADS-B](kraken_doa_v2/plugins/adsb/README.md) (1090 MHz) | Aircraft with position, altitude, speed, squawk; a sortable table of all aircraft |
| [UAT](kraken_doa_v2/plugins/uat/README.md) (978 MHz, United States) | Aircraft, rebroadcast traffic, ground stations and their weather / NOTAM text |
| [SSR / Mode S / TCAS interrogations](kraken_doa_v2/plugins/ssr1030/README.md) (1030 MHz) | The radars, multilateration stations and aircraft that ask the questions ADS-B answers |
| [ACARS](kraken_doa_v2/plugins/acars/README.md), [VDL Mode 2](kraken_doa_v2/plugins/vdl2/README.md) | Aircraft datalink messages: registration, flight, label, text; VDL2 ground stations on the map |
| [FLARM / OGN](kraken_doa_v2/plugins/flarm/README.md) (868 / 915 MHz) | Gliders and light aircraft. OGN trackers give a position; FLARM is encrypted, so only its ID and bearing |
| [DME / TACAN](kraken_doa_v2/plugins/dme/README.md) (962-1213 MHz) | Morse ident, channel and its VOR / ILS pairing, TACAN bearing, aircraft interrogations |
| [ILS / VOR / markers](kraken_doa_v2/plugins/ils/README.md) | Localizer and glide slope deviation, VOR radial, marker beacons, Morse idents |
| [Pulse radar](kraken_doa_v2/plugins/radar/README.md) (1215-1400 MHz and others) | Pulse width, chirp, PRF and PRI pattern, antenna rotation, beam width |
| [AIS](kraken_doa_v2/plugins/ais/README.md) (162 MHz) | Ships, base stations, aids to navigation |
| [406 MHz distress beacons](kraken_doa_v2/plugins/beacon406/README.md) | EPIRB / ELT / PLB: beacon ID, country, type, position. Self-tests are shown as tests |
| [Radiosondes](kraken_doa_v2/plugins/radiosonde/README.md) (400-406 MHz) | Weather balloons (RS41, DFM, M10 / M20, iMet, LMS6, MRZ): position, weather data, flight track, a sounding |
| [ISM devices](kraken_doa_v2/plugins/ism/README.md) (315 / 345 / 433 / 868 / 915 MHz) | About 140 device models: weather stations, tyre pressure sensors, key fobs, alarm sensors, energy meters |
| [LoRa / Meshtastic / LoRaWAN](kraken_doa_v2/plugins/lora/README.md) | Meshtastic's public default channel (text, node names, positions); the clear headers of everything else |
| [Wildlife collars and tags](kraken_doa_v2/plugins/wildlife/README.md) | Each pulsing transmitter by frequency and pulse rate, with mortality and activity changes |
| [RDS / RBDS + SCA](kraken_doa_v2/plugins/rds/README.md) (FM broadcast) | Station name, RadioText, programme type, alternative frequencies; SCA subcarriers, with audio |
| [Digital TV](kraken_doa_v2/plugins/dtv/README.md) | DVB-T / T2, ISDB-T, ATSC, DTMB: standard, mode, transmission parameters, an echo profile, a bearing per path |
| [LTE / 5G NR cells](kraken_doa_v2/plugins/cells/README.md) | Each cell's operator, tracking area, cell identity, band and the system information it broadcasts |
| [V.23 telemetry](kraken_doa_v2/plugins/v23_telemetry/README.md) | 1200 bit/s AFSK telemetry frames |

Things to know:

- **Decoders that tune for you.** ADS-B, UAT, the 1030 MHz decoder, AIS and
  the 406 MHz beacon decoder put the VFO on their own frequency when picked.
- **Whole-band decoders.** ISM, FLARM, LoRa, pulse radar, Digital TV and
  LTE / NR cells use the tuner's full 2.4 MHz: tune the receiver to the
  band. Auto detect leaves out ADS-B and any decoder that needs a wider VFO
  than the one it runs on.
- **Only what is sent in the clear is decoded.** Encrypted calls are shown
  and muted. No keys are guessed or broken: Meshtastic private channels,
  LoRaWAN payloads and FLARM packets stay unread, and of LTE / NR only the
  broadcast system information is read, never anything addressed to a phone.
- **Distress beacons.** If you hear a real 406 MHz beacon, report it to
  your rescue coordination centre with its ID, time, frequency and bearing.
  The receiver reports nothing by itself.
- **Digital squelch.** On a VFO with a decoder, set the squelch to
  **Digital**: it opens only while valid frames arrive, and the bearing is
  computed from those frames alone.
- **On the map.** Tick **Plot on map** in a decoder's tab to show the
  positions it decodes.

### Direction finding

Direction finding needs Coherent mode.

- **Live bearing.** MUSIC DoA for every VFO, as a plot and as a lobe and
  bearing line on the map.
- **A bearing per transmitter.** When a signal names its transmitter (a
  radio ID, callsign, aircraft address, ship, sonde serial, cell ID ...),
  each one gets a bearing from its own transmissions only. Several radios
  on one channel no longer blur into one bearing. Pick a transmitter in the
  map's **DF** panel. On a repeater's output every radio's bearing is the
  repeater's: listen on its input frequency to find the radios.
- **Finding a transmitter while driving.** The map collects a bearing every
  15-60 m of driving and builds a heat map with the most likely position
  and its 95 % ellipse. It needs the station's position and heading
  (Station Information → GPS, through gpsd), and the array's ANT0 must face
  the front of the vehicle.
- **Drive mode.** For direction finding from a vehicle: the **Drive**
  button at the foot of the rail (on a phone: in the sidebar's Panels box),
  or the page's address with `#drive` at the end - bookmark it on the
  phone or tablet on the dashboard. The map fills the screen and follows
  the vehicle; one big readout shows the selected VFO's bearing - an arrow
  from the vehicle's front (up = straight ahead), the compass bearing in
  large digits and a word (ahead left, right, behind ...) - with the signal
  level and the confidence, or why there is no bearing. Big buttons: the
  previous / next VFO, audio on / off, Follow, reset the bearings, zoom,
  the map's layers, **Day** (a light, high-contrast look for daylight) /
  **Night**, and **Exit** (or Esc). The screen stays on while it is open
  (browsers with the Screen Wake Lock API). The rest of the page comes back
  as it was.
- **Range from signal strength.** From one bearing, the signal's level and
  your estimate of the transmitter's power, the map shows how far away the
  transmitter probably is, using the terrain. It is only as good as the
  power estimate and the receiver's level calibration.
- **Replay.** The map's **Layers** → **Replay** plays back a drive logged by the
  KrakenSDR Android app (CSV or SQLite) or by this receiver's **DoA
  Recording**. The file is read in your browser and not uploaded.
- **Array calibration from aircraft** (experimental). ADS-B aircraft report
  where they are, so they can calibrate the cable lengths, antenna positions
  and rotation of the array: sidebar → **Experimental**.

### Map

The **DoA & Map** button on the rail at the right edge of the page shows
or hides the right-hand pane; its **Map** tab is a street or satellite map
(outside coherent mode the pane is only the map, and the button says "Map").
The map's **Layers** button (top left) picks the base map and turns on or off
what is drawn on it - tracks, masts, the DoA lobes, bearing lines and heat
map - and the tools with a panel of their own: Viewshed, Replay and Range.

- **Decoded positions**: aircraft, ships, APRS stations, radios sending
  GPS, sondes and more, with their tracks. Click a marker for its details.
- **Your own markers**: right-click (long press on a touch screen) →
  **Add Marker**. A marker lists the saved frequencies at that place.
- **Search** (the magnifier): coordinates in any usual form, UTM, MGRS, Maidenhead
  locators and map links work offline; addresses and places are looked up
  online. Home goes to the station, the target to the estimated transmitter.
- **Masts**: the radio masts and towers mapped in OpenStreetMap.
- **Viewshed**: the ground in line of sight of the station's antenna, over
  the terrain, up to 1000 km.
- **Viewshed Signal mode**: coverage of a transmitter you place, also
  beyond the line of sight (ITU-R P.1812, Longley-Rice or free space), and a
  path loss analysis between two points with the terrain profile and the
  link budget.
- **Incident map**: street addresses in pager messages, looked up online
  and shown as markers for 24 hours.
- **The station**: click and drag it to move it, or right-click →
  **Set station here**.
- **Map provider**: OpenStreetMap and Esri imagery need no key. With your
  own API key you can switch to Mapbox, Google Maps, Azure Maps, MapTiler,
  Thunderforest, Stadia Maps, HERE or any tile URL: the **Map** tab of
  **Setup Settings** (the cog at the right of the top bar). The provider
  and key are kept in that browser only, and **Test** checks the key before
  you save it.

Map tiles, terrain, masts and address lookups come from the internet,
loaded by your browser. Without a connection the map still works on a plain
latitude / longitude grid.

### Saved frequencies and scanning

The sidebar's **Saved Frequencies** box keeps your list on the receiver
(`kraken_doa_v2/saved_frequencies.json`), so every browser sees it. An
entry has a name and a frequency, and
optionally a category, a location (a map marker), how to receive it (demod,
bandwidth, gain, decoder), a tone or code and notes. Click an entry to tune
the selected VFO to it. Entries are also marked on the spectrum.

To scan, tick entries or whole categories and press **Scan**. The
selected VFO goes to each ticked frequency that is active and is heard until
it ends. Every ticked frequency inside the band is watched at once; the
receiver retunes only for other bands. The receiver does the scanning, so it
carries on with no browser open. Options include a VFO of its own for each
active frequency, so that all are heard, decoded and transcribed at once.

### Recording and logging

- **IQ & Audio Recorder**: the calibrated IQ samples of every antenna
  (the whole 2.4 MHz, or one VFO's band) as a [SigMF](https://sigmf.org)
  recording, and VFO audio as WAV or Opus. **Open** plays an IQ recording
  back through the receiver as if the antennas picked it up again. The
  full band is 24 MB/s with 5 antennas, too fast for an SD card: use a USB
  SSD or record one VFO's band. IQ recordings include the station's position
  and GPS track; untick "Include the station position" before
  recording something you will share. Format:
  [SIGMF.md](kraken_doa_v2/SIGMF.md).
- **Decoder Logging**: allow it once (the ⚙ Setup Settings button,
  Decoder Logging, switch on "Allow logging to disk"), then pick what to
  keep where the data is: a decoder's **Logging** in its tab under the
  waterfall - **Log** (its events, messages, positions, incidents) or
  **Log All** (also raw frames and the messages a control channel repeats)
  - and the Transcription panel's **Logging** for voice transcripts. One
  JSON-lines file per day in `kraken_doa_v2/decoder_logs/` (or a drive you
  pick), gzipped at midnight and kept 7 days by default.
- **DoA Recording**: the bearings to CSV or SQLite.
- **Voice to Text**: tick **Transcribe** on a VFO and what is said there
  is written down by a speech-to-text model running on the receiver. No
  account, nothing sent anywhere. It works on analog voice and on decoded
  digital voice, and keeps each transmission's time, radio ID, bearing and
  audio. Set it up once: [Voice to Text](#voice-to-text).

### AI assistant

The **Assistant** column on the right is a chat with an AI agent (Claude
Code) that sees and controls the receiver. It can:

- answer questions about what the page shows: "what is the signal on D0?",
  "which aircraft did we hear in the last hour?";
- work out who is transmitting, from the decoded data, your saved
  frequencies, your own files in `kraken_doa_v2/ai/knowledge/` and the
  internet;
- do things: "put a DMR decoder on 439.475", "look for an active weather
  balloon";
- draw on the map: licensed sites, bearing lines, line-of-sight checks;
- write a decoder plugin for a signal: capture, analyse, write, build, test.

A **Read only** switch stops it changing anything. It is off until you set
it up: [AI assistant setup](#ai-assistant-setup).

## Optional setup

### Digital voice codecs

A decoder can play the voice of a call: click **Listen** in its tab, or
set the VFO's demod to **Digital**. Encrypted calls can't be decoded.

| System | Voice codec | Needs |
|---|---|---|
| P25 Phase 1 | IMBE | nothing - built in |
| DMR | AMBE+2 | mbelib, installed by you |
| D-STAR | AMBE | mbelib, installed by you |
| NXDN (NXDN48 and NXDN96) | AMBE+2 | mbelib, installed by you |
| TETRA | ACELP | the ETSI reference codec, built by you |
| MPT1327 | analogue FM | nothing - listen to the traffic channel with NBFM |

The AMBE and TETRA codecs are not shipped with this software: AMBE+2 is
commercially licensed (DVSI), and the ETSI codec may not be redistributed.
You can build them yourself for personal use; whether that is allowed where
you live is your responsibility. **Setup Settings → Decoders** (the cog at the
right of the top bar) shows which codecs are installed.

**DMR, D-STAR and NXDN: mbelib**

```
sudo apt-get install -y git cmake build-essential
git clone https://github.com/szechyjs/mbelib.git
cd mbelib && mkdir build && cd build
cmake .. -DDISABLE_TEST=ON
make -j3
sudo make install
sudo ldconfig
```

kraken_doa finds `libmbe.so.1` in the system library path. Set
`KRAKEN_MBELIB=/path/to/libmbe.so.1` to use one elsewhere.

**TETRA: the ETSI codec, with osmo-tetra's patches**

```
sudo apt-get install -y git build-essential unzip patch wget
git clone https://github.com/osmocom/osmo-tetra.git
cd osmo-tetra/etsi_codec-patches
wget -O etsi_tetra_codec.zip https://www.etsi.org/deliver/etsi_en/300300_300399/30039502/01.03.01_60/en_30039502v010301p0.zip
md5sum etsi_tetra_codec.zip     # must be a8115fe68ef8f8cc466f4192572a1e3e
sh ./download_and_patch.sh      # unpacks and patches the zip downloaded above
cd ../codec/c-code
make
sudo install -m 755 cdecoder /usr/local/bin/tetra-cdecoder
sudo install -m 755 sdecoder /usr/local/bin/tetra-sdecoder
```

kraken_doa runs `tetra-cdecoder` and `tetra-sdecoder` from the PATH. Or set
`KRAKEN_TETRA_CODEC_DIR` to a folder holding `cdecoder` and `sdecoder`.
TETRA voice plays about half a second late, and only on cells without air
interface encryption.

After installing either codec, restart the KrakenSDR software (`./run.sh`,
or reboot when it runs as a service).

**Docker.** Build the codecs on the host as above (the `sudo make install` /
`install` steps can be skipped), copy the ones you built into the
container's data folder and restart the client:

```
sudo mkdir -p docker-data/codecs
sudo cp -L mbelib/build/libmbe.so.1 docker-data/codecs/
sudo cp osmo-tetra/codec/c-code/cdecoder osmo-tetra/codec/c-code/sdecoder docker-data/codecs/
docker compose restart kraken_doa
```

**Windows.** The same codecs, built in MSYS2: see [WINDOWS.md](WINDOWS.md).

### Voice to Text

Voice to Text uses [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
with OpenAI's Whisper models, on the receiver itself. Set it up once (about
10 minutes on a Pi 5):

```
cd krakensdr_suite/kraken_doa_v2
make whisper
```

This builds the helper and downloads the default model (base.en, 142 MB).
No restart is needed: tick **Transcribe** on a VFO. `WHISPER=1 ./install.sh`
does the same during the install.

| Model | Download | On a Pi 5 | |
|---|---|---|---|
| tiny.en | 75 MB | ~5x faster than real time | least accurate |
| base.en | 142 MB | ~2-3x faster than real time | the default |
| base.en-q5_1 | 57 MB | about the same | less memory |
| small.en / small.en-q5_1 | 466 / 181 MB | ~2x slower than real time | most accurate on a Pi; for quiet channels |
| tiny, base, small (+ -q5_1) | as above | as above | any language, or translated to English |

Get other models in **Setup Settings → Voice to Text** (the cog at the
right of the top bar; **Get**), or with
`transcribe/setup.sh --models-only small.en` (`transcribe/setup.sh --list`
shows them all). That tab also shows whether Voice to Text is set up (and
what to run while it is not), and sets the CPU cores the model may use and
the "Words to expect": callsigns and names spelled as you want them.

- Transcripts appear in the **Transcription** panel (its button is on the
  rail at the right edge of the page) a few seconds after each
  transmission ends. Long transmissions and broadcasts come in parts.
- In Coherent mode with DoA on, each transcript keeps its bearing. Click it
  to draw that transmission's lobe on the map.
- While the model is busy, transmissions wait in a queue. If they pile up,
  give it more CPU cores, use a smaller model or transcribe fewer VFOs.
- The audio is kept as Ogg Opus (about 7 MB per hour of speech) in the
  Decoder Logging folder, at most 2 GB, and follows its "Keep" days
  (⚙ Setup Settings, Decoder Logging).
- On a Mac with Apple Silicon the model runs on the GPU;
  `make whisper WHISPER_GPU=off` forces the CPU.
- Docker: the image includes the helper. Get the models with **Get**.

Expect radio-quality transcripts: clear speech comes out well, weak signals
and unusual names less so. The audio is kept so you can check.

### AI assistant setup

Once, on the receiver:

1. Install Claude Code and log in (a Claude subscription or an Anthropic
   API key):

   ```
   curl -fsSL https://claude.ai/install.sh | bash
   claude          # log in, then /exit
   ```

2. Enable the assistant:

   ```
   cd krakensdr_suite/kraken_doa_v2
   python3 ai/kraken_ai.py setup
   ```

`python3 ai/kraken_ai.py disable` switches it off again. In the web page,
**Setup Settings → Assistant** shows whether the assistant is set up,
and the steps while it is not. The web page can't
switch it on, because the assistant lets anyone using the page run an AI
agent on the receiver that controls it and compiles code. **If other people
can reach your receiver's web page, set an API token first**
(`KRAKEN_API_TOKEN`, or the `api_token` file).

What it can and can't do:

- **Files.** It reads the `kraken_doa_v2` folder, except the API token, the
  settings file, the TLS key and its own configuration. It writes only its
  conversation folder, decoder plugin folders and `ai/knowledge/`. Your own
  Claude Code settings, MCP servers and memory are not loaded into it.
- **Shell.** Its commands run in a sandbox that enforces those write
  limits, hides the credentials in your home folder and has no network.
  Without the sandbox it has no shell: it still answers and controls the
  receiver, but can't build plugins.
- **Plugins it changed** are run in a sandbox too. List them with
  `python3 ai/kraken_ai.py sandboxed`; after checking what it changed,
  `python3 ai/kraken_ai.py trust <id>` runs one normally again.
- **Receiver control.** Tuning, gain, VFOs, decoders, DoA on / off and
  saved frequencies only. No settings resets, folders, web mapper or array
  setup.
- **Misleading text.** Web pages and decoded messages can contain text
  written to mislead an AI. The assistant is told never to follow
  instructions found there and never to send local files or keys anywhere.
  What you ask about may still go to the sites it searches.

**Cost.** Usage counts against your Claude plan or API budget. A question
about the screen costs a cent or two (API-equivalent), a search such as the
weather balloon about $0.10-0.30, a decoder plugin about $1-5. The panel's
gear button picks the model and the effort.

**Limits.** The assistant needs the native install; it is not available in
Docker. On Windows it has no shell, so it can't build plugins there (see
[WINDOWS.md](WINDOWS.md)).

**Other LLM tools.** Another LLM command-line tool can be used instead. It
gets none of the restrictions above and no access to the receiver's tools:

```
python3 ai/kraken_ai.py setup --backend command --command "your-cli args"
```

### Decoder plugins

Every decoder is a plugin: a folder of C++ source in
`kraken_doa_v2/plugins/<id>/`, built by `./install.sh` or `make`. Plugins
run as separate processes, so a faulty one can't crash the receiver.

To add a plugin, or move one to another KrakenSDR, copy its folder (without
`build/`) into `kraken_doa_v2/plugins/`, then build it:

```bash
cd ~/krakensdr_suite/kraken_doa_v2      # wherever the suite is installed
make -j3                                # kraken_doa and every plugin
make -C plugins PLUGIN=<id>             # or just one plugin
```

Then press **Rescan** next to "Decoder plugins" in **Setup Settings → Decoders**,
or restart the software. The same list picks which decoders take part in
Auto detect; untick the ones you never need, since each costs CPU.

A plugin is native code that runs on your receiver: **only install plugins
from people you trust.** To write one, see
[plugins/SDK.md](kraken_doa_v2/plugins/SDK.md), or ask the AI assistant.

### Debug playback

The sidebar's **Debug Playback** box plays a decoder's synthetic test
transmission through the receiver: several transmitters with known IDs at
known bearings, as if the antennas had picked them up. Use it to see what a
decoder shows, or to check that it still finds every transmitter at the
right bearing (**Check**). Press **Generate** once per test, then
**Play**. There is a test for every decoder.

While it plays, a red **DEBUG PLAYBACK - not live** banner is shown, the
live signal is disconnected and nothing is logged or sent out. **Stop**
puts your own settings back.

Generating needs python3 with numpy and scipy. `install.sh` installs them on
Linux; on macOS run `python3 -m pip install --user numpy scipy`. They are
not in the Docker image: generate on the host and copy the files into
`docker-data/kraken_doa/testsignal_cache/`.

## More documentation

| Topic | Where |
|---|---|
| Docker | [DOCKER.md](DOCKER.md) |
| Windows | [WINDOWS.md](WINDOWS.md) |
| Each decoder | `kraken_doa_v2/plugins/<id>/README.md` (linked in the [decoder table](#decoders)) |
| Each feature in detail | [kraken_doa_v2/docs/dev/](kraken_doa_v2/docs/dev/README.md), one file per feature |
| Heimdall: calibration and internals | [heimdall_v2/README.md](heimdall_v2/README.md), [heimdall_v2/docs/dev/](heimdall_v2/docs/dev/) |
| The whole suite in one file | [docs/dev/suite-reference.md](docs/dev/suite-reference.md) |
| Writing a decoder plugin | [kraken_doa_v2/plugins/SDK.md](kraken_doa_v2/plugins/SDK.md) |
| IQ recording format | [kraken_doa_v2/SIGMF.md](kraken_doa_v2/SIGMF.md) |
| GNU Radio source block | [gr-krakensdr_v2/](gr-krakensdr_v2/) |

The files under `docs/dev/` are developer notes, but they describe each
feature completely, including its limits and how it was tested.

## Licence

The suite is made of separate programs under different licences (details:
[LICENSE](LICENSE)):

- **KrakenSDR DOA** (`kraken_doa_v2/`, including its decoder plugins, except
  as below): the [KrakenSDR Source Available Licence](kraken_doa_v2/LICENSE).
  **Free for personal and hobbyist use**: you may use, study, change and
  share it for that. **Organisations may evaluate it free for up to 180
  days**: test it and compare it with their equipment, but not yet rely on
  it in their work. **Deploying it, and any other commercial use, requires
  a licence** from KrakenRF Inc.
  ([standard terms](kraken_doa_v2/COMMERCIAL_TERMS.md)). Contact
  thekraken@krakenrf.com.
- **Heimdall** (`heimdall_v2/`) and the **GNU Radio module**
  (`gr-krakensdr_v2/`): the
  [GNU General Public License v3 or later](heimdall_v2/LICENSE).
- The **plugin SDK** (`kraken_doa_v2/plugins/sdk/`, `plugins/SDK.md`, the
  Windows / macOS helpers in `win/` and `mac/`) and the build, install and
  run scripts: MIT.
- A **decoder plugin with its own `LICENSE` file** is a separate program
  under that licence: the ISM device decoder (`kraken_doa_v2/plugins/ism/`,
  following rtl_433's decoders) is under the
  [GNU General Public License v2 or later](kraken_doa_v2/plugins/ism/LICENSE),
  and the FLARM / OGN decoder (`kraken_doa_v2/plugins/flarm/`, with the OGN
  project's parity-check matrix) under the
  [GNU General Public License v3](kraken_doa_v2/plugins/flarm/LICENSE).

Third-party components keep their own licences:
[kraken_doa_v2/THIRD_PARTY_NOTICES.md](kraken_doa_v2/THIRD_PARTY_NOTICES.md),
[heimdall_v2/THIRD_PARTY_NOTICES.md](heimdall_v2/THIRD_PARTY_NOTICES.md).
The Windows package and the Docker image carry every licence text.
