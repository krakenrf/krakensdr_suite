#!/usr/bin/env python3
"""Synthetic FLARM / OGN tracker packets at known bearings on kraken_doa's UCA,
at 2.4 MHz - test signals for the flarm plugin (decoding, ID separation).

  flarm_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces
(a 5-antenna scene of several seconds is never held in memory whole).
OUT.json (OUT with the extension replaced) gets rate, channels, rf_hz,
radius_m, noise_lsb, station and the truth: every source's bearing and
every packet it sent ([time of its preamble, channel MHz, "flarm" /
"position" / "info", the 26 bytes in hex]).

Physical layer: GFSK BT 0.5 (Gaussian frequency pulse), +-50 kHz, 100
kchip/s, IEEE Manchester (a data bit 1 = chips "0 1", chip 1 = +50 kHz):
16 preamble chips, the 32-bit sync word, 26 bytes MSB first = 496 chips.
  FLARM: preamble of 1 bits, sync F531FAB6; bytes 0-2 the address (little
         endian), byte 3 = address type << 4, bytes 4-23 RANDOM (they stand in
         for the encrypted part, which is not reproduced), CRC-16 (0x1021,
         init 0xFFFF) over 31 FA B6 + bytes 0-23, MSB first.
  OGN:   preamble of 0 bits, sync 0AF3656C; header word + four data words
         (little endian; position or info report), the data words whitened
         with TEA (key 0, 8 cycles) unless "encrypted" (then random), and 6
         LDPC parity bytes from the code's parity-check matrix - read from
         the plugin's data file ../ogn_ldpc_matrix.hpp (systematic encoder by
         GF(2) elimination), every packet checked against all 48 checks.

SCENE (JSON; defaults shown):
{
  "rf_hz": 868.8e6, "seconds": 8, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 2.5,              receiver noise rms per I / Q (8-bit LSB), every antenna
  "station": [-41.0, 174.0],     sources with "range_km" get a position at compass
                                 bearing 360 - bearing (a static station heading 0)
  "bt": 0.5,
  "sources": [
    {"kind": "flarm", "name": "FLR DD1234", "address": "DD1234", "addr_type": 2,
     "channels_mhz": [868.2, 868.4], "phase": 0,  packet k on channel (k + phase) mod n
     "bearing": 60, "level_db": 15,  per-sample SNR (signal / complex noise power)
     "period_s": 1.0, "t0": 0.45,    the first preamble (default: random 0.4..1.2 s)
     "df_hz": 0, "jitter": false,    +-0.1 s random per period
     "invert_payload": false},       test only: all 26 bytes complemented on air
    {"kind": "ogn", "name": "OGN 3C5A21", "address": "3C5A21", "addr_type": 3,
     "acft_type": 1, "alt_m": 1000, "baro_diff_m": -30, "climb_ms": 0, "speed_kmh": 0,
     "track": 0, "turn_dps": 0, "dop": 12, "stealth": false, "encrypted": false,
     "relay": 0, "emergency": false,
     "info": {"Registration": "ZK-GOX"}, "info_every": 3,  packet k with k mod n = n - 1 is an info report
     "range_km": 9 (or "lat" / "lon"; neither = the station),
     "same_as": "name"}              a RELAY of another source: its position / motion /
                                     type / stealth, sent from this source's bearing
  ]                                  with this source's relay count
}
Bearings as the MUSIC DoA plot shows them (CCW from ANT0); antennas on
kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as uca_scene.py).

Test:  plugins/flarm/build/decoder --file OUT.cu8 --channels 5 --uca 0.12 --rf 868.8e6 --opt tune=0
"""
import json
import math
import os
import re
import sys

import numpy as np
from scipy.special import erf

FS = 2.4e6
C = 299792458.0
CHIP_RATE = 100e3
DEV_HZ = 50e3
SYNC = {"flarm": 0xF531FAB6, "ogn": 0x0AF3656C}
INFO_CODES = {"Pilot": 0, "Manufacturer": 1, "Model": 2, "Type": 3, "Serial": 4, "Registration": 5,
              "Competition ID": 6, "Class": 7, "Task": 8, "Base": 9, "ICE": 10, "Pilot ID": 11,
              "Hardware": 12, "Software": 13, "Crew": 14}
