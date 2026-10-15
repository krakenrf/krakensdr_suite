#!/usr/bin/env python3
"""Radiosonde test transmissions with the real frame formats and checks, one
sonde per run, a frame every second while it climbs:

  rs41  Vaisala RS41-SG: GFSK 4800 bit/s (BT 0.5, +-2.4 kHz), keyed ~0.6 s per
        second (40 preamble bytes, the 8-byte header); 320-byte frame, bytes
        LSB first, whitened with the 64-byte mask; blocks 0x79 status (frame
        number, serial, battery, the calibration fragment of the frame -
        fragment 0 = the transmit frequency), 0x7A measurements, 0x7C GPS
        week / time of week, 0x7D raw GPS, 0x7B ECEF position + velocity +
        satellites, 0x76 padding - each with its CRC-16/CCITT; two
        Reed-Solomon (255,231) codewords over the even / odd bytes from 56
        (GF(256) 0x11D, first root alpha^0)
  m20   Meteomodem M20: 2-FSK 9600 Bd (+-6.5 kHz), Manchester, differentially
        coded, bytes MSB first, keyed ~0.17 s per second; 70-byte frame
        (length 0x45, type 0x20): altitude, velocity, time of week, serial,
        week, latitude / longitude, PTU raw values, the 16-bit frame check

  sonde_gen.py OUT.cf32 --type rs41|m20 --serial ID --lat LAT --lon LON
               [--range_km 25] [--az_deg 0] [--alt0 3000] [--climb 5]
               [--seconds 12] [--phase 0] [--freq_mhz 403.0] [--rate 48000]

  --serial  RS41: 8 characters ("U2410853"); M20: "YMM-T-NNNNN" (e.g. "911-2-00269":
            year digit + month, the type digit, the 5-digit number)
  --lat / --lon  the STATION (kraken_doa's playback passes its location):
            the sonde is placed --range_km away at compass bearing --az_deg,
            climbing from --alt0 m at --climb m/s, drifting 8 m/s east
  --phase   where in each second its transmission starts (s)

The frame layouts / checks are the decoder's (plugins/radiosonde/rs41.cpp,
m10.cpp), validated on the radiosonde_auto_rx recordings; the protocol
descriptions of the open-source radiosonde community (rs1729/RS) as
documentation. Writes OUT.cf32 (complex baseband, the sonde on 0 Hz) +
OUT.json. Put several sondes at their own bearings with
plugins/sdk/tools/uca_scene.py.
"""
import json
import math
import sys

import numpy as np

RATE = 48000.0

# --- geodesy ------------------------------------------------------------------------
A_WGS, F_WGS = 6378137.0, 1 / 298.257223563
E2 = F_WGS * (2 - F_WGS)


def geo_to_ecef(lat, lon, h):
    la, lo = math.radians(lat), math.radians(lon)
    n = A_WGS / math.sqrt(1 - E2 * math.sin(la) ** 2)
    return ((n + h) * math.cos(la) * math.cos(lo), (n + h) * math.cos(la) * math.sin(lo), (n * (1 - E2) + h) * math.sin(la))


def enu_to_ecef_vel(ve, vn, vu, lat, lon):
    la, lo = math.radians(lat), math.radians(lon)
    sl, cl, so, co = math.sin(la), math.cos(la), math.sin(lo), math.cos(lo)
    return (-so * ve - sl * co * vn + cl * co * vu, co * ve - sl * so * vn + cl * so * vu, cl * vn + sl * vu)


def destination(lat, lon, az_deg, dist_m):
    R = 6371008.8
    d = dist_m / R
    la, lo, az = math.radians(lat), math.radians(lon), math.radians(az_deg)
    la2 = math.asin(math.sin(la) * math.cos(d) + math.cos(la) * math.sin(d) * math.cos(az))
    lo2 = lo + math.atan2(math.sin(az) * math.sin(d) * math.cos(la), math.cos(d) - math.sin(la) * math.sin(la2))
    return math.degrees(la2), (math.degrees(lo2) + 540) % 360 - 180


