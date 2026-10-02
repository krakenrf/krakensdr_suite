# Running the KrakenSDR suite in Docker

The whole suite runs from one Docker image, as two containers:

| Container    | Runs                                   | Web UI                         |
|--------------|----------------------------------------|--------------------------------|
| `heimdall`   | coherent receiver server (owns the USB dongles) | http://PI_IP_ADDR:8070 |
| `kraken_doa` | DoA client (spectrum, audio, MUSIC DoA) | https://PI_IP_ADDR:8080 (DoA value page http://PI_IP_ADDR:8081) |

Both use the host's network directly, so every port, URL and tool works the
same as with `./run.sh`: 8070, 8080, 8081, 8091, 8092, 1234 (RTL-TCP), 8021
(local web mapper). gpsd on the host is reached on `localhost:2947`.

The dongles are passed through as the host's whole USB bus. That doesn't
change the coherence or calibration: heimdall talks to the same dongles
through the same libusb calls as it does natively.

## Supported hosts

The container needs the host's real USB bus, which limits where it runs:

| Host | Works? |
|---|---|
| Raspberry Pi 4 / 5, 64-bit Raspberry Pi OS | Yes. Tested on a Pi 5 (see *Test results*) |
| Linux PC (x86_64 or arm64) with Docker Engine | Should work, not yet tested |
| Linux with Docker Desktop | No |
| Mac (Intel or Apple Silicon) | No |
| Windows | Not recommended |

- **Linux PC**:
  - Nothing in the image is Pi-specific: the base image and both Makefiles
    support x86_64, and the runtime packages follow the architecture.
  - Use Docker Engine (`docker.io`, or Docker's get.docker.com script).
  - On a first run, check that heimdall finds every dongle and converges.
  - `--kerberos_sw` needs the Raspberry Pi's header GPIOs, so it falls back
    to manual calibration on a PC.
- **Docker Desktop (Linux, Mac, Windows)** runs containers inside a VM,
  which can't see the host's USB bus, so heimdall finds no dongles.
- **Windows**: USB devices can be attached to WSL2 with `usbipd-win`, with
  Docker Engine running inside WSL2. But every dongle then goes over USB/IP,
  and heimdall needs about 24 MB/s from 5 dongles without drops to stay
  coherent.

On a Mac or Windows PC, run the suite on a Pi (or a Linux machine) next to
the KrakenSDR instead. Then use the PC's browser for the web UIs on :8070 and
:8080.

## 1. Host setup (once)

You need a 64-bit Linux (Raspberry Pi OS / Debian / Ubuntu, arm64 or
x86_64; see *Supported hosts*) with the KrakenSDR plugged in. You **don't** need `install.sh`: the image
contains every build and runtime dependency, including the librtlsdr fork.

**Install Docker and Compose:**

```bash
# Raspberry Pi OS / Debian 13 (trixie):
sudo apt-get install -y docker.io docker-compose
# Raspberry Pi OS / Debian 12 (bookworm), Ubuntu - Docker's own packages:
#   curl -fsSL https://get.docker.com | sudo sh

sudo usermod -aG docker $USER      # use docker without sudo; log out and back in
```

**Keep the DVB-T kernel driver off the dongles.** Kernel modules belong to
the host, so this can't be done inside the container:

```bash
echo 'blacklist dvb_usb_rtl28xxu' | sudo tee /etc/modprobe.d/blacklist-dvb_usb_rtl28xxu.conf
sudo modprobe -r dvb_usb_rtl28xxu  # or reboot
```

**Don't run the native stack at the same time.** Only one program can own
the dongles and the ports. If you used `install-pi-service.sh`, turn its boot
service off:

```bash
./run.sh stop
sudo systemctl disable --now krakensdr     # or: ./install-pi-service.sh --uninstall
```

## 2. Install (build the image)

```bash
git clone https://github.com/krakenrf/krakensdr_suite
cd krakensdr_suite
docker compose build
```

The first build compiles everything and takes about 5 minutes on a Pi 5
(longer on a Pi 4).
Later builds reuse the dependency layers and only recompile the apps. Build
the image **on the machine that will run it**: kraken_doa compiles with
`-march=native`, so an image built on a Pi 5 can crash with "Illegal
instruction" on a Pi 4.

The build runs 3 compile jobs. That's right for an 8 GB Pi. On a smaller
Pi, set `JOBS=2` (4 GB) or `JOBS=1` (2 GB) in `.env`, because each job needs
about 1 GB of RAM.

## 3. Run

```bash
docker compose up -d          # start (also builds the image if there is none)
docker compose ps             # state + health of both containers
docker compose logs -f        # follow both logs (Ctrl+C stops following, not the apps)
docker compose restart        # restart both
docker compose stop           # stop; stays stopped, also across reboots
docker compose start          # start again after a stop
docker compose down           # stop and remove the containers
```

`up -d` returns straight away. Startup then runs the same way as `run.sh`:

1. heimdall waits until the KrakenSDR's 5 dongles are on the USB bus (at most
   45 s), opens them and calibrates.