M32 = 0xFFFFFFFF
DELTA = 0x9E3779B9


# ---------------------------------------------------------------- codes
def crc16(data):
    """CRC-16/IBM-3740: poly 0x1021, init 0xFFFF, MSB first, no final XOR"""
    r = 0xFFFF
    for b in data:
        r ^= b << 8
        for _ in range(8):
            r = ((r << 1) ^ 0x1021) & 0xFFFF if r & 0x8000 else (r << 1) & 0xFFFF
    return r


def tea_encrypt(v0, v1):
    """TEA with an all-zero key, 8 cycles: the OGN whitening of one 8-byte block"""
    s = 0
    for _ in range(8):
        s = (s + DELTA) & M32
        v0 = (v0 + ((((v1 << 4) & M32) ^ ((v1 + s) & M32) ^ (v1 >> 5)))) & M32
        v1 = (v1 + ((((v0 << 4) & M32) ^ ((v0 + s) & M32) ^ (v0 >> 5)))) & M32
    return v0, v1


class Ldpc:
    """LDPC(208,160) from the plugin's matrix file: rows = 26-byte masks in
    packet byte order (column j = bit 7 - j % 8 of byte j // 8)"""

    def __init__(self, path):
        text = open(path).read()
        rows = re.findall(r'"([0-9A-Fa-f]{52})"', text)
        if len(rows) != 48:
            sys.exit('%s: expected 48 matrix rows, found %d' % (path, len(rows)))
        self.H = np.array([[(int(h[2 * (j // 8):2 * (j // 8) + 2], 16) >> (7 - j % 8)) & 1 for j in range(208)]
                           for h in rows], np.uint8)
        A, B = self.H[:, :160], self.H[:, 160:]
        # B^-1 over GF(2) (Gauss-Jordan on [B | I])
        aug = np.concatenate([B, np.eye(48, dtype=np.uint8)], axis=1)
        for c in range(48):
            piv = next((r for r in range(c, 48) if aug[r, c]), None)
            if piv is None:
                sys.exit('the parity part of the LDPC matrix is singular')
            aug[[c, piv]] = aug[[piv, c]]
            for r in range(48):
                if r != c and aug[r, c]:
                    aug[r] ^= aug[c]
        self.P = (aug[:, 48:].astype(np.int64) @ A.astype(np.int64)) % 2   # parity = P d

    def encode(self, data20):
        d = np.unpackbits(np.frombuffer(bytes(data20), np.uint8))
        p = (self.P @ d.astype(np.int64)) % 2
        pk = bytes(data20) + np.packbits(p.astype(np.uint8)).tobytes()
        if not self.ok(pk):
            sys.exit('LDPC encoder: a packet fails its parity checks')
        return pk

    def ok(self, pk):
        b = np.unpackbits(np.frombuffer(bytes(pk), np.uint8)).astype(np.int64)
        return not ((self.H.astype(np.int64) @ b) % 2).any()


# ---------------------------------------------------------------- OGN fields
def vr_enc(v, B):
    """variable resolution, truncating: steps of 1 / 2 / 4 / 8 under a 2-bit range"""
    T = 1 << B
    v = int(v)
    if v < 0:
        return 0
    if v < T:
        return v
    if v < 3 * T:
        return T + (v - T) // 2
    if v < 7 * T:
        return 2 * T + (v - 3 * T) // 4
    if v < 15 * T:
        return 3 * T + (v - 7 * T) // 8
    return 4 * T - 1


def svr_enc(v, B):
    """signed: the rounded magnitude, the sign bit above the range field"""
    mag = int(round(abs(v)))
    code = vr_enc(mag, B)
    return code | (1 << (B + 2)) if v < 0 and mag > 0 else code


def words_le(b):
    return [int.from_bytes(b[4 * i:4 * i + 4], 'little') for i in range(len(b) // 4)]


def header(s, non_position):
    w = int(s['address'], 16) & 0xFFFFFF
    w |= (int(s.get('addr_type', 3)) & 3) << 24
    w |= (1 if non_position else 0) << 26
    w |= (int(s.get('relay', 0)) & 3) << 28
    w |= (1 if s.get('encrypted') else 0) << 30
    w |= (1 if s.get('emergency') else 0) << 31
    if bin(w & 0x0FFFFFFF).count('1') & 1:      # parity: bits 0..27 even
        w |= 1 << 27
    return w


def position_words(m, t):
    """the four data words of a position report at time t (m = the motion source)"""
    v = float(m.get('speed_kmh', 0)) / 3.6
    trk = math.radians(float(m.get('track', 0)))
    lat0, lon0 = m['_lat'], m['_lon']
    lat = lat0 + v * t * math.cos(trk) / 111320.0
    lon = lon0 + v * t * math.sin(trk) / (111320.0 * max(0.01, math.cos(math.radians(lat0))))
    alt = float(m.get('alt_m', 1000)) + float(m.get('climb_ms', 0)) * t
    la = (int(round(lat * 600000)) >> 3) & 0xFFFFFF
    lo = (int(round(lon * 600000)) >> 4) & 0xFFFFFF
    d0 = la | ((int(t) % 60) << 24) | (1 << 30)                       # fix quality 1 (GPS)
    diff = int(round(float(m.get('baro_diff_m', -30))))
    baro_msb = 1 if 0 <= diff <= 255 else 0
    d1 = lo | (vr_enc(max(0, float(m.get('dop', 12)) - 10), 4) << 24) | (baro_msb << 30) | (1 << 31)   # 3D fix
    d2 = vr_enc(alt, 12) | (vr_enc(v * 10, 8) << 14) | (svr_enc(float(m.get('turn_dps', 0)) * 10, 5) << 24)
    d3 = (int(round(float(m.get('track', 0)) % 360 * 1024 / 360)) & 0x3FF)
    d3 |= svr_enc(float(m.get('climb_ms', 0)) * 10, 6) << 10
    d3 |= (1 if m.get('stealth') else 0) << 19
    d3 |= (int(m.get('acft_type', 1)) & 15) << 20
    d3 |= (diff & 0xFF) << 24
    return [d0, d1, d2, d3]


def info_words(info):
    chars = []
    for name, text in info.items():
        if name not in INFO_CODES:
            sys.exit('unknown info field %r' % name)
        chars += [ord(ch) & 0x7F for ch in str(text)] + [INFO_CODES[name]]
    chars = chars[:16]
    big = 0
    for i, ch in enumerate(chars):
        big |= ch << (7 * i)
    c = bytearray(big.to_bytes(14, 'little'))
    c.append(0x10 | (len(chars) - 1))            # report type 1 (info) above the count
    x = 0
    for v in c:
        x ^= v
    c.append(x)
    return words_le(bytes(c))


def ogn_packet(s, m, k, t, ldpc, rng):
    n = int(s.get('info_every', 0))
    info = bool(s.get('info')) and n > 0 and k % n == n - 1 and 'same_as' not in s
    w0 = header(s, info)
    if s.get('encrypted'):
        D = [int(x) for x in rng.integers(0, 1 << 32, 4, dtype=np.uint64)]
    else:
        D = info_words(s['info']) if info else position_words(m, t)
        D[0], D[1] = tea_encrypt(D[0], D[1])
        D[2], D[3] = tea_encrypt(D[2], D[3])
    data = b''.join(w.to_bytes(4, 'little') for w in [w0] + D)
    return ldpc.encode(data), 'info' if info else 'position'


def flarm_packet(s, rng):
    a = int(s['address'], 16) & 0xFFFFFF
    b = bytearray([a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (int(s.get('addr_type', 2)) & 3) << 4])
    b += bytes(rng.integers(0, 256, 20, dtype=np.uint8))
    c = crc16(bytes([0x31, 0xFA, 0xB6]) + bytes(b))
    b += bytes([c >> 8, c & 0xFF])
    return bytes(b)


# ---------------------------------------------------------------- air
def chips_of(kind, pk, invert=False):
    """16 preamble chips + sync + 26 bytes, Manchester (1 -> "0 1"), as +-1"""
    bits = [1 if kind == 'flarm' else 0] * 8
    bits += [(SYNC[kind] >> (31 - i)) & 1 for i in range(32)]
    body = bytes(x ^ 0xFF for x in pk) if invert else pk
    for byte in body:
        bits += [(byte >> (7 - i)) & 1 for i in range(8)]
    ch = []
    for b in bits:
        ch += [0, 1] if b else [1, 0]
    return 2.0 * np.array(ch, np.float64) - 1


def gfsk(chips, t0, bt):
    """unit-amplitude complex baseband of one packet whose first chip starts at
    t0 s; returns it and its first sample index"""
    T = 1.0 / CHIP_RATE
    ramp = 2
    nc = len(chips)
    k0 = int(math.floor((t0 - ramp * T) * FS))
    k1 = int(math.ceil((t0 + (nc + ramp) * T) * FS)) + 1
    tt = np.arange(k0, k1) / FS - t0
    sigma = math.sqrt(math.log(2)) / (2 * math.pi * bt / T)
    kc = np.floor(tt / T).astype(np.int64)
    f = np.zeros(len(tt))
    for d in range(-3, 4):
        k = kc + d
        ok = (k >= 0) & (k < nc)
        tau = tt - (k + 0.5) * T                  # from that chip's centre
        g = 0.5 * (erf((tau + T / 2) / (sigma * math.sqrt(2))) - erf((tau - T / 2) / (sigma * math.sqrt(2))))
        f += np.where(ok, chips[np.clip(k, 0, nc - 1)] * g, 0.0)
    phase = 2 * np.pi * DEV_HZ * np.cumsum(f) / FS
    env = np.clip(np.minimum(tt + ramp * T, (nc + ramp) * T - tt) / (ramp * T), 0, 1)
    env = 0.5 - 0.5 * np.cos(np.pi * env)        # raised-cosine ramps
    return env * np.exp(1j * phase), k0


def destination(lat, lon, brg_deg, km):
    R = 6371.0
    d = km / R
    b = math.radians(brg_deg)
    la1, lo1 = math.radians(lat), math.radians(lon)
    la2 = math.asin(math.sin(la1) * math.cos(d) + math.cos(la1) * math.sin(d) * math.cos(b))
    lo2 = lo1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(la1), math.cos(d) - math.sin(la1) * math.sin(la2))
    return math.degrees(la2), (math.degrees(lo2) + 540) % 360 - 180


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    scene_path, out = sys.argv[1], sys.argv[2]
    if os.path.splitext(out)[0] == os.path.splitext(scene_path)[0]:
        sys.exit('refused: the sidecar %s.json would overwrite the scene' % os.path.splitext(out)[0])
    fmt = os.path.splitext(out)[1].lstrip('.').lower()
    if fmt not in ('cu8', 'cf32'):
        sys.exit('OUT must be .cu8 or .cf32')
    scene = json.load(open(scene_path))
    ldpc = Ldpc(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ogn_ldpc_matrix.hpp'))
    M = int(scene.get('channels', 5))
    R = float(scene.get('radius_m', 0.12))
    rf = float(scene.get('rf_hz', 868.8e6))
    lam = C / rf
    seconds = float(scene.get('seconds', 8))
    N = int(round(seconds * FS))
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 2.5))
    noise_p = 2 * sig_n ** 2
    bt = float(scene.get('bt', 0.5))
    st = scene.get('station', [-41.0, 174.0])
    phi = -2 * np.pi * np.arange(M) / M           # ANT0 on +x, the others clockwise

    srcs = scene['sources']
    by_name = {s.get('name'): s for s in srcs}
    for s in srcs:
        b = float(s.get('bearing', 0))
        if 'range_km' in s:
            s['_lat'], s['_lon'] = destination(st[0], st[1], (360 - b) % 360, float(s['range_km']))
        else:
            s['_lat'], s['_lon'] = float(s.get('lat', st[0])), float(s.get('lon', st[1]))

    items = []      # (t0, chips, amp, steer, offset_hz)
    truth = []
    for si, s in enumerate(srcs):
        kind = s.get('kind', 'flarm')
        if kind not in ('flarm', 'ogn'):
            sys.exit('unknown kind %r' % kind)
        name = s.get('name', 's%d' % si)
        b = float(s.get('bearing', 0))
        steer = np.exp(2j * np.pi * R / lam * np.cos(np.deg2rad(b) - phi))
        level = float(s.get('level_db', 15))
        amp = math.sqrt(noise_p * 10 ** (level / 10))
        chans = [float(c) for c in s.get('channels_mhz', [868.2])]
        phase = int(s.get('phase', 0))
        period = float(s.get('period_s', 1.0))
        df = float(s.get('df_hz', 0))
        # a relay sends its source's position / motion / type / stealth (the
        # header - address, relay count - is its own)
        motion = s
        if 'same_as' in s:
            if s['same_as'] not in by_name:
                sys.exit('%s: same_as %r: no such source' % (name, s['same_as']))
            motion = by_name[s['same_as']]
        t = float(s['t0']) if 't0' in s else float(rng.uniform(0.4, 1.2))
        sent = []
        k = 0
        while t < seconds - 0.008:
            ch = chans[(k + phase) % len(chans)]
            if kind == 'flarm':
                pk, what = flarm_packet(s, rng), 'flarm'
            else:
                pk, what = ogn_packet(s, motion, k, t, ldpc, rng)
            items.append((t, chips_of(kind, pk, bool(s.get('invert_payload')) and kind == 'flarm'), amp, steer,
                          ch * 1e6 - rf + df))
            sent.append([round(t, 6), round(ch, 4), what, pk.hex()])
            k += 1
            t += period + (float(rng.uniform(-0.1, 0.1)) if s.get('jitter') else 0.0)
        tr = {'name': name, 'kind': kind, 'bearing': b, 'level_db': s.get('level_db', 15),
              'address': s.get('address', ''), 'packets': sent}
        if kind == 'ogn':
            tr['lat'], tr['lon'] = motion['_lat'], motion['_lon']
            for key in ('stealth', 'encrypted', 'relay', 'acft_type', 'alt_m', 'info'):
                if key in s:
                    tr[key] = s[key]
        if s.get('invert_payload'):
            tr['invert_payload'] = True
        truth.append(tr)
        print('%-22s %-5s bearing %6.1f  %3d packets on %s MHz  level %5.1f dB' % (
            name, kind, b, len(sent), '/'.join('%.3f' % c for c in chans), level))

    # every packet's waveform once (5 ms each), then the file piece by piece
    items.sort(key=lambda x: x[0])
    waves = []
    for t0, chips, amp, steer, off in items:
        w, k0 = gfsk(chips, t0, bt)
        kk = np.arange(k0, k0 + len(w))
        w = w * amp * np.exp(1j * (rng.uniform(0, 2 * np.pi) + 2 * np.pi * off * kk / FS))
        waves.append((k0, w, steer))
    starts = np.array([w[0] for w in waves], np.int64) if waves else np.zeros(0, np.int64)
    longest = max((len(w[1]) for w in waves), default=0)
    PIECE = int(0.1 * FS)
    with open(out, 'wb') as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n
            lo = int(np.searchsorted(starts, c0 - longest))
            hi = int(np.searchsorted(starts, c1))
            for k0, w, steer in waves[lo:hi]:
                a, e = max(c0, k0), min(c1, k0 + len(w))
                if e > a:
                    x[a - c0:e - c0, :] += w[a - k0:e - k0, None] * steer[None, :]
            if fmt == 'cu8':
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                y = x / 127.5
                (np.clip(y.real, -1, 1) + 1j * np.clip(y.imag, -1, 1)).astype(np.complex64).tofile(fo)
    side = {'rate': FS, 'channels': M, 'rf_hz': rf, 'radius_m': R, 'noise_lsb': sig_n, 'station': st,
            'truth': truth}
    with open(os.path.splitext(out)[0] + '.json', 'w') as f:
        json.dump(side, f, indent=1)
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s), %d packets' % (out, M, N / FS, fmt, len(items)))


if __name__ == '__main__':
    main()
