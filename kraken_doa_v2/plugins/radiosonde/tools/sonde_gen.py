#!/usr/bin/env python3
"""Radiosonde test signal: one sonde's transmissions as complex baseband.

  sonde_gen.py OUT.cf32 --type rs41|m20 --serial ID --lat LAT --lon LON
               [--range_km 25] [--az_deg 0] [--alt0 3000] [--climb 5]
               [--seconds 12] [--phase 0] [--freq_mhz 403.0] [--rate 48000]

--lat / --lon are the STATION's position: the sonde starts --range_km away at
compass bearing --az_deg, climbs from --alt0 m at --climb m/s and drifts east
at 8 m/s. It sends one frame a second, starting --phase s into each second:

  rs41  Vaisala RS41-SG: GFSK 4800 bit/s (+-2.4 kHz, BT 0.5), a 320-byte frame
        (status with a calibration fragment, measurements without sensor
        constants, GPS time and ECEF position, padding), whitened, the two
        RS(255,231) codewords' parity; --serial is 8 characters
  m20   Meteomodem M20: 9600 chips/s (+-6.5 kHz, Gaussian BT 1.0), the 70-byte
        frame in biphase-mark coding with its check; --serial YMM-T-NNNNN
        (e.g. 911-2-00269)

Writes OUT (complex64 I/Q, the sonde on 0 Hz, no noise) and OUT's name
without extension + .json = {"rate": RATE}; prints one summary line. numpy;
scipy only for --rate other than 48000.
"""
import json
import math
import os
import sys

import numpy as np

FS = 48000
R_SPHERE = 6371008.8

RS41_HEADER_BITS = '0000100001101101010100111000100001000100011010010100100000011111'
RS41_WHITEN = bytes.fromhex(
    '96833E51B1490898 3205590EF944C626 2160C2EA795D6DA1 5469470CDCE85CF1'
    'F776827F0799A22C 937C3063F5102E61 D0BCB4B606AAF423 786E3BAEBF7B4CC1'.replace(' ', ''))
MODEM_SYNC_CHIPS = '10011001100110010100110010011001'

GPS_WEEK = 2440
GPS_TOW0 = 425700.0          # time of week at t = 0 (2026-10-15 ~22:15 UTC)


# --- checks ------------------------------------------------------------------------
def crc16_ccitt_false(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def meteomodem_check(data):
    """The 16-bit Meteomodem frame check (every byte rotated into an LFSR)."""
    c = 0
    for x in data:
        r = ((x >> 1) | (x << 7)) & 0xFF
        r ^= r >> 2
        bit = [(c >> i) & 1 for i in range(6)]
        lo = r ^ ((c >> 7) & 0xFF) ^ ((c >> 9) & 0x3F) ^ (c & 0x3F)
        lo ^= (bit[0] ^ bit[2] ^ bit[4]) << 6
        lo ^= (bit[1] ^ bit[3] ^ bit[5]) << 7
        c = ((c & 0xFF) << 8) | (lo & 0xFF)
    return c


class ReedSolomon:
    """Systematic RS encoder over GF(2^8): roots alpha^(fcr + j), alpha = 2."""

    def __init__(self, poly, fcr, nroots):
        self.exp = [0] * 512
        self.log = [0] * 256
        v = 1
        for i in range(255):
            self.exp[i] = v
            self.log[v] = i
            v <<= 1
            if v & 0x100:
                v ^= poly
        for i in range(255, 512):
            self.exp[i] = self.exp[i - 255]
        self.nroots = nroots
        g = [1]
        for j in range(nroots):
            r = self.exp[(fcr + j) % 255]
            ng = [0] * (len(g) + 1)
            for k, gk in enumerate(g):      # g(x) * (x - r), g[k] = coefficient of x^k
                ng[k + 1] ^= gk
                ng[k] ^= self.mul(gk, r)
            g = ng
        self.gen = g

    def mul(self, a, b):
        return self.exp[self.log[a] + self.log[b]] if a and b else 0

    def parity(self, data):
        """data[i] = coefficient of x^(nroots + i): returns the nroots parity
        bytes, parity[k] = coefficient of x^k (= data x^nroots mod g)."""
        n = self.nroots
        r = [0] * n
        for d in reversed(data):
            fb = d ^ r[n - 1]
            for k in range(n - 1, 0, -1):
                r[k] = r[k - 1] ^ self.mul(fb, self.gen[k])
            r[0] = self.mul(fb, self.gen[0])
        return r


# --- geometry ----------------------------------------------------------------------
def destination(lat, lon, bearing_deg, dist_m):
    """Great-circle destination on a sphere."""
    p1, l1, b = math.radians(lat), math.radians(lon), math.radians(bearing_deg)
    d = dist_m / R_SPHERE
    p2 = math.asin(math.sin(p1) * math.cos(d) + math.cos(p1) * math.sin(d) * math.cos(b))
    l2 = l1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(p1), math.cos(d) - math.sin(p1) * math.sin(p2))
    return math.degrees(p2), (math.degrees(l2) + 540) % 360 - 180


