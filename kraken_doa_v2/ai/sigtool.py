#!/usr/bin/env python3
"""KrakenSDR signal tool - capture and measure a signal (numpy only).

Used by the AI Signal Lab (ai/kraken_ai.py) and handy on its own. Captures
come from heimdall's data port (8091, channel 0 - in wideband / independent
mode the tuner whose band holds the frequency) and are channelized around
the requested frequency; everything else works on the resulting
complex-float32 files (.cf32 + .json sidecar with the sample rate and RF).

  sigtool.py capture --freq HZ [--rate HZ] [--bw HZ] [--seconds S] -o OUT.cf32
  sigtool.py analyze IN.cf32 [--outdir DIR]       report + PNG plots
  sigtool.py spectrogram IN.cf32 -o OUT.png [--start S] [--seconds S] [--fft N]
  sigtool.py extract IN.cf32 --offset HZ [--rate HZ] [--bw HZ] -o OUT.cf32
  sigtool.py demod IN.cf32 --mode fm|am|usb|lsb -o OUT.wav
  sigtool.py tones IN.cf32|IN.wav                 audio-band tones (FM demod)
  sigtool.py symbols IN.cf32 --baud B [--levels 2|4] [--demod fm|am] [-o OUT.txt]
                                                   symbol/bit recovery + sync-word
                                                   and frame-length candidates
Frequencies: --freq is absolute RF in Hz; --offset is relative to the file's
centre. Times in seconds.
"""
import argparse
import json
import math
import os
import queue
import shutil
import signal
import socket
import struct
import sys
import threading
import time
import zlib

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "memguard"))
import kraken_memguard  # noqa: E402

# a job too big for the Pi's free memory fails with MemoryError instead of
# swapping the receiver to a standstill (see memguard/kraken_memguard.py)
kraken_memguard.apply()

HEIMDALL_FS = 2.4e6
MAGIC = 0x4D434851
STATS_MAX = 1 << 22           # samples analyze's modulation statistics use at most
MAX_CAPTURE_S = 600          # captures are streamed to disk; this only bounds the file
DISK_RESERVE = 200 << 20     # bytes left free on the disk


# ---------------------------------------------------------------------------
# I/O
# ---------------------------------------------------------------------------
def load(path, start=0.0, seconds=0.0):
    meta = {}
    for side in (path + ".json", os.path.splitext(path)[0] + ".json"):
        if os.path.exists(side):
            with open(side) as f:
                meta = json.load(f)
            break
    if path.endswith(".wav"):
        fs, data = read_wav(path)
        meta.setdefault("rate", fs)
        x = data[:, 0] + 1j * data[:, 1] if data.ndim == 2 and data.shape[1] == 2 else data[:, 0] if data.ndim == 2 else data
    elif path.endswith((".cu8", ".u8")):
        b = np.fromfile(path, dtype=np.uint8).astype(np.float32)
        x = ((b[0::2] - 127.5) + 1j * (b[1::2] - 127.5)) / 127.5
    elif os.path.getsize(path) >= 8:
        # mapped, not read: the recording itself takes no memory (the page
        # cache holds what is being worked on, and can drop it again)
        x = np.memmap(path, dtype=np.complex64, mode="r")
    else:
        x = np.zeros(0, np.complex64)
    fs = float(meta.get("rate", 0))
    if fs <= 0:
        sys.exit("error: sample rate unknown (no .json sidecar)")
    s0 = int(start * fs)
    s1 = len(x) if seconds <= 0 else min(len(x), s0 + int(seconds * fs))
    return np.asarray(x[s0:s1], dtype=np.complex64), fs, meta


def save(path, x, fs, meta):
    np.asarray(x, dtype=np.complex64).tofile(path)
    save_meta(path, len(x), fs, meta)


def save_meta(path, n, fs, meta):
    m = dict(meta)
    m["rate"] = fs
    m["samples"] = int(n)
    m["format"] = "cf32"
    with open(os.path.splitext(path)[0] + ".json", "w") as f:
        json.dump(m, f, indent=1)


def read_wav(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        sys.exit("error: not a WAV file")
    p, fmt, data = 12, None, None
    while p + 8 <= len(raw):
        cid, ln = raw[p:p + 4], struct.unpack("<I", raw[p + 4:p + 8])[0]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", raw[p + 8:p + 24])
        elif cid == b"data":
            data = raw[p + 8:p + 8 + ln]
        p += 8 + ln + (ln & 1)
    afmt, nch, fs, _, _, bits = fmt
    if afmt == 3:
        a = np.frombuffer(data, dtype=np.float32)
    elif bits == 16:
        a = np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768
    elif bits == 8:
        a = (np.frombuffer(data, dtype=np.uint8).astype(np.float32) - 128) / 128
    else:
        sys.exit("error: unsupported WAV format")
    return fs, a.reshape(-1, nch)


def write_wav(path, a, fs):
    a = np.clip(np.asarray(a, dtype=np.float32), -1, 1)
    pcm = (a * 32767).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
                struct.pack("<IHHIIHH", 16, 1, 1, int(fs), int(fs) * 2, 2, 16) +
                b"data" + struct.pack("<I", len(pcm)) + pcm)