def track(o, t):
    """the sonde t seconds into the run: lat, lon, alt, ve, vn, vu"""
    lat, lon = destination(o['lat'], o['lon'], o['az_deg'], o['range_km'] * 1000)
    ve, vn, vu = 8.0, 0.0, o['climb']
    lat2, lon2 = destination(lat, lon, 90.0, ve * t)
    return lat2, lon2, o['alt0'] + vu * t, ve, vn, vu


GPS_WEEK, TOW0 = 2440, 425700.0          # a fixed GPS time (2026-10-16, ~22:15 UTC)


# --- RS41 ---------------------------------------------------------------------------
RS41_HEADER = '0000100001101101010100111000100001000100011010010100100000011111'
RS41_MASK = [0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98, 0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
             0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1, 0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
             0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C, 0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
             0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23, 0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1]

EXP = [0] * 512
LOG = [0] * 256
_x = 1
for _i in range(255):
    EXP[_i] = EXP[_i + 255] = _x
    LOG[_x] = _i
    _x <<= 1
    if _x & 0x100:
        _x ^= 0x11D


def gmul(a, b):
    return EXP[LOG[a] + LOG[b]] if a and b else 0


def rs_parity(data, nroots=24):
    """systematic RS(255, 255-nroots), cw[i] = coefficient of x^i, data at x^nroots..,
    roots alpha^0 .. alpha^(nroots-1): returns the nroots parity bytes (cw[0..nroots-1])"""
    g = [1]
    for j in range(nroots):                      # g(x) *= (x + alpha^j), g[k] = coefficient of x^k
        r = EXP[j]
        ng = [0] * (len(g) + 1)
        for k, c in enumerate(g):
            ng[k + 1] ^= c
            ng[k] ^= gmul(c, r)
        g = ng
    rem = [0] * (nroots + len(data))
    for i, d in enumerate(data):
        rem[nroots + i] = d
    for p in range(len(rem) - 1, nroots - 1, -1):  # long division from the top power
        c = rem[p]
        if c:
            for k in range(nroots + 1):
                rem[p - nroots + k] ^= gmul(c, g[k])
    return rem[:nroots]


def crc16(d):
    c = 0xFFFF
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def le(v, n):
    v = int(v) & ((1 << (8 * n)) - 1)
    return [(v >> (8 * i)) & 0xFF for i in range(n)]


def block(bid, data):
    return [bid, len(data)] + list(data) + le(crc16(data), 2)


