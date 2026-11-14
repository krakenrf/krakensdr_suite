#!/usr/bin/env python3
"""Synthetic 1090 MHz Mode S / ADS-B transmissions at known bearings on
kraken_doa's UCA, at 2.4 MHz - test signals for the adsb plugin (decoding,
DoA per aircraft).

  adsb_gen.py SCENE.json OUT.cu8|OUT.cf32

Every antenna, sample by sample (antenna 0 first), written in 0.1 s pieces;
OUT.json gets rate, channels, rf_hz, radius_m, station and the truth (each
aircraft's bearing, position and the messages it sent, as hex).

Messages (ICAO Annex 10 Vol IV / DO-260 layouts, CRC-24 generator 0xFFF409):
DF17 extended squitters - identification (TC 4, category + 8-character
callsign), airborne position (TC 11: 25 ft barometric altitude, CPR even /
odd in turn, NZ 15), airborne velocity (TC 19 subtype 1: east / north
ground speed, vertical rate) - and DF11 all-call replies (interrogator
code 0: the parity = the CRC); with "replies" also address / parity
replies to an interrogator (DF4 / DF20 altitude, DF5 / DF21 identity in
turn: the parity = the CRC XOR the address). Pulse-position modulation (preamble at 0 /
1 / 3.5 / 4.5 us, 1 us bits, 0.5 us pulses) built at 24 MHz with ~70 ns
edges and integrated to 2.4 MHz - the sub-sample timing is random, as is
each message's carrier phase; each aircraft has its own carrier offset.

SCENE (JSON):
{
  "rf_hz": 1090e6, "seconds": 6, "channels": 5, "radius_m": 0.12, "seed": 1,
  "noise_lsb": 2.5,              receiver noise rms per I / Q (8-bit LSB)
  "station": [lat, lon],         the receiver: an aircraft's position is at
                                 its compass bearing 360 - bearing (a static
                                 station heading 0) and range_km from here
  "sources": [{"name": "C81A2B", "icao": "C81A2B", "callsign": "ANZ123",
     "cat": 3 (A1..A7), "alt_ft": 23000, "gs_kt": 420, "track": 310,
     "vrate_fpm": -1200, "range_km": 60, "bearing": 40, "level_db": 22,
     "df_hz": 50000, "rate": 1.0 (x the messages per second below),
     "replies": 0 (address / parity replies per second), "squawk": 1234}],
  "collisions": [{"t": 1.5, "a": "C81A2B", "b": "7C4A51", "dt_us": 40}]
                                 an extra position message of a at t and a
                                 velocity message of b dt_us later (overlapping)
}
Per aircraft and second: 2 positions (even / odd in turn), 2 velocities,
1 DF11, an identification every 2.5 s (the first right at its start) - with
random jitter, so some messages of different aircraft overlap. Positions
are kept fixed: the playback loops the file, and a moving aircraft would
jump back at every loop (the plugin rejects a jump faster than ~Mach 2.5);
"moving": true (scene) flies them on with their ground speed and track
(the truth then also has lat_end / lon_end) - for offline tests only.
Bearings as the MUSIC DoA plot shows them (CCW from ANT0); antennas on
kraken_doa's UCA - ANT0 on +x, the others CLOCKWISE (as uca_scene.py).

Test:  plugins/adsb/build/decoder --file OUT.cf32 --channels 5 --uca 0.12 --rf 1090e6 --station LAT,LON
"""
import json
import math
import sys

import numpy as np

FS = 2.4e6
OS = 10                         # built at 24 MHz
C = 299792458.0
CS = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######"


def crc24(data):
    c = 0
    for byte in data:
        c ^= byte << 16
        for _ in range(8):
            c <<= 1
            if c & 0x1000000:
                c ^= 0x1FFF409
    return c & 0xFFFFFF


def df17(icao, me):
    b = bytes([(17 << 3) | 5]) + icao.to_bytes(3, 'big') + me.to_bytes(7, 'big')
    return b + crc24(b).to_bytes(3, 'big')


def df11(icao):
    b = bytes([(11 << 3) | 5]) + icao.to_bytes(3, 'big')
    return b + crc24(b).to_bytes(3, 'big')


def ac13(alt_ft):
    """13-bit altitude code, 25 ft (Q = 1, M = 0)"""
    n = int(round((alt_ft + 1000) / 25.0))
    return ((n >> 5) << 7) | (((n >> 4) & 1) << 5) | 0x10 | (n & 0xF)


def id13(squawk):
    """13-bit identity code C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4 of a 4-digit octal squawk"""
    a, b, c, d = (int(ch) for ch in '%04d' % int(squawk))
    pos = {'C1': 0, 'A1': 1, 'C2': 2, 'A2': 3, 'C4': 4, 'A4': 5, 'B1': 7, 'D1': 8, 'B2': 9, 'D2': 10, 'B4': 11, 'D4': 12}
    v = 0
    for name, digit in (('A', a), ('B', b), ('C', c), ('D', d)):
        for w in (1, 2, 4):
            if digit & w:
                v |= 1 << (12 - pos[name + str(w)])
    return v


