KrakenSDR V2 software. The new V2 software moves away from Python and implements everything in C++, which has resulted in massive speed improvements on slow hardware like the Pi 4 and Pi 5.

We are now able to have a real time spectrum and audio demodulation at the same time that the MUSIC DoA is run.

The **Mode** selector in the top bar of the KrakenSDR DOA page picks how the receiver's tuners are used:

- **Coherent** (default): all tuners work as one phase-calibrated antenna array - MUSIC DoA, beamforming, the web mapper and DoA logging.
- **Wideband**: the tuners sit side by side for one wide stitched spectrum (about 5 x 1.9 MHz on a KrakenSDR); the discrete scanner runs in this mode. No DoA.
- **Independent**: every tuner is its own receiver with its own frequency and gain - the page shows one spectrum + waterfall per tuner in a grid. Each pane has its own frequency digits (click the top / bottom half of a digit, or use the mouse wheel on it) and gain slider - the top bar's frequency and gain are hidden in this mode, and put VFOs (demodulation, digital decoders, the AI Signal Lab) on any pane: drag a pane's spectrum to tune that tuner (like the main display), drag a VFO bar to move it, click a pane to move the selected VFO there, or pick the tuner on the VFO's card. No DoA, and no calibration runs (the noise source stays off).

In every mode the spectrum zooms with the mouse wheel (or the Zoom slider under the waterfall, up to 16x; in Independent mode the pane under the cursor / the selected pane), and the bar between the spectrum and the waterfall can be dragged to change the split (in Independent mode it moves every pane at once; double-click for the default).

The DoA panel and the DoA / coherent-only sidebar sections are hidden outside Coherent mode. The mode and Independent mode's per-tuner tuning are remembered across restarts. Switching back to Coherent retunes every tuner to the common frequency and runs a phase recalibration. (The KrakenSDR Wideband hardware variant only has Coherent mode: its tuners are fixed at the IF.)

Any number of VFOs can decode digital radio signals at once - P25 Phase 1, DMR, TETRA, D-STAR, NXDN, MPT1327 (analogue trunking control channels), POCSAG pagers, APRS and ADS-B aircraft (1090 MHz) - with automatic mode detection: tick "Digital decoder" on a VFO in the Decimators box and pick "Auto detect" or a decoder. Each decoder mode gets its own tab in the panel under the waterfall ("🔐 Decoders" button, or ⤢ on the VFO), showing every VFO that runs it: a settings row per VFO, the decoded data (tables with VFO and frequency columns), and one event log of all of them. There is always a tab for every installed decoder (an unused one offers to start it on a VFO); when a VFO is switched to another mode or removed, what it received stays in the old mode's tab; the sidebar "Digital Decoders" box lists the installed voice codecs and decoder plugins. It shows network/site identities, talkgroups, radio IDs, callsigns, messages and call activity, and can play the voice (P25 built in; DMR, D-STAR, NXDN and TETRA with codecs you install yourself - see "Digital voice codecs" below). Every decoder is a plugin, so new ones can be added.

The **🗺 Map** button (under the waterfall) opens a street / satellite map in the right-hand pane - in Coherent mode as a tab next to MUSIC DoA, in the other modes on its own. It shows the positions the decoders report - ADS-B aircraft, APRS stations and objects, DMR radios sending GPS, D-STAR GPS / DPRS - with their tracks; click a marker for its details (callsign, altitude, speed, squawk, distance and bearing from the station). A decoder's positions appear once you tick "🗺 Plot on map" in its tab under the waterfall (off by default; remembered per VFO). The ADS-B tab also lists every aircraft in a table with all its decoded data (sort by any column, click a row to show the aircraft on the map). The Listen button only appears for decoders that carry voice.

**Incident map (POCSAG)**: street addresses in POCSAG text pages ("STRUC1 SMOKE ISSUING 12 QUEEN ST AUCKLAND CENTRAL...") become incidents: the address is found in the text (a house number, street name and type such as ST / RD / AVE; a suburb or town and a cross street named in the message help pick the right one) and looked up online in OpenStreetMap (Nominatim) within a distance of the station - set it in the POCSAG tab (default 300 km; the station location comes from Station Information). Incidents are listed in the POCSAG tab (VFO and frequency, time, address, area, how exactly it was located, the message; the same address paged again - also on another VFO, e.g. fire and ambulance pagers - counts as one incident) and shown as red markers on the 🗺 Map when "Plot on map" is ticked; they are kept 24 hours. The lookup needs internet: without it the pages wait and are tried again every minute for half an hour. It reads addresses in the usual forms of Latin-script countries: "12 Queen St", "123 N Main St", "350 5th Ave" (English-speaking countries), "55 rue du Faubourg Saint-Honoré", "Calle Mayor 1", "Via del Corso 100", "Rua Augusta 100", "ul. Marszałkowska 10" (type before the name), "Friedrichstr. 43", "Prinsengracht 263", "Drottninggatan 50", "Karl Johans gate 22", "Váci utca 1" (German, Dutch, Nordic, Hungarian), with the house number before or after. Not found: other scripts (Cyrillic, Greek, Arabic, Asian), route numbers (SH1, I-95), landmarks, and names the page abbreviates differently from OpenStreetMap. The station (Station Information in the sidebar) is drawn with range rings. Map tiles come from OpenStreetMap (street) and Esri World Imagery (satellite) and need an internet connection; without one the map still works on a plain latitude / longitude grid, with a notice. Drag to pan, mouse wheel or +/− to zoom; drag the pane's left edge to make it wider.

