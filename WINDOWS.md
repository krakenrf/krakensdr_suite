# KrakenSDR v2 on Windows

The suite builds and runs natively on Windows 10 / 11 (x64): `heimdall.exe`
(the coherent receiver), `kraken_doa.exe` (the DoA client and its web UI) and
the decoder plugins are ordinary, self-contained Windows programs - no WSL, no
Cygwin, no MSYS2 DLLs at run time. `krakensdr.exe` (or `run.bat`) starts both
apps the way `run.sh` does on Linux.

## Quick start

1. **USB driver (once per dongle).** Windows needs the WinUSB driver for the
   KrakenSDR's five RTL2832U tuners. Download [Zadig](https://zadig.akeo.ie),
   plug in the KrakenSDR, then in Zadig: *Options > List All Devices*, pick
   **Bulk-In, Interface (Interface 0)** (USB ID `0BDA 2838`), driver **WinUSB**,
   *Replace Driver* (or *Install Driver*). Repeat for each of the five entries
   (the list shows one per tuner). Do NOT replace the driver of interface 1, and
   on the KrakenSDR Wideband variant leave the moRFeus LO (`10C4 EAC9`) on
   Windows' own HID driver.
2. **Build.** Double-click `build_windows.bat` (or run it in a terminal). The
   first run installs [MSYS2](https://www.msys2.org) into `C:\msys64` if it is
   not there (no administrator rights needed; ~1 GB with the compilers), then
   builds everything - 10-20 minutes the first time, incremental afterwards.
   An existing MSYS2 is used from `%LOCALAPPDATA%\msys64` or `C:\msys64`, or
   from the folder `set MSYS2_ROOT=...` names (e.g. `D:\msys64`, no longer
   looked for by itself). One that belongs to another account is not used (it
   may have been planted there), and one every account can change - what
   `C:\` gives its folders, so most `C:\msys64` installs - builds with a
   warning that shows the `icacls` command making it yours (others can still
   read and run it); a new install is made yours straight away.
3. **Run.** Double-click `run.bat`. With Windows Terminal (standard on Windows
   11) one window opens, split in two: Heimdall's dashboard on top, the DoA
   client's below it (the tmux layout of run.sh); without it, two console
   windows. The client starts once Heimdall's phase calibration has converged,
   and the browser opens the client UI.
   - Heimdall: <http://localhost:8070>
   - DoA client: <https://localhost:8080> (self-signed certificate: accept the
     browser warning once), DoA value page for the Android app <http://localhost:8081>
4. **Windows Firewall** asks once per program whether `heimdall.exe` and
   `kraken_doa.exe` may accept connections - allow them on private networks
   to reach the web UIs from other PCs / phones (localhost always works).

Check the dongles at any time with `librtlsdr\build\src\rtl_test.exe -t`
(`tools\rtl_test.exe` in a package): it must list `SN: 1000` .. `1004`.

## Running

`run.bat` passes its options to `krakensdr.exe`:

| Command | |
|---|---|
| `run.bat` | start (Windows Terminal split, else two windows) |
| `run.bat stop` | stop everything cleanly (heimdall releases the dongles), force after 10 s |
| `run.bat --headless` | no dashboards: both apps log to `logs\heimdall.log` / `logs\kraken_doa.log`, this console follows them; Ctrl+C stops (`NO_TMUX=1` works too) |
| `run.bat --wideband` | KrakenSDR Wideband variant (both apps) |
| `run.bat --kerberos` | KerberosSDR, manual calibration only |
| `run.bat --ext_noise` | add-on array with its own noise source on the CH0 bias tee |
| `run.bat --wait-timeout 600` | convergence wait (default 300 s, 0 = forever; `WAIT_TIMEOUT`) |
| `run.bat --no-browser` / `--no-wt` | don't open the browser / two windows even with Windows Terminal |

As with run.sh, each app runs under a supervisor that restarts it after a
crash; Ctrl+C in a pane, closing the window or `run.bat stop` stops it for
good. Signing out, shutting down or a Windows Update restart stops the
receiver cleanly too (the dongles are released, settings and logs saved
first). The apps can also be started on their own (`heimdall_v2\heimdall.exe`,
`kraken_doa_v2\kraken_doa.exe`) with the same options as on Linux - their
working directory is their own folder (they switch to it when started from
elsewhere), where their settings and data files live.

## Building by hand

`build_windows.bat` runs `windows/build.sh` in MSYS2's **UCRT64** environment.
In an "MSYS2 UCRT64" shell the usual commands work:

```bash
cd heimdall_v2 && make -j8          # heimdall.exe (+ the librtlsdr fork, uWebSockets)
cd kraken_doa_v2 && make -j8        # kraken_doa.exe + the plugins (+ liquid-dsp)
make -C windows                     # krakensdr.exe
windows/package.sh                  # dist\KrakenSDR-windows\ + .zip: copy to any PC
```

The default build targets x86-64-v3 (AVX2: Intel Haswell / AMD Zen and later)
so the exes run on other PCs; `make WIN_MARCH=-march=native` tunes for the
build machine only. For an older CPU without AVX2 (`krakensdr.exe` warns about
it, and the apps stop with an illegal instruction) build for x86-64-v2 (SSE4.2)
as a clean rebuild: `set WIN_MARCH=-march=x86-64-v2` (plus `set WHISPER=1`
when Voice to Text is set up, so it is rebuilt too), then
`build_windows.bat clean`. By hand: `export WIN_MARCH=-march=x86-64-v2`, then
`make clean && make` in `heimdall_v2` and in `kraken_doa_v2` (whose clean
includes the plugins), and `make whisper` there for Voice to Text. Keep
`WIN_MARCH` set for later builds (Windows' Environment Variables dialog makes
it permanent): the objects remember the `WIN_MARCH` they were compiled with,
so a build without it recompiles everything for x86-64-v3 again.

A package from `windows/package.sh` holds only what runs - no sources, no
Makefiles, no `build_windows.bat` - so it can't build anything itself: Voice
to Text (`WHISPER=1`) and new or changed decoder plugins are built in the
source checkout (`build_windows.bat`), then packaged again with
`windows/package.sh` (or copy the new
`kraken_doa_v2\transcribe\build\kraken_whisper.exe` /
`kraken_doa_v2\plugins\<id>\build\decoder.exe` into the same folder of the
package; ↻ in the plugin list picks up a new plugin). The package carries no
speech models: the Voice to Text box downloads them. The build steps the web
UI shows on a Windows receiver are meant for that source checkout.

## What is different from Linux

- **USB**: heimdall reaches the dongles through WinUSB with a few Windows-only
  fixes in the librtlsdr fork ([krakenrf/librtlsdr](https://github.com/krakenrf/librtlsdr)
  since commit 00606c0; heimdall needs 578b43d or later on every platform -
  its faster retunes and the retry of a stalled USB control write - and its
  `make` updates an older `librtlsdr` clone, or stops if it can't). The important one: the sample stream runs
  with WinUSB's RAW_IO policy - without it the dongles dropped short bursts of
  samples a few times a minute and the array lost its sample alignment. It
  needs libusb 1.0.30 or newer (MSYS2's current one; an older libusb gives a
  build warning). The dongles are opened and tuned one after the other, as
  concurrent USB control transfers to them time out on Windows.
- **Retuning** takes about 0.11 s on Windows for all five tuners (on a
  Raspberry Pi 5 about 0.03 s). On some PCs every USB control transfer to
  the KrakenSDR costs 3-4 ms - measured on an AMD X870E board's chipset USB
  ports - and they run one at a time, so the librtlsdr fork keeps a retune to
  5 of them per tuner (it was 18-20, about 0.42 s). While you drag the
  spectrum or spin the frequency digits each step after the first needs only
  3 per tuner, about 0.07 s for the five; the receiver skips the frequencies
  it has already passed and settles on the last one shortly after you let
  go. (The same library changes make a retune cheaper on Linux too: on the
  Pi 6 transfers per tuner instead of 18, a drag step 4.) If
  dragging still feels slow, try the KrakenSDR on a USB port of another
  controller (on AMD boards, a rear port wired to the CPU rather than the
  chipset).
- **Realtime threads**: the USB reader and drain threads are registered with
  the Multimedia Class Scheduler (MMCSS, "Pro Audio") - no administrator rights
  or limits.conf needed; heimdall runs at above-normal priority and keeps the
  PC from going to sleep while it runs (sleep would stop the USB streams).
- **`--kerberos_sw`** needs a Raspberry Pi's GPIO header for the antenna
  switches: on Windows heimdall falls back to `--kerberos` (manual calibration)
  with a warning.
- **Wideband variant**: the moRFeus LO is driven through Windows' HID driver.
- **GPS**: the client reads gpsd's JSON protocol (port 2947) as on Linux; on
  Windows that needs a gpsd-compatible server, otherwise use a static location.
- **Python** (the AI assistant, Debug Playback's test transmissions): Python 3
  from python.org (found through its `py` launcher) or the Microsoft Store.
  The test transmissions need numpy + scipy: `py -m pip install numpy scipy`.
  `set KRAKEN_PYTHON=C:\path\to\python.exe` picks a particular interpreter.
- **AI assistant** (the 🤖 dock): enabled on the PC itself with
  `py -3 ai/kraken_ai.py setup` in the `kraken_doa_v2` folder; it needs Claude
  Code (in PowerShell `irm https://claude.ai/install.ps1 | iex`, then `claude`
  once to log in) and Git for Windows, which Claude Code requires. It answers,
  looks things up, controls the receiver, captures and plots as on Linux, but
  **without its sandboxes**: Claude Code has no sandbox on Windows, so the
  assistant gets no shell there (it can't build plugins or run scripts - the
  dock says so), and a plugin it changed is not run at all (no bubblewrap to
  run it in) until you have checked it and run
  `py -3 ai/kraken_ai.py trust <id>` (`py -3 ai/kraken_ai.py sandboxed` lists
  them). Build such a plugin yourself with `build_windows.bat`.