2. kraken_doa waits for heimdall's phase calibration to converge (at most
   300 s, then it starts anyway), then starts its web UI.

The logs show errors only, as in `run.sh`'s headless mode. The apps' live
terminal dashboards aren't available in Docker. For full log output, put
`HEIMDALL_VERBOSE_LOG=1` / `KRAKEN_DOA_VERBOSE_LOG=1` in `.env` and run
`docker compose up -d`.

## 4. Start automatically at boot

1. Make sure the Docker service is enabled. Its packages enable it on install:

   ```bash
   sudo systemctl enable docker
   ```

2. Start the stack once with `docker compose up -d`.

That's all. Both containers have `restart: unless-stopped`, so Docker starts
them at every boot, and restarts an app that crashes. The USB wait in step 3
covers a cold boot where the dongles enumerate after Docker has started.

- `docker compose stop` keeps it off until you `docker compose start` it
  again, also across reboots.
- `docker compose down` removes the containers, so nothing starts at boot
  until the next `up -d`.

## 5. Settings (`.env`)

Optional settings go in a file called `.env` next to `docker-compose.yml`.
After changing it, run `docker compose up -d`, which recreates the containers
that changed.

```bash
# Hardware variant, the same flags as run.sh:
#   --ext_noise   add-on array with its own noise source on the CH0 bias tee
#   --wideband    KrakenSDR Wideband   (see "Hardware variants" below)
#   --kerberos    KerberosSDR, manual calibration
#   --kerberos_sw KerberosSDR with CKOVAL antenna switches (see below)
VARIANT_FLAGS=

KRAKEN_TUNERS=5        # dongles to wait for at startup (4 for a KerberosSDR, 0 = don't wait)
WAIT_TIMEOUT=300       # seconds kraken_doa waits for convergence (0 = forever)
KRAKEN_DATA_DIR=./docker-data   # where settings and recordings are kept
JOBS=3                 # compile jobs for the image build

KRAKEN_ALLOWED_HOSTS=  # extra host names for the web UIs (reverse proxy, custom DNS)
KRAKEN_API_TOKEN=      # client web UI API token (instead of the api_token file)
KRAKEN_GPSD_HOST=      # gpsd somewhere other than localhost:2947
KRAKEN_GPSD_PORT=
```

## 6. Your data

Everything the apps save lives under `docker-data/` (or `KRAKEN_DATA_DIR`)
and survives rebuilds, `down` and reboots:

```
docker-data/heimdall/heimdall_settings.conf   last frequency/gain, element count, ...
docker-data/heimdall/s2p_calibration/         forward-compensation files
docker-data/kraken_doa/doa_settings.json      client settings, incl. KrakenPro API key + station location
docker-data/kraken_doa/api_token              web UI API token (if used)
docker-data/kraken_doa/server.crt/.key        TLS certificate (self-signed, made on first start)
docker-data/kraken_doa/fft_wisdom.dat         FFTW plans (made on first start)
docker-data/kraken_doa/doa_recordings/        DoA recordings
```

The containers run as root, so these files belong to root. Read them freely;
to edit one, stop the stack and use `sudo`.

**Moving from a native install**: before the first start, copy the settings
you want to keep:

```bash
mkdir -p docker-data/heimdall docker-data/kraken_doa
cp heimdall_v2/heimdall_settings.conf docker-data/heimdall/
cp kraken_doa_v2/doa_settings.json kraken_doa_v2/server.crt kraken_doa_v2/server.key docker-data/kraken_doa/
# plus kraken_doa_v2/api_token if you have one
```

Copy `doa_settings.json` even if nothing else: the client sends its saved
frequency and gain to heimdall when it starts, so a client with no settings
retunes the array to its default (100 MHz).

`docker-data/` is in `.gitignore`, and `.dockerignore` keeps every settings
file out of the image, so the API key and station location never end up in
a commit or an image.

## 7. Updating

```bash
git pull
docker compose up -d --build
```

## 8. Hardware variants

- **Standard KrakenSDR**, **`--ext_noise`** and **`--kerberos`** need only USB
  and work with the compose file as it is.
