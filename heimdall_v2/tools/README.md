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

For a zero-copy build of the library, configure a separate build directory
(the source tree is not touched) and link that `librtlsdr.a` instead:

```bash
cmake -S ../librtlsdr -B /tmp/rtlsdr-zc -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DENABLE_ZEROCOPY=1
cmake --build /tmp/rtlsdr-zc -j3 --target rtlsdr_static     # -j3 at most on a Pi
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

## librtlsdr-zerocopy-map-once.patch

The change to krakenrf/librtlsdr from that investigation, kept here until it
is in the fork: zero-copy stays off by default (`-DENABLE_ZEROCOPY=1`
enables it), zero-copy buffers are mapped once per handle and kept until
`rtlsdr_close()`, `rtlsdr_prealloc_async()` maps them ahead of the stream
start (`RTLSDR_HAS_PREALLOC_ASYNC`; heimdall calls it in
`init_rtlsdr_device()`), upstream's usbfs-mmap-bug check, and the stop path
waits for every cancelled transfer. Apply it in `../librtlsdr` with
`patch -p1 < ../heimdall_v2/tools/librtlsdr-zerocopy-map-once.patch` and
rebuild the static library (`make -C heimdall_v2` does). Details and
measurements: `../../docs/dev/librtlsdr-fork.md`, *Zero-copy USB buffers*.
