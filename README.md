KrakenSDR V2 software. The new V2 software moves away from Python and implements everything in C++, which has resulted in massive speed improvements on slow hardware like the Pi 4 and Pi 5.

We are now able to have a real time spectrum and audio demodulation at the same time that the MUSIC DoA is run.

Each VFO can also decode digital radio signalling - P25 Phase 1, DMR, TETRA, D-STAR, NXDN and MPT1327 (analogue trunking control channels), with automatic mode detection (sidebar "Digital Decoder" panel). It shows network/site identities, talkgroups, radio IDs, callsigns and call activity, and can play the voice (P25 built in; DMR, D-STAR, NXDN and TETRA with codecs you install yourself - see "Digital voice codecs" below).

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

The Digital Decoder (sidebar "🔐 Digital Decoder", or "Digital" in a VFO's
Demod list) can play the voice of P25, DMR, D-STAR, NXDN and TETRA calls. Click
"🔊 Listen" in the panel, or pick Demod "Digital" on the VFO that is the Audio
Src. Encrypted calls are shown and muted - they can't be decoded.

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
allowed where you live is your responsibility. The panel's "Voice codecs"
line shows what was found.

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