- **Voice to Text** is optional as on Linux: `set WHISPER=1` before
  `build_windows.bat` (or `make -C kraken_doa_v2 whisper` in an MSYS2 UCRT64
  shell) builds whisper.cpp and fetches the base.en model.
- **Digital voice codecs**: DMR / D-STAR / NXDN AMBE voice uses a user-built
  mbelib DLL (`libmbe.dll`, or `KRAKEN_MBELIB=<path>`), as the `.so` on Linux.
  **TETRA voice is not available on Windows** (the ETSI codec programs are
  chained through their standard input / output, which Windows can't name as
  files); everything else of TETRA decodes.
- **RTL-TCP (port 1234)**: connect SDR# / GQRX to `127.0.0.1:1234` rather than
  `localhost` - heimdall listens on IPv4, and Windows takes about 2 s to give
  up on `localhost`'s IPv6 address first.
- **Network folders**: the IQ / audio recordings and Decoder Logging folders
  can't be a share (`\\server\share`): Windows signs in to the host a folder
  names as you, and any page reaching the web UI could name one. Map the share
  to a drive letter (`Z:\...`), or set the environment variable
  `KRAKEN_ALLOW_NETWORK_DIRS=1` (Windows' Environment Variables dialog) and
  start the stack again.

## Troubleshooting

- **"No RTL-SDR devices found" / "no WinUSB driver"**: install WinUSB with Zadig
  (step 1). A dongle opened by another program (SDR#, a second heimdall) is busy:
  WinUSB lets only one program open a device.
- **A port is in use / access denied**: `run.bat stop` ends a previous stack.
  With Hyper-V, WSL2 or Docker Desktop, Windows reserves port ranges - check
  `netsh int ipv4 show excludedportrange protocol=tcp` (8070, 8080, 8081, 8091,
  8092, 1234 must be free).
- **Antivirus**: freshly built, unsigned exes are sometimes quarantined or slow
  to start the first time; allow the repository folder if needed.
- **Logs**: with `--headless` the apps log errors only (as on Linux);
  `set HEIMDALL_VERBOSE_LOG=1` / `set KRAKEN_DOA_VERBOSE_LOG=1` for everything.
