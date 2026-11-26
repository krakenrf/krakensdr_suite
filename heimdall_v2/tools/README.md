# heimdall tools

## zctest.c - byte-exact USB sample-loss test

Tests librtlsdr's async read path the way heimdall uses it
(`rtlsdr_read_async(dev, cb, ctx, 32, 32768)`, one SCHED_RR reader thread per
dongle) without heimdall. The RTL2832 runs in counter test mode
(`rtlsdr_set_testmode`): every byte is the previous one + 1 (mod 256), so
every buffer can be checked exactly - counter continuity across and inside
buffers (one break = a clean gap, more = corrupted data), repeated byte pairs
(stale or zero fill), and the arrival time of every buffer (a whole lost or
duplicated 32 KB transfer is invisible to an 8-bit counter, so a step of
more than half a buffer period between 1 s windows of arrival residuals is
counted as one). It was written for the zero-copy investigation
(`../../docs/dev/librtlsdr-fork.md`, *Zero-copy USB buffers*).

Build it against the fork's static library (from `heimdall_v2/`; verified
2026-11-13 on x86_64 Ubuntu):

```bash
gcc -O2 -Wall -I../librtlsdr/include -o zctest tools/zctest.c ../librtlsdr/build/src/librtlsdr.a -lusb-1.0 -lpthread -lm
```

Zero-copy is compiled into the library by default since 2026-11-14 and used
only by a handle that calls `rtlsdr_prealloc_async()`. This zctest does not
call it, so it tests the user-space buffer path (the Pi measurements of the
zero-copy path used a variant with a prealloc option that is not in the
repo). For a library without the zero-copy code, configure a separate build directory (the source
tree is not touched) and link that `librtlsdr.a` instead:

```bash
cmake -S ../librtlsdr -B /tmp/rtlsdr-nozc -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DENABLE_ZEROCOPY=0
cmake --build /tmp/rtlsdr-nozc -j3 --target rtlsdr_static     # -j3 at most on a Pi
```

Run it with heimdall (and anything else using the dongles) stopped - it
opens the dongles by serial and puts them in test mode:

```bash
./zctest -s 1000,1001,1002,1003,1004 -t 600        # 10 min, all 5 KrakenSDR channels
```

Options: `-t` seconds (60), `-b` / `-l` transfer count / length (32 /
32768, heimdall's), `-P` SCHED_RR priority (20; without rtprio the threads
run SCHED_OTHER - the report says which), `-p` poison every buffer after
its check (catches a buffer the next completion did not refill), `-r N`
cancel and restart every stream every N s, `-R` with `-r` also close and
reopen the dongles (heimdall's stream-restart path), `-S ms` stall channel
0's callback once, 5 s in (a self-test of the gap and timing detectors).
It prints one line per dongle and a `TOTAL:` line; a clean run has
`gaps 0, corrupt 0, short 0, poison-pairs 0, timing +T 0 -T 0`. Gaps in the
first buffers of a stream are reported separately (`startup:`) - the
RTL2832 FIFO may overflow before the transfers are submitted.

## The zero-copy change in the fork

The change to krakenrf/librtlsdr from that investigation is in the fork
(701a832 and 3ad1746, on its `master`; the patch that stood in for them here
until then is gone): zero-copy is compiled in on Linux and used by a handle that
calls `rtlsdr_prealloc_async()` (`-DENABLE_ZEROCOPY=0` leaves it out),
zero-copy buffers are mapped once per handle and kept until
`rtlsdr_close()`, `rtlsdr_prealloc_async()` maps them ahead of the stream
start (`RTLSDR_HAS_PREALLOC_ASYNC`; heimdall calls it in
`init_rtlsdr_device()`), upstream's usbfs-mmap-bug check, the stop path
waits for every cancelled transfer, and (added 2026-11-13) the FIFO hold:
`rtlsdr_set_fifo_hold()` / `rtlsdr_release_fifos()` (`RTLSDR_HAS_FIFO_HOLD`;
heimdall holds every dongle's FIFO at open and starts all streams together
on Linux - `../../docs/dev/librtlsdr-fork.md`, *FIFO hold*). Details and
measurements: `../../docs/dev/librtlsdr-fork.md`, *Zero-copy USB buffers*.

## rtl_filter_phase/ - the R820T IF filter registers from the environment (a test build)

For measurements of the tuner's IF filter settings on the running array
(`../../docs/dev/librtlsdr-fork.md`, *The R820T IF filter*). Nothing here is
part of a normal build; it works on scratch copies only.

- `r82xx-env-test.patch` (for a COPY of the fork, 9644d07): switches on the
  fork's own `USE_R82XX_ENV_VARS` code (`include/tuner_r82xx.h` defines it 0
  unconditionally, so a `-D` does not work) and adds test prints. The tuner
  then takes, at every open and for every dongle of the process:
  `RTL_R820_R10_LO` (R10 bits 3:0, FILT_CODE, 0-15; 16 = the chip's own
  calibration code, as stock osmocom rtl-sdr writes it), `RTL_R820_R10_HI`
  (R10 bits 7:4; without `R10_LO` the low bits are then not written),
  `RTL_R820_R11_HI` (bits 7:5, FILT_BW), `RTL_R820_R11_LO` (bits 3:0,
  HP_COR), `RTL_R820_R9_76`, `RTL_R820_R30_HI`, `RTL_R820_IF_CENTER` (Hz; the
  tuned frequency stays), and with `RTL_R820_PRINT_I2C=1` prints every such
  register write, a `REGDUMP` line (R9 / R10 / R11 / R30, the IF centre, the
  calibration code) and a read-back, each tagged with the dongle's serial.