def reply(icao, df, alt_ft, squawk):
    """address / parity reply: DF4 / DF20 altitude, DF5 / DF21 identity (flight status 0 airborne,
    Comm-B MB empty); the parity = the CRC XOR the address"""
    code = ac13(alt_ft) if df in (4, 20) else id13(squawk)
    head = (df << 27) | (code & 0x1FFF)      # DF(5) FS(3)=0 DR(5)=0 UM(6)=0 code(13)
    b = head.to_bytes(4, 'big') + (bytes(7) if df >= 20 else b'')
    return b + (crc24(b) ^ icao).to_bytes(3, 'big')


def me_ident(callsign, cat):
    me = (4 << 51) | ((cat & 7) << 48)
    cs = (callsign.upper() + '        ')[:8]
    for k, ch in enumerate(cs):
        me |= CS.index(ch if ch in CS and ch != '#' else ' ') << (42 - 6 * k)
    return me


def cpr_nl(lat):
    lat = abs(lat)
    if lat < 1e-9:
        return 59
    if lat > 87.0:
        return 1
    if lat == 87.0:
        return 2
    a = 1.0 - math.cos(math.pi / 30.0)
    c = math.cos(math.radians(lat))
    return int(math.floor(2 * math.pi / math.acos(1.0 - a / (c * c))))


def cpr_encode(lat, lon, odd):
    """airborne CPR, 17 bits each"""
    i = 1 if odd else 0
    dlat = 360.0 / (60 - i)
    yz = int(math.floor(131072 * ((lat % dlat) / dlat) + 0.5))
    rlat = dlat * (yz / 131072.0 + math.floor(lat / dlat))
    nl = cpr_nl(rlat) - i
    dlon = 360.0 / max(nl, 1)
    xz = int(math.floor(131072 * ((lon % dlon) / dlon) + 0.5))
    return yz & 0x1FFFF, xz & 0x1FFFF


def me_position(lat, lon, alt_ft, odd):
    n = int(round((alt_ft + 1000) / 25.0))
    alt12 = ((n >> 4) << 5) | 0x10 | (n & 0xF)
    yz, xz = cpr_encode(lat, lon, odd)
    return (11 << 51) | (alt12 << 36) | ((1 if odd else 0) << 34) | (yz << 17) | xz


def me_velocity(gs_kt, track, vrate_fpm):
    vew = gs_kt * math.sin(math.radians(track))
    vns = gs_kt * math.cos(math.radians(track))
    ew = min(1022, int(round(abs(vew)))) + 1
    ns = min(1022, int(round(abs(vns)))) + 1
    vr = min(510, int(round(abs(vrate_fpm) / 64.0))) + 1
    me = (19 << 51) | (1 << 48)
    me |= (1 if vew < 0 else 0) << 42 | ew << 32 | (1 if vns < 0 else 0) << 31 | ns << 21
    me |= 1 << 20 | (1 if vrate_fpm < 0 else 0) << 19 | vr << 10   # baro vertical rate
    return me


def ppm(msg, phase_hi):
    """the message's envelope at 2.4 MHz: 24 MHz chips, edges, integrated;
    phase_hi = its start's offset in 24 MHz samples (0..9)"""
    nbits = len(msg) * 8
    sp = 12                                       # 0.5 us at 24 MHz
    n = phase_hi + (16 + 2 * nbits) * sp + 2 * OS
    n += -n % OS
    e = np.zeros(n)
    for c in (0, 2, 7, 9):
        e[phase_hi + c * sp:phase_hi + (c + 1) * sp] = 1
    for i in range(nbits):
        bit = (msg[i >> 3] >> (7 - (i & 7))) & 1
        c = 16 + 2 * i + (0 if bit else 1)
        e[phase_hi + c * sp:phase_hi + (c + 1) * sp] = 1
    e = np.convolve(e, [0.25, 0.5, 0.25], mode='same')
    return e.reshape(-1, OS).mean(axis=1)


