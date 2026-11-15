#!/usr/bin/env python3
"""Synthetic screen-leakage (TEMPEST) scene at 2.4 MHz, for the tempest plugin.

  tempest_gen.py OUT.cu8|OUT.cf32 --mode 1024x768@60 --bearing 35 --seconds 4 \
                 [--snr 25] [--offset 300000] [--rf 520e6] [--channels 5] \
                 [--radius 0.12] [--seed 1] [--noise-lsb 1.0]

A near-static test picture is rendered to the mode's active WxH framebuffer,
then its UNINTENDED emanation is modelled per the physics: along each scan
line the radiated intensity is the magnitude of the derivative of luma across
pixels at the pixel clock, zero during the horizontal/vertical BLANKING (so a
per-line and per-frame comb survives for the autocorrelation to lock to). That
real, non-negative intensity envelope AM-modulates a carrier placed at
--offset in the band, is steered onto kraken_doa's UCA at --bearing, and
receiver noise is added. There are NO sync pulses - exactly like the real
leakage the plugin recovers with |x| + autocorrelation.

Written every antenna sample-by-sample (antenna 0 first) in 0.1 s pieces (a
5-antenna 2.4 MHz scene is 24 MB/s as cu8, 96 MB/s as cf32 - keep it short).
OUT.json gets rate, channels, rf_hz, radius_m and the truth (mode, bearing,
geometry, pixel clock, offset).

  cu8   8-bit IQ like the receiver's (127.5 + x, clipped 0..255)
  cf32  full-scale 1.0 (+-1 clip), no quantisation

Test:  plugins/tempest/build/decoder --file OUT.cf32 --channels 5 --uca 0.12 --rf RF_HZ

Antennas: kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as
plugins/sdk/tools/uca_scene.py and radar/tools/radar_gen.py).
Clean-room from the physics and VESA timings; no GPL sources consulted.
"""
import argparse
import json
import os
import sys

import numpy as np

FS = 2.4e6
C = 299792458.0

# VESA DMT geometry: must match plugins/lib/video_raster.cpp's table so the
# plugin's detected mode equals the truth. (width, height, htotal, vtotal,
# pixel_clock_hz, nominal_refresh)
MODES = {
    "640x480@60": (640, 480, 800, 525, 25175000.0, 60),
    "800x600@60": (800, 600, 1056, 628, 40000000.0, 60),
    "1024x768@60": (1024, 768, 1344, 806, 65000000.0, 60),
    "1280x720@60": (1280, 720, 1650, 750, 74250000.0, 60),
    "1280x1024@60": (1280, 1024, 1688, 1066, 108000000.0, 60),
    "1366x768@60": (1366, 768, 1792, 798, 85500000.0, 60),
    "1600x900@60": (1600, 900, 1800, 1000, 108000000.0, 60),
    "1920x1080@60": (1920, 1080, 2200, 1125, 148500000.0, 60),
}


