KrakenSDR V2 software. The new V2 software moves away from Python and implements everything in C++, which has resulted in massive speed improvements on slow hardware like the Pi 4 and Pi 5.

We are now able to have a real time spectrum and audio demodulation at the same time that the MUSIC DoA is run.

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
