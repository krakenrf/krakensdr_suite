#!/usr/bin/env python3
"""Synthetic UAT (978 MHz Universal Access Transceiver) transmissions at known
bearings on kraken_doa's UCA, at 2.4 MHz - test signals for the uat plugin
(decoding, ID separation).

  uat_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces.
OUT.json gets rate, channels, rf_hz, radius_m and the truth (every
transmitter's bearing, what it sent).

Physical layer (RTCA DO-282B, as publicly described): binary CPFSK, 1.041667
Mbit/s, modulation index 0.6 (a 1 = +312.5 kHz), here with a Gaussian
frequency pulse (BT "bt", default 0.5) and a 2-bit amplitude ramp; 36-bit
sync 0xEACDDA4E2 (ADS-B) / 0x153225B1D (ground uplink); Reed-Solomon over
GF(256), field polynomial 0x187, roots alpha^120.., systematic, parity after
the data: basic ADS-B RS(30,18), long ADS-B RS(48,34), uplink 6 x RS(92,72)
byte-interleaved (on-air byte 6 i + b = byte i of block b).

ADS-B payload (18 / 34 bytes): header (payload type, address qualifier,
address), state vector (23-bit latitude / 24-bit longitude, 360 / 2^24 deg,
altitude (25 ft steps), NIC, air / ground state, N / S and E / W velocity,
vertical rate, UTC / TIS-B site ID), mode status (emitter category +
8-character callsign in base 40, emergency, UAT version, SIL, NACp / NACv,
capability / mode bits, CSID) and the secondary altitude. Uplink payload
(432 bytes): ground station position, UTC / application data flags, slot
ID, TIS-B site ID, then information frames holding FIS-B APDUs (product ID,
time, data; text products DLAC-encoded).

SCENE (JSON):
{
  "rf_hz": 978e6, "seconds": 6, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 2.0,              receiver noise rms per I / Q (8-bit LSB)
  "station": [-41.0, 174.0],     where the receiver is: sources with "range_km"
                                 get a position at their compass bearing
                                 (360 - bearing: a static station heading 0)
  "bt": 0.5, "clock_ppm": 0,
  "sources": [
    {"kind": "adsb", "name": "A1B2C3", "address": "A1B2C3", "aq": 0,
     "callsign": "N123AB", "cat": 1, "alt_ft": 4500, "gs_kt": 120, "track": 90,
     "vrate_fpm": -320, "range_km": 30,  (or "lat" / "lon")
     "squawk": 1200 (callsign field holds the squawk, CSID 0) | absent,
     "emergency": 0, "ground": false,
     "bearing": 60, "level_db": 20, "period_s": 1.0, "t0": 0.3, "df_hz": 0},
    {"kind": "uplink", "name": "GS site 11", "site": 11, "slot": 7,
     "range_km": 40, "bearing": 200, "level_db": 25, "period_s": 1.0,
     "products": [{"id": 413, "text": ["METAR NZAA 150600Z 24012KT 9999 FEW030 14/08 Q1015="]},
                  {"id": 63, "bytes": 60}, {"id": 8, "bytes": 30}],
     "replay": "sample.txt"     optional: payloads of real uplinks ("+hex;" lines,
                                 dump978's text format) sent in turn instead
    },
    {"kind": "tisb", "name": "GS site 11", "site": 11, "address": "ABC123",
     "aq": 2, ...adsb fields...,  a TIS-B target broadcast BY the ground station:
     "bearing": 200, ...}        the bearing is the ground station's
    {"kind": "replay", "file": "sample.txt", "bearing": 300, "level_db": 20,
     "period_s": 0.05, "t0": 0.1}  every ADS-B / uplink payload of a dump978 text
                                 file in turn (real content, for field checks)
  ]
}
Bearings as the MUSIC DoA plot shows them (CCW from ANT0); antennas on
kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as uca_scene.py).

Test:  plugins/uat/build/decoder --file OUT.cu8 --channels 5 --uca 0.12 --rf 978e6
"""
import json
import math
import sys

import numpy as np
from scipy.special import erf

FS = 2.4e6
C = 299792458.0
BITRATE = 1e6 / 0.96            # 1.041667 Mbit/s
DEV = 312.5e3                   # h = 0.6
SYNC_ADSB = 0xEACDDA4E2
SYNC_UPLINK = 0x153225B1D
BASE40 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ  .."
DLAC = "\x03ABCDEFGHIJKLMNOPQRSTUVWXYZ\x1a\t\x1e\n| !\"#$%&'()*+,-./0123456789:;<=>?"