**Decoder logging**: sidebar → 🗂 Decoder Logging saves what the digital decoders report to disk: events (each decoder's log lines), text messages (pager texts), positions (aircraft, APRS stations, DMR / D-STAR GPS - each object at most every N seconds), raw frames (ADS-B Mode S messages, APRS packets, POCSAG codewords) and incidents - tick the types you want. One file per day, `decoders-YYYY-MM-DD.jsonl` (one JSON record per line: time, VFO, frequency, decoder, type and the data); at midnight the finished day is gzipped, and days older than "Keep" (default 7, 0 = forever) are deleted - only these files, nothing else in the folder. To spare the SD card the records are collected in memory and written every 5 seconds; writing pauses when less than 100 MB is free. The box shows the free space on the drive the logs go to, today's size and an estimate per day, and lets you pick the folder: the default is `decoder_logs` in kraken_doa's folder, or pick a USB drive / other mounted disk from the list. Raw ADS-B frames near a busy airport are roughly 100-300 MB a day before gzip. Positions are logged with the decoder's details (for aircraft: squawk, vertical rate, speeds, category, emergency status). Anything unusual is logged as an event starting with ⚠ - for ADS-B: emergencies (7500 / 7600 / 7700) and when they end, squawk changes, the special squawks 7400 / 7777 / 0000, unusual aircraft types (UAV, balloon, parachutist, ultralight, high performance, space vehicle), climbs or descents of 6000 ft/min or more, and 400 kt or faster below 10000 ft; pilot IDENTs are logged too.

**Direction finding on the map (coherent mode)**: the 🗺 Map shows each VFO's live DoA lobe around the station, turned to true north with the station's heading, and the bearing line. While you drive, it builds a heat map of where the transmitter is: bearings are taken every 15-60 m of driving (none while stopped or turning), each is drawn as a faint line from where it was taken, and together they give the most likely transmitter position with its 95 % uncertainty ellipse ("TX ±120 m"). The right pane's "⊞ Both" tab shows the MUSIC DoA plots and the map at once (drag the divider between them). Use the "📡 DF" panel at the bottom left of the map to pick the VFO (or All), switch Lobe / Lines / Heat, set the grid range (km around where you started; the panel warns when the transmitter seems to be outside it), centre on the estimate, or reset. It needs the station position and heading: Station Information → GPS (gpsd; the GPS course is used above 7 km/h, or a compass if gpsd has one); the array's ANT0 must face the front of the vehicle. The receiver also measures whether all bearings are rotated (array mounted a few degrees off) and says so. Retuning a VFO starts a new heat map; the bearings are saved and picked up again after a restart on the same frequency (24 h).

**Direction finding per radio (P25, DMR, NXDN, D-STAR)**: with a VFO's digital decoder on P25, DMR, NXDN or D-STAR, the receiver knows which radio (unit / radio ID, or the D-STAR callsign) is transmitting and when, and cuts the VFO's signal at those points: each radio's bearing is computed only from its own transmissions, so several radios sharing one channel no longer blur into one bearing. In the map's "📡 DF" panel, pick a radio from the list (or the ID selector): you see its latest bearing (plus earlier transmissions as faint lines) and, while driving, its own heat map and position estimate. "Every radio" shows all their bearings at once, labelled with their IDs; "Whole signal" is the plain VFO view. Needs the coherent mode with DoA on; works on encrypted calls too, since the IDs are sent in the clear. On a repeater's output every radio's bearing is the repeater's own (DMR and NXDN labels say "via repeater") - listen on the repeater's input frequency, or to direct (simplex) traffic, to find the radios. TETRA isn't split (the decoder only hears the base station). Other decoders can provide the same split through the plugin API (`host.talker`, see `kraken_doa_v2/plugins/SDK.md`).

**ADS-B**: pick "ADS-B (1090 MHz)" as a VFO's digital decoder - that sets the VFO to 2.4 MHz bandwidth and tunes it and its tuner to 1090.000 MHz (in Coherent mode the whole array, in Independent mode just that VFO's tuner); the VFO is then shown as a single line at the centre of the spectrum. ADS-B is never part of Auto detect. Set the station location for distances and range checks. It needs a 1090 MHz antenna on the VFO's channel - in Coherent mode the CH selector's antenna; keep beamforming off for ADS-B (the beamformer follows one direction, the aircraft are all around). Independent mode lets one tuner sit on 1090 MHz while the others do something else.