# ---------------------------------------------------------------------------
# DSP
# ---------------------------------------------------------------------------
class Channelizer:
    """Low-pass to +-bw/2 and decimate by an integer factor (fast-convolution
    filter: overlap-save in the frequency domain, decimation by picking the
    centre bins) - fed a piece at a time, so a long recording never has to
    be in memory: push() returns the output so far, finish() the rest. The
    output is the same as filtering the whole recording at once."""

    def __init__(self, fs, decim, bw):
        self.decim = decim = int(decim)
        self.bypass = decim <= 1 and bw >= fs
        M = 4096
        self.N = N = M * decim
        self.V = V = N // 4 // decim * decim          # overlap (multiple of decim)
        self.hop = N - V
        f = np.fft.fftfreq(N, 1 / fs)
        fo = fs / decim
        edge = min(bw / 2, 0.48 * fo)
        trans = max(0.05 * fo, 4 * fs / N)
        H = np.clip((edge + trans / 2 - np.abs(f)) / trans, 0, 1)
        H = 0.5 - 0.5 * np.cos(np.pi * H)    # raised-cosine edge
        self.keep = np.concatenate([np.arange(0, M // 2), np.arange(N - M // 2, N)])
        self.Hk = (H[self.keep] / decim).astype(np.complex64)
        self.buf = np.zeros(V, np.complex64)  # the run-in: V zeros, then the input
        self.n_in = self.n_out = 0

    def _blocks(self, buf, limit=None):
        out, p = [], 0
        while len(buf) - p >= self.N and (limit is None or limit > 0):
            y = np.fft.ifft(np.fft.fft(buf[p:p + self.N])[self.keep] * self.Hk)
            y = y[self.V // self.decim:]
            out.append(y)
            p += self.hop
            if limit is not None:
                limit -= len(y)
        return (np.concatenate(out).astype(np.complex64) if out else np.zeros(0, np.complex64)), p

    def push(self, x):
        x = np.asarray(x, dtype=np.complex64)
        if self.bypass:
            return x
        self.n_in += len(x)
        buf = np.concatenate([self.buf, x])
        y, p = self._blocks(buf)
        self.buf = buf[p:]
        self.n_out += len(y)
        return y

    def finish(self):
        if self.bypass:
            return np.zeros(0, np.complex64)
        want = self.n_in // self.decim - self.n_out
        if want <= 0:
            return np.zeros(0, np.complex64)
        y, _ = self._blocks(np.concatenate([self.buf, np.zeros(self.N, np.complex64)]), want)
        self.n_out += min(len(y), want)
        return y[:want]


def channelize(x, fs, decim, bw):
    """Channelizer on a whole array."""
    c = Channelizer(fs, decim, bw)
    if c.bypass:
        return x
    return np.concatenate([c.push(x), c.finish()])


def mix(x, fs, hz, phase0=0.0, n0=0):
    """x shifted down by hz: x[n] * exp(-j 2 pi hz/fs (n0 + n) + j phase0).
    n0 = the first sample's index in a stream (a continuous phase over
    pieces). In blocks: no recording-long float64 temporaries."""
    out = np.empty(len(x), np.complex64)
    step = hz / fs
    B = 1 << 18
    for p in range(0, len(x), B):
        n = np.arange(n0 + p, n0 + min(p + B, len(x)), dtype=np.float64)
        ph = -2 * np.pi * np.mod(n * step, 1.0) + phase0
        out[p:p + B] = x[p:p + B] * np.exp(1j * ph).astype(np.complex64)
    return out


def fm_disc(x, fs):
    """Instantaneous frequency (Hz), float32, in blocks (no recording-long
    complex temporaries)."""
    n = len(x)
    d = np.empty(n, np.float32)
    if n < 2:
        d[:] = 0
        return d
    B = 1 << 20
    k = np.float32(fs / (2 * np.pi))
    for p in range(1, n, B):
        q = min(n, p + B)
        d[p:q] = np.angle(x[p:q] * np.conj(x[p - 1:q - 1])) * k
    d[0] = d[1]
    return d


def blocks(x, B=1 << 20):
    for p in range(0, len(x), B):
        yield x[p:p + B]


def lowpass_real(a, fs, cutoff):
    n = len(a)
    A = np.fft.rfft(np.asarray(a, dtype=np.float32))      # single precision: half the memory
    A[int(cutoff * n / fs) + 1:] = 0                       # bins above the cut-off
    return np.fft.irfft(A, n).astype(np.float32)


def resample_real(a, fs, fo):
    if abs(fs - fo) < 1e-6:
        return a
    n = len(a)
    m = int(round(n * fo / fs))
    A = np.fft.rfft(np.asarray(a, dtype=np.float32))
    B = np.zeros(m // 2 + 1, A.dtype)
    k = min(len(A), len(B))
    B[:k] = A[:k]
    return (np.fft.irfft(B, m) * m / n).astype(np.float32)


def welch(x, fs, nfft=4096, mask=None, fn=None):
    """Averaged power spectrum. fn: applied to each segment first (e.g. the
    envelope of a segment instead of the whole recording's - no full-length
    temporaries). Real x gives the two-sided (mirrored) spectrum."""
    nfft = int(min(nfft, 2 ** int(math.log2(max(len(x), 64)))))
    w = np.hanning(nfft).astype(np.float32)
    hop = nfft // 2
    acc = np.zeros(nfft)
    k = 0
    for p in range(0, len(x) - nfft + 1, hop):
        if mask is not None and not mask[p:p + nfft].mean() > 0.9:
            continue
        seg = x[p:p + nfft] if fn is None else fn(x[p:p + nfft])
        acc += np.abs(np.fft.fft(seg * w)) ** 2
        k += 1
    if k == 0:
        return welch(x, fs, nfft, fn=fn) if mask is not None else (np.fft.fftshift(np.fft.fftfreq(nfft, 1 / fs)), np.full(nfft, -200.0), 0)
    psd = np.fft.fftshift(acc / k) / (np.sum(w ** 2) * fs)
    f = np.fft.fftshift(np.fft.fftfreq(nfft, 1 / fs))
    return f, 10 * np.log10(psd + 1e-20), k


def spectral_lines(f, db, fmin, fmax, min_prom=8.0, n=6):
    """Narrow peaks standing min_prom dB above the local median."""
    sel = (f >= fmin) & (f <= fmax)
    idx = np.where(sel)[0]
    if len(idx) < 8:
        return []
    out = []
    w = max(5, len(f) // 200)
    for i in idx:
        lo, hi = max(0, i - w), min(len(db), i + w + 1)
        if db[i] < db[lo:hi].max():
            continue
        prom = db[i] - np.median(db[lo:hi])
        if prom >= min_prom:
            # parabolic interpolation
            if 0 < i < len(db) - 1:
                a, b, c = db[i - 1], db[i], db[i + 1]
                d = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0
            else:
                d = 0
            out.append((float(f[i] + d * (f[1] - f[0])), float(prom), float(db[i])))
    out.sort(key=lambda t: -t[1])
    return out[:n]


def hist_peaks(v, lo, hi, bins=200, min_frac=0.2):
    h, e = np.histogram(v, bins=bins, range=(lo, hi))
    hs = np.convolve(h, np.hanning(9) / np.hanning(9).sum(), "same")
    c = 0.5 * (e[1:] + e[:-1])
    peaks = []
    for i in range(1, len(hs) - 1):
        if hs[i] >= hs[i - 1] and hs[i] > hs[i + 1] and hs[i] >= min_frac * hs.max():
            peaks.append((float(c[i]), float(hs[i] / hs.max())))
    # merge peaks closer than 4 bins
    merged = []
    for p in peaks:
        if merged and p[0] - merged[-1][0] < 4 * (c[1] - c[0]):
            if p[1] > merged[-1][1]:
                merged[-1] = p
        else:
            merged.append(p)
    return merged, h, c


# ---------------------------------------------------------------------------
# PNG plots (no matplotlib): a tiny raster renderer with a 5x7 font
# ---------------------------------------------------------------------------
FONT = {
    "0": "01110100011001110101110011000101110", "1": "00100011000010000100001000010001110",
    "2": "01110100010000100010001000100011111", "3": "11111000100010000010000011000101110",
    "4": "00010001100101010010111110001000010", "5": "11111100001111000001000011000101110",
    "6": "00110010001000011110100011000101110", "7": "11111000010001000100010000100001000",
    "8": "01110100011000101110100011000101110", "9": "01110100011000101111000010001001100",
    "-": "00000000000000011111000000000000000", "+": "00000001000010011111001000010000000",
    ".": "00000000000000000000000000110001100", " ": "0" * 35,
    "k": "10000100001001010100110001010010010", "H": "10001100011000111111100011000110001",
    "z": "00000000001111100010001000100011111", "s": "00000000000111010000011100000111110",
    "d": "00001000010110110011100011000101111", "B": "11110100011000111110100011000111110",
    "m": "00000000001101010101101011000110001", "%": "11000110010001000100010000100110011",
    "M": "10001110111010110001100011000110001", "/": "00000000010001000100010001000000000",
    ":": "00000011000110000000011000110000000", "t": "01000010001110001000010000100100110",
    "f": "00110010010100011100010000100001000", "r": "00000000001011011001100001000010000",
    "e": "00000000000111010001111111000001110", "q": "00000000000110110011011110000100001",
    "i": "00100000000110000100001000010001110", "n": "00000000001011011001100011000110001",
    "a": "00000000000111000001011111000101111", "p": "00000000001111010001111101000010000",
    "l": "01100001000010000100001000010001110", "o": "00000000000111010001100011000101110",
    "c": "00000000000111010000100001000101110", "u": "00000000001000110001100011001101101",
    "g": "00000011111000110001011110000101110", "y": "00000000001000110001011110000101110",
    "w": "00000000001000110001101011010101010", "h": "10000100001011011001100011000110001",
    "v": "00000000001000110001100010101000100", "b": "10000100001011011001100011000111110",
}


class Canvas:
    def __init__(self, w, h, bg=(16, 18, 24)):
        self.w, self.h = w, h
        self.px = np.zeros((h, w, 3), np.uint8)
        self.px[:] = bg

    def text(self, x, y, s, col=(220, 220, 220)):
        for ch in s:
            g = FONT.get(ch, FONT.get(ch.lower(), FONT[" "]))
            for r in range(7):
                for c in range(5):
                    if g[r * 5 + c] == "1" and 0 <= y + r < self.h and 0 <= x + c < self.w:
                        self.px[y + r, x + c] = col
            x += 6

    def hline(self, y, x0, x1, col):
        if 0 <= y < self.h:
            self.px[y, max(0, x0):min(self.w, x1)] = col

    def vline(self, x, y0, y1, col):
        if 0 <= x < self.w:
            self.px[max(0, y0):min(self.h, y1), x] = col

    def polyline(self, xs, ys, col):
        for i in range(len(xs) - 1):
            x0, y0, x1, y1 = int(xs[i]), int(ys[i]), int(xs[i + 1]), int(ys[i + 1])
            n = max(abs(x1 - x0), abs(y1 - y0), 1)
            for t in range(n + 1):
                x = x0 + (x1 - x0) * t // n
                y = y0 + (y1 - y0) * t // n
                if 0 <= x < self.w and 0 <= y < self.h:
                    self.px[y, x] = col

    def save(self, path):
        raw = b"".join(b"\x00" + self.px[r].tobytes() for r in range(self.h))

        def chunk(t, d):
            c = struct.pack(">I", len(d)) + t + d
            return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
        with open(path, "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
                    chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def nice_ticks(lo, hi, n=8):
    span = hi - lo
    if span <= 0:
        return [lo]
    step = 10 ** math.floor(math.log10(span / n))
    for m in (1, 2, 5, 10):
        if span / (step * m) <= n:
            step *= m
            break
    t = math.ceil(lo / step) * step
    out = []
    while t <= hi + 1e-9:
        out.append(t)
        t += step
    return out


def fmt_hz(v):
    a = abs(v)
    if a >= 1e6:
        return f"{v / 1e6:g}M"
    if a >= 1e3:
        return f"{v / 1e3:g}k"
    return f"{v:g}"


def line_plot(path, x, y, title, xlabel_fmt=fmt_hz, ylabel="dB", w=900, h=360, marks=()):
    cv = Canvas(w, h)
    L, R, T, B = 52, 10, 20, 26
    pw, ph = w - L - R, h - T - B
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    ylo, yhi = np.percentile(y, 1) - 3, y.max() + 3
    xlo, xhi = x.min(), x.max()
    X = L + (x - xlo) / max(xhi - xlo, 1e-12) * (pw - 1)
    Y = T + (1 - (np.clip(y, ylo, yhi) - ylo) / max(yhi - ylo, 1e-12)) * (ph - 1)
    for t in nice_ticks(xlo, xhi):
        px = int(L + (t - xlo) / (xhi - xlo) * (pw - 1))
        cv.vline(px, T, T + ph, (45, 50, 62))
        cv.text(px - 3 * len(xlabel_fmt(t)), T + ph + 6, xlabel_fmt(t), (170, 170, 170))
    for t in nice_ticks(ylo, yhi, 6):
        py = int(T + (1 - (t - ylo) / (yhi - ylo)) * (ph - 1))
        cv.hline(py, L, L + pw, (45, 50, 62))
        cv.text(2, py - 3, f"{t:.0f}", (170, 170, 170))
    for m in marks:
        px = int(L + (m - xlo) / max(xhi - xlo, 1e-12) * (pw - 1))
        cv.vline(px, T, T + ph, (120, 60, 60))
    cv.polyline(X, Y, (90, 200, 255))
    cv.text(L, 5, title, (240, 240, 120))
    cv.text(w - 6 * len(ylabel) - 4, 5, ylabel, (170, 170, 170))
    cv.save(path)


def colormap(v):
    # dark blue -> cyan -> yellow -> red
    stops = np.array([[0, 0, 20], [0, 60, 160], [0, 200, 220], [250, 240, 40], [255, 60, 20]], float)
    v = np.clip(v, 0, 1) * (len(stops) - 1)
    i = np.minimum(v.astype(int), len(stops) - 2)
    t = (v - i)[..., None]
    return (stops[i] * (1 - t) + stops[i + 1] * t).astype(np.uint8)


def spectrogram_png(path, x, fs, title, nfft=None, max_rows=600, width=900, f_centre=0.0):
    if nfft is None:
        nfft = int(2 ** round(math.log2(max(64, fs / 100))))   # ~100 Hz bins
        nfft = min(max(nfft, 128), 8192)
    hop = max(nfft // 2, len(x) // max_rows)
    w = np.hanning(nfft).astype(np.float32)
    rows = []
    for p in range(0, len(x) - nfft + 1, hop):
        rows.append(np.abs(np.fft.fftshift(np.fft.fft(x[p:p + nfft] * w))) ** 2)
    if not rows:
        return
    S = 10 * np.log10(np.array(rows) + 1e-20)
    # resample columns to the picture width
    cols = np.linspace(0, nfft - 1, min(width, nfft)).astype(int) if nfft > width else np.arange(nfft)
    S = S[:, cols]
    lo, hi = np.percentile(S, 20), np.percentile(S, 99.7)
    img = colormap((S - lo) / max(hi - lo, 1e-9))
    L, T, B = 52, 20, 26
    H = img.shape[0]
    W = img.shape[1]
    if W < 300:   # stretch narrow ones
        rep = int(math.ceil(300 / W))
        img = np.repeat(img, rep, axis=1)
        W = img.shape[1]
    cv = Canvas(W + L + 10, H + T + B)
    cv.px[T:T + H, L:L + W] = img
    f0, f1 = -fs / 2, fs / 2
    for t in nice_ticks(f0, f1):
        px = int(L + (t - f0) / (f1 - f0) * (W - 1))
        cv.vline(px, T + H, T + H + 4, (170, 170, 170))
        cv.text(px - 3 * len(fmt_hz(t)), T + H + 8, fmt_hz(t), (170, 170, 170))
    dur = len(x) / fs
    for t in nice_ticks(0, dur, 8):
        py = int(T + t / dur * (H - 1))
        cv.hline(py, L - 4, L, (170, 170, 170))
        cv.text(2, py - 3, f"{t:g}s", (170, 170, 170))
    cv.text(L, 5, title, (240, 240, 120))
    cv.save(path)


# ---------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------
def heimdall_status(host, port=8092, timeout=3.0):
    s = socket.create_connection((host, port), timeout=timeout)
    s.sendall(b'{"command":"get_status"}\n')
    buf = b""
    t0 = time.time()
    try:
        while time.time() - t0 < timeout:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            # split concatenated / newline-separated JSON objects
            depth, start, objs = 0, None, []
            instr = esc = False
            for i, c in enumerate(buf.decode("utf-8", "replace")):
                if instr:
                    if esc:
                        esc = False
                    elif c == "\\":
                        esc = True
                    elif c == '"':
                        instr = False
                    continue
                if c == '"':
                    instr = True
                elif c == "{":
                    if depth == 0:
                        start = i
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0 and start is not None:
                        objs.append(buf.decode("utf-8", "replace")[start:i + 1])
            for o in objs:
                try:
                    j = json.loads(o)
                except ValueError:
                    continue
                if j.get("status") == "success" and "settings" in j:
                    return j
    finally:
        s.close()
    raise RuntimeError("no status reply from heimdall control port")


def recv_exact(s, view):
    got = 0
    while got < len(view):
        n = s.recv_into(view[got:], len(view) - got)
        if n == 0:
            raise RuntimeError("heimdall closed the data connection")
        got += n


def capture(args):
    host = args.host
    center = args.center
    st = None
    try:
        st = heimdall_status(host)
    except (OSError, RuntimeError) as e:
        if center is None:
            sys.exit(f"error: cannot read heimdall status ({e}); pass --center HZ")
    ch = args.channel
    if st is not None:
        if st.get("operating_mode", "coherent") != "coherent":
            # wideband scan / independent tuners: every tuner has its own
            # frequency - record the one whose band holds the signal
            tf = st.get("tuner_frequencies") or []
            if not tf:
                sys.exit("error: heimdall reports no per-tuner frequencies in "
                         f"{st.get('operating_mode')} mode")
            if ch is None:
                ch = min(range(len(tf)), key=lambda i: abs(float(tf[i]) - args.freq))
            if ch >= len(tf):
                sys.exit(f"error: no tuner {ch} (heimdall has {len(tf)})")
            if center is None:
                center = float(tf[ch])
        elif center is None:
            center = float(st["settings"]["center_freq"])
    if ch is None:
        ch = 0
    fs = HEIMDALL_FS
    offset = args.freq - center
    if abs(offset) > fs / 2 - args.rate / 2:
        sys.exit(f"error: {args.freq / 1e6:.6f} MHz is outside the receiver's current span "
                 f"({(center - fs / 2) / 1e6:.4f} - {(center + fs / 2) / 1e6:.4f} MHz); retune first")
    decim = max(1, int(round(fs / args.rate)))
    rate_out = fs / decim
    bw = args.bw if args.bw else 0.8 * rate_out
    if not 0 < args.seconds <= MAX_CAPTURE_S:
        sys.exit(f"error: --seconds must be between 0 and {MAX_CAPTURE_S:g}")
    want = int(args.seconds * fs)
    out_bytes = int(want / decim) * 8
    free = shutil.disk_usage(os.path.dirname(os.path.abspath(args.output))).free
    if out_bytes + DISK_RESERVE > free:
        sys.exit(f"error: the capture needs {out_bytes / 1e6:.0f} MB on disk, only {free / 1e6:.0f} MB are free "
                 "(less --seconds or a lower --rate)")

    # A reader thread takes the packets off the socket (the stream must be
    # read in real time), the main thread processes ~0.2 s at a time and
    # appends the result to the file: memory stays small however long the
    # capture is (it used to hold the whole recording, then several
    # double-precision copies of it - ~140 MB per second, and a 45 s
    # capture froze a 4 GB Pi).
    q = queue.Queue()
    info = {"gain": None, "skipped": 0, "gaps": 0}
    stop = threading.Event()

    def reader():
        s = None
        try:
            s = socket.create_connection((host, 8091), timeout=5)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
            hdr = bytearray(32)
            got = 0
            in_gap = False
            t_end = time.time() + args.seconds + 10
            while got < want and not stop.is_set():
                if time.time() > t_end:
                    raise RuntimeError("capture timed out (heimdall calibrating / not streaming?)")
                recv_exact(s, memoryview(hdr))
                magic, nch, ns, phase, noise, fchg, grp, retune = struct.unpack(">8I", hdr)
                if magic != MAGIC:
                    raise RuntimeError("bad packet magic (wrong port?)")
                chinfo = bytearray(8 * nch)
                recv_exact(s, memoryview(chinfo))
                data = bytearray(nch * ns * 2)
                recv_exact(s, memoryview(data))
                if ch >= nch:
                    raise RuntimeError(f"no channel {ch} in the stream ({nch} channels)")
                pf, info["gain"] = struct.unpack("<ff", chinfo[8 * ch: 8 * ch + 8])
                if abs(pf - center) > 1000 and args.center is None:
                    raise RuntimeError(f"the receiver retuned during the capture ({pf / 1e6:.4f} MHz)")
                if noise or retune:
                    info["skipped"] += 1
                    if not in_gap and got:
                        info["gaps"] += 1
                    in_gap = True
                    continue
                in_gap = False
                take = min(ns, want - got)
                q.put(bytes(data[ch * ns * 2: ch * ns * 2 + take * 2]))
                got += take
            q.put(None)
        except Exception as e:      # handed to the main thread
            q.put(e)
        finally:
            if s is not None:
                s.close()

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    chan = Channelizer(fs, decim, bw)
    part = args.output + ".part"
    n_in = n_out = 0
    dc = None
    CHUNK = 1 << 19                 # samples processed at a time (~0.22 s)
    pend, pend_n = [], 0
    try:
        with open(part, "wb") as fo:
            done = False
            while not done:
                item = q.get()
                if isinstance(item, Exception):
                    sys.exit(f"error: {item}")
                if item is None:
                    done = True
                else:
                    pend.append(item)
                    pend_n += len(item) // 2
                if pend_n < CHUNK and not done:
                    continue
                if pend_n:
                    b = np.frombuffer(b"".join(pend), dtype=np.uint8).astype(np.float32)
                    pend, pend_n = [], 0
                    x = np.empty(len(b) // 2, np.complex64)
                    x.real = b[0::2]
                    x.imag = b[1::2]
                    del b
                    x -= 127.5 + 127.5j
                    x /= 127.5
                    # DC: each piece's mean smoothed (~1 s) and ramped across
                    # the piece, so the correction has no steps
                    m = complex(x.mean())
                    prev = m if dc is None else dc
                    dc = m if dc is None else dc + 0.2 * (m - dc)
                    x -= np.linspace(prev, dc, len(x), dtype=np.complex64)
                    y = chan.push(mix(x, fs, offset, n0=n_in))
                    n_in += len(x)
                    y.tofile(fo)
                    n_out += len(y)
                if done:
                    y = chan.finish()
                    y.tofile(fo)
                    n_out += len(y)
        stop.set()
        meta = {"rf_hz": args.freq, "center_hz": center, "offset_hz": offset, "rate": rate_out, "bandwidth_hz": bw,
                "gain_db": info["gain"], "captured_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "source": f"heimdall 8091 channel {ch}", "channel": ch, "calibration_gaps": info["gaps"],
                "skipped_packets": info["skipped"]}
        save_meta(args.output, n_out, rate_out, meta)
        os.replace(part, args.output)
    finally:
        stop.set()
        if os.path.exists(part):
            os.unlink(part)
    gaps = info["gaps"]
    print(f"captured {n_out / rate_out:.2f} s at {rate_out:g} Hz (bw {bw:g} Hz) around "
          f"{args.freq / 1e6:.6f} MHz -> {args.output}" + (f"  [{gaps} calibration gap(s) cut out]" if gaps else ""))


# ---------------------------------------------------------------------------
# analysis
# ---------------------------------------------------------------------------
def bursts(x, fs, block_s=0.002):
    blk = max(1, int(fs * block_s))
    nb = len(x) // blk
    if nb < 4:
        return None
    step = max(1, (1 << 20) // blk) * blk
    p = np.concatenate([(np.abs(c) ** 2).reshape(-1, blk).mean(1) for c in blocks(x[: nb * blk], step)])
    pdb = 10 * np.log10(p + 1e-20)
    floor = np.percentile(pdb, 10)
    top = np.percentile(pdb, 99)
    thr = floor + max(6.0, 0.4 * (top - floor))
    on = pdb > thr
    # burst list
    edges = np.diff(np.concatenate([[0], on.astype(int), [0]]))
    starts = np.where(edges == 1)[0]
    ends = np.where(edges == -1)[0]
    lst = [(s * block_s, (e - s) * block_s) for s, e in zip(starts, ends)]
    return {"block_s": block_s, "floor_db": float(floor), "peak_db": float(top), "threshold_db": float(thr),
            "on": on, "duty": float(on.mean()), "bursts": lst, "pdb": pdb}


def analyze(args):
    x, fs, meta = load(args.input, args.start, args.seconds)
    outdir = args.outdir or os.path.dirname(os.path.abspath(args.input))
    base = os.path.join(outdir, os.path.splitext(os.path.basename(args.input))[0])
    rf = meta.get("rf_hz")
    rep = []
    P = rep.append
    dur = len(x) / fs
    P(f"# Signal analysis: {os.path.basename(args.input)}")
    P(f"RF {rf / 1e6:.6f} MHz" if rf else "RF unknown")
    P(f"sample rate {fs:g} Hz, {dur:.2f} s, capture bandwidth {meta.get('bandwidth_hz', fs):g} Hz, "
      f"gain {meta.get('gain_db', '?')} dB")
    pw = sum(float(np.sum(np.abs(c) ** 2)) for c in blocks(x))
    rms = math.sqrt(pw / max(len(x), 1)) + 1e-12
    peak = max((float(np.abs(c).max()) for c in blocks(x)), default=0.0)
    P(f"level: {20 * np.log10(rms):.1f} dBFS rms, peak {20 * np.log10(peak + 1e-12):.1f} dBFS")

    # --- activity / bursts
    b = bursts(x, fs)
    on_mask = None
    if b:
        P("")
        P("## Activity")
        P(f"power floor {b['floor_db']:.1f} dB, peak {b['peak_db']:.1f} dB (per 2 ms block, dBFS); "
          f"signal present {100 * b['duty']:.1f}% of the time")
        bl = b["bursts"]
        if b["duty"] > 0.97:
            P("continuous transmission (no gaps)")
        elif bl:
            durs = np.array([d for _, d in bl])
            P(f"{len(bl)} bursts; duration median {1000 * np.median(durs):.1f} ms "
              f"(min {1000 * durs.min():.1f}, max {1000 * durs.max():.1f})")
            if len(bl) >= 3:
                st = np.array([s for s, _ in bl])
                per = np.diff(st)
                P(f"burst start spacing median {1000 * np.median(per):.2f} ms "
                  f"(min {1000 * per.min():.1f}, max {1000 * per.max():.1f})")
            P("first bursts (start s, length ms): " + ", ".join(f"{s:.3f}/{1000 * d:.1f}" for s, d in bl[:12]))
        if b["duty"] < 0.02:
            P("WARNING: hardly any signal above the noise - check the frequency / wait for activity")
        blk = max(1, int(fs * b["block_s"]))
        on_mask = np.repeat(b["on"], blk)
        on_mask = np.concatenate([on_mask, np.zeros(len(x) - len(on_mask), bool)])

    # --- spectrum
    f, db, nseg = welch(x, fs, 4096, on_mask if (on_mask is not None and 0.02 < b["duty"] < 0.97) else None)
    # statistics only inside the capture filter's passband (outside it is
    # the filter's stopband, not the noise floor)
    bw = float(meta.get("bandwidth_hz") or fs)
    inband = np.abs(f) <= 0.97 * min(bw, fs) / 2
    f_all, db_all = f, db
    f, db = f[inband], db[inband]
    floor = float(np.percentile(db, 15))
    pk = int(np.argmax(db))
    P("")
    P("## Spectrum (while the signal is on)")
    P(f"resolution {f[1] - f[0]:.1f} Hz; noise floor {floor:.1f} dB/Hz; peak {db[pk] - floor:.1f} dB above it "
      f"at {f[pk]:+.0f} Hz from the centre")
    lin = 10 ** ((db - floor) / 10) - 1
    lin[lin < 0] = 0
    if lin.sum() > 0:
        c = np.cumsum(lin) / lin.sum()
        f_lo, f_hi = f[np.searchsorted(c, 0.005)], f[min(len(f) - 1, np.searchsorted(c, 0.995))]
        centroid = float((f * lin).sum() / lin.sum())
        P(f"99% occupied bandwidth {f_hi - f_lo:.0f} Hz ({f_lo:+.0f} .. {f_hi:+.0f} Hz); "
          f"power centroid {centroid:+.0f} Hz (carrier offset estimate)")
    for lvl in (3, 10, 20, 30):
        above = np.where(db > db[pk] - lvl)[0]
        if len(above):
            P(f"  -{lvl} dB width {f[above[-1]] - f[above[0]]:.0f} Hz ({f[above[0]]:+.0f} .. {f[above[-1]]:+.0f})")
    snr = db[pk] - floor
    lines = spectral_lines(f, db, f[0], f[-1], 10)
    if lines:
        P("narrow spectral lines (carriers / pilots): " +
          ", ".join(f"{fr:+.0f} Hz ({pr:.0f} dB)" for fr, pr, _ in lines))

    # the signal's samples (the bursts, when it is bursty) for the
    # modulation statistics - at most STATS_MAX of them, so the memory they
    # need doesn't grow with the length of the capture
    if on_mask is not None and on_mask.sum() > fs * 0.05:
        blk = max(1, int(fs * b["block_s"]))
        parts, got = [], 0
        for st, du in b["bursts"]:
            a0 = int(round(st / b["block_s"])) * blk
            a1 = min(a0 + int(round(du / b["block_s"])) * blk, a0 + STATS_MAX - got)
            parts.append(np.asarray(x[a0:a1]))
            got += a1 - a0
            if got >= STATS_MAX:
                break
        xs = np.concatenate(parts)
    else:
        xs = np.asarray(x[:STATS_MAX])
    if len(xs) < (on_mask.sum() if on_mask is not None and on_mask.sum() > fs * 0.05 else len(x)):
        P("")
        P(f"(the modulation statistics below use the first {len(xs) / fs:.1f} s of signal)")
    # --- envelope
    env = np.abs(xs)
    cv = float(env.std() / (env.mean() + 1e-12))
    P("")
    P("## Envelope / modulation hints")
    P(f"amplitude variation (std/mean) {cv:.3f}  -> " +
      ("near-constant envelope: FM / FSK / GMSK / CPM / PM" if cv < 0.2 else
       "moderate: filtered PSK/QAM, or FSK at low SNR" if cv < 0.45 else "strong: AM / OOK / SSB / noise-like"))
    m2 = float(np.mean(env * env))
    fe, dbe, _ = welch(env, fs, 8192, fn=lambda seg: seg * seg - m2)
    del env
    lines_e = spectral_lines(fe, dbe, 20, fs / 2, 10)
    if lines_e:
        P("envelope spectral lines (AM rate / PSK symbol rate): " +
          ", ".join(f"{fr:.1f} Hz ({pr:.0f} dB)" for fr, pr, _ in lines_e[:5]))
    for k, name in ((2, "BPSK-like (x^2)"), (4, "QPSK-like (x^4)")):
        fk, dbk, _ = welch(xs, fs, 8192, fn=lambda seg: (seg / (np.abs(seg) + 1e-9)) ** k)
        lk = spectral_lines(fk, dbk, -fs / 2, fs / 2, 15, 1)
        if lk:
            P(f"{name}: line at {lk[0][0]:+.1f} Hz ({lk[0][1]:.0f} dB) -> carrier offset {lk[0][0] / k:+.1f} Hz")

    # --- FM discriminator
    d = fm_disc(xs, fs)
    del xs
    occ = (f_hi - f_lo) if lin.sum() > 0 else fs / 2
    dl = lowpass_real(d, fs, min(fs / 2 * 0.95, max(1500.0, occ * 0.6)))
    p1, p99 = np.percentile(dl, [1, 99])
    P("")
    P("## FM discriminator (instantaneous frequency)")
    P(f"mean {dl.mean():+.0f} Hz (carrier offset), 1-99 percentile {p1:+.0f} .. {p99:+.0f} Hz, std {dl.std():.0f} Hz")
    pk_l, h, hc = hist_peaks(dl, p1 - 0.2 * (p99 - p1), p99 + 0.2 * (p99 - p1))
    if pk_l:
        P(f"histogram peaks ({len(pk_l)}): " + ", ".join(f"{c:+.0f} Hz ({100 * r:.0f}%)" for c, r in pk_l[:8]) +
          ("  -> looks like %d-level FSK" % len(pk_l) if 2 <= len(pk_l) <= 4 and cv < 0.45 else ""))
    # transition-rate line: symbol rate of FSK
    tr = np.abs(np.diff(np.sign(dl - np.median(dl)))).astype(np.float32)
    ft, dbt, _ = welch(tr, fs, 16384)
    del tr
    lt = spectral_lines(ft, dbt, 30, fs / 2 * 0.9, 6, 5)
    if lt:
        P("symbol-rate candidates (lines in the discriminator transition spectrum): " +
          ", ".join(f"{fr:.1f} Hz ({pr:.0f} dB)" for fr, pr, _ in lt))
    dm = float(d.mean())
    fa, dba, _ = welch(d, fs, 16384, fn=lambda seg: seg - dm)
    la = spectral_lines(fa, dba, 20, min(fs / 2, 6000), 12, 8)
    if la:
        P("audio-band tones after FM demod: " + ", ".join(f"{fr:.1f} Hz ({pr:.0f} dB)" for fr, pr, _ in la))
        ct = [fr for fr, _, _ in la if 60 < fr < 260]
        if ct:
            P(f"  (a line at {ct[0]:.1f} Hz could be a CTCSS sub-audible tone)")

    # --- plots
    pngs = []
    p = base + "_spectrum.png"
    line_plot(p, f_all, db_all, f"spectrum  {fmt_hz(rf) + 'Hz' if rf else ''}  rbw {f[1] - f[0]:.0f} Hz", ylabel="dB/Hz")
    pngs.append(p)
    p = base + "_waterfall.png"
    spectrogram_png(p, x, fs, f"waterfall {dur:.1f} s  {fmt_hz(fs)}Hz span")
    pngs.append(p)
    if pk_l is not None:
        p = base + "_fm_histogram.png"
        line_plot(p, hc, 10 * np.log10(h + 1), "FM discriminator histogram (Hz)", ylabel="dB")
        pngs.append(p)
    p = base + "_fm_audio_spectrum.png"
    sel = (fa >= 0) & (fa <= min(fs / 2, 8000))
    line_plot(p, fa[sel], dba[sel], "spectrum after FM demod (Hz)", ylabel="dB")
    pngs.append(p)
    if b:
        p = base + "_power.png"
        t = np.arange(len(b["pdb"])) * b["block_s"]
        line_plot(p, t, b["pdb"], "power vs time (s)", xlabel_fmt=lambda v: f"{v:g}s", ylabel="dBFS")
        pngs.append(p)
    P("")
    P("plots: " + ", ".join(pngs))
    text = "\n".join(rep)
    with open(base + "_report.txt", "w") as fo:
        fo.write(text + "\n")
    print(text)


def cmd_spectrogram(args):
    x, fs, meta = load(args.input, args.start, args.seconds)
    spectrogram_png(args.output, x, fs, args.title or f"waterfall {len(x) / fs:.2f} s", nfft=args.fft)
    print(f"wrote {args.output}")


def cmd_extract(args):
    x, fs, meta = load(args.input, args.start, args.seconds)
    decim = max(1, int(round(fs / args.rate))) if args.rate else 1
    bw = args.bw or 0.8 * fs / decim
    m = dict(meta)
    if m.get("rf_hz"):
        m["rf_hz"] = m["rf_hz"] + args.offset
    m["bandwidth_hz"] = bw
    # a piece at a time, straight to the file (any length of recording)
    chan = Channelizer(fs, decim, bw)
    part = args.output + ".part"
    n = 0
    try:
        with open(part, "wb") as fo:
            P = 1 << 19
            for p in range(0, len(x), P):
                y = chan.push(mix(x[p:p + P], fs, args.offset, n0=p))
                y.tofile(fo)
                n += len(y)
            y = chan.finish()
            y.tofile(fo)
            n += len(y)
        save_meta(args.output, n, fs / decim, m)
        os.replace(part, args.output)
    finally:
        if os.path.exists(part):
            os.unlink(part)
    print(f"wrote {args.output}: {n / (fs / decim):.2f} s at {fs / decim:g} Hz, bw {bw:g} Hz")


def cmd_demod(args):
    x, fs, meta = load(args.input, args.start, args.seconds)
    out_fs = min(48000.0, fs)
    decim = max(1, int(fs // out_fs))
    fs1 = fs / decim
    cut = min(out_fs / 2 * 0.9, 8000)
    P = 1 << 19
    # in pieces: demodulate, then low-pass + decimate with a streamed
    # Channelizer, so no step needs the whole recording at the input rate
    # (a single FFT over 45 s at 480 kHz alone took ~580 MB)
    if args.mode in ("usb", "lsb"):
        # the sideband is moved to +-1500 Hz around 0, filtered to 2.7 kHz,
        # moved back (to 0..3 kHz / -3..0 kHz) and the real part taken
        sh = 1500.0 if args.mode == "usb" else -1500.0
        chan = Channelizer(fs, decim, 2700)
        y = np.concatenate([chan.push(mix(x[p:p + P], fs, sh, n0=p)) for p in range(0, len(x), P)] + [chan.finish()])
        a = np.real(mix(y, fs1, -sh)).astype(np.float32)
        a = a / (np.percentile(np.abs(a), 99.5) + 1e-9) * 0.7
    else:
        if args.mode == "am":
            n = max(len(x), 1)
            s1 = sum(float(np.sum(np.abs(c), dtype=np.float64)) for c in blocks(x))
            s2 = sum(float(np.sum(np.abs(c).astype(np.float64) ** 2)) for c in blocks(x))
            mean = s1 / n
            scale = math.sqrt(max(s2 / n - mean * mean, 0.0)) * 4 + 1e-9
        chan = Channelizer(fs, decim, 2 * cut)
        out = []
        for p in range(0, len(x), P):
            if args.mode == "fm":
                q = max(0, p - 1)
                d = fm_disc(x[q:p + P], fs)[p - q:] / max(args.deviation, 1.0)
            else:
                d = (np.abs(x[p:p + P]) - mean) / scale
            out.append(chan.push(d.astype(np.complex64)).real)
        out.append(chan.finish().real)
        a = np.concatenate(out).astype(np.float32)
    if abs(fs1 - out_fs) > 1e-6:
        a = resample_real(a, fs1, out_fs)
    write_wav(args.output, a, out_fs)
    print(f"wrote {args.output}: {len(a) / out_fs:.2f} s of {args.mode.upper()} audio at {out_fs:g} Hz")


def cmd_tones(args):
    if args.input.endswith(".wav") and not os.path.exists(args.input + ".json"):
        fs, a = read_wav(args.input)
        a = a[:, 0]
    else:
        x, fs, _ = load(args.input)
        a = fm_disc(x, fs)
    a = a - a.mean()
    f, db, _ = welch(a, fs, 32768 if len(a) > 65536 else 4096)
    sel = (f >= 0) & (f <= min(fs / 2, 8000))
    lines = spectral_lines(f[sel], db[sel], 20, 8000, 8, 15)
    print(f"resolution {f[1] - f[0]:.2f} Hz")
    for fr, pr, lv in sorted(lines):
        print(f"  {fr:8.1f} Hz  {pr:5.1f} dB above the local floor")
    # block-wise dominant tone sequence (DTMF / selcall / FFSK hints)
    blk = int(fs * 0.04)
    seq = []
    for p in range(0, len(a) - blk, blk):
        s = np.abs(np.fft.rfft(a[p:p + blk] * np.hanning(blk)))
        ff = np.fft.rfftfreq(blk, 1 / fs)
        m = (ff > 200) & (ff < 4000)
        i = np.argmax(s[m])
        if s[m][i] > 6 * np.median(s[m]):
            seq.append(round(ff[m][i] / 25) * 25)
        else:
            seq.append(0)
    runs = []
    for v in seq:
        if runs and runs[-1][0] == v:
            runs[-1][1] += 1
        else:
            runs.append([v, 1])
    shown = [f"{v}Hz x{n * 40}ms" for v, n in runs if v and n >= 2][:40]
    if shown:
        print("dominant tone sequence (40 ms blocks): " + ", ".join(shown))


def kmeans1d(v, k, iters=30):
    c = np.percentile(v, np.linspace(10, 90, k))
    for _ in range(iters):
        lab = np.argmin(np.abs(v[:, None] - c[None, :]), axis=1)
        for j in range(k):
            if np.any(lab == j):
                c[j] = v[lab == j].mean()
    lab = np.argmin(np.abs(v[:, None] - c[None, :]), axis=1)
    return np.sort(c), lab


def cmd_symbols(args):
    x, fs, meta = load(args.input, args.start, args.seconds)
    sps = fs / args.baud
    if sps < 2:
        sys.exit(f"error: {fs:g} Hz is too slow for {args.baud} Bd - capture/extract at >= {4 * args.baud:g} Hz")
    if args.demod == "fm":
        y = fm_disc(x, fs)
    else:
        y = np.abs(x)
    n = max(1, int(round(sps)))
    y = np.convolve(y, np.full(n, 1 / n, np.float32), "same")   # integrate over a symbol (float32)
    # sample per segment at the phase with the best cluster separation
    seg = int(200 * sps)
    t_all, v_all = [], []

    def at(t):                      # y at fractional sample times (linear)
        i = np.minimum(t.astype(np.int64), len(y) - 2)
        fr = np.minimum(t - i, 1.0).astype(np.float32)
        return y[i] * (1 - fr) + y[i + 1] * fr

    for p0 in range(0, len(y) - seg + 1, seg):
        best = None
        for ph in np.linspace(0, sps, 16, endpoint=False):
            t = p0 + ph + np.arange(int((seg - ph) / sps)) * sps
            v = at(t)
            c, lab = kmeans1d(v, args.levels, 8)
            within = np.mean((v - c[lab]) ** 2)
            score = np.var(v) / (within + 1e-9)
            if best is None or score > best[0]:
                best = (score, t, v)
        t_all.append(best[1])
        v_all.append(best[2])
    if not v_all:
        sys.exit("error: capture too short")
    v = np.concatenate(v_all)
    c, lab = kmeans1d(v, args.levels)
    within = np.sqrt(np.mean((v - c[lab]) ** 2))
    spacing = np.min(np.diff(c)) if len(c) > 1 else 1
    print(f"{len(v)} symbols at {args.baud:g} Bd ({sps:.2f} samples/symbol), demod {args.demod}")
    print("levels: " + ", ".join(f"{l:+.1f}" for l in c) + f"  (spread {within:.1f}, eye ratio "
          f"{spacing / (2 * within + 1e-9):.2f} - >1.5 clean, <0.7 wrong rate/levels)")
    sym = lab.astype(np.uint8)          # 0 = lowest level
    if args.levels == 2:
        bits = sym.copy()
        bit_note = "bits: 1 = higher level"
    else:
        # P25/DMR/NXDN dibits: +3 -> 01, +1 -> 00, -1 -> 10, -3 -> 11 (index 3 = +3)
        dib = {3: (0, 1), 2: (0, 0), 1: (1, 0), 0: (1, 1)}
        bits = np.array([b for s in sym for b in dib.get(int(s), (0, 0))], np.uint8)
        bit_note = "dibits mapped +3->01 +1->00 -1->10 -3->11 (P25/DMR/NXDN convention)"
    # repeated words = sync candidates
    for L in (24, 32, 48):
        if len(bits) < L * 4:
            continue
        w = np.zeros(len(bits) - L + 1, dtype=np.uint64)
        for i in range(L):
            w = (w << np.uint64(1)) | bits[i:len(bits) - L + 1 + i].astype(np.uint64)
        vals, cnt = np.unique(w, return_counts=True)
        order = np.argsort(-cnt)
        shown = 0
        print(f"most repeated {L}-bit words (sync candidates; {bit_note}):")
        for o in order[:40]:
            word = int(vals[o])
            if cnt[o] < 3:
                break
            ones = bin(word).count("1")
            if ones in (0, L) or word in (int("01" * (L // 2), 2), int("10" * (L // 2), 2)):
                continue   # idle / preamble
            pos = np.where(w == vals[o])[0]
            sp = np.diff(pos)
            print(f"  0x{word:0{L // 4}X} x{cnt[o]}  spacing (bits) median {int(np.median(sp)) if len(sp) else 0}"
                  f" min {int(sp.min()) if len(sp) else 0}")
            shown += 1
            if shown >= 5:
                break
    # bit autocorrelation: frame period
    if len(bits) > 2000:
        bb = bits[:20000].astype(np.float32) * 2 - 1
        ac = np.correlate(bb, bb[:4000], "valid")[1:] if len(bb) > 8000 else None
        if ac is not None:
            lag = int(np.argmax(ac[16:]) + 17)
            print(f"strongest bit-pattern repetition at a lag of {lag} bits ({lag / (args.baud * (1 if args.levels == 2 else 2)):.4f} s)")
    if args.output:
        with open(args.output, "w") as fo:
            fo.write(f"# {len(sym)} symbols, levels {' '.join(f'{l:.1f}' for l in c)}; symbol 0 = lowest\n")
            s = "".join(str(int(v)) for v in sym)
            for i in range(0, len(s), 100):
                fo.write(s[i:i + 100] + "\n")
        print(f"symbols written to {args.output}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("--freq", type=float, required=True, help="RF centre of the signal, Hz")
    c.add_argument("--rate", type=float, default=48000, help="output sample rate, Hz (2.4 MHz / integer)")
    c.add_argument("--bw", type=float, default=0, help="filter bandwidth, Hz (default 0.8 x rate)")
    c.add_argument("--seconds", type=float, default=10)
    c.add_argument("--host", default=os.environ.get("KRAKEN_HEIMDALL_HOST", "127.0.0.1"))
    c.add_argument("--center", type=float, default=None, help="receiver centre, Hz (default: ask heimdall)")
    c.add_argument("--channel", type=int, default=None,
                   help="tuner / channel to record (default 0; in wideband / independent mode the tuner whose band holds --freq)")
    c.add_argument("-o", "--output", required=True)
    for name in ("analyze", "spectrogram", "extract", "demod", "symbols"):
        p = sub.add_parser(name)
        p.add_argument("input")
        p.add_argument("--start", type=float, default=0)
        p.add_argument("--seconds", type=float, default=0)
        if name == "analyze":
            p.add_argument("--outdir")
        if name == "spectrogram":
            p.add_argument("-o", "--output", required=True)
            p.add_argument("--fft", type=int, default=None)
            p.add_argument("--title")
        if name == "extract":
            p.add_argument("--offset", type=float, required=True)
            p.add_argument("--rate", type=float, default=0)
            p.add_argument("--bw", type=float, default=0)
            p.add_argument("-o", "--output", required=True)
        if name == "demod":
            p.add_argument("--mode", choices=["fm", "am", "usb", "lsb"], default="fm")
            p.add_argument("--deviation", type=float, default=5000)
            p.add_argument("-o", "--output", required=True)
        if name == "symbols":
            p.add_argument("--baud", type=float, required=True)
            p.add_argument("--levels", type=int, choices=[2, 4, 8], default=2)
            p.add_argument("--demod", choices=["fm", "am"], default="fm")
            p.add_argument("-o", "--output")
    t = sub.add_parser("tones")
    t.add_argument("input")
    a = ap.parse_args()
    # Stop (SIGTERM from the AI Signal Lab) unwinds normally, so a capture /
    # extract in progress removes its unfinished .part file
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    try:
        {"capture": capture, "analyze": analyze, "spectrogram": cmd_spectrogram, "extract": cmd_extract,
         "demod": cmd_demod, "tones": cmd_tones, "symbols": cmd_symbols}[a.cmd](a)
    except MemoryError:
        lim = kraken_memguard.limit_mb()
        sys.exit("error: not enough free memory for this" + (f" (this process may use {lim} MB - the receiver "
                 "needs the rest)" if lim else "") + ": work on a part of the recording (--start / --seconds), "
                 "or extract it to a lower --rate first")


if __name__ == "__main__":
    main()