def geodetic_to_ecef(lat, lon, h):
    a = 6378137.0
    b = 6356752.314245
    e2 = 1 - (b * b) / (a * a)
    p, l = math.radians(lat), math.radians(lon)
    n = a / math.sqrt(1 - e2 * math.sin(p) ** 2)
    return ((n + h) * math.cos(p) * math.cos(l), (n + h) * math.cos(p) * math.sin(l), (n * (1 - e2) + h) * math.sin(p))


def enu_to_ecef(lat, lon, e, n, u):
    p, l = math.radians(lat), math.radians(lon)
    sp, cp, sl, cl = math.sin(p), math.cos(p), math.sin(l), math.cos(l)
    return (-sl * e - sp * cl * n + cp * cl * u, cl * e - sp * sl * n + cp * sl * u, cp * n + sp * u)


class Track:
    def __init__(self, o):
        self.lat0, self.lon0 = destination(o['lat'], o['lon'], o['az_deg'], o['range_km'] * 1000)
        self.alt0, self.climb = o['alt0'], o['climb']

    def at(self, t):
        lat, lon = destination(self.lat0, self.lon0, 90.0, 8.0 * t)
        return lat, lon, self.alt0 + self.climb * t


# --- frames ------------------------------------------------------------------------
def le(v, n):
    return int(v).to_bytes(n, 'little', signed=v < 0)


def be(v, n):
    return int(v).to_bytes(n, 'big', signed=v < 0)


def rs41_block(bid, data):
    data = bytes(data)
    return bytes([bid, len(data)]) + data + crc16_ccitt_false(data).to_bytes(2, 'little')