# ---------------------------------------------------------------- Reed-Solomon
class RS:
    """systematic RS over GF(256), field polynomial 0x187, roots alpha^(fcr + i);
    codeword = data || parity, data[0] the highest power"""

    def __init__(self, nroots, poly=0x187, fcr=120):
        self.exp = [0] * 512
        self.log = [0] * 256
        x = 1
        for i in range(255):
            self.exp[i] = self.exp[i + 255] = x
            self.log[x] = i
            x <<= 1
            if x & 0x100:
                x ^= poly
        g = [1]
        for i in range(nroots):
            r = self.exp[(fcr + i) % 255]
            ng = [0] * (len(g) + 1)
            for j, c in enumerate(g):            # g(x) * (x - r), g[0] highest
                ng[j] ^= c
                ng[j + 1] ^= self.mul(c, r)
            g = ng
        self.g = g
        self.nroots = nroots

    def mul(self, a, b):
        return 0 if a == 0 or b == 0 else self.exp[self.log[a] + self.log[b]]

    def encode(self, data):
        rem = [0] * self.nroots
        for d in data:
            fb = d ^ rem[0]
            rem = rem[1:] + [0]
            if fb:
                for j in range(self.nroots):
                    rem[j] ^= self.mul(fb, self.g[j + 1])
        return bytes(data) + bytes(rem)


RS_BASIC, RS_LONG, RS_UP = RS(12), RS(14), RS(20)


# ---------------------------------------------------------------- payloads
def latlon_raw(lat, lon):
    rl = int(round((lat if lat >= 0 else lat + 180) / 360 * (1 << 24))) & 0x7FFFFF
    ro = int(round((lon % 360) / 360 * (1 << 24))) & 0xFFFFFF
    return rl, ro


def put_latlon(f, lat, lon, low_bit):
    rl, ro = latlon_raw(lat, lon)
    f[0] = rl >> 15
    f[1] = (rl >> 7) & 0xFF
    f[2] = ((rl & 0x7F) << 1) | (ro >> 23)
    f[3] = (ro >> 15) & 0xFF
    f[4] = (ro >> 7) & 0xFF
    f[5] = ((ro & 0x7F) << 1) | (low_bit & 1)


def vel_raw(v, scale=1):
    v = int(round(v / scale))
    m = min(abs(v), 1022) + 1
    return (0x400 if v < 0 else 0) | m