def message(s, kind, t=0.0):
    """aircraft s's next message of a kind (positions even / odd in turn) at time t"""
    icao = int(s['icao'], 16)
    if kind == 'pos':
        s['_odd'] = not s['_odd']
        lat, lon = s['_lat'], s['_lon']
        if s.get('_moving'):   # flown on with its ground speed and track since t = 0
            lat, lon = destination(lat, lon, float(s['track']), float(s['gs_kt']) * 1.852 * t / 3600.0)
        return df17(icao, me_position(lat, lon, float(s['alt_ft']), s['_odd']))
    if kind == 'vel':
        return df17(icao, me_velocity(float(s['gs_kt']), float(s['track']), float(s.get('vrate_fpm', 0))))
    if kind == 'df11':
        return df11(icao)
    if kind == 'reply':
        s['_reply'] = (s.get('_reply', -1) + 1) % 4
        return reply(icao, (4, 5, 20, 21)[s['_reply']], float(s['alt_ft']), s.get('squawk', 1234))
    return df17(icao, me_ident(s['callsign'], int(s.get('cat', 3))))


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
    scene = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    if out.rsplit('.', 1)[0] == sys.argv[1].rsplit('.', 1)[0]:
        sys.exit('the output sidecar would overwrite the scene')
    fmt = out.rsplit('.', 1)[-1]
    M = int(scene.get('channels', 5))
    R = float(scene.get('radius_m', 0.12))
    rf = float(scene.get('rf_hz', 1090e6))
    lam = C / rf
    T = float(scene.get('seconds', 6))
    N = int(T * FS)
    rng = np.random.default_rng(int(scene.get('seed', 1)))
    sig_n = float(scene.get('noise_lsb', 2.5))
    noise_p = 2 * sig_n ** 2
    st = scene.get('station', [0.0, 0.0])
    phi = -2 * np.pi * np.arange(M) / M

    items = []          # (t, msg bytes, source index)
    src = {}
    truth = []
    for si, s in enumerate(scene['sources']):
        b = float(s['bearing'])
        s['_steer'] = np.exp(2j * np.pi * R / lam * np.cos(np.deg2rad(b) - phi))
        s['_amp'] = math.sqrt(noise_p * 10 ** (float(s.get('level_db', 20)) / 10))
        s['_lat'], s['_lon'] = destination(st[0], st[1], (360 - b) % 360, float(s['range_km']))
        s['_moving'] = bool(scene.get('moving', False))
        src[s['name']] = si
        s['_odd'] = False
        rate = float(s.get('rate', 1.0))
        t = float(s.get('t0', rng.uniform(0.05, 0.4)))
        items.append((t, message(s, 'ident'), si))
        # the regular messages, each kind on its own jittered clock
        kinds = [('pos', 2.0), ('vel', 2.0), ('df11', 1.0), ('ident', 0.4)]
        if float(s.get('replies', 0)) > 0:
            kinds.append(('reply', float(s['replies'])))
        for kind, per_s in kinds:
            tt = t + rng.uniform(0.01, 1.0 / (per_s * rate))
            while tt < T - 0.0002:
                items.append((tt, message(s, kind, tt), si))
                tt += rng.uniform(0.8, 1.2) / (per_s * rate)
    for c in scene.get('collisions', []):
        a, bb = scene['sources'][src[c['a']]], scene['sources'][src[c['b']]]
        items.append((float(c['t']), message(a, 'pos', float(c['t'])), src[c['a']]))
        items.append((float(c['t']) + float(c['dt_us']) * 1e-6, message(bb, 'vel'), src[c['b']]))
    items.sort(key=lambda x: x[0])
    for si, s in enumerate(scene['sources']):
        sent = [[round(t, 6), m.hex()] for t, m, k in items if k == si]
        truth.append({'name': s['name'], 'icao': s['icao'].upper(), 'callsign': s['callsign'], 'bearing': float(s['bearing']),
                      'lat': s['_lat'], 'lon': s['_lon'], 'alt_ft': s['alt_ft'], 'level_db': s.get('level_db', 20),
                      'messages': sent})
        if s['_moving']:   # where it is at the end
            truth[-1]['lat_end'], truth[-1]['lon_end'] = destination(
                s['_lat'], s['_lon'], float(s['track']), float(s['gs_kt']) * 1.852 * T / 3600.0)
        print('%-8s %-8s bearing %6.1f  %5.1f km  %5.0f ft  %3d messages  level %4.1f dB  df %+.0f kHz' % (
            s['icao'], s['callsign'], float(s['bearing']), float(s['range_km']), float(s['alt_ft']), len(sent),
            float(s.get('level_db', 20)), float(s.get('df_hz', 0)) / 1e3))
    # every waveform (~300 samples each)
    waves = []
    for t, m, si in items:
        s = scene['sources'][si]
        k_hi = int(round(t * FS * OS))
        k0 = k_hi // OS
        e = ppm(m, k_hi % OS)
        kk = np.arange(k0, k0 + len(e))
        w = e * s['_amp'] * np.exp(1j * (rng.uniform(0, 2 * np.pi) + 2 * np.pi * float(s.get('df_hz', 0)) * kk / FS))
        waves.append((k0, w, s['_steer']))
    starts = np.array([w[0] for w in waves]) if waves else np.zeros(0)
    PIECE = int(0.1 * FS)
    with open(out, 'wb') as fo:
        for c0 in range(0, N, PIECE):
            c1 = min(N, c0 + PIECE)
            n = c1 - c0
            x = (rng.standard_normal((n, M)) + 1j * rng.standard_normal((n, M))) * sig_n
            lo = np.searchsorted(starts, c0 - 1000)
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
    print('%s: %d antennas, %.1f s at 2.4 MHz (%s), %d messages' % (out, M, N / FS, fmt, len(items)))


if __name__ == '__main__':
    main()
