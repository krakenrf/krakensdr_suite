KrakenSDR V2 software. The new V2 software moves away from Python and implements everything in C++, which has resulted in massive speed improvements on slow hardware like the Pi 4 and Pi 5.

We are now able to have a real time spectrum and audio demodulation at the same time that the MUSIC DoA is run.

Any number of VFOs can decode digital radio signals at once - P25 Phase 1, DMR, TETRA, D-STAR, NXDN, MPT1327 (analogue trunking control channels), POCSAG pagers and APRS - with automatic mode detection: tick "Digital decoder" on a VFO in the Decimators box and pick "Auto detect" or a decoder. Each decoding VFO gets its own tab (settings, decoded data, full event log) in the panel under the waterfall ("🔐 Decoders" button, or ⤢ on the VFO); the sidebar "Digital Decoders" box lists the installed voice codecs and decoder plugins. It shows network/site identities, talkgroups, radio IDs, callsigns, messages and call activity, and can play the voice (P25 built in; DMR, D-STAR, NXDN and TETRA with codecs you install yourself - see "Digital voice codecs" below). Every decoder is a plugin, so new ones can be added.

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
The suite ships P25, DMR, TETRA, D-STAR, NXDN, MPT1327, POCSAG (pagers) and
APRS (AX.25 1200 bit/s packet; written by the AI Signal Lab). "Auto detect"
runs every decoder ticked "Auto detect" in the sidebar's plugin list (AI Signal
Lab box) - all of them by default; untick the ones you never need, since each
costs CPU on every VFO in Auto detect. A decoder's own settings - the DMR timeslot,
the P25 NAC - appear in the panel while that decoder is selected, or once
Auto detect has locked onto it. Plugins run as separate processes, so a
faulty one cannot crash the receiver. To write one by hand, see
[kraken_doa_v2/plugins/SDK.md](kraken_doa_v2/plugins/SDK.md).

To move a plugin to another KrakenSDR, either copy its folder (without
`build/`) into that receiver's `kraken_doa_v2/plugins/` and run `make` in
`kraken_doa_v2`, or use **Export** in the Digital Decoders box (one
`.krakenplugin.json` file) and **Import** it on the other receiver.

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
   in every VFO's decoder list and in the "Digital Decoders" box ("Use on VFO"
   selects it on the AI box's VFO).
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
