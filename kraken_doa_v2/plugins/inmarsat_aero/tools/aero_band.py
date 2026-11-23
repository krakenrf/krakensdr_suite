#!/usr/bin/env python3
"""A 2.4 MHz Inmarsat Aero band for the "All channels in the spectrum" mode: several
P channels (aero_gen.py) side by side, a C channel opened by an assignment
on one of them, and carriers of other systems between them - every antenna
of a 5-element UCA (r 0.06 m: L band, README), the satellite at one bearing, noise per
antenna. Writes OUT.cu8 (2.4 MHz, sample-major, rms 0.15 like heimdall's) +
OUT.json {rate, channels, rf_hz, radius_m}.

  aero_band.py OUT.cu8 [--seconds S] [--rf HZ] [--bearing DEG] [--seed N]
               [--chan KIND:RATE:OFFSET_HZ:EBN0_DB[:START_S[:ITEM;ITEM...]]]...
               [--cw OFFSET_HZ:SNR_DB] [--other OFFSET_HZ:SNR_DB]

KIND p (RATE 600 / 1200 / 2400 / 4800 / 10500) or c (RATE 8400); ITEMs as
aero_gen.py --script (a C channel: aes:HEX, its aircraft). --cw: an unmodulated carrier; --other: a carrier with a
strong unmodulated part and 1200 Bd BPSK on it (another system). Without
--chan: the default scene (see DEFAULT). Content is synthetic.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

import numpy as np

FS = 2400000
CH_FS = 24000
SEG = CH_FS            # 1 s segments, hop 0.5 s (Hann, 50 %: sums to 1)
C_LIGHT = 299792458.0

# rf 1545.1 MHz; the C channel at +400 kHz = 1545.5 MHz: code (1545.5 - 1510) / 0.0025 = 14200 = 0x3778
DEFAULT = [
    "p:600:-300000:14",
    "p:600:-295000:14",
    "p:1200:-250000:14",
    "p:2400:-120000:14",
    "p:4800:60000:14",
    "p:10500:200000:14:0.3:logon:4CA2B1;acars:4CA2B1:EI-ABC:H1:A:METAR UPDATE FOLLOWS;cass:4CA2B1:3778:0B50",
    "c:8400:400000:12:6.5:aes:4CA2B1",
]
DEFAULT_CW = ["-200000:25"]
DEFAULT_OTHER = ["-50000:22"]


def gen(tools, kind, rate, seconds, start, script, seed, path):
    cmd = [sys.executable, "-I", os.path.join(tools, "aero_gen.py"), path, kind, "--seconds", str(seconds),
           "--start", str(start), "--seed", str(seed), "--out-rate", str(CH_FS)]
    if kind == "p":
        cmd += ["--rate", str(rate)]
        if script:
            cmd += ["--script"] + script
    else:   # a C channel: its AES (ITEM aes:HEX)
        for it in script:
            if it.startswith("aes:"):
                cmd += ["--aes", it[4:]]
    subprocess.run(cmd, check=True, stderr=subprocess.DEVNULL)
    x = np.fromfile(path, dtype=np.complex64).astype(np.complex128)
    p = np.mean(np.abs(x[np.abs(x) > 1e-3]) ** 2)
    return x / np.sqrt(p)       # unit power while on


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=12)
    ap.add_argument("--rf", type=float, default=1545.1e6)
    ap.add_argument("--bearing", type=float, default=120)
    ap.add_argument("--radius", type=float, default=0.06)
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--chan", action="append", default=None)
    ap.add_argument("--cw", action="append", default=None)
    ap.add_argument("--other", action="append", default=None)
    a = ap.parse_args()
    tools = os.path.dirname(os.path.abspath(__file__))
    chans = a.chan if a.chan is not None else DEFAULT
    cws = a.cw if a.cw is not None else ([] if a.chan is not None else DEFAULT_CW)
    others = a.other if a.other is not None else ([] if a.chan is not None else DEFAULT_OTHER)
    rng = np.random.default_rng(a.seed)
    nout = int(a.seconds * FS)
    nch = int(a.seconds * CH_FS)
    # noise: unit variance per complex sample at 2.4 MHz -> N0 = 1 / FS
    n0 = 1.0 / FS
    srcs = []   # (24 kHz baseband, offset Hz)
    truth = []
    with tempfile.TemporaryDirectory() as td:
        for k, spec in enumerate(chans):
            p = spec.split(":", 5)
            kind, rate, off, ebn0 = p[0], int(p[1]), float(p[2]), float(p[3])
            start = float(p[4]) if len(p) > 4 else 0.3
            script = p[5].split(";") if len(p) > 5 and p[5] else []
            x = gen(tools, kind, rate, a.seconds - start, start, script, a.seed + k, os.path.join(td, "c%d.cf32" % k))
            amp = np.sqrt(10 ** (ebn0 / 10) * n0 * rate)   # Eb = P / rate
            srcs.append((np.resize(x, nch) * amp if len(x) >= nch else np.concatenate([x, np.zeros(nch - len(x))]) * amp, off))
            truth.append({"kind": kind, "rate": rate, "offset_hz": off, "ebn0_db": ebn0, "start_s": start})
    t = np.arange(nch) / CH_FS
    for spec in cws:
        off, snr = (float(v) for v in spec.split(":"))
        amp = np.sqrt(10 ** (snr / 10) * n0 * 1000)      # SNR in 1 kHz
        srcs.append((amp * np.ones(nch, complex), off))
        truth.append({"kind": "cw", "offset_hz": off})
    for spec in others:
        off, snr = (float(v) for v in spec.split(":"))
        amp = np.sqrt(10 ** (snr / 10) * n0 * 1000)
        sym = rng.choice([-1.0, 1.0], int(a.seconds * 1200) + 2)
        bb = np.repeat(sym, CH_FS // 1200)[:nch]
        # a strong carrier with a weak BPSK on it (modulation index 0.3)
        srcs.append((amp * np.exp(1j * 0.3 * bb), off))
        truth.append({"kind": "other", "offset_hz": off})
    # every antenna sees the same signal with its UCA phase (the satellite at one bearing)
    lam = C_LIGHT / a.rf
    az = np.deg2rad(a.bearing)
    m = 5
    steer = np.array([np.exp(2j * np.pi * a.radius * np.cos(az - (-2 * np.pi * i / m)) / lam) for i in range(m)])
    # overlap-add synthesis of the 2.4 MHz sum, 1 s segments every 0.5 s
    hop = SEG // 2
    w = 0.5 - 0.5 * np.cos(2 * np.pi * np.arange(SEG) / SEG)   # periodic Hann: sums to 1 at 50 %
    up = FS // CH_FS
    y = np.zeros(nout + up * SEG, complex)
    kb = np.fft.fftfreq(SEG, 1 / CH_FS).astype(int)    # Hz of each 24 kHz bin (1 Hz bins)
    for s0 in range(-hop, nch, hop):
        spec = np.zeros(SEG * up, complex)
        for x, off in srcs:
            seg = np.zeros(SEG, complex)
            a0, a1 = max(0, s0), min(nch, s0 + SEG)
            if a1 <= a0:
                continue
            seg[a0 - s0:a1 - s0] = x[a0:a1]
            X = np.fft.fft(seg * w)
            # the offset as a whole number of Hz; the segment start s0 / 24 kHz s: phase 2 pi off t0
            fo = int(round(off))
            X *= np.exp(2j * np.pi * fo * (s0 / CH_FS))
            spec[(kb + fo) % (SEG * up)] += X
        seg_out = np.fft.ifft(spec) * up
        b0 = s0 * up
        lo = max(0, b0)
        y[lo:b0 + SEG * up] += seg_out[lo - b0:]
    y = y[:nout]
    # cu8: sample-major, antennas 0..4, plus noise, rms 0.15
    sc = None
    with open(a.out, "wb") as fo:
        blk = FS // 4
        for i0 in range(0, nout, blk):
            s = y[i0:i0 + blk]
            arr = np.empty((len(s), m), complex)
            for i in range(m):
                arr[:, i] = s * steer[i] + (rng.standard_normal(len(s)) + 1j * rng.standard_normal(len(s))) / np.sqrt(2)
            if sc is None:
                sc = 0.15 / np.sqrt(np.mean(np.abs(arr) ** 2))
            iq = np.empty((len(s) * m, 2), np.float32)
            v = (arr * sc).reshape(-1)
            iq[:, 0] = v.real
            iq[:, 1] = v.imag
            np.clip(np.rint(iq * 127.5 + 127.5), 0, 255).astype(np.uint8).tofile(fo)
    with open(a.out.rsplit(".", 1)[0] + ".json", "w") as f:
        json.dump({"rate": FS, "channels": m, "rf_hz": a.rf, "radius_m": a.radius, "format": "cu8", "truth": truth}, f)
    print("%s: %.1f s, %d channels at %d Hz" % (a.out, a.seconds, len(srcs), FS), file=sys.stderr)


if __name__ == "__main__":
    main()