def rs41_frame(o, k, t0, track, rs):
    counter = 5145 + k
    frag = counter % 51
    serial = (o['serial'] + ' ' * 8)[:8].encode('ascii', 'replace')
    f16 = bytearray(16)
    if frag == 0:
        f16[2] = 0
        f16[3] = round((o['freq_mhz'] * 1000 - 400000) / 40) & 0xFF
    elif frag == 2:
        f16[7] = f16[8] = 0xFF                      # flight kill timer off
    elif frag == 0x32:
        f16[0] = f16[1] = 0xFF                      # kill countdown inactive
    elif frag == 0x21:
        f16[8:16] = b'RS41-SG\0'
    status = le(counter, 2) + serial + bytes([29]) + bytes.fromhex('0000000000 1F0000260003 32'.replace(' ', '')) + \
        bytes([frag]) + bytes(f16)
    meas = b''.join(le(v, 3) for v in (0x0AFE21, 0x084C3C, 0x0A1D43, 0x09E0E0, 0x07D2B4, 0x0A8F6A, 0x0A4B12,
                                        0x084C3C, 0x0A1D43, 0, 0, 0)) + bytes(6)
    gpsinfo = le(GPS_WEEK, 2) + le(round((GPS_TOW0 + t0) * 1000), 4) + bytes(24)
    lat, lon, alt = track.at(t0)
    x, y, z = geodetic_to_ecef(lat, lon, alt)
    vx, vy, vz = enu_to_ecef(lat, lon, 8.0, 0.0, o['climb'])
    pos = b''.join(le(round(v * 100), 4) for v in (x, y, z)) + b''.join(le(round(v * 100), 2) for v in (vx, vy, vz)) + \
        bytes([9, 12, 15])
    body = rs41_block(0x79, status) + rs41_block(0x7A, meas) + rs41_block(0x7C, gpsinfo) + \
        rs41_block(0x7D, bytes(89)) + rs41_block(0x7B, pos)
    pad = 320 - 57 - len(body) - 4
    body += rs41_block(0x76, bytes(pad))
    frame = bytearray(8 + 48) + bytes([0x0F]) + body
    assert len(frame) == 320, len(frame)
    for c in range(2):
        data = [frame[56 + 2 * i + c] for i in range(132)]
        frame[8 + 24 * c: 32 + 24 * c] = bytes(rs.parity(data))
    bits = [int(b) for b in '01' * 160] + [int(b) for b in RS41_HEADER_BITS]
    for i in range(8, 320):
        v = frame[i] ^ RS41_WHITEN[i % 64]
        bits += [(v >> b) & 1 for b in range(8)]           # LSB first
    return bits


def m20_serial_bytes(text):
    """YMM-T-NNNNN -> B0 B1 B2 (the leading digit of the number in bit 23,
    the last four digits in bits 10..22)."""
    p = text.split('-')
    if len(p) != 3 or len(p[0]) != 3 or len(p[2]) != 5 or not (p[0] + p[1] + p[2]).isdigit():
        raise SystemExit('--serial for m20: YMM-T-NNNNN, e.g. 911-2-00269')
    y, mm, t = int(p[0][0]), int(p[0][1:]), int(p[1])
    d, n = int(p[2][0]), int(p[2][1:])
    if d > 1 or n > 8191 or not 1 <= mm <= 12 or not 1 <= t <= 8:
        raise SystemExit('--serial %s cannot be sent by an M20' % text)
    s = (n << 10) | (d << 23) | ((((t - 1) >> 1) & 3) << 8)
    b0 = ((y * 12 + mm - 1) & 0x7F) + 128 * ((t - 1) & 1)
    return bytes([b0, (s >> 8) & 0xFF, (s >> 16) & 0xFF])


def m20_frame(o, k, t0, track):
    f = bytearray(70)
    lat, lon, alt = track.at(t0)
    f[0], f[1] = 0x45, 0x20
    f[2:4] = le(0x58EB, 2)
    f[4:6] = le(0x05A4, 2)
    f[6:8] = le(0x01C1, 2)
    f[8:11] = be(round(alt * 100), 3)
    f[0x0B:0x0D] = be(round(8.0 * 100), 2)
    f[0x0D:0x0F] = be(0, 2)
    f[0x0F:0x12] = be(math.floor(GPS_TOW0 + t0), 3)
    f[0x12:0x15] = m20_serial_bytes(o['serial'])
    f[0x15] = (0x4B + k) % 256
    f[0x16:0x18] = be(meteomodem_check(bytes([22]) + bytes(f[2:22])), 2)
    f[0x18:0x1A] = be(round(o['climb'] * 100), 2)
    f[0x1A:0x1C] = be(GPS_WEEK, 2)
    f[0x1C:0x20] = be(round(lat * 1e6), 4)
    f[0x20:0x24] = be(round(lon * 1e6), 4)
    f[0x24:0x26] = le(0x3F28, 2)
    f[0x26] = 0xD4
    f[0x2F:0x31] = le(0x5472, 2)
    f[0x43] = 0x04
    f[0x44:0x46] = be(meteomodem_check(bytes(f[0:0x44])), 2)
    chips = '10' * 48 + MODEM_SYNC_CHIPS
    m = 0
    for byte in f:
        for i in range(7, -1, -1):                       # MSB first
            if not (byte >> i) & 1:
                m ^= 1                                   # a 0 changes the level, a 1 keeps it
            chips += '01' if m else '10'
    chips += '10' * 8
    return [int(c) for c in chips]