- `heimdall-log-precision.patch` (for a COPY of `heimdall_v2`): heimdall's
  log rounds every number to 0.1 after the startup banner; `analyze.py`
  needs each channel's trim to 3 decimals.
- `measure.py`: starts that heimdall from a scratch directory, sets
  frequency and gain, and per round (a full `reset_lag_compensation`
  between rounds) averages 1024-point cross-spectra of every channel against
  CH0 from the 8091 stream with the noise source on in CONVERGED; saves
  `runs/<label>/results.json`, `curves.npz`, `heimdall.log`.
- `analyze.py`: per run and channel the phase against CH0 as heimdall left
  it and with constant and slope removed, levels, coherence; the trim
  filter's own response divided out (`NOFIR=1`: not).

```bash
T=/tmp/rtlfilt && mkdir -p $T                      # from the repository root; heimdall stopped
cp -a librtlsdr $T/librtlsdr && rm -rf $T/librtlsdr/build
patch -d $T/librtlsdr -p1 < heimdall_v2/tools/rtl_filter_phase/r82xx-env-test.patch
cp -a heimdall_v2 $T/heimdall_v2
patch -d $T/heimdall_v2 -p1 < heimdall_v2/tools/rtl_filter_phase/heimdall-log-precision.patch
make -C $T/heimdall_v2 -j3                         # builds the patched fork copy too (../librtlsdr)
cp $T/heimdall_v2/heimdall $T/heimdall_v2/rfp_heimdall
cp heimdall_v2/tools/rtl_filter_phase/*.py $T/
python3 $T/measure.py F0 --rounds 5                                    # the fork's setting
python3 $T/measure.py B1 --rounds 5 --env RTL_R820_R10_LO=15           # FILT_CODE as before 359a94d
python3 $T/measure.py B2 --rounds 5 --env RTL_R820_R10_LO=16 --env RTL_R820_R10_HI=12 \
        --env RTL_R820_IF_CENTER=1815000 --env RTL_R820_PRINT_I2C=1    # stock osmocom's values
python3 $T/analyze.py F0 B1 B2 --baseline F0       # tables: $T/analysis/<label>.txt
```

`measure.py` needs numpy, the KrakenSDR's serials 1000-1004, heimdall's ports
free, and nothing else on the dongles. Written and used on x86_64 Linux
(2026-11-26); not run on a Pi.