New decoders can be added as plugins, and the **AI Signal Lab** (sidebar "🤖 AI Signal Lab") can write them for you: it points an LLM coding agent (Claude Code, or another LLM CLI) at the signal in a VFO, tells you what the signal is, and on request writes, builds and tests a decoder plugin for it - see "AI Signal Lab and decoder plugins" below.

The KrakenSDR Suite V2 software is currently in beta.

Install:

```
git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
./install.sh
```

You may need to reboot after running ./install.

Docker install (instead of install.sh / run.sh):

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

The containers then start automatically at every boot. Don't run the native
stack (run.sh or its boot service) at the same time. See [DOCKER.md](DOCKER.md)
for settings, hardware variants and troubleshooting.

To Run:

Simply use the command:

```
./run.sh
```

In any terminal window, with your KrakenSDR connected and powered up.

The Web UI can then be accessed at:

**Heimdall:** http://krakensdr.local:8070 or http://PI_IP_ADDR:8070

**KrakenSDR DOA:** https://krakensdr.local:8080, or https://PI_IP_ADDR:8080 (note, remember the 's' in  https)

## Digital voice codecs

A VFO's digital decoder can play the voice of P25, DMR, D-STAR, NXDN and TETRA
calls. Click "🔊 Listen" in the VFO's decoder tab (panel under the waterfall),
or pick Demod "Digital" on the VFO that is the Audio Src. Encrypted calls are shown and muted - they can't be decoded.

| System | Voice codec | Needs |
|---|---|---|
| P25 Phase 1 | IMBE | nothing - built in |
| DMR | AMBE+2 | mbelib, installed by you (below) |
| D-STAR | AMBE | mbelib, installed by you (below) |
| NXDN (NXDN48 and NXDN96) | AMBE+2 | mbelib, installed by you (below) |
| MPT1327 | analogue FM | nothing - listen to the granted traffic channel with NBFM |
| TETRA | ACELP | the ETSI reference codec, built by you (below) |

The AMBE / AMBE+2 and TETRA codecs are not shipped with this software:
AMBE+2 is a commercially licensed codec (DVSI), and the ETSI codec may not be
redistributed. You can build them yourself for personal use; whether that is
allowed where you live is your responsibility. The sidebar's "🔐 Digital
Decoders" box shows which codecs are installed and the install steps for the
missing ones (press "↻ Check again" after installing).

### DMR, D-STAR and NXDN voice: mbelib

```
sudo apt-get install -y git cmake build-essential
git clone https://github.com/szechyjs/mbelib.git
cd mbelib && mkdir build && cd build
cmake .. -DDISABLE_TEST=ON
make -j3
sudo make install
sudo ldconfig
```

Then restart the KrakenSDR software (`./run.sh`, or reboot when it runs as a
service). kraken_doa finds `libmbe.so.1` in the system library path; set
`KRAKEN_MBELIB=/path/to/libmbe.so.1` to use one elsewhere. API-compatible
mbelib forks work too.

### TETRA voice: ETSI codec (via osmo-tetra's patches)

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

Then restart the KrakenSDR software. kraken_doa runs `tetra-cdecoder` and
`tetra-sdecoder` from the PATH; or set `KRAKEN_TETRA_CODEC_DIR` to a directory
holding `cdecoder` and `sdecoder`. TETRA voice plays about half a second late
(the codec programs buffer their output), and only on cells without air
interface encryption.

### Docker

Build the codecs on the host as above (the `sudo make install` / `install`
steps can be skipped), then copy them into the container's data directory and
restart the client:

```
sudo mkdir -p docker-data/codecs
sudo cp -L mbelib/build/libmbe.so.1 docker-data/codecs/
sudo cp osmo-tetra/codec/c-code/cdecoder osmo-tetra/codec/c-code/sdecoder docker-data/codecs/
docker compose restart kraken_doa
```

(paths relative to where you cloned mbelib and osmo-tetra; copy only the ones
you built)

## AI Signal Lab and decoder plugins

### Decoder plugins

