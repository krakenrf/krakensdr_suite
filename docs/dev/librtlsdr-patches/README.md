# librtlsdr fork: three commits that are not upstream yet (2026-11-21 / 23)

The patches here go, in order, on top of krakenrf/librtlsdr 3ad1746 (the fork's
`master`). They are kept in this repository only until the fork has them;
then this folder goes, and the Dockerfile's `LIBRTLSDR_COMMIT` can move.

1. `0001-no-stream-slip-...`: a USB control IN of 3, 7, 11 ... bytes made the
   RTL2832U drop 4 bytes (2 I/Q samples) of its stream - every retune's PLL
   lock check read 3 tuner registers, so each retune shifted that dongle
   against the others. Such reads are padded, the lock check reads 4
   registers, the PLL settle wait also follows a big VCO jump,
   `RTLSDR_NO_STREAM_SLIP` tells applications, and
   `RTLSDR_TUNE_NO_DUMMY_READ` saves a transfer per sweep hop.
2. `0002-repair-registers-...`: tuner registers an early lock read can
   corrupt are written back after a lock retry; a failed first read under
   the new flag is retried by the library; the header says exactly what the
   no-slip promise covers.

3. `0003-fast-tuner-gain-write-...` (2026-11-23): `rtlsdr_set_tuner_gain_fast`
   and `rtlsdr_set_center_freq_gain_fast` (macro `RTLSDR_HAS_GAIN_FAST`):
   sweep mode's per-hop gain written with the retune in 1 more USB transfer
   instead of 4 (the same gain table and register cache as
   `rtlsdr_set_tuner_gain`); `../librtlsdr-fork.md`, *A fast tuner gain write*.

What was measured, and what was not (a Raspberry Pi, Windows, macOS):
`../librtlsdr-fork.md`, *A 3-register tuner read drops 2 samples*, *The
rule, and the fix*, *Review round*.

Apply, from the repository root (the fork checkout is `librtlsdr/`):

```bash
git -C librtlsdr switch -c lock-read-no-slip
git -C librtlsdr am ../docs/dev/librtlsdr-patches/0001-*.patch ../docs/dev/librtlsdr-patches/0002-*.patch ../docs/dev/librtlsdr-patches/0003-*.patch
make -C heimdall_v2        # rebuilds the static library from the fork's sources, then heimdall
```

heimdall builds and runs against the fork with or without them: without,
its return from sweep / independent mode to coherent re-acquires every
channel's lag (25-30 s instead of ~2 s) and sweep hops use the slower tune
call.