def adsb_payload(s, ptype, t):
    """18 (type 0) or 34 byte payload for an aircraft / TIS-B target"""
    long_ = ptype != 0
    f = bytearray(34 if long_ else 18)
    aq = int(s.get('aq', 2 if s['kind'] == 'tisb' else 0))
    f[0] = (ptype << 3) | aq
    a = int(s['address'], 16)
    f[1], f[2], f[3] = (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF
    lat, lon = s['_lat'], s['_lon']
    gs, trk = float(s.get('gs_kt', 0)), math.radians(float(s.get('track', 0)))
    # move along the track
    lat += gs * 0.514444 * t * math.cos(trk) / 111320.0
    lon += gs * 0.514444 * t * math.sin(trk) / (111320.0 * max(0.01, math.cos(math.radians(lat))))
    tmp = bytearray(6)
    put_latlon(tmp, lat, lon, 1 if s.get('geo_alt') else 0)
    f[4:10] = tmp
    alt = float(s.get('alt_ft', 0)) + float(s.get('vrate_fpm', 0)) / 60 * t
    ra = int(round((alt + 1000) / 25)) + 1
    ra = max(1, min(ra, 4095))
    nic = int(s.get('nic', 8))
    f[10] = ra >> 4
    f[11] = ((ra & 0xF) << 4) | nic
    if s.get('ground'):
        ag = 2
        rg = min(int(round(gs)), 1022) + 1
        rt = (1 << 9) | (int(round(math.degrees(trk) % 360 * 512 / 360)) & 0x1FF)   # true track
        f[12] = (ag << 6) | (rg >> 6)
        f[13] = ((rg & 0x3F) << 2) | (rt >> 9)
        f[14] = (rt >> 1) & 0xFF
        f[15] = ((rt & 1) << 7) | (2 << 3)       # length / width code 2
    else:
        ag = 0
        rn = vel_raw(gs * math.cos(trk))
        re = vel_raw(gs * math.sin(trk))
        vr = float(s.get('vrate_fpm', 0))
        rv = (0x400 if s.get('vrate_baro', True) else 0) | (0x200 if vr < 0 else 0) | (min(int(round(abs(vr) / 64)), 510) + 1)
        f[12] = (ag << 6) | (rn >> 6)
        f[13] = ((rn & 0x3F) << 2) | (re >> 9)
        f[14] = (re >> 1) & 0xFF
        f[15] = ((re & 1) << 7) | (rv >> 4)
        f[16] = (rv & 0xF) << 4
    if aq in (2, 3):
        f[16] = (f[16] & 0xF0) | (int(s.get('site', 0)) & 0x0F)
    else:
        f[16] |= 0x08                              # UTC coupled
    if ptype in (1, 3):                            # mode status
        cs = s.get('callsign', '')
        csid = 1
        if 'squawk' in s:
            cs = '%04d' % int(s['squawk'])
            csid = 0
        cs = (cs.upper() + ' ' * 8)[:8]
        idx = [BASE40.index(c) if c in BASE40 else 36 for c in cs]
        cat = int(s.get('cat', 1))
        v = [cat * 1600 + idx[0] * 40 + idx[1], idx[2] * 1600 + idx[3] * 40 + idx[4],
             idx[5] * 1600 + idx[6] * 40 + idx[7]]
        for i, x in enumerate(v):
            f[17 + 2 * i] = x >> 8
            f[18 + 2 * i] = x & 0xFF
        f[23] = (int(s.get('emergency', 0)) << 5) | (2 << 2) | 3       # UAT version 2, SIL 3
        f[24] = (int(s.get('mso', 20)) & 0x3F) << 2
        f[25] = (int(s.get('nacp', 9)) << 4) | (int(s.get('nacv', 2)) << 1) | 1
        f[26] = (0x80 if s.get('cdti', True) else 0) | (0x10 if s.get('ident') else 0) | (0x08 if s.get('atc', True) else 0) | (csid << 1)
    if ptype in (1, 2, 5, 6):                      # auxiliary state vector: the other altitude
        a2 = alt + float(s.get('geo_diff_ft', 150))
        r2 = max(1, min(4095, int(round((a2 + 1000) / 25)) + 1))
        f[29] = r2 >> 4
        f[30] = (r2 & 0xF) << 4
    return bytes(f)


def dlac_encode(text):
    out = []
    for ch in text:
        out.append(DLAC.index(ch) if ch in DLAC else DLAC.index(' '))
    while len(out) % 4:
        out.append(0)                              # ETX padding
    b = bytearray()
    for i in range(0, len(out), 4):
        a, c, d, e = out[i:i + 4]
        b += bytes([(a << 2) | (c >> 4), ((c & 0xF) << 4) | (d >> 2), ((d & 3) << 6) | e])
    return bytes(b)


def apdu(pid, data, hh, mm):
    h = bytearray(4)
    h[0] = (pid >> 6) & 0x1F
    h[1] = ((pid & 0x3F) << 2)                     # s flag 0, t_opt 0 (hours, minutes)
    h[2] = (hh << 2) | (mm >> 4)
    h[3] = (mm & 0xF) << 4
    return bytes(h) + data


def uplink_payload(s, k, rng):
    f = bytearray(432)
    put_latlon(f, s['_lat'], s['_lon'], 1)
    f[6] = 0x80 | 0x20 | (int(s.get('slot', 1)) & 0x1F)
    f[7] = (int(s.get('site', 0)) & 0xF) << 4
    pos = 8
    for p in s.get('products', []):
        pid = int(p['id'])
        if 'text' in p:
            body = dlac_encode('\x1e'.join(p['text']) + '\x1e')
        else:
            body = bytes(rng.integers(0, 256, int(p.get('bytes', 40)), dtype=np.uint8))
        d = apdu(pid, body, 6, (k // 60) % 60)
        if pos + 2 + len(d) > 432:
            break
        f[pos] = len(d) >> 1
        f[pos + 1] = ((len(d) & 1) << 7) | 0          # type 0 = FIS-B APDU
        f[pos + 2:pos + 2 + len(d)] = d
        pos += 2 + len(d)
    return bytes(f)


def frame_bits(payload, uplink):
    if uplink:
        blocks = [RS_UP.encode(payload[72 * b:72 * (b + 1)]) for b in range(6)]
        air = bytearray(552)
        for b in range(6):
            for i in range(92):
                air[6 * i + b] = blocks[b][i]
        sync = SYNC_UPLINK
    else:
        air = (RS_BASIC if len(payload) == 18 else RS_LONG).encode(payload)
        sync = SYNC_ADSB
    bits = [(sync >> (35 - i)) & 1 for i in range(36)]
    for byte in air:
        bits += [(byte >> (7 - i)) & 1 for i in range(8)]
    return np.array(bits, np.int8)


def waveform(bits, t0, bt, bitrate):
    """complex baseband of one transmission starting at t0 s (the sync's first
    bit), unit amplitude, its first sample index"""
    T = 1.0 / bitrate
    ramp = 2
    k0 = int(math.floor((t0 - ramp * T) * FS))
    k1 = int(math.ceil((t0 + (len(bits) + ramp) * T) * FS)) + 1
    tt = np.arange(k0, k1) / FS - t0                  # time from the first bit
    nb = len(bits)
    sym = 2.0 * bits - 1
    sigma = math.sqrt(math.log(2)) / (2 * math.pi * bt / T)
    f = np.zeros(len(tt))
    kb = np.floor(tt / T).astype(int)
    for d in range(-3, 4):
        kk = kb + d
        ok = (kk >= 0) & (kk < nb)
        c = (kk + 0.5) * T
        tau = tt - c
        g = 0.5 * (erf((tau + T / 2) / (sigma * math.sqrt(2))) - erf((tau - T / 2) / (sigma * math.sqrt(2))))
        f += np.where(ok, sym[np.clip(kk, 0, nb - 1)] * g, 0.0)
    ph = 2 * np.pi * DEV * np.cumsum(f) / FS
    env = np.clip(np.minimum(tt + ramp * T, (nb + ramp) * T - tt) / (ramp * T), 0, 1)
    env = 0.5 - 0.5 * np.cos(np.pi * env)
    return env * np.exp(1j * ph), k0


def destination(lat, lon, brg_deg, km):
    R = 6371.0
    d = km / R
    b = math.radians(brg_deg)
    la1, lo1 = math.radians(lat), math.radians(lon)
    la2 = math.asin(math.sin(la1) * math.cos(d) + math.cos(la1) * math.sin(d) * math.cos(b))
    lo2 = lo1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(la1), math.cos(d) - math.sin(la1) * math.sin(la2))
    return math.degrees(la2), (math.degrees(lo2) + 540) % 360 - 180


def read_dump978(path):
    """payloads of a dump978 text file: [(uplink?, bytes)]"""
    out = []
    for line in open(path):
        line = line.strip()
        if len(line) < 3 or line[0] not in '+-':
            continue
        h = line[1:].split(';', 1)[0]
        try:
            b = bytes.fromhex(h)
        except ValueError:
            continue
        if (line[0] == '+' and len(b) == 432) or (line[0] == '-' and len(b) in (18, 34)):
            out.append((line[0] == '+', b))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    scene = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    if out.rsplit('.', 1)[0] == sys.argv[1].rsplit('.', 1)[0]:
        sys.exit('the output sidecar would overwrite the scene')
    fmt = out.rsplit('.', 1)[-1]
    M = int(scene.get('channels', 5))
    R = float(scene.get('radius_m', 0.12))
    rf = float(scene.get('rf_hz', 978e6))
    lam = C / rf
    T = float(scene.get('seconds', 6))
    N = int(T * FS)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 2.0))
    noise_p = 2 * sig_n ** 2
    bt = float(scene.get('bt', 0.5))
    bitrate = BITRATE * (1 + float(scene.get('clock_ppm', 0)) * 1e-6)
    st = scene.get('station', [-41.0, 174.0])
    phi = -2 * np.pi * np.arange(M) / M

    items = []      # (t0, bits, amp, steer, df, name)
    truth = []
    for si, s in enumerate(scene['sources']):
        kind = s['kind']
        b = float(s.get('bearing', 0))
        steer = np.exp(2j * np.pi * R / lam * np.cos(np.deg2rad(b) - phi))
        amp = math.sqrt(noise_p * 10 ** (float(s.get('level_db', 20)) / 10))
        df = float(s.get('df_hz', 0))
        if 'range_km' in s:
            s['_lat'], s['_lon'] = destination(st[0], st[1], (360 - b) % 360, float(s['range_km']))
        else:
            s['_lat'], s['_lon'] = float(s.get('lat', st[0])), float(s.get('lon', st[1]))
        period = float(s.get('period_s', 1.0))
        t = float(s.get('t0', rng.uniform(0.19, 0.99)))
        sent = []
        if kind == 'replay':
            frames = read_dump978(s['file'])
            k = 0
            while t < T - 0.01 and k < len(frames):
                up, pl = frames[k]
                items.append((t, frame_bits(pl, up), amp, steer, df, s.get('name', 'replay')))
                sent.append([round(t, 6), ('+' if up else '-') + pl.hex()])
                k += 1
                t += period
        else:
            k = 0
            cycle = [int(c) for c in s.get('types', [1, 0, 2, 0])]
            while t < T - 0.006:
                if kind == 'uplink':
                    if s.get('replay'):
                        fr = [p for u, p in read_dump978(s['replay']) if u]
                        pl = fr[k % len(fr)]
                    else:
                        pl = uplink_payload(s, k, rng)
                    bits = frame_bits(pl, True)
                    sent.append([round(t, 6), '+' + pl.hex()])
                else:
                    pt = cycle[k % len(cycle)]
                    pl = adsb_payload(s, pt, t)
                    bits = frame_bits(pl, False)
                    sent.append([round(t, 6), '-' + pl.hex()])
                items.append((t, bits, amp, steer, df, s.get('name', 's%d' % si)))
                k += 1
                # ADS-B: one message per UAT frame at a pseudo-random MSO
                t += period + (rng.uniform(-0.2, 0.2) if kind != 'uplink' and s.get('jitter', True) else 0)
        tr = {'name': s.get('name', 's%d' % si), 'kind': kind, 'bearing': b, 'level_db': s.get('level_db', 20),
              'frames': sent}
        for key in ('address', 'aq', 'site', 'slot', 'callsign', 'squawk', 'alt_ft', 'gs_kt', 'track'):
            if key in s:
                tr[key] = s[key]
        if kind != 'replay':
            tr['lat'], tr['lon'] = s['_lat'], s['_lon']
        truth.append(tr)
        print('%-12s %-7s bearing %6.1f  %3d frames  level %5.1f dB' % (tr['name'], kind, b, len(sent),
              float(s.get('level_db', 20))))
    items.sort(key=lambda x: x[0])
    # every waveform once (a few ms each)
    waves = []
    for t0, bits, amp, steer, df, name in items:
        w, k0 = waveform(bits, t0, bt, bitrate)
        ph = rng.uniform(0, 2 * np.pi)
        kk = np.arange(k0, k0 + len(w))
        w = w * amp * np.exp(1j * (ph + 2 * np.pi * df * kk / FS))
        waves.append((k0, w, steer))
    starts = np.array([w[0] for w in waves]) if waves else np.zeros(0)
    PIECE = int(0.1 * FS)
    with open(out, 'wb') as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n
            lo = np.searchsorted(starts, c0 - 20000)
            hi = np.searchsorted(starts, c1)
            for k0, w, steer in waves[lo:hi]:
                a, e = max(c0, k0), min(c1, k0 + len(w))
                if e <= a:
                    continue
                x[a - c0:e - c0, :] += w[a - k0:e - k0, None] * steer[None, :]
            if fmt == 'cu8':
                u = np.empty((n, M, 2), np.uint8)
                u[:, :, 0] = np.clip(np.rint(127.5 + x.real), 0, 255)
                u[:, :, 1] = np.clip(np.rint(127.5 + x.imag), 0, 255)
                u.tofile(fo)
            else:
                y = x / 127.5
                y = np.clip(y.real, -1, 1) + 1j * np.clip(y.imag, -1, 1)
                y.astype(np.complex64).tofile(fo)
    side = {'rate': FS, 'channels': M, 'rf_hz': rf, 'radius_m': R, 'noise_lsb': sig_n, 'station': st, 'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s), %d transmissions' % (out, M, N / FS, fmt, len(items)))


if __name__ == '__main__':
    main()