- **`--wideband`** also needs the moRFeus LO (`/dev/hidraw*`).
- **`--kerberos_sw`** also needs the Pi's GPIO chip (`/dev/gpiochip*`) and
  `/proc/device-tree`, which Docker hides from normal containers. Without
  them heimdall falls back to manual calibration, as it does natively.

For those two, give heimdall full device access with a file called
`compose.override.yaml` next to `docker-compose.yml`. Compose picks it up
automatically:

```yaml
services:
  heimdall:
    privileged: true
```

## How it works

- **`Dockerfile`** has two stages:
  - The build stage (Debian trixie) clones the librtlsdr fork and uWebSockets
    at the commits the native build uses, then runs both apps' Makefiles.
  - The runtime stage installs only the Debian packages that own the
    libraries the binaries load, which `ldd` finds at build time, plus
    `openssl` and `usbutils`.
- **`docker/entrypoint.sh`** runs each app in its own directory under
  `/data`, with the web files linked in. The apps save their state in their
  working directory, so it ends up in `docker-data/`.
  - For heimdall it waits for the dongles first.
  - For kraken_doa it creates the TLS certificate if needed, then waits for
    convergence by reading the phase state from the 8091 packet header, like
    `run.sh`. It doesn't wait in KerberosSDR manual mode, which it detects
    from the header flag.
  - Empty variables from compose are removed, because the apps treat an
    empty variable as set.
- **`docker-compose.yml`**:
  - `network_mode: host`.
  - `/dev/bus/usb` bind-mounted plus the cgroup rule `c 189:* rmw`, rather
    than a fixed device list. A dongle that re-enumerates gets a new
    `/dev/bus/usb/...` node: heimdall resets the USB devices at startup, and
    you can unplug and replug the KrakenSDR. A device list is fixed when the
    container is created; the bind mount follows the host.
  - `ulimits: rtprio: 30` lets heimdall raise its USB reader and sample-drain
    threads to SCHED_RR 30. It's the same as `LimitRTPRIO=30` in the native
    boot service.
  - `ulimits: core: 0`: containers inherit the Docker daemon's unlimited core
    size, so every crash wrote a ~280 MB core file into `docker-data/`.
  - Log rotation (3 × 10 MB) and health checks: the control port (8092)
    answering for heimdall, the web port for kraken_doa.

## Test results

Tested on a Raspberry Pi 5 (8 GB, Raspberry Pi OS trixie, Docker 26.1,
Compose 2.26) with a 5-channel KrakenSDR. Docker and native heimdall were
compared with the same settings and the same cold-start sequence:

| | Docker | Native |
|---|---|---|
| First phase convergence after start | 20-26 s | 17-25 s |
| Recalibration after a retune | 3.5 s | 3.5 s |
| Check Calibration, worst residual phase | 0.13-0.34° | 0.12-0.24° |
| Noise source on, worst channel phase error (compensated stream) | 0.5-1.7° | 0.4-1.6° |
| Coherence-loss events (USB drops) | 0 | 0 |
| SCHED_RR USB threads | yes (rtl-rx 20, sample-drain 15) | yes |

The differences are within the spread between two calibrations of the same
setup.

Also tested:
- kraken_doa waits for convergence, then streams spectrum and MUSIC DoA over
  WSS.
- A heimdall crash (SIGSEGV) is restarted by Docker and recalibrates in
  ~26 s, and the client reconnects by itself.
- Settings survive `down` / `up`.
- The USB wait gives up and starts anyway after `USB_WAIT` seconds.
- Not tested on hardware: `--wideband` and `--kerberos_sw` (privileged
  mode), a physical USB replug, and an actual reboot. The boot start uses
  Docker's restart policy.

## Troubleshooting

- **heimdall finds fewer than 5 dongles / "no devices"**:
  - Check that the host sees them: `lsusb | grep 0bda:2838`.
  - Check that the container sees them:
    `docker compose exec heimdall lsusb | grep 0bda:2838`.
  - For a full device test, stop heimdall first, then run the fork's
    `rtl_test` inside the image:
    `docker compose stop heimdall && docker compose run --rm --no-deps heimdall rtl_test -t`
  - Check that the DVB-T driver is blacklisted (`lsmod | grep dvb_usb_rtl28xxu`
    should print nothing).
- **"SCHED_RR unavailable" in the heimdall log**: the `ulimits: rtprio`
  setting is missing from the compose file. heimdall still runs, but at
  normal priority, which risks USB overruns under load.
- **Port already in use / "address in use"**: the native stack is running.
  Stop it with `./run.sh stop` and `sudo systemctl disable --now krakensdr`.
- **"Illegal instruction"**: the image was built on a different CPU. Rebuild
  it on this machine with `docker compose build --no-cache`.
