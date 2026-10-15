#!/usr/bin/env python3
"""Synthetic FLARM and OGN tracker packets at known bearings on kraken_doa's
UCA, at 2.4 MHz - test signals for the flarm plugin (decoding, ID
separation, channel filtering).

  flarm_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces;
OUT.json gets rate, channels, rf_hz, radius_m and the truth (every
transmitter's bearing and the packets it sent).

Radio (as publicly documented for both): GFSK (Gaussian BT "bt", default
0.5), 100 kchip/s, +-50 kHz (a chip 1 = +50 kHz), Manchester (IEEE: a bit 1
= chips 0 1), 16 preamble chips (FLARM bits 1: the nRF905 preamble 1111110101
ends in the sync's F5; OGN bits 0), then the 32-bit sync and the 26 bytes, MSB
first:
  FLARM  sync F531FAB6, 24 bytes + CRC-16 (CCITT, init FFFF, over 31 FA B6
         and the 24 bytes, sent MSB first). Bytes 0-2 = the radio ID
         (little-endian), byte 3 = address type << 4; bytes 4-23 stand for
         the encrypted part: random here (the plugin never reads them)
  OGN    sync 0AF3656C, 20 bytes = five little-endian 32-bit words (header:
         address, address type, non-position, parity, relay count, encrypted,
         emergency; position / info words whitened with TEA under the all-zero
         key, 8 rounds per pair) + 6 bytes LDPC(208,160) parity - the
         generator is derived here from the plugin's parity-check matrix
         (plugins/flarm/ogn_ldpc.h) by GF(2) elimination

SCENE (JSON):
{
  "rf_hz": 868.8e6,            the VFO centre (sets the wavelength too)
  "seconds": 8, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 2.5,            receiver noise rms per I / Q (8-bit LSB)
  "station": [-41.0, 174.0],   sources with "range_km" get a position at their
                               compass bearing (360 - bearing: heading 0)
  "bt": 0.5,
  "sources": [
    {"kind": "flarm", "name": "FLR DD1234", "address": "DD1234", "addr_type": 2,
     "channels_mhz": [868.2, 868.4], "phase": 0,   the packet k uses channel (k + phase)
     "bearing": 60, "level_db": 14, "period_s": 1.0, "t0": 0.45, "df_hz": 2500},
    {"kind": "ogn", "name": "OGN 3C5A21", "address": "3C5A21", "addr_type": 3,
     "acft_type": 1, "alt_m": 1250, "baro_diff_m": -40, "climb_ms": 1.8,
     "speed_kmh": 95, "track": 210, "turn_dps": 3, "range_km": 9,
     "stealth": false, "encrypted": false, "relay": 0, "emergency": false,
     "info": {"Registration": "ZK-GOX", "Competition ID": "OX", "Model": "ASW 28"},
     "info_every": 3,               every 3rd packet the info report instead
     "same_as": "OGN 3C5A21",       (relays) that source's position + motion, sent with
                                    "relay": 1 from this source's own bearing
     "channels_mhz": [868.2, 868.4], "bearing": 230, "level_db": 16, "t0": 0.62}
  ]
}
Bearings as the MUSIC DoA plot shows them (CCW from ANT0); antennas on
kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as uca_scene.py).

Test:  plugins/flarm/build/decoder --file OUT.cu8 --channels 5 --uca 0.12 --rf 868.8e6
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
CHIP = 100e3
DEV = 50e3
SYNC_FLARM = 0xF531FAB6
SYNC_OGN = 0x0AF3656C
INFO_CODES = {"Pilot": 0, "Manufacturer": 1, "Model": 2, "Type": 3, "Serial": 4, "Registration": 5,
              "Competition ID": 6, "Class": 7, "Task": 8, "Base": 9, "ICE": 10, "Pilot ID": 11,
              "Hardware": 12, "Software": 13, "Crew": 14}


# ---------------------------------------------------------------- LDPC
def load_h():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ogn_ldpc.h')
    txt = open(path).read()
    nums = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{8}', txt[txt.index('LDPC_H'):])]
    return [nums[7 * r:7 * r + 7] for r in range(48)]


def generator(H):
    """parity bit r = parity of (G[r] & data bits 0..159)"""
    hb = [[(H[r][c // 32] >> (c % 32)) & 1 for c in range(208)] for r in range(48)]
    A = np.array([row[:160] for row in hb], np.uint8)
    B = np.array([row[160:] for row in hb], np.uint8)
    M = np.concatenate([B, np.eye(48, dtype=np.uint8)], axis=1)
    for col in range(48):
        piv = col + int(np.nonzero(M[col:, col])[0][0])
        M[[col, piv]] = M[[piv, col]]
        for r in range(48):
            if r != col and M[r, col]:
                M[r] ^= M[col]
    Binv = M[:, 48:]
    return (Binv.astype(int) @ A.astype(int)) % 2


H = load_h()
G = generator(H)


def ldpc_encode(data20):
    bits = np.array([(data20[i // 8] >> (i % 8)) & 1 for i in range(160)], int)
    par = (G @ bits) % 2
    pb = bytearray(6)
    for r in range(48):
        if par[r]:
            pb[r // 8] |= 1 << (r % 8)
    return bytes(data20) + bytes(pb)


def ldpc_ok(pkt):
    for r in range(48):
        acc = 0
        for w in range(7):
            word = 0
            for b in range(4):
                i = 4 * w + b
                if i < 26:
                    word |= pkt[i] << (8 * b)
            acc ^= word & H[r][w]
        if bin(acc).count('1') & 1:
            return False
    return True


# ---------------------------------------------------------------- OGN fields
def vr_encode(v, bits):
    t = 1 << bits
    v = int(max(0, v))
    if v < t:
        return v
    if v < 3 * t:
        return t | ((v - t) >> 1)
    if v < 7 * t:
        return 2 * t | ((v - 3 * t) >> 2)
    if v < 15 * t:
        return 3 * t | ((v - 7 * t) >> 3)
    return 4 * t - 1


def svr_encode(v, bits):
    s = 1 << (bits + 2)
    return (s if v < 0 else 0) | vr_encode(abs(int(round(v))), bits)


def tea_encrypt_key0(v0, v1, loops=8):
    delta, s = 0x9E3779B9, 0
    M = 0xFFFFFFFF
    for _ in range(loops):
        s = (s + delta) & M
        v0 = (v0 + ((((v1 << 4) & M) ^ ((v1 + s) & M) ^ (v1 >> 5)))) & M
        v1 = (v1 + ((((v0 << 4) & M) ^ ((v0 + s) & M) ^ (v0 >> 5)))) & M
    return v0, v1


def ogn_header(s, non_pos):
    h = int(s['address'], 16) & 0xFFFFFF
    h |= (int(s.get('addr_type', 3)) & 3) << 24
    h |= (1 if non_pos else 0) << 26
    h |= (int(s.get('relay', 0)) & 3) << 28
    h |= (1 if s.get('encrypted') else 0) << 30
    h |= (1 if s.get('emergency') else 0) << 31
    if bin(h & 0x0FFFFFFF).count('1') & 1:     # even parity over bits 0..27
        h ^= 1 << 27
    return h


def ogn_position(s, t, k):
    lat, lon = s['_lat'], s['_lon']
    spd = float(s.get('speed_kmh', 0)) / 3.6
    trk = math.radians(float(s.get('track', 0)))
    lat += spd * t * math.cos(trk) / 111320.0
    lon += spd * t * math.sin(trk) / (111320.0 * max(0.01, math.cos(math.radians(lat))))
    alt = float(s.get('alt_m', 1000)) + float(s.get('climb_ms', 0)) * t
    la = int(round(lat * 600000)) >> 3
    lo = int(round(lon * 600000)) >> 4
    d0 = (la & 0xFFFFFF) | ((int(t) % 60) << 24) | (1 << 30)               # fix quality 1
    bdiff = int(round(float(s.get('baro_diff_m', -30))))
    bmsb = 1 if (bdiff & 0xFF00) == 0 else 0
    d1 = (lo & 0xFFFFFF) | (vr_encode(int(s.get('dop', 12)) - 10, 4) << 24) | (bmsb << 30) | (1 << 31)
    d2 = vr_encode(alt, 12) | (vr_encode(spd * 10, 8) << 14) | ((svr_encode(float(s.get('turn_dps', 0)) * 10, 5) & 0xFF) << 24)
    hdg = int(round(float(s.get('track', 0)) % 360 * 1024 / 360)) & 0x3FF
    d3 = hdg | ((svr_encode(float(s.get('climb_ms', 0)) * 10, 6) & 0x1FF) << 10) | ((1 if s.get('stealth') else 0) << 19)
    d3 |= (int(s.get('acft_type', 1)) & 0xF) << 20 | ((bdiff & 0xFF) << 24)
    return [d0, d1, d2, d3]


def ogn_info(s):
    chars = []
    for k, v in s.get('info', {}).items():
        if chars:
            pass
        chars += [ord(c) & 0x7F for c in v] + [INFO_CODES[k]]
    chars = chars[:16]
    b = bytearray(16)
    for i, c in enumerate(chars):
        bit = 7 * i
        by, of = bit >> 3, bit & 7
        b[by] |= (c << of) & 0xFF
        if of > 1 and by + 1 < 14:
            b[by + 1] |= c >> (8 - of)
    b[14] = (1 << 4) | ((len(chars) - 1) & 0x0F)
    chk = 0
    for i in range(15):
        chk ^= b[i]
    b[15] = chk
    return [int.from_bytes(b[4 * i:4 * i + 4], 'little') for i in range(4)]


def ogn_packet(s, t, k, rng):
    every = int(s.get('info_every', 0))
    info = every > 0 and s.get('info') and k % every == every - 1
    words = ogn_info(s) if info else ogn_position(s, t, k)
    if s.get('encrypted'):
        words = [int(x) for x in rng.integers(0, 1 << 32, 4, dtype=np.uint64)]   # the owner's cipher text
    else:
        words[0], words[1] = tea_encrypt_key0(words[0], words[1])
        words[2], words[3] = tea_encrypt_key0(words[2], words[3])
    data = ogn_header(s, info).to_bytes(4, 'little') + b''.join(w.to_bytes(4, 'little') for w in words)
    pkt = ldpc_encode(data)
    assert ldpc_ok(pkt)
    return pkt, ('info' if info else 'position')


def crc16(c, data):
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def flarm_packet(s, rng):
    a = int(s['address'], 16)
    p = bytearray([a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, (int(s.get('addr_type', 2)) & 3) << 4])
    p += bytes(rng.integers(0, 256, 20, dtype=np.uint8))
    c = crc16(crc16(0xFFFF, b'\x31\xfa\xb6'), p)
    return bytes(p) + bytes([c >> 8, c & 0xFF]), 'flarm'


def chips_of(sync, pkt):
    bits = [(sync >> (31 - i)) & 1 for i in range(32)]
    for byte in pkt:
        bits += [(byte >> (7 - i)) & 1 for i in range(8)]
    # preamble: FLARM = the nRF905's 1111110101 (its last 8 bits are the
    # sync's F5) - bits 1 before F5, as SoftRF sends (0x55 chips); OGN = 0xAA
    # chips (bits 0)
    ch = [0, 1] * 8 if sync == SYNC_FLARM else [1, 0] * 8
    for b in bits:
        ch += [0, 1] if b else [1, 0]
    return np.array(ch, np.int8)


def waveform(chips, t0, bt):
    T = 1.0 / CHIP
    k0 = int(math.floor((t0 - 2 * T) * FS))
    k1 = int(math.ceil((t0 + (len(chips) + 2) * T) * FS)) + 1
    tt = np.arange(k0, k1) / FS - t0
    n = len(chips)
    sym = 2.0 * chips - 1
    sigma = math.sqrt(math.log(2)) / (2 * math.pi * bt / T)
    f = np.zeros(len(tt))
    kb = np.floor(tt / T).astype(int)
    for d in range(-3, 4):
        kk = kb + d
        ok = (kk >= 0) & (kk < n)
        tau = tt - (kk + 0.5) * T
        g = 0.5 * (erf((tau + T / 2) / (sigma * math.sqrt(2))) - erf((tau - T / 2) / (sigma * math.sqrt(2))))
        f += np.where(ok, sym[np.clip(kk, 0, n - 1)] * g, 0.0)
    ph = 2 * np.pi * DEV * np.cumsum(f) / FS
    env = np.clip(np.minimum(tt + 2 * T, (n + 2) * T - tt) / (2 * T), 0, 1)
    return (0.5 - 0.5 * np.cos(np.pi * env)) * np.exp(1j * ph), k0


def destination(lat, lon, brg_deg, km):
    d = km / 6371.0
    b = math.radians(brg_deg)
    la1, lo1 = math.radians(lat), math.radians(lon)
    la2 = math.asin(math.sin(la1) * math.cos(d) + math.cos(la1) * math.sin(d) * math.cos(b))
    lo2 = lo1 + math.atan2(math.sin(b) * math.sin(d) * math.cos(la1), math.cos(d) - math.sin(la1) * math.sin(la2))
    return math.degrees(la2), (math.degrees(lo2) + 540) % 360 - 180


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
    rf = float(scene.get('rf_hz', 868.8e6))
    T = float(scene.get('seconds', 8))
    N = int(T * FS)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 2.5))
    noise_p = 2 * sig_n ** 2
    bt = float(scene.get('bt', 0.5))
    st = scene.get('station', [-41.0, 174.0])
    phi = -2 * np.pi * np.arange(M) / M
    waves, truth = [], []
    by_name = {s.get('name'): s for s in scene['sources']}
    for si, s in enumerate(scene['sources']):
        b = float(s.get('bearing', 0))
        if s.get('same_as'):
            # a relay: another tracker re-sends that aircraft's packets (its
            # position, its motion) from where the relay is
            o = by_name[s['same_as']]
            for key in ('acft_type', 'alt_m', 'baro_diff_m', 'climb_ms', 'speed_kmh', 'track', 'turn_dps', 'stealth'):
                if key in o:
                    s[key] = o[key]
            ob = float(o.get('bearing', 0))
            s['_lat'], s['_lon'] = (destination(st[0], st[1], (360 - ob) % 360, float(o['range_km'])) if 'range_km' in o
                                    else (float(o.get('lat', st[0])), float(o.get('lon', st[1]))))
        elif 'range_km' in s:
            s['_lat'], s['_lon'] = destination(st[0], st[1], (360 - b) % 360, float(s['range_km']))
        else:
            s['_lat'], s['_lon'] = float(s.get('lat', st[0])), float(s.get('lon', st[1]))
        amp = math.sqrt(noise_p * 10 ** (float(s.get('level_db', 15)) / 10))
        chans = [float(c) * 1e6 for c in s.get('channels_mhz', [868.2])]
        phase = int(s.get('phase', 0))
        period = float(s.get('period_s', 1.0))
        t = float(s.get('t0', rng.uniform(0.4, 1.2)))
        sent = []
        k = 0
        while t < T - 0.008:
            if s['kind'] == 'flarm':
                pkt, what = flarm_packet(s, rng)
                sync = SYNC_FLARM
            else:
                pkt, what = ogn_packet(s, t, k, rng)
                sync = SYNC_OGN
            fch = chans[(k + phase) % len(chans)]
            off = fch - rf + float(s.get('df_hz', 0))
            w, k0 = waveform(chips_of(sync, pkt), t, bt)
            kk = np.arange(k0, k0 + len(w))
            w = w * amp * np.exp(1j * (rng.uniform(0, 2 * np.pi) + 2 * np.pi * off * kk / FS))
            steer = np.exp(2j * np.pi * R / (C / rf) * np.cos(np.deg2rad(b) - phi))
            waves.append((k0, w, steer))
            sent.append([round(t, 6), round(fch / 1e6, 4), what, pkt.hex()])
            k += 1
            t += period + (rng.uniform(-0.1, 0.1) if s.get('jitter', False) else 0)
        tr = {'name': s.get('name', 's%d' % si), 'kind': s['kind'], 'bearing': b, 'level_db': s.get('level_db', 15),
              'address': s['address'], 'packets': sent}
        if s['kind'] == 'ogn':
            tr['lat'], tr['lon'] = s['_lat'], s['_lon']
            for key in ('stealth', 'encrypted', 'relay', 'acft_type', 'alt_m', 'info'):
                if key in s:
                    tr[key] = s[key]
        truth.append(tr)
        print('%-12s %-5s bearing %6.1f  %3d packets on %s MHz  level %5.1f dB' % (
            tr['name'], s['kind'], b, len(sent), '/'.join('%.3f' % (c / 1e6) for c in chans), float(s.get('level_db', 15))))
    waves.sort(key=lambda x: x[0])
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
    side = {'rate': FS, 'channels': M, 'rf_hz': rf, 'radius_m': R, 'noise_lsb': sig_n, 'station': st, 'truth': truth}
    json.dump(side, open(out.rsplit('.', 1)[0] + '.json', 'w'), indent=1)
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s), %d packets' % (out, M, N / FS, fmt, len(waves)))


if __name__ == '__main__':
    main()