def framebuffer(w, h, rng):
    """A near-static synthetic test picture (grayscale luma 0..1) with large,
    strong-edged features - no copyrighted assets. Large features survive the
    ~pixel_clock/fs horizontal resolution limit of a 2.4 MHz capture."""
    fb = np.full((h, w), 0.12, np.float32)
    b = max(2, h // 240)                 # a bright window border (per-line/frame edge)
    # top third: 8 vertical luma-step bars
    top = h // 3
    nb = 8
    for k in range(nb):
        x0, x1 = k * w // nb, (k + 1) * w // nb
        fb[b:top, x0:x1] = k / (nb - 1)
    # middle third: a coarse checkerboard
    cell = max(32, w // 12)
    for r in range(top, 2 * top, cell):
        for c in range(0, w, cell):
            if ((r // cell) + (c // cell)) & 1:
                fb[r:min(r + cell, 2 * top), c:min(c + cell, w)] = 0.85
    # bottom third: UI-like panels and "text" bars (thick rows of blocks)
    y = 2 * top + b
    panel_h = (h - y - b)
    fb[y:y + panel_h, b:w // 2] = 0.3                          # a panel fill
    lh = max(6, panel_h // 10)
    for i, row in enumerate(range(y + lh, y + panel_h - lh, 2 * lh)):
        length = int((0.35 + 0.6 * ((i * 37) % 11) / 11) * (w // 2 - 2 * b))
        fb[row:row + lh, b + 4:b + 4 + length] = 0.95          # a line of text
    fb[y:y + panel_h, w // 2 + b:w - b] = 0.55                 # a second panel
    for c in range(w // 2 + 2 * b, w - 2 * b, max(24, w // 20)):
        fb[y + b:y + panel_h - b, c:c + 3] = 0.05              # vertical rules
    # the bright window border last (so it is on top everywhere)
    fb[0:b, :] = 1.0
    fb[h - b:h, :] = 1.0
    fb[:, 0:b] = 1.0
    fb[:, w - b:w] = 1.0
    return fb


def main():
    ap = argparse.ArgumentParser(description="TEMPEST screen-leakage scene generator")
    ap.add_argument("out", help="OUT.cu8 or OUT.cf32")
    ap.add_argument("--mode", default="1024x768@60", choices=sorted(MODES))
    ap.add_argument("--bearing", type=float, default=35.0, help="deg, as the MUSIC plot shows (CCW from ANT0)")
    ap.add_argument("--seconds", type=float, default=4.0)
    ap.add_argument("--snr", type=float, default=25.0, help="peak-edge SNR per sample over noise (dB)")
    ap.add_argument("--offset", type=float, default=300000.0, help="carrier offset in the band (Hz, off DC)")
    ap.add_argument("--rf", type=float, default=0.0, help="RF of the band centre (Hz); 0 = a plausible harmonic")
    ap.add_argument("--channels", type=int, default=5)
    ap.add_argument("--radius", type=float, default=0.12, help="UCA radius (m)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--noise-lsb", type=float, default=1.0, help="receiver noise rms per I/Q (8-bit LSB)")
    args = ap.parse_args()

    fmt = args.out.rsplit(".", 1)[-1]
    if fmt not in ("cu8", "cf32"):
        sys.exit("output must be .cu8 or .cf32")
    if os.path.abspath(args.out.rsplit(".", 1)[0] + ".json") == os.path.abspath(args.out):
        sys.exit("OUT.json would overwrite the output")

    w, h, htotal, vtotal, pclk, nominal = MODES[args.mode]
    refresh = pclk / (htotal * vtotal)                 # actual refresh (e.g. 59.94)
    frame_period = FS / refresh                        # samples per frame (fractional)
    spl = frame_period / vtotal                        # samples per line
    dpix = pclk / FS                                   # pixels advanced per sample (~27)
    M = args.channels
    R = args.radius
    rf = args.rf
    if not rf:
        # a pixel-clock harmonic in UHF (where a KrakenSDR tunes)
        k = max(1, int(round(500e6 / pclk)))
        rf = k * pclk
    lam = C / rf
    seed = args.seed
    rng = np.random.default_rng(seed)
    sig_n = args.noise_lsb
    noise_p = 2 * sig_n ** 2
    a_peak = float(np.sqrt(noise_p * 10 ** (args.snr / 10.0)))

    fb = framebuffer(w, h, rng)
    # active region in the full raster is top-left; blanking (right/bottom) is 0
    ext = np.zeros((vtotal, htotal), np.float32)
    ext[0:h, 0:w] = fb

    b = np.deg2rad(args.bearing)
    phi = -2 * np.pi * np.arange(M) / M
    steer = np.exp(2j * np.pi * R / lam * np.cos(b - phi)).astype(np.complex64)
    phase0 = 0.3

    T = args.seconds
    N = int(T * FS)
    PIECE = int(0.1 * FS)
    with open(args.out, "wb") as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            i = np.arange(c0, c1, dtype=np.float64)
            fp = np.mod(i, frame_period)
            line = np.floor(fp / spl).astype(np.int64)
            np.clip(line, 0, vtotal - 1, out=line)
            into = fp - line * spl
            colf = into / spl * htotal
            # LINEAR interpolation of the raster (not nearest pixel): the real
            # capture is band-limited, so edges span samples rather than being a
            # 1-sample step - which also keeps e(fp) a smooth, exactly periodic
            # function of the fractional frame period.
            def tap(pos):
                c0i = np.floor(pos).astype(np.int64)
                frac = (pos - c0i).astype(np.float32)
                a = np.clip(c0i, 0, htotal - 1)
                b = np.clip(c0i + 1, 0, htotal - 1)
                return ext[line, a] * (1.0 - frac) + ext[line, b] * frac
            # |d luma / d pixel| across one sample's worth of pixels: the edge
            # intensity that radiates (0 in blanking)
            e = np.abs(tap(colf + dpix) - tap(colf)).astype(np.float32)
            carrier = np.exp(1j * (2 * np.pi * args.offset * i / FS + phase0)).astype(np.complex64)
            s = (a_peak * e) * carrier                                  # (n,) baseband leakage
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))).astype(np.complex64) * sig_n
            x += s[:, None] * steer[None, :]
            if fmt == "cu8":
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                y = x / 127.5
                y = np.clip(y.real, -1, 1) + 1j * np.clip(y.imag, -1, 1)
                y.astype(np.complex64).tofile(fo)

    truth = {
        "mode": args.mode, "bearing": args.bearing, "width": w, "height": h,
        "htotal": htotal, "vtotal": vtotal, "refresh_hz": refresh, "nominal_refresh": nominal,
        "pixel_clock_hz": pclk, "offset_hz": args.offset, "frame_period_samples": frame_period,
        "samples_per_line": spl, "snr_db": args.snr,
    }
    side = {"rate": FS, "channels": M, "rf_hz": rf, "radius_m": R, "noise_lsb": sig_n, "truth": truth}
    json.dump(side, open(args.out.rsplit(".", 1)[0] + ".json", "w"), indent=1)
    print("%s: %d antennas, %.2f s at 2.4 MHz (%s), mode %s, bearing %.1f, RF %.3f MHz, offset %.0f Hz" %
          (args.out, M, N / FS, fmt, args.mode, args.bearing, rf / 1e6, args.offset))
    print("  truth: refresh %.3f Hz, %d total lines, line rate %.1f kHz, pixel clock %.1f MHz, eff %.0f px/line" %
          (refresh, vtotal, refresh * vtotal / 1e3, pclk / 1e6, spl))


if __name__ == "__main__":
    main()