Every decoder in a VFO's decoder list is a plugin: a folder of C++
source in `kraken_doa_v2/plugins/<id>/`, which `./install.sh` / `make` builds.
The suite ships P25, DMR, TETRA, D-STAR, NXDN, MPT1327, POCSAG (pagers),
APRS (AX.25 1200 bit/s packet; written by the AI Signal Lab) and ADS-B
(1090 MHz aircraft, plotted on the 🗺 Map). "Auto detect"
runs every decoder ticked "Auto detect" in the sidebar's plugin list (Digital
Decoders box, "Decoder plugins") - all of them by default; untick the ones you never need, since each
costs CPU on every VFO in Auto detect. ADS-B has no Auto detect tick: it is
only used when picked. Decoders that need a much wider VFO than the one Auto
detect runs on are left out there. A decoder's own settings - the DMR timeslot,
the P25 NAC - appear in the panel while that decoder is selected, or once
Auto detect has locked onto it. Plugins run as separate processes, so a
faulty one cannot crash the receiver. To write one by hand, see
[kraken_doa_v2/plugins/SDK.md](kraken_doa_v2/plugins/SDK.md).

To add a plugin, or move one to another KrakenSDR, copy its folder (without
`build/`) into the receiver's `kraken_doa_v2/plugins/`, then build it and
press ↻ next to "Decoder plugins" in the Digital Decoders box (or restart the
software):

```bash
cd ~/krakensdr_suite/kraken_doa_v2      # wherever the suite is installed
make -j3                                # builds kraken_doa + every plugin
# or just the one plugin:
make -C plugins PLUGIN=<id>
```

Run the same `make` after editing a plugin's source; decoders that are using
it restart on the new build by themselves. The plugin compiles against the
receiver's own `plugins/sdk/` and `plugins/lib/`, so the receiving KrakenSDR
should run the same (or a newer) version of this software. A plugin is
native code that runs on your receiver: only install plugins from people you
trust.

### AI Signal Lab

1. Install Claude Code on the Pi and log in once (a Claude subscription or an
   Anthropic API key):

   ```
   curl -fsSL https://claude.ai/install.sh | bash
   claude          # log in, then /exit
   ```

2. Enable the lab, which checks the login first:

   ```
   cd krakensdr_suite/kraken_doa_v2
   python3 ai/kraken_ai.py setup
   ```

   It stays off until you do this. The web UI can't switch it on, because it
   lets UI users run an AI agent on the Pi and compile code there. If other
   people can reach your receiver's web UI, set an API token
   (`KRAKEN_API_TOKEN` / the `api_token` file) first. `python3 ai/kraken_ai.py disable`
   switches it off again.

3. In the web UI (sidebar "🤖 AI Signal Lab"), pick the VFO sitting on the
   signal and press **🔍 Investigate this signal with AI**. The lab records the
   signal (5-60 s at the VFO's bandwidth) and measures it: spectrum, bursts,
   envelope, FM levels, symbol rate and repeated sync words, plus plots. The
   agent can make more captures and run its own analysis scripts. Then it
   writes a report: what the signal is, the evidence for it, and whether an
   existing decoder handles it. The report, the live progress and the
   agent's steps appear in the **🤖 AI** tab of the panel under the waterfall
   (the sidebar only shows the job's status); ask follow-up questions there.
4. **🛠 Create Decoder** has the agent write a plugin for it, build it and test
   it on the captures. This takes about 5-30 minutes. The plugin then shows up
   in every VFO's decoder list and in the "Digital Decoders" box; "Use on VFO"
   next to the finished job selects it on the AI box's VFO.
   **✍ Custom Instructions** passes your own hints to the agent: specs or
   links, what to show, what to ignore. Pressing the button again on an
   existing plugin improves that plugin.

Every investigation is kept with its conversation: an "AI" badge appears in
the spectrum above the analysed frequency, and the AI Signal Lab's History list
shows them all. Clicking either opens the **🤖 AI** tab of the panel under the
waterfall (also the "🤖 AI" button there), with the whole chat, the agent's
steps and a box for further questions; ✕ deletes an analysis together with its
captures and chat.

The agent's tools are restricted. It can read the plugin SDK and the existing
decoder plugins, run the capture/analysis tool (`ai/sigtool.py`) and Python scripts
in its session folder, and write only in `ai/sessions/<session>/` and
`plugins/<id>/`. It can't read the receiver's settings, API key or TLS key.
Sessions (captures, plots, reports, chats) are kept in `kraken_doa_v2/ai/sessions/`.
Usage counts against your Claude plan or API budget. A typical
investigation costs about $0.30-0.50 API-equivalent, and a decoder about
$1-5.

Other LLM command-line tools can be used instead:
`python3 ai/kraken_ai.py setup --backend command --command "your-cli args"`
(the prompt goes to its stdin, and it runs in the session folder). Such a tool
gets no tool restrictions from the lab; its own configuration decides what it
may do. The AI Signal Lab needs the native install (it is not available in
Docker). The plugins that ship with the suite work in Docker as well.