def rs41_frame(o, k, t):
    # frame numbers whose calibration fragments are 45..50, 0, 1 ...: fragment
    # 0 (the transmit frequency) in the first seconds; the sensor constants
    # (fragments 3.. ) are not sent (zeros), so no temperature / humidity is
    # computed from them, like a receiver that has just tuned in
    fno = 51 * 100 + 45 + k
    st = [ord(c) for c in (o['serial'] + '        ')[:8]]
    frag = fno % 51
    cal = [0] * 16
    if frag == 0:
        cal[3] = int(round((o['freq_mhz'] * 1000 - 400000) / 40)) & 0xFF
    elif frag == 2:
        cal[7:9] = [0xFF, 0xFF]                  # kill timer off
    elif frag == 0x32:
        cal[0:2] = [0xFF, 0xFF]                  # burst countdown off
    elif frag == 0x21:
        cal[8:16] = [ord(c) for c in 'RS41-SG\0']
    if 3 <= frag <= 50 and frag not in (0x21, 0x31, 0x32):
        cal = [0] * 16
    status = le(fno, 2) + st + [29, 0, 0, 0, 0, 0, 0x1F, 0, 0, 0x26, 0, 0x03, 0x32, frag] + cal
    meas = []
    for f in (0x0AFE21, 0x084C3C, 0x0A1D43, 0x09E0E0, 0x07D2B4, 0x0A8F6A, 0x0A4B12, 0x084C3C, 0x0A1D43, 0, 0, 0):
        meas += le(f, 3)
    meas += [0] * 6
    lat, lon, alt, ve, vn, vu = track(o, t)
    x, y, z = geo_to_ecef(lat, lon, alt)
    vx, vy, vz = enu_to_ecef_vel(ve, vn, vu, lat, lon)
    gps_t = le(GPS_WEEK, 2) + le((TOW0 + t) * 1000, 4) + [0] * 24
    ecef = le(round(x * 100), 4) + le(round(y * 100), 4) + le(round(z * 100), 4) + le(round(vx * 100), 2) + \
        le(round(vy * 100), 2) + le(round(vz * 100), 2) + [9, 12, 15]
    body = [0x0F] + block(0x79, status) + block(0x7A, meas) + block(0x7C, gps_t) + block(0x7D, [0] * 89) + block(0x7B, ecef)
    body += block(0x76, [0] * (320 - 56 - len(body) - 4))
    fr = [0] * 8 + [0] * 48 + body
    assert len(fr) == 320, len(fr)
    for c in range(2):
        data = [fr[56 + 2 * i + c] for i in range(132)]
        par = rs_parity(data)
        fr[8 + 24 * c:8 + 24 * c + 24] = par
    bits = [int(b) for b in RS41_HEADER]
    for i in range(8, 320):
        b = fr[i] ^ RS41_MASK[i % 64]
        bits += [(b >> j) & 1 for j in range(8)]                  # LSB first
    return [0, 1] * 160 + bits                                     # 40 preamble bytes


# --- M20 -------------------------------------------------------------------------------
M10_HEADER = '10011001100110010100110010011001'


def m10_check(d):
    c = 0
    for b in d:
        c1 = c & 0xFF
        b = ((b >> 1) | ((b & 1) << 7)) & 0xFF
        b ^= (b >> 2) & 0xFF
        t6 = (c & 1) ^ ((c >> 2) & 1) ^ ((c >> 4) & 1)
        t7 = ((c >> 1) & 1) ^ ((c >> 3) & 1) ^ ((c >> 5) & 1)
        t = (c & 0x3F) | (t6 << 6) | (t7 << 7)
        s = (c >> 7) & 0xFF
        s ^= (s >> 2) & 0xFF
        c = ((c1 << 8) | ((b ^ t ^ s) & 0xFF)) & 0xFFFF
    return c


def be(v, n):
    v = int(v) & ((1 << (8 * n)) - 1)
    return [(v >> (8 * (n - 1 - i))) & 0xFF for i in range(n)]


def m20_serial(s):
    ym, ty, num = s.split('-')
    ym = int(ym)
    y, m = ym // 100, ym % 100
    return ((y * 12 + m - 1) & 0x7F) | ((int(ty) - 1) & 7) << 7 | (int(num[-4:]) & 0x1FFF) << 10 | (int(num[:-4] or 0) & 1) << 23