# --- modulation ----------------------------------------------------------------------
def modulate(symbols, rate, dev_hz, bt):
    sps = FS / rate
    n = int(math.floor(len(symbols) * sps)) + 1
    idx = np.minimum((np.arange(n) / sps).astype(int), len(symbols) - 1)
    f = np.where(np.asarray(symbols)[idx] > 0, dev_hz, -dev_hz).astype(np.float64)
    if bt:
        sigma = sps * math.sqrt(math.log(2)) / (2 * math.pi * bt)
        m = int(math.floor(4 * sigma))
        k = np.exp(-np.arange(-m, m + 1) ** 2 / (2 * sigma * sigma))
        f = np.convolve(f, k / k.sum(), mode='same')
    x = np.exp(2j * np.pi * np.cumsum(f) / FS)
    a = np.ones(n)
    r = int(0.001 * FS)
    ramp = np.linspace(0, 1, r, endpoint=False)
    a[:r] = ramp
    a[-r:] = ramp[::-1]
    return x * a


def main():
    argv = sys.argv[1:]
    if not argv or argv[0].startswith('-'):
        print(__doc__)
        sys.exit(2)
    out = argv[0]
    o = {'type': 'rs41', 'serial': 'U2410853', 'lat': 0.0, 'lon': 0.0, 'range_km': 25.0, 'az_deg': 0.0, 'alt0': 3000.0,
         'climb': 5.0, 'seconds': 12.0, 'phase': 0.0, 'freq_mhz': 403.0, 'rate': 48000.0}
    rest = argv[1:]
    if len(rest) % 2:
        print(__doc__)
        sys.exit(2)
    for k, v in zip(rest[0::2], rest[1::2]):
        key = k.lstrip('-')
        if not k.startswith('--') or key not in o:
            print('unknown option %s' % k, file=sys.stderr)
            sys.exit(2)
        o[key] = v if key in ('type', 'serial') else float(v)
    o['type'] = o['type'].lower()
    if o['type'] not in ('rs41', 'm20'):
        raise SystemExit('--type rs41 or m20')
    track = Track(o)
    total = int(round(o['seconds'] * FS))
    y = np.zeros(total, dtype=np.complex128)
    rs = ReedSolomon(0x11D, 0, 24)
    frames = 0
    for k in range(int(math.floor(o['seconds']))):
        t0 = k + o['phase']
        if o['type'] == 'rs41':
            x = modulate(rs41_frame(o, k, t0, track, rs), 4800, 2400, 0.5)
        else:
            x = modulate(m20_frame(o, k, t0, track), 9600, 6500, 1.0)
        s = int(round(t0 * FS))
        if s < 0 or s + len(x) > total:
            continue
        y[s:s + len(x)] += x
        frames += 1
    rate = o['rate']
    if abs(rate - FS) > 1e-6:
        from fractions import Fraction
        from scipy.signal import resample_poly
        q = Fraction(rate / FS).limit_denominator(1000)
        y = resample_poly(y, q.numerator, q.denominator)
    y.astype(np.complex64).tofile(out)
    with open(os.path.splitext(out)[0] + '.json', 'w') as fh:
        json.dump({'rate': rate}, fh)
    serial = o['serial'] if o['type'] == 'm20' else (o['serial'] + ' ' * 8)[:8]
    print('%s %s: %d frames, from %.1f km at %.0f deg, %.0f m climbing %.1f m/s'
          % (o['type'].upper(), serial, frames, o['range_km'], o['az_deg'], o['alt0'], o['climb']))


if __name__ == '__main__':
    main()