def m20_frame(o, k, t):
    lat, lon, alt, ve, vn, vu = track(o, t)
    b = [0] * 70
    b[0], b[1] = 0x45, 0x20
    b[2:4] = le(0x58EB, 2)                       # humidity count, temperature ADC (range 2),
    b[4:6] = le(0x05A4, 2)                       # humidity-sensor NTC: plausible raw values
    b[6:8] = le(0x01C1, 2)
    b[8:11] = be(round(alt * 100), 3)
    b[0x0B:0x0D] = be(round(ve * 100), 2)
    b[0x0D:0x0F] = be(round(vn * 100), 2)
    b[0x0F:0x12] = be(int(TOW0 + t), 3)
    b[0x12:0x15] = le(m20_serial(o['serial']), 3)
    b[0x15] = 0x4B
    b[0x16:0x18] = be(m10_check(b[:0x16]), 2)    # the block check over the first 22 bytes
    b[0x18:0x1A] = be(round(vu * 100), 2)
    b[0x1A:0x1C] = be(GPS_WEEK, 2)
    b[0x1C:0x20] = be(round(lat * 1e6), 4)
    b[0x20:0x24] = be(round(lon * 1e6), 4)
    b[0x24:0x26] = le(0x3F28, 2)                 # pressure (raw)
    b[0x26] = 0xD4                               # battery
    b[0x2F:0x31] = le(0x5472, 2)                 # humidity calibration
    b[0x43] = 0x04                               # firmware
    b[0x44:0x46] = be(m10_check(b[:0x44]), 2)
    # differential Manchester: a data bit 1 = the same Manchester bit as the one before
    data = []
    for x in b:
        data += [(x >> (7 - j)) & 1 for j in range(8)]
    m, chips = 0, []
    for d in data:
        m = m if d else 1 - m
        chips += [0, 1] if m else [1, 0]
    return [1, 0] * 48 + [int(c) for c in M10_HEADER] + chips + [1, 0] * 8


# --- modulation -------------------------------------------------------------------------
def fsk(sym, baud, dev, bt):
    sps = RATE / baud
    n = int(len(sym) * sps) + 1
    idx = np.minimum((np.arange(n) / sps).astype(int), len(sym) - 1)
    f = (2 * np.asarray(sym, float)[idx] - 1) * dev
    if bt:
        s = sps * math.sqrt(math.log(2)) / (2 * math.pi * bt)    # Gaussian filter of BT
        k = np.arange(-int(4 * s), int(4 * s) + 1)
        g = np.exp(-k ** 2 / (2 * s * s))
        f = np.convolve(f, g / g.sum(), 'same')
    return f


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = a[0]
    o = {'type': 'rs41', 'serial': 'U2410853', 'lat': 0.0, 'lon': 0.0, 'range_km': 25.0, 'az_deg': 0.0, 'alt0': 3000.0,
         'climb': 5.0, 'seconds': 12.0, 'phase': 0.0, 'freq_mhz': 403.0, 'rate': RATE}
    for i in range(1, len(a), 2):
        k = a[i].lstrip('-')
        o[k] = a[i + 1] if k in ('type', 'serial') else float(a[i + 1])
    n = int(o['seconds'] * RATE)
    freq = np.zeros(n)
    amp = np.zeros(n)
    nfr = 0
    for k in range(int(o['seconds'])):
        t0 = k + o['phase']
        if o['type'] == 'rs41':
            f = fsk(rs41_frame(o, k, t0), 4800.0, 2400.0, 0.5)
        else:
            f = fsk(m20_frame(o, k, t0), 9600.0, 6500.0, 1.0)
        p = int(round(t0 * RATE))
        if p + len(f) > n:
            continue
        freq[p:p + len(f)] = f
        ramp = np.ones(len(f))
        r = int(0.001 * RATE)
        ramp[:r] = np.linspace(0, 1, r)
        ramp[-r:] = np.linspace(1, 0, r)
        amp[p:p + len(f)] = ramp
        nfr += 1
    x = amp * np.exp(2j * np.pi * np.cumsum(freq) / RATE)
    if abs(o['rate'] - RATE) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        fr = Fraction(o['rate'] / RATE).limit_denominator(1000)
        x = resample_poly(x, fr.numerator, fr.denominator)
    x.astype(np.complex64).tofile(out)
    lat, lon, alt, _, _, _ = track(o, 0)
    print('%s %s: %d frames, from %.1f km at %.0f deg, %.0f m climbing %.1f m/s' % (
        o['type'].upper(), o['serial'], nfr, o['range_km'], o['az_deg'], alt, o['climb']))
    json.dump({'rate': o['rate']}, open(out.rsplit('.', 1)[0] + '.json', 'w'))


if __name__ == '__main__':
    main()
